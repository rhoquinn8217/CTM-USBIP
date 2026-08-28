// Gyro-to-mouse for DualSense / DualSense Edge.
//
// WHAT THIS IS. A DS5 input report carries the gyroscope and accelerometer.
// This turns the gyro's angular velocity into relative mouse movement, so a
// TV -- which has no mouse -- gains one driven by tilting the controller. The
// motion maths (calibration, drift removal, player-space) is Jibb Smart's
// GamepadMotionHelpers (MIT), the same method Steam Input is built on; we only
// read the bytes, gate, scale, and carry the sub-pixel remainder.
//
// WHERE IT SITS. device.inl calls ctm_gyro_mouse::on_ds5_input() once per
// mapped DS5 input report, just before enqueue_input_report(). It never
// modifies the report -- the controller passes through untouched, exactly as
// today -- it only pushes a mouse delta into a queue. A separate synthetic
// mouse device (see ds5_input_overrides / the mouse profile) drains that queue.
//
// WHAT IS OURS vs BORROWED. The byte offsets, the gate logic, and the config
// keys are ours, ported from the on-hardware DS5Dongle reference (which paid
// for the byte-17-not-19 yaw correction). The float maths is the library's.
//
// UNITS. The DualSense reports gyro at 1024 raw units per degree/second and
// accel at 8192 raw units per g. The library wants degrees/second and g.
//
// OFFSETS ARE OURS (report id at index 0). The DS5Dongle reference omits the
// report id, so every one of its offsets is ours - 1. Cross-checked against
// the mapped report this function receives (id 0x01 at [0]).
//   gyro  pitch int16 LE at [16], yaw at [18], roll at [20]
//   accel x int16 LE at [22], y at [24], z at [26]
//   L2 analog [5], R2 analog [6]; buttons byte [9] (L1 bit0, R1 bit1)
//   touchpad finger-1-down = !(byte[33] & 0x80)
//
// GATE VALUES (config, per §6 of the design doc). Naming the gate turns the
// feature on; blank/absent = off. always | L2 | R2 | L1 | R1 | touchpad |
// !touchpad.

#pragma once

// GamepadMotion.hpp is a standalone MIT header. main.cpp includes this file
// inside an anonymous namespace; the library's own headers (<math.h>,
// <algorithm>) are pulled in at the top of main.cpp already, so including the
// hpp here lands its class inside the same anonymous namespace, which is fine
// -- it is self-contained and needs no external linkage.
#include "gamepadmotion/GamepadMotion.hpp"

