// Touchpad-to-mouse: the DS5 touchpad drives the synthetic mouse.
//
//   one finger   moves the cursor, laptop-trackpad style (relative -- lifting
//                and repositioning does not jump)
//   two fingers  scroll, emitted as wheel ticks
//   a quick tap  clicks: one finger = left, two fingers = right, the Apple
//                trackpad convention; double-click is just tapping twice
//
// ⭐ READ-ONLY, like the gyro hook. The controller report is never modified, so
// games keep seeing the touchpad exactly as before and this cannot regress
// anything. Everything feeds the SAME synthetic mouse the gyro drives -- one
// emit point, however many sources.
//
// ⭐ ABSENT = OFF, per the project's config rule. With no touchpad_* keys set,
// every path below returns without touching any shared state.
//
// ⚠️ Touch point layout, measured 2026-08-29 during the chord work (offsets are
// ours -- report id at index 0):
//     [33..36] point 1, [37..40] point 2
//     byte 0: bit 0x80 CLEAR = finger down; low 7 bits = touch id, which
//             increments on every new touch and is how a lift-and-retouch is
//             told apart from continuous contact
//     x = b1 | ((b2 & 0x0f) << 8)        0..1919 left to right
//     y = ((b2 & 0xf0) >> 4) | (b3 << 4) 0..1079 top to bottom
//
// ⓘ Relies on its includer (main.cpp) for: device_config_*, device_section_for,
// device_settings_section, ctm_rebind_config_mode_effective, the gyro mailbox,
// ctm_mouse_device, and ctm_gyro_mouse_ensure_mouse_started -- the same pattern
// as gyro_mouse.inl and rebind.inl.

#pragma once

namespace ctm_touch_mouse {

// Tap limits. A tap is a touch that ends quickly and barely moved -- both
// bounds exist to keep an ordinary grip from clicking things.
constexpr long long kTapMaxMs = 250;
constexpr int kTapSlopUnits = 15;

// Pad units of two-finger travel per wheel tick, at scroll speed 100.
constexpr int kScrollUnitsPerTick = 60;

struct TouchPoint {
    bool down = false;
    int id = -1;
    int x = 0;
    int y = 0;
};

inline TouchPoint read_point(const uint8_t *data, size_t base)
{
    TouchPoint p;
    p.down = (data[base] & 0x80) == 0;
    p.id = data[base] & 0x7f;
    p.x = data[base + 1] | ((data[base + 2] & 0x0f) << 8);
    p.y = ((data[base + 2] & 0xf0) >> 4) | (data[base + 3] << 4);
    return p;
}

// Per-controller state, keyed by device pointer like the gyro registry --
// unique, always present, and unlike a serial never empty or shared.
struct TouchState {
    // Cursor tracking (one finger).
    bool cursorTracking = false;
    int cursorId = -1;
    int lastX = 0;
    int lastY = 0;
    float carryX = 0.0f;
    float carryY = 0.0f;

    // Scroll tracking (two fingers), on the average of both y positions.
    bool scrollTracking = false;
    float lastAvgY = 0.0f;
    float scrollCarry = 0.0f;

    // Tap session: from first finger down to all fingers up. Movement is
    // judged from the AVERAGE of the active fingers, re-anchored whenever the
    // finger count changes -- otherwise the second finger of a two-finger tap
    // would read as a huge jump and no two-finger tap could ever land.
    // ⭐ DRAG (rhoquinn8217, 2026-08-31). Click the pad in to grab, move with
    // the finger still down, LIFT THE FINGER to drop -- the physical click can
    // be released immediately, which is what makes a long drag comfortable.
    //
    // ⓘ This is the "drag lock" a trackpad offers, with a clearer trigger. The
    // usual two-touch convention is tap-then-touch-and-move, which is a timing
    // guess; a physical click is not. Three-finger drag is not available to us
    // at all -- the pad reports two touch points.
    bool dragging = false;

