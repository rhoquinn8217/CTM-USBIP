// Gyro-to-mouse for any pad whose layout names a motion sensor: DualSense,
// DualSense Edge and DualShock 4.
//
// ⭐ OFFSETS COME FROM THE PAD'S LAYOUT (input/button_layout.inl), not from the
// DualSense numbers below, which are kept as the record of where they came from.
// A DS4 carries the same sensor three bytes earlier -- gyro at [13] [15] [17],
// accelerometer at [19] [21] [23] -- and the touchpad and the buttons a gate
// reads sit elsewhere too.
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
// ⓘ Those are the DUALSENSE'S. Every pad's own now lives in its layout
// (input/button_layout.inl, MotionSpots and friends), and this file reads
// through that: a DS4's gyro sits at [13] [15] [17] and its first finger at
// [35], because [33] on a DS4 is the touch-packet count.
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
// ⓘ Where each pad keeps what the gates and the motion read. Depends on nothing,
// so including it here keeps this file compiling in the test binary too.
#include "input/button_layout.inl"
// ⓘ The trigger's hold on this pad's cursor, which gate_open reads. Depends on
// nothing, so the trigger's tests include the real one too.
#include "input/gyro_hold.inl"

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
    // T-235: the right stick pressed in. Nothing new was needed to read it --
    // kBtnR3 already has a spot in BOTH button tables, the DualSense's and the
    // generic one -- so this is the same shape as L1 and R1 below.
    R3,
    // ⭐ TRIGGER: the gyro moves the cursor EXCEPT while a trigger is being
    // worked, so a click lands on a cursor that is already still. Unlike every
    // gate above, this one is not a function of the report bytes -- the trigger
    // logic owns a small state machine (a press, a double click, a drag are all
    // different) and simply says whether it wants the cursor held.
    TriggerHold,
};

// ⓘ The trigger's hold on the cursor, set_gyro_hold() and gyro_hold(), is in
// input/gyro_hold.inl, included at the top of this file.

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
    // ⓘ An old spelling of "always"; the steady is no longer a gate.
    if (v == "trigger") return Gate::Always;
    if (v == "l2") return Gate::L2;
    if (v == "r2") return Gate::R2;
    if (v == "l1") return Gate::L1;
    if (v == "r1") return Gate::R1;
    if (v == "touchpad") return Gate::Touchpad;
    if (v == "!touchpad" || v == "not_touchpad") return Gate::NotTouchpad;
    if (v == "touchpad_click" || v == "click") return Gate::TouchpadClick;
    if (v == "trigger") return Gate::TriggerHold;
    if (v == "ps") return Gate::PS;
    if (v == "r3") return Gate::R3;
    // Unknown value is OFF, never an error -- a typo silently disables the
    // feature, it never breaks a session. Same rule as every config lookup.
    return Gate::Off;
}