namespace ctm_gyro_mouse {

// A pending relative mouse movement, in whole pixels, produced by the gyro.
struct MouseDelta {
    int32_t dx = 0;
    int32_t dy = 0;
};

// ---- Gate ------------------------------------------------------------------

enum class Gate {
    Off,
    Always,
    L2,
    R2,
    L1,
    R1,
    Touchpad,
    NotTouchpad,
    TouchpadClick,
    PS,
};

inline Gate parse_gate(const std::string &raw)
{
    // device_config already trims and lowercases callers as needed; match on a
    // lowered copy so "L2" and "l2" both work.
    std::string v;
    v.reserve(raw.size());
    for (char c : raw) {
        v.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    }
    if (v.empty()) return Gate::Off;
    if (v == "always") return Gate::Always;
    if (v == "l2") return Gate::L2;
    if (v == "r2") return Gate::R2;
    if (v == "l1") return Gate::L1;
    if (v == "r1") return Gate::R1;
    if (v == "touchpad") return Gate::Touchpad;
    if (v == "!touchpad" || v == "not_touchpad") return Gate::NotTouchpad;
    if (v == "touchpad_click" || v == "click") return Gate::TouchpadClick;
    if (v == "ps") return Gate::PS;
    // Unknown value is OFF, never an error -- a typo silently disables the
    // feature, it never breaks a session. Same rule as every config lookup.
    return Gate::Off;
}

// True when the gate condition says gyro should be producing movement right
// now. `d` is the mapped DS5 report (id at [0]); `len` must cover the gate
// byte the chosen gate reads.
inline bool gate_open(Gate gate, const uint8_t *d, size_t len)
{
    switch (gate) {
        case Gate::Off:
            return false;
        case Gate::Always:
            return true;
        case Gate::L2:
            return len > 5 && d[5] >= 30;               // analog, ~12% travel
        case Gate::R2:
            return len > 6 && d[6] >= 30;
        case Gate::L1:
            return len > 9 && (d[9] & 0x01);
        case Gate::R1:
            return len > 9 && (d[9] & 0x02);
        case Gate::Touchpad:
            return len > 33 && !(d[33] & 0x80);          // finger 1 down
        case Gate::NotTouchpad:
            return len > 33 && (d[33] & 0x80);           // ratchet: touch pauses
        case Gate::TouchpadClick:
            return len > 10 && (d[10] & 0x02);           // pad pressed in
        case Gate::PS:
            return len > 10 && (d[10] & 0x01);
    }
    return false;
}

// ---- Config (read live per report; the watcher applies changes instantly) --

struct Config {
    Gate gate = Gate::Off;
    // ⭐ Calibration, Steam/JSM style. "Pixels per 360 degrees": turn the
    // controller a full circle and the cursor travels this many pixels at
    // sensitivity 1. 1920 makes one full turn sweep a 1080p screen, which is
    // JoyShockMapper's documented 2D-cursor calibration (1920/360 = 5.333
    // pixels per degree).
    int px_per_360 = 1920;
    // Two-tier sensitivity, JSM's shipped 2D defaults. Slow movement uses
    // min_sens for precision, fast movement ramps to max_sens for big turns,
    // interpolated by rotation speed between the two thresholds.
    int min_sens = 8;
    int max_sens = 16;
    float speed_h = 100.0f;     // percent: horizontal speed, 100 = unchanged
    float speed_v = 100.0f;     // percent: vertical speed, 100 = unchanged
    bool debug_scale = false;   // print measured rate, dt and pixels at 2Hz
    int min_threshold = 5;      // deg/sec: below this, min_sens applies
    int max_threshold = 75;     // deg/sec: above this, max_sens applies
    bool invert_x = false;
    bool invert_y = false;
    bool player_space = true;   // matches Steam's default for a standalone pad
    // ⭐ Recenter. Names a button that warps the real Windows cursor back to
    // the middle of the primary screen. Blank = off.
    //
    // ⚠️ THIS IS A DESKTOP FEATURE, NOT AN AIMING ONE. Fullscreen games hide
    // the cursor and read raw relative movement, so they never look at cursor
    // POSITION -- warping it does nothing there. It exists because navigating
    // Windows from a couch has no desk to lift a mouse off, so running the
    // cursor into a screen edge is otherwise a dead end.
    Gate recenter = Gate::Off;
};

// The section is "ds5" or "ds5_edge" -- same keys under each so an Edge can be
// tuned independently. Reads through the same device_config_* accessors the
// audio overrides use.
inline Config load_config(const char *section)
{
    Config c;
    c.gate = parse_gate(device_config_str(section, "gyro_to_mouse_gate"));
    c.px_per_360 = device_config_int(section, "gyro_mouse_px_per_360", 1920);
    if (c.px_per_360 < 1) c.px_per_360 = 1920;
    c.speed_h = static_cast<float>(device_config_int(section, "gyro_mouse_speed_h", 100));
    c.speed_v = static_cast<float>(device_config_int(section, "gyro_mouse_speed_v", 100));
    c.debug_scale = device_config_bool(section, "gyro_mouse_debug_scale", false);
    c.min_sens = device_config_int(section, "gyro_mouse_min_sens", 8);
    c.max_sens = device_config_int(section, "gyro_mouse_max_sens", 16);
    if (c.min_sens < 0) c.min_sens = 0;
    if (c.max_sens < 0) c.max_sens = 0;
    c.min_threshold = device_config_int(section, "gyro_mouse_min_threshold", 5);
    c.max_threshold = device_config_int(section, "gyro_mouse_max_threshold", 75);
    if (c.max_threshold <= c.min_threshold) c.max_threshold = c.min_threshold + 1;
    const int inv = device_config_int(section, "gyro_mouse_invert", 0);
    c.invert_x = (inv & 1) != 0;
    c.invert_y = (inv & 2) != 0;
    c.player_space = device_config_bool(section, "gyro_mouse_player_space", true);
    c.recenter = parse_gate(device_config_str(section, "gyro_mouse_recenter_button"));

    // ⓘ Back-compat: a single `gyro_mouse_sens` still works and scales both
    // tiers, so an existing config keeps meaning something. 50 = the defaults
    // above; 100 = double; 25 = half.
    const int legacy = device_config_int(section, "gyro_mouse_sens", 0);
    if (legacy > 0) {
        c.min_sens = (c.min_sens * legacy) / 50;
        c.max_sens = (c.max_sens * legacy) / 50;
    }
    return c;
}

// ---- Cursor recentre -------------------------------------------------------
//
// Warps the REAL Windows cursor to the middle of the primary screen. This is
// deliberately NOT routed through the synthetic mouse: that device sends
// relative movement and has no idea where the cursor is, so it cannot target a
// position. SetCursorPos can, and this is a desktop-navigation feature.
//
// ⚠️ Does nothing visible in a fullscreen game -- games hide the cursor and
// read raw relative movement, never cursor position. That is expected.
inline void warp_cursor_to_centre()
{
    const int w = GetSystemMetrics(SM_CXSCREEN);
    const int h = GetSystemMetrics(SM_CYSCREEN);
    if (w > 0 && h > 0) {
        SetCursorPos(w / 2, h / 2);
    }
}

// ---- Per-device state ------------------------------------------------------
//
// One instance per bridged DS5 session. Holds the motion filter (calibration
// state lives here) and the sub-pixel remainder that MUST persist between
// reports -- without it a slow turn producing <1px per report rounds to zero
// forever and the cursor never moves.

class GyroMouse {
public:
    GyroMouse()
    {
        // Stillness auto-calibration: the filter watches for the controller
        // being held still (low variance, not low value) and learns the resting
        // bias on its own. This is what keeps the deadzone tiny, which is what
        // makes slow aiming survive. No "put it down for 2 seconds" prompt.
        motion_.SetCalibrationMode(GamepadMotionHelpers::CalibrationMode::Stillness);
    }