    bool sessionActive = false;
    long long sessionStart = 0;
    int sessionMaxFingers = 0;
    int sessionFingers = 0;
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    bool sessionMoved = false;
};

inline std::mutex g_touchMutex;
inline std::map<const void *, TouchState> g_touch;

inline void forget(const void *deviceKey)
{
    std::lock_guard<std::mutex> lock(g_touchMutex);
    auto it = g_touch.find(deviceKey);
    // ⛔ A controller that unbridges mid-drag must not leave the mouse button
    // held down on the desktop with nothing able to release it.
    if (it != g_touch.end() && it->second.dragging) ctm_mouse_device::set_drag(0x00);
    g_touch.erase(deviceKey);
}

// The core, with the clock passed in so tests can drive time directly.
inline void step(const void *deviceKey, const std::string &section,
                 const uint8_t *data, size_t len, long long nowMs)
{
    if (data == nullptr || len <= 40) return;

    // ⭐ THE SAME GATE VOCABULARY AS GYRO AND THE STICK, parsed by the same
    // function (rhoquinn8217, 2026-08-31). ⚠️ DEFAULT IS ALWAYS, not off:
    // gyro's gate defaults to off because naming a gate is what turns gyro on,
    // but the touchpad features have their own switches -- so an absent gate
    // here means "no extra condition", never "disabled".
    const std::string gateRaw = device_config_str(section.c_str(), "touchpad_to_mouse_gate");
    const ctm_gyro_mouse::Gate gate =
        gateRaw.empty() ? ctm_gyro_mouse::Gate::Always : ctm_gyro_mouse::parse_gate(gateRaw);

    const bool cursorOn = device_config_bool(section.c_str(), "touchpad_to_mouse", false);
    const bool scrollOn = device_config_bool(section.c_str(), "touchpad_scroll", false);
    const bool tapsOn = device_config_bool(section.c_str(), "touchpad_tap_click", false);

    std::lock_guard<std::mutex> lock(g_touchMutex);
    TouchState &st = g_touch[deviceKey];

    // ⭐ Everything off: keep no state, so turning a feature on later starts
    // clean rather than against a stale anchor.
    if (!cursorOn && !scrollOn && !tapsOn &&
        !device_config_bool(section.c_str(), "touchpad_click_drag", false)) {
        if (st.dragging) ctm_mouse_device::set_drag(0x00);
        st = TouchState();
        return;
    }

    // ⛔ NOT WHILE THE PAD IS DRIVING THE SETTINGS PAGE -- the same standdown
    // as the gyro, for the same reason: a cursor that moves while its buttons
    // are gated is a broken mouse, not a suspended one.
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


    // ⛔ A SHUT GATE DROPS THE STATE, so re-opening it starts from a clean
    // anchor rather than measuring movement against where a finger was before
    // the gate closed -- which would arrive as one jump.
    if (!ctm_gyro_mouse::gate_open(gate, data, len)) {
        if (st.dragging) ctm_mouse_device::set_drag(0x00);
        st = TouchState();
        return;
    }

    // ---- Drag ---------------------------------------------------------------
    // Read before anything else so a drag survives whatever the cursor and tap
    // paths decide to do with the same touch.
    if (device_config_bool(section.c_str(), "touchpad_click_drag", false)) {
        const bool padPressed = len > 10 && (data[10] & 0x02) != 0;
        const TouchPoint d1 = read_point(data, 33);
        const TouchPoint d2 = read_point(data, 37);
        const bool anyFinger = d1.down || d2.down;

        if (!st.dragging) {
            // Grab: the pad clicked in WITH a finger on it. A click with no
            // finger is an ordinary click and is left alone.
            if (padPressed && anyFinger) {
                st.dragging = true;
                ctm_mouse_device::set_drag(0x01);
                ctm_gyro_mouse_ensure_mouse_started();
            }
        } else if (!anyFinger) {
            // Drop: every finger has left the pad. ⓘ NOT when the click is
            // released -- holding a button down for the length of a drag is
            // the thing this exists to avoid.
            st.dragging = false;
            ctm_mouse_device::set_drag(0x00);
        }
    } else if (st.dragging) {
        // Turned off mid-drag: never leave the button held.
        st.dragging = false;
        ctm_mouse_device::set_drag(0x00);
    }

    const TouchPoint p1 = read_point(data, 33);
    const TouchPoint p2 = read_point(data, 37);
    const int fingers = (p1.down ? 1 : 0) + (p2.down ? 1 : 0);
    const TouchPoint &only = p1.down ? p1 : p2;   // meaningful when fingers == 1

    // ---- Tap session --------------------------------------------------------
    {
        float ax = 0.0f;
        float ay = 0.0f;
        if (fingers > 0) {
            ax = (p1.down ? static_cast<float>(p1.x) : 0.0f) +
                 (p2.down ? static_cast<float>(p2.x) : 0.0f);
            ay = (p1.down ? static_cast<float>(p1.y) : 0.0f) +
                 (p2.down ? static_cast<float>(p2.y) : 0.0f);
            ax /= static_cast<float>(fingers);
            ay /= static_cast<float>(fingers);
        }
        if (fingers > 0 && !st.sessionActive) {
            st.sessionActive = true;
            st.sessionStart = nowMs;
            st.sessionMaxFingers = fingers;
            st.sessionFingers = fingers;
            st.anchorX = ax;
            st.anchorY = ay;
            st.sessionMoved = false;
        } else if (fingers > 0) {
            if (fingers > st.sessionMaxFingers) st.sessionMaxFingers = fingers;
            if (fingers != st.sessionFingers) {
                // Finger count changed: the average jumps by construction, so
                // re-anchor instead of reading the jump as movement.
                st.sessionFingers = fingers;
                st.anchorX = ax;
                st.anchorY = ay;
            } else {
                const float mx = ax - st.anchorX;
                const float my = ay - st.anchorY;
                const float slop = static_cast<float>(kTapSlopUnits);
                if (mx > slop || mx < -slop || my > slop || my < -slop) {
                    st.sessionMoved = true;   // sticky: a scroll is not a tap
                }
            }
        } else if (st.sessionActive) {
            const long long heldMs = nowMs - st.sessionStart;
            if (tapsOn && !st.sessionMoved && heldMs <= kTapMaxMs) {
                // 1 finger = left (0x01), 2 fingers = right (0x02) -- and a
                // double-click is simply two taps, no special case needed.
                ctm_mouse_device::add_click(
                    st.sessionMaxFingers >= 2 ? 0x02 : 0x01);
                ctm_gyro_mouse_ensure_mouse_started();
            }
            st.sessionActive = false;
            st.sessionMaxFingers = 0;
            st.sessionFingers = 0;
            st.sessionMoved = false;
        }
    }

    // ---- One finger: cursor -------------------------------------------------
    if (cursorOn && fingers == 1) {
        // ⭐ Re-anchor rather than jump: on a new touch (or a lift-and-retouch,
        // which the id change reveals), the first report only sets the anchor.
        if (!st.cursorTracking || st.cursorId != only.id) {
            st.cursorTracking = true;
            st.cursorId = only.id;
            st.lastX = only.x;
            st.lastY = only.y;
        } else if (tapsOn && st.sessionActive && !st.sessionMoved &&
                   (nowMs - st.sessionStart) <= kTapMaxMs) {
            // ⭐ THE TAP GUARD (hardware finding, 2026-08-31): tapping nudged
            // the cursor a few pixels. While a touch could still become a tap
            // -- inside the slop, inside the tap window -- the cursor holds
            // still and the ANCHOR FOLLOWS THE FINGER, so when the touch stops
            // being tap-shaped, movement flows from right here with no
            // replayed jump. Only active when taps are on: a pure cursor
            // config keeps zero-latency movement.
            st.lastX = only.x;
            st.lastY = only.y;
            st.carryX = 0.0f;
            st.carryY = 0.0f;
        } else {
            const int speed = device_config_int(section.c_str(), "touchpad_mouse_speed", 100);
            const float scale = static_cast<float>(speed <= 0 ? 100 : speed) / 100.0f;
            // ⭐ Carry the sub-pixel remainder, or slow precise movement rounds
            // to zero forever -- the same lesson the gyro path learned.
            const float fx = st.carryX + (only.x - st.lastX) * scale;
            const float fy = st.carryY + (only.y - st.lastY) * scale;
            const int32_t px = static_cast<int32_t>(fx);
            const int32_t py = static_cast<int32_t>(fy);
            st.carryX = fx - static_cast<float>(px);
            st.carryY = fy - static_cast<float>(py);
            st.lastX = only.x;
            st.lastY = only.y;
            if (px != 0 || py != 0) {
                ctm_gyro_mouse::shared_mailbox().push(
                    ctm_gyro_mouse::MouseDelta{px, py});
                ctm_gyro_mouse_ensure_mouse_started();
            }
        }
    } else {
        st.cursorTracking = false;
        st.cursorId = -1;
        st.carryX = 0.0f;
        st.carryY = 0.0f;
    }

    // ---- Two fingers: scroll ------------------------------------------------
    if (scrollOn && fingers == 2) {
        const float avgY = (static_cast<float>(p1.y) + static_cast<float>(p2.y)) / 2.0f;
        if (!st.scrollTracking) {
            st.scrollTracking = true;
            st.lastAvgY = avgY;
            st.scrollCarry = 0.0f;
        } else {
            const int speed = device_config_int(section.c_str(), "touchpad_scroll_speed", 100);
            const float scale = static_cast<float>(speed <= 0 ? 100 : speed) / 100.0f;
            st.scrollCarry += (avgY - st.lastAvgY) * scale;
            st.lastAvgY = avgY;
            int ticks = static_cast<int>(st.scrollCarry / kScrollUnitsPerTick);
            if (ticks != 0) {
                st.scrollCarry -= static_cast<float>(ticks * kScrollUnitsPerTick);
                // Classic (default): fingers moving down scroll the page down,
                // which is wheel-down, negative. Natural inverts -- content
                // follows the fingers, the phone convention.
                const bool natural = device_config_bool(
                    section.c_str(), "touchpad_scroll_natural", false);
                ctm_mouse_device::add_wheel(natural ? ticks : -ticks);
                ctm_gyro_mouse_ensure_mouse_started();
            }
        }
    } else {
        st.scrollTracking = false;
        st.scrollCarry = 0.0f;
    }
}

inline long long touch_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void on_ds5_input(const void *deviceKey,
                         const std::vector<unsigned char> &descriptor,
                         const std::string &linkedConfig,
                         const uint8_t *data, size_t len)
{
    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) return;
    step(deviceKey, device_settings_section(kind, linkedConfig), data, len,
         touch_now_ms());
}

} // namespace ctm_touch_mouse

// Defined out here for the forward declarations in main.cpp -- device.inl
// calls the hook on the input path, and gyro_mouse.inl chains the forget, and
// both are included long before this file.
void ctm_touch_mouse_apply(const void *deviceKey,
                           const std::vector<unsigned char> &descriptor,
                           const std::string &linkedConfig,
                           const uint8_t *data, size_t len)
{
    ctm_touch_mouse::on_ds5_input(deviceKey, descriptor, linkedConfig, data, len);
}

void ctm_touch_mouse_forget(const void *deviceKey)
{
    ctm_touch_mouse::forget(deviceKey);
}