// True when the gate condition says gyro should be producing movement right
// now. `d` is the mapped report (id at [0]), read at `lay`'s offsets.
// ⓘ The device key is optional because every gate but one is a pure function of
// the report bytes, and every existing caller passes only those. Gate::TriggerHold
// is the exception: whose trigger is being worked is a question about a pad.
//
// ⛔⛔ EVERY GATE READ DUALSENSE BYTES, FOR EVERY PAD. L2 was [5], the PS button
// [10], a finger [33]. On a DS4 those are the hat and face buttons, a timestamp
// that changes on most reports, and the touch-packet count -- so the gyro
// preset's recenter button, touchpad_click, would have fired over and over, and
// a touchpad gate would have read "finger down" forever. The stick and touchpad
// mouse share these gates, so they had the same fault.
//
// ⚠️ For a DualSense each case below is exactly the read it replaced, bounds
// checks included.
inline bool gate_open(Gate gate, const ctm_rebind::Layout &lay, const uint8_t *d, size_t len,
                      const void *deviceKey = nullptr)
{
    // ⭐⭐ THE TRIGGER'S STEADY SUPPRESSES THE GYRO WHATEVER THE GATE IS
    // (rhoquinn8217, 2026-09-11).
    //
    // ⛔ It used to be a gate VALUE, "trigger", which meant the two could not be
    // combined: choosing L2 as the gate silently gave up the steady, and
    // choosing the steady gave up the gate. ⚠️ And it read wrong -- every other
    // value names a button you HOLD to enable the gyro, while "trigger" meant
    // "always, minus the steady". rhoquinn8217: *"trigger doesn't gate the
    // gyro-to-mouse. Actually it should be 'always' because it is."*
    //
    // ➡️ So the steady is a suppression ON TOP of whichever gate was chosen, and
    // "trigger" is now just another spelling of "always".
    // ⓘ A null key means no controller is in play -- the recenter check calls it
    // that way -- and gyro_hold() answers false for one, so nothing changes there.
    if (gyro_hold(deviceKey)) return false;
    switch (gate) {
        case Gate::Off:
            return false;
        case Gate::Always:
            return true;
        case Gate::L2:
            // Analog, ~12% travel -- on the DualSense's 0..255 scale for every pad.
            // ⓘ The same depth an Xbox trigger presses a binding at, from one
            // constant, so "pulled" cannot mean two things (2026-09-15).
            return ctm_rebind::trigger_travel(lay, d, len, true) >= ctm_rebind::kTriggerPulledTravel;
        case Gate::R2:
            return ctm_rebind::trigger_travel(lay, d, len, false) >= ctm_rebind::kTriggerPulledTravel;
        case Gate::L1:
            return ctm_rebind::is_pressed(lay, d, len, ctm_rebind::kBtnL1);
        case Gate::R1:
            return ctm_rebind::is_pressed(lay, d, len, ctm_rebind::kBtnR1);
        case Gate::R3:
            return ctm_rebind::is_pressed(lay, d, len, ctm_rebind::kBtnR3);
        case Gate::Touchpad:
            return ctm_rebind::touch_finger_down(lay, d, len, 0);   // finger 1 down
        case Gate::NotTouchpad:
            // ⓘ "Move unless a finger is down" -- and a pad with no touchpad has
            // no finger to pause for, so it is open there. ⛔ touch_finger_up()
            // answers false for such a pad on purpose (a report has to STATE "no
            // finger"), which shut this gate forever on an Xbox pad once the stick
            // mouse reached one (found in review, 2026-09-15).
            if (!lay.touch.present) return true;
            return ctm_rebind::touch_finger_up(lay, d, len, 0);     // ratchet: touch pauses
        case Gate::TouchpadClick:
            return ctm_rebind::touch_pressed(lay, d, len);          // pad pressed in
        case Gate::PS:
            return ctm_rebind::is_pressed(lay, d, len, ctm_rebind::kBtnHome);
        case Gate::TriggerHold:
            // ⓘ Kept so configs written before 2026-09-11 still parse. The hold
            // is checked above for every gate now, so this IS Always.
            return true;
    }
    return false;
}