    // Feed one mapped DS5 report. Returns true and fills `out` when there is a
    // non-zero mouse movement to emit; returns false when the gate is closed,
    // the config is off, or the movement rounded to zero this tick.
    // The controller's own gyro calibration. Set once when the session comes up;
    // defaults to the old fixed divisor so an uncalibrated pad still works.
    void set_calibration(const ctm_gyro_calib::Scale &s) { cal_ = s; }

    bool on_report(const uint8_t *d, size_t len, const char *section, MouseDelta *out)
    {
        if (d == nullptr || len < 28 || out == nullptr) {
            return false;                       // need through the accel block
        }

        const Config cfg = load_config(section);

        // ⓘ Gate diagnostic. Off unless gyro_mouse_debug_gate is set, and rate
        // limited to twice a second -- this path runs 250x/sec. Prints the raw
        // bytes every gate reads so a gate that never opens can be diagnosed by
        // measurement rather than by guessing at offsets.
        if (device_config_bool(section, "gyro_mouse_debug_gate", false)) {
            static auto lastPrint = std::chrono::steady_clock::now();
            const auto nowDbg = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowDbg - lastPrint).count() >= 500) {
                lastPrint = nowDbg;
                device_log::input_s() << "[gyro] len=" << len
                          << " L2[5]=" << (len > 5 ? (int)d[5] : -1)
                          << " R2[6]=" << (len > 6 ? (int)d[6] : -1)
                          << " btn[9]=0x" << std::hex << (len > 9 ? (int)d[9] : 0)
                          << " btn[10]=0x" << (len > 10 ? (int)d[10] : 0) << std::dec
                          << " touch[33]=0x" << std::hex << (len > 33 ? (int)d[33] : 0) << std::dec
                          << " gateOpen=" << (gate_open(cfg.gate, d, len) ? 1 : 0)
                          << std::endl;
            }
        }

        // ⭐ Recenter runs BEFORE the gate check, and regardless of whether
        // gyro is producing movement -- it is a navigation aid, useful exactly
        // when the cursor is stranded and gyro may well be off.
        //
        // Edge-triggered: fires once on press, not repeatedly while held.
        if (cfg.recenter != Gate::Off) {
            const bool down = gate_open(cfg.recenter, d, len);
            if (down && !recenterWasDown_) {
                warp_cursor_to_centre();
            }
            recenterWasDown_ = down;
        } else {
            recenterWasDown_ = false;
        }

        if (cfg.gate == Gate::Off) {
            reset_remainder();
            return false;
        }

        // deltaTime from the report cadence. First report seeds the clock.
        const auto now = std::chrono::steady_clock::now();
        float dt = 0.0f;
        if (haveClock_) {
            dt = std::chrono::duration<float>(now - lastReport_).count();
        }
        lastReport_ = now;
        haveClock_ = true;
        // Guard against a stalled session resuming with a huge dt (which would
        // fling the cursor). Clamp to a sane window; 0 dt is fine (library
        // treats it as a still-sample tick).
        if (dt < 0.0f || dt > 0.1f) dt = 0.0f;

        // Raw signed 16-bit little-endian reads at our offsets.
        auto rd16 = [&](size_t off) -> int32_t {
            return static_cast<int16_t>(
                static_cast<uint16_t>(d[off]) |
                (static_cast<uint16_t>(d[off + 1]) << 8));
        };

        // Convert to the library's units.
        //
        // ⭐ GYRO USES THE CONTROLLER'S OWN CALIBRATION when it could be read.
        // The raw values are not deg/s over a fixed divisor -- every unit ships
        // its own scale in feature report 0x05, and 1024 is what the Linux
        // driver normalises TO after applying it, not a substitute for it.
        // Measured 2026-08-22: the fixed divisor read ~2 deg/s for a turn that
        // was really ~45.
        //
        // ⓘ When calibration is unavailable the Scale defaults to the old
        // 1/1024 with zero bias, so behaviour is unchanged rather than absent.
        const ctm_gyro_calib::Scale &cal = cal_;
        const float gyroPitch = (rd16(16) - cal.biasPitch) * cal.pitch;
        const float gyroYaw   = (rd16(18) - cal.biasYaw)   * cal.yaw;
        const float gyroRoll  = (rd16(20) - cal.biasRoll)  * cal.roll;
        // accel: 8192 raw units per g, and not calibrated here -- the library
        // only uses it to work out which way is down.
        const float accelX    = rd16(22) / 8192.0f;
        const float accelY    = rd16(24) / 8192.0f;
        const float accelZ    = rd16(26) / 8192.0f;

        // The library ALWAYS runs -- its calibration must keep observing even
        // when the gate is shut, or it never learns the bias. Axis order is the
        // library's Y-up convention: (pitch=X, yaw=Y, roll=Z) matches how it
        // derives player-space from a PlayStation pad.
        motion_.ProcessMotion(gyroPitch, gyroYaw, gyroRoll,
                              accelX, accelY, accelZ, dt);

        // Gate AFTER processing, so calibration is continuous but movement only
        // emits when the player is actually aiming.
        if (!gate_open(cfg.gate, d, len)) {
            reset_remainder();
            return false;
        }

        // ⭐⭐ AXIS MAPPING. Both of the library's two-axis outputs return
        // x = VERTICAL (pitch) and y = HORIZONTAL (yaw) -- they stay in the
        // controller's own axes rather than screen order. From the library's
        // README: "Y is the horizontal part of the rotation, and X is the
        // vertical part ... treat the Y as the horizontal or yaw input and X
        // as the vertical or pitch input."
        //
        // ⛔ An earlier version fed these straight through as (horizontal,
        // vertical), which is why the axes came out swapped on hardware. Both
        // branches now swap identically -- this is also exactly what
        // JoyShockMapper does (MOUSE_X_FROM_GYRO_AXIS = Y, MOUSE_Y = X).
        float vertical = 0.0f;      // pitch, deg/sec
        float horizontal = 0.0f;    // yaw, deg/sec
        if (cfg.player_space) {
            motion_.GetPlayerSpaceGyro(vertical, horizontal);
        } else {
            float roll;
            motion_.GetCalibratedGyro(vertical, horizontal, roll);
        }