// ⓘ The DualSense's gates, for callers that only ever had a DualSense report.
inline bool gate_open(Gate gate, const uint8_t *d, size_t len, const void *deviceKey = nullptr)
{
    return gate_open(gate, ctm_rebind::kDs5Layout, d, len, deviceKey);
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
    // ⭐ WHICH PAD THIS IS. Every gate but one is a pure function of the report
    // bytes, so this class never needed to know. Gate::TriggerHold does: the
    // trigger state machine keeps its answer per pad, and two bridged
    // controllers must not freeze each other's cursor.
    // ⓘ Set once per report by gyro_for(), which is the only place a key and an
    // instance are both in hand.
    const void *key_ = nullptr;
    void set_key(const void *k) { key_ = k; }

    GyroMouse()
    {
        // Stillness auto-calibration: the filter watches for the controller
        // being held still (low variance, not low value) and learns the resting
        // bias on its own. This is what keeps the deadzone tiny, which is what
        // makes slow aiming survive. No "put it down for 2 seconds" prompt.
        motion_.SetCalibrationMode(GamepadMotionHelpers::CalibrationMode::Stillness);
    }

    // Feed one mapped report, read at `lay`'s offsets (the overload below takes
    // a DualSense's). Returns true and fills `out` when there is a
    // non-zero mouse movement to emit; returns false when the gate is closed,
    // the config is off, or the movement rounded to zero this tick.
    // The controller's own gyro calibration. Set once when the session comes up;
    // defaults to the old fixed divisor so an uncalibrated pad still works.
    void set_calibration(const ctm_gyro_calib::Scale &s) { cal_ = s; }

    // ⓘ A DualSense report, for callers that only ever had one.
    bool on_report(const uint8_t *d, size_t len, const char *section, MouseDelta *out)
    {
        return on_report(ctm_rebind::kDs5Layout, d, len, section, out);
    }

    bool on_report(const ctm_rebind::Layout &lay, const uint8_t *d, size_t len,
                   const char *section, MouseDelta *out)
    {
        if (d == nullptr || out == nullptr || !lay.motion.present ||
            len < ctm_rebind::motion_min_len(lay)) {
            return false;                       // need through the accel block
        }

        const Config cfg = load_config(section);

        // ⓘ Gate diagnostic. Off unless gyro_mouse_debug_gate is set, and rate
        // limited to twice a second -- this path runs 250x/sec. Prints what every
        // gate reads, AT THIS PAD'S OFFSETS, so a gate that never opens can be
        // diagnosed by measurement rather than by guessing at them.
        if (device_config_bool(section, "gyro_mouse_debug_gate", false)) {
            static auto lastPrint = std::chrono::steady_clock::now();
            const auto nowDbg = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowDbg - lastPrint).count() >= 500) {
                lastPrint = nowDbg;
                using namespace ctm_rebind;
                device_log::input_s() << "[gyro] pad=" << lay.name << " len=" << len
                          << " L2=" << trigger_travel(lay, d, len, true)
                          << " R2=" << trigger_travel(lay, d, len, false)
                          << " L1=" << (is_pressed(lay, d, len, kBtnL1) ? 1 : 0)
                          << " R1=" << (is_pressed(lay, d, len, kBtnR1) ? 1 : 0)
                          << " PS=" << (is_pressed(lay, d, len, kBtnHome) ? 1 : 0)
                          << " finger1=" << (touch_finger_down(lay, d, len, 0) ? 1 : 0)
                          << " click=" << (touch_pressed(lay, d, len) ? 1 : 0)
                          << " gateOpen=" << (gate_open(cfg.gate, lay, d, len, key_) ? 1 : 0)
                          << std::endl;
            }
        }

        // ⭐ Recenter runs BEFORE the gate check, and regardless of whether
        // gyro is producing movement -- it is a navigation aid, useful exactly
        // when the cursor is stranded and gyro may well be off.
        //
        // Edge-triggered: fires once on press, not repeatedly while held.
        if (cfg.recenter != Gate::Off) {
            const bool down = gate_open(cfg.recenter, lay, d, len);
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

        // Raw signed 16-bit little-endian reads, at this pad's offsets.
        ctm_rebind::MotionSample m;
        if (!ctm_rebind::read_motion(lay, d, len, &m)) {
            return false;
        }

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
        const float gyroPitch = (m.gyroPitch - cal.biasPitch) * cal.pitch;
        const float gyroYaw   = (m.gyroYaw   - cal.biasYaw)   * cal.yaw;
        const float gyroRoll  = (m.gyroRoll  - cal.biasRoll)  * cal.roll;
        // accel: 8192 raw units per g, and not calibrated here -- the library
        // only uses it to work out which way is down. ⓘ The same units on a DS4:
        // the Linux driver's DS4_ACC_RES_PER_G is 8192 as well.
        const float accelX    = m.accelX / 8192.0f;
        const float accelY    = m.accelY / 8192.0f;
        const float accelZ    = m.accelZ / 8192.0f;

        // The library ALWAYS runs -- its calibration must keep observing even
        // when the gate is shut, or it never learns the bias. Axis order is the
        // library's Y-up convention: (pitch=X, yaw=Y, roll=Z) matches how it
        // derives player-space from a PlayStation pad.
        motion_.ProcessMotion(gyroPitch, gyroYaw, gyroRoll,
                              accelX, accelY, accelZ, dt);

        // Gate AFTER processing, so calibration is continuous but movement only
        // emits when the player is actually aiming.
        if (!gate_open(cfg.gate, lay, d, len, key_)) {
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

// Declared here so forget_device below can clear the trigger hold too.
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
    // A pad that goes away must not leave its cursor frozen for whatever lands
    // on the same pointer next. The trigger clears this too, so this is belt
    // and braces -- and a stale hold is invisible until someone wonders why
    // their gyro stopped working.
    set_gyro_hold(deviceKey, false);
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

// Called once per mapped input report, from any pad. `descriptor` is the device
// descriptor (for vendor/product section matching); `d`/`len` is the report.
// ⓘ The name is historical: a DualSense was the only pad it read.
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
    // ⓘ The CAPABILITY question is answered below, by the pad's layout. It was
    // once a kind check, which would have read a DS4's motion at DualSense
    // offsets; device_section_for() says yes to a DS4, so it is not the
    // question to ask here either.
    // ⛔ NOT WHILE THE PAD IS DRIVING THE SETTINGS PAGE.
    //
    // ⚠️ A pointer that moves while its buttons do nothing is a BROKEN mouse,
    // not a suspended one -- you would waggle the pad, watch the cursor drift,
    // press select, get nothing, and conclude the feature was broken. Partial is
    // worse than either extreme.
    //
    // ⓘ And "controller inputs locked to page" has to mean all of them. A cursor
    // crossing the screen while the footer says that is the same class of lie as
    // the footer showing green while nothing was gated.
    // ⛔ THE GATE NO LONGER SUSPENDS THE CURSOR (rhoquinn8217, 2026-09-02).
    //
    // ⚠️ Safe Edit Mode exists to stop a bridged pad MIRRORING INTO A GAME, and
    // a cursor cannot do that: pointer movement goes to whatever has focus,
    // which while the gate applies is our own settings window. Suspending it
    // protected nothing and made the page look broken -- the pad appeared dead
    // when it was simply forbidden from doing the one thing it could do safely.
    //
    // ⓘ Kept as a comment rather than deleted so the next person wondering why
    // the cursor works here finds the reasoning instead of the absence.

    // ⭐ ANY PAD WHOSE LAYOUT NAMES A MOTION SENSOR, read at that layout's
    // offsets. ⓘ This replaces a DualSense-only capability check, which was the
    // right answer while the offsets below were DualSense numbers and is the
    // wrong one now that each pad's layout carries its own.
    const InputPad pad = device_input_pad_for(descriptor);
    if (pad.layout == nullptr || !pad.layout->motion.present) {
        return;
    }
    // Same resolution the audio path uses: shared section unless linked.
    const std::string resolved = device_settings_section(pad.kind, linkedConfig);
    const char *section = resolved.c_str();
    MouseDelta delta;
    GyroMouse &g = gyro_for(deviceKey);
    g.set_key(deviceKey);
    // Cheap: a struct copy per report, and it keeps the calibration lookup off
    // the report path where it would need a mutex 250 times a second.
    g.set_calibration(ctm_gyro_calib::scale_for(deviceKey));
    if (g.on_report(*pad.layout, d, len, section, &delta)) {
        shared_mailbox().push(delta);
        g_diag_last_dx.store(static_cast<uint32_t>(delta.dx < 0 ? -delta.dx : delta.dx));
        g_diag_last_dy.store(static_cast<uint32_t>(delta.dy < 0 ? -delta.dy : delta.dy));
    }
}

} // namespace ctm_gyro_mouse