        // ⚠️ SIGNS ARE EMPIRICAL, NOT DERIVED. The DualSense's physical
        // positive-rotation directions are not authoritatively documented, and
        // screen Y grows downward while the library's frame is Y-up. These two
        // constants were set by turning a real controller and watching the
        // cursor. If a future controller or library version disagrees, flip
        // them here -- or, without rebuilding, use gyro_mouse_invert.
        constexpr float kSignH = -1.0f;   // turn left -> cursor left
        constexpr float kSignV = -1.0f;   // tilt up   -> cursor up

        // ⭐ Speed-based sensitivity, JSM's shaped-sensitivity approach: slow
        // movement stays precise, fast movement ramps up for big turns.
        const float speed = std::sqrt(horizontal * horizontal + vertical * vertical);
        const float loT = static_cast<float>(cfg.min_threshold);
        const float hiT = static_cast<float>(cfg.max_threshold);
        float t = (speed - loT) / (hiT - loT);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        const float sens = static_cast<float>(cfg.min_sens) +
                           t * static_cast<float>(cfg.max_sens - cfg.min_sens);

        // ⭐⭐ THE SCALE, with the step that was missing before.
        //
        // The gyro reports degrees per SECOND. Movement for this report is
        // therefore rate * dt -- degrees actually turned since the last one.
        // ⛔ An earlier version omitted dt entirely and treated deg/sec as
        // pixels, which made the result both wrong and dependent on report
        // rate. Real World Calibration then converts degrees to pixels:
        // px_per_360 / 360 pixels for every degree turned.
        const float pxPerDegree = static_cast<float>(cfg.px_per_360) / 360.0f;
        const float step = dt * pxPerDegree * sens;

        // ⭐⭐ SCALE DIAGNOSTIC. Off unless gyro_mouse_debug_scale is set.
        //
        // ⛔ WHY IT EXISTS. px_per_360 = 1920 with sens 8 should move the cursor
        // 3840 px for a 90 degree turn, and on paper it does -- the arithmetic
        // and the sub-pixel carry were both checked and are correct. In practice
        // a usable speed needed px_per_360 around 64000, roughly 33x. A gap that
        // size is a fault somewhere, not a preference, and it must be MEASURED
        // rather than guessed at.
        //
        // Accumulates over the print window instead of sampling one report, so a
        // deliberate turn can be compared against what actually came out:
        //
        //   deg   -- degrees the gyro says were turned in this window
        //   px    -- pixels emitted for them
        //   dt    -- mean seconds between reports. ⚠️ Expect ~0.004 at 250 Hz.
        //            Much smaller, or often zero, and that IS the answer: the
        //            guard above zeroes any gap over 100 ms, contributing
        //            nothing at all.
        //   rate  -- mean deg/sec while moving. Turn ~90 degrees over two
        //            seconds and this should read ~45. If it reads ~1.4, the
        //            1024 raw-units-per-deg/sec divisor is wrong -- that is the
        //            figure the Linux driver NORMALISES to after applying the
        //            controller's own calibration report, not necessarily what
        //            raw values divide by without it.
        //
        // Expected ratio: px / deg == px_per_360/360 * sens. Whatever it
        // actually reads localises the loss.
        if (cfg.debug_scale) {
            static auto lastScale = std::chrono::steady_clock::now();
            static double accDeg = 0.0, accPx = 0.0, accDt = 0.0, accRate = 0.0;
            static int nReports = 0, nMoving = 0;
            const float mag = std::sqrt(horizontal * horizontal + vertical * vertical);
            accDeg += mag * dt;
            // dx/dy are not built yet at this point, so derive the same
            // magnitude from the inputs to the step.
            accPx += static_cast<double>(mag) * step;
            accDt += dt;
            ++nReports;
            if (mag > 1.0f) { accRate += mag; ++nMoving; }
            const auto nowScale = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowScale - lastScale).count() >= 500) {
                lastScale = nowScale;
                device_log::input_s() << "[gyro] deg=" << accDeg
                          << " px=" << accPx
                          << " px/deg=" << (accDeg > 0.001 ? accPx / accDeg : 0.0)
                          << " expected=" << (cfg.px_per_360 / 360.0f) * sens
                          << " dt=" << (nReports ? accDt / nReports : 0.0)
                          << " rate=" << (nMoving ? accRate / nMoving : 0.0)
                          << " reports=" << nReports
                          << std::endl;
                accDeg = accPx = accDt = accRate = 0.0;
                nReports = nMoving = 0;
            }
        }

        // ⭐ Per-axis scale. Screens are wider than they are tall, so equal
        // sensitivity means crossing the width takes longer than the height --
        // and fine aiming often wants vertical slower than horizontal
        // regardless. 100 is unchanged, so absent behaves as it always did.
        //
        // ⓘ JoyShockMapper spells this as a second value on its sensitivity
        // commands. Two keys here instead, because this project's config format
        // is one value per key and a silently-optional second number would be
        // easy to miss on a settings page.
        const float scaleH = cfg.speed_h / 100.0f;
        const float scaleV = cfg.speed_v / 100.0f;

        float dx = horizontal * step * kSignH * scaleH;
        float dy = vertical * step * kSignV * scaleV;
        if (cfg.invert_x) dx = -dx;
        if (cfg.invert_y) dy = -dy;

        // Carry the sub-pixel remainder between reports.
        remX_ += dx;
        remY_ += dy;
        const int32_t outX = static_cast<int32_t>(remX_);   // trunc toward zero
        const int32_t outY = static_cast<int32_t>(remY_);
        remX_ -= static_cast<float>(outX);
        remY_ -= static_cast<float>(outY);

        if (outX == 0 && outY == 0) {
            return false;
        }
        out->dx = outX;
        out->dy = outY;
        return true;
    }

private:
    ctm_gyro_calib::Scale cal_;

    void reset_remainder()
    {
        // When the gate closes, drop the fractional carry so a re-open starts
        // clean rather than releasing a stored fraction as a tiny jump.
        remX_ = 0.0f;
        remY_ = 0.0f;
    }

    GamepadMotion motion_;
    float remX_ = 0.0f;
    float remY_ = 0.0f;
    std::chrono::steady_clock::time_point lastReport_{};
    bool haveClock_ = false;
    bool recenterWasDown_ = false;      // edge detection for the recentre button
};

// ---- Cross-session mailbox -------------------------------------------------
//
// The DS5 session produces deltas; the synthetic mouse device consumes them.
// They are separate CtmUsbipDevice objects with separate endpoints, so a
// simple mutex-guarded accumulator couples them without sharing lifetimes.
// The mouse device drains this on each interrupt-IN poll.

class MouseMailbox {
public:
    void push(const MouseDelta &delta)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Accumulate rather than queue: many gyro reports arrive between mouse
        // polls, and the cursor only cares about the sum since the last poll.
        // Clamp to the HID mouse report's signed-byte range on drain, not here,
        // so fast flicks are not silently truncated mid-accumulation.
        pendingX_ += delta.dx;
        pendingY_ += delta.dy;
        hasPending_ = true;
    }

    // Returns true and fills a clamped [-127,127] delta if movement is pending.
    // Leaves any overflow beyond one report in the accumulator for the next
    // poll, so a large flick spreads across a couple of reports rather than
    // being clipped.
    bool drain(int8_t *dx, int8_t *dy)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasPending_ || (pendingX_ == 0 && pendingY_ == 0)) {
            hasPending_ = false;
            return false;
        }
        const int32_t cx = clamp8(pendingX_);
        const int32_t cy = clamp8(pendingY_);
        pendingX_ -= cx;
        pendingY_ -= cy;
        hasPending_ = (pendingX_ != 0 || pendingY_ != 0);
        *dx = static_cast<int8_t>(cx);
        *dy = static_cast<int8_t>(cy);
        return true;
    }

private:
    static int32_t clamp8(int32_t v)
    {
        if (v > 127) return 127;
        if (v < -127) return -127;
        return v;
    }

    std::mutex mutex_;
    int32_t pendingX_ = 0;
    int32_t pendingY_ = 0;
    bool hasPending_ = false;
};

// ---- Entry point called from device.inl ------------------------------------
//
// One GyroMouse and one mailbox per process is the simplest correct thing for
// the single-DS5 case. Two DualSenses bridged at once would share these, which
// is acceptable for a first cut (both feed one cursor, which is how Windows
// merges mice anyway) and is called out as a known limitation. If per-device
// separation is wanted later, key these by device pointer.

// ⭐ ONE GyroMouse PER PHYSICAL CONTROLLER, keyed by the device instance.
//
// ⛔ A single shared instance was a real defect the moment two DualSenses are
// bridged: BOTH fed one motion filter, so controller B's rotation was added to
// controller A's calibration and fractional remainder. Neither aims correctly,
// and the symptom -- drift and stutter that only appears with two pads -- is
// miserable to diagnose. Per-controller config makes it worse still, since B's
// settings would be read while A's state ran.
//
// Keyed by the device POINTER, which is unique and always present, unlike a
// serial that may be empty or shared between units.
struct GyroRegistry {
    std::mutex mutex;
    std::map<const void *, std::unique_ptr<GyroMouse>> instances;
};

inline GyroRegistry &registry()
{
    static GyroRegistry r;
    return r;
}

inline GyroMouse &gyro_for(const void *deviceKey)
{
    GyroRegistry &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.instances.find(deviceKey);
    if (it == r.instances.end()) {
        it = r.instances.emplace(deviceKey, std::make_unique<GyroMouse>()).first;
    }
    return *it->second;
}

// Call when a device goes away, so its motion state does not outlive it: a
// reconnecting controller starts with clean calibration rather than inheriting
// a stale bias, and the map does not grow across a long session of reconnects.
inline void forget_device(const void *deviceKey)
{
    GyroRegistry &r = registry();
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        r.instances.erase(deviceKey);
    }
    // The calibration belongs to the physical controller, so it goes when the
    // device does -- a different pad on the same slot must not inherit it.
    ctm_gyro_calib::forget(deviceKey);
}

inline MouseMailbox &shared_mailbox()
{
    static MouseMailbox m;
    return m;
}

// Diagnostic: raw |yaw| magnitude, pre-scale, exposed for tuning the way the
// DS5Dongle portal exposes its own gyro magnitude.
inline std::atomic<uint32_t> g_diag_last_dx{0};
inline std::atomic<uint32_t> g_diag_last_dy{0};

// Called once per mapped DS5 input report. `descriptor` is the device
// descriptor (for vendor/product section matching); `d`/`len` is the report.
// Never modifies the report.
// `deviceKey` identifies the physical controller for motion-state purposes --
// pass the CtmUsbipDevice instance. It is used only as a map key and never
// dereferenced.
//
// ⭐ `linkedConfig` is what makes a gyro setting per-controller. Without it this
// always read the shared [ds5] section, so gyro_to_mouse_gate in a linked
// config would have been ignored while everything reported success -- the same
// failure the audio path had before the link was threaded through.
inline void on_ds5_input(const void *deviceKey,
                         const std::vector<unsigned char> &descriptor,
                         const std::string &linkedConfig,
                         const uint8_t *d, size_t len)
{
    // ⛔ The CAPABILITY question. This reads motion out of the DUALSENSE input
    // report at DualSense offsets -- the DS4 has a gyro too, but at different
    // positions, so a kind check would have silently read the wrong bytes.
    //
    // ⓘ device_section_for now answers for controllers this path cannot handle,
    // which is why it is no longer the right question to ask here.
    if (!device_has_ds5_motion(descriptor)) {
        return;
    }
    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) {
        return;
    }
    // Same resolution the audio path uses: shared section unless linked.
    const std::string resolved = device_settings_section(kind, linkedConfig);
    const char *section = resolved.c_str();
    MouseDelta delta;
    GyroMouse &g = gyro_for(deviceKey);
    // Cheap: a struct copy per report, and it keeps the calibration lookup off
    // the report path where it would need a mutex 250 times a second.
    g.set_calibration(ctm_gyro_calib::scale_for(deviceKey));
    if (g.on_report(d, len, section, &delta)) {
        shared_mailbox().push(delta);
        g_diag_last_dx.store(static_cast<uint32_t>(delta.dx < 0 ? -delta.dx : delta.dx));
        g_diag_last_dy.store(static_cast<uint32_t>(delta.dy < 0 ? -delta.dy : delta.dy));
    }
}

} // namespace ctm_gyro_mouse
