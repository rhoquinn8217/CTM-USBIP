// The trigger as a mouse click, with the cursor held still for the whole click.
//
// ⭐⭐ THE GESTURE (rhoquinn8217, 2026-09-10), once the adaptive trigger effect
// gave the click point somewhere a finger could find it:
//
//   R2 at rest                    the gyro moves the cursor
//   starting to pull              the cursor FREEZES
//   past the click point          the button goes DOWN
//   let back up, not to rest      the button goes UP, the cursor stays frozen
//   fully released                the cursor comes back
//   held past the window          the cursor comes back with the button DOWN
//
// ⭐ ONE RULE MAKES ALL THREE OUTCOMES. The cursor is frozen for exactly as long
// as the finger is committed to clicking, and only a FULL release hands it back:
//
//   1. a single click   -- release straight away, and the cursor returns.
//   2. a double click   -- lift and press again without going home, and the
//                          cursor stays still across BOTH presses.
//   3. a drag           -- keep holding, and past the window the cursor returns
//                          while the button stays down until R2 goes home.
//
// ⛔ WHY THE FREEZE COVERS THE WHOLE GESTURE rather than each press. A double
// click needs both presses to land on the same pixel. Thawing between them lets
// the gyro move the cursor in the gap, which is precisely what stops a double
// click working on a pointer you aim with your hands.
//
// ⭐ THE WINDOW IS ONE NUMBER, NOT TWO. "Long enough that you meant to hold it"
// and "too long to still be a double click" are the same judgement, so the drag
// delay IS the double-click window. Two settings could disagree; this cannot.
//
// ⓘ Reads the report and never modifies it. If a game should not also see the
// trigger, that is `mouse_exclusive.inl`'s job, not this file's.
//
// ⚠️ A trigger rebound to a mouse button in the same config would click twice,
// once here and once through the rebinder. A preset that turns this on should
// leave the trigger unbound.

#pragma once

namespace trigger_click {

// L2 and R2 analog positions in the DualSense input report: 0 at rest, 255 at
// the stop. The same bytes the gyro's L2/R2 gate reads.
constexpr size_t kL2Position = 5;
constexpr size_t kR2Position = 6;
constexpr int    kFullPull   = 255;

// USB mouse button bits, the same two the touchpad drag uses.
constexpr uint8_t kButtonLeft  = 0x01;
constexpr uint8_t kButtonRight = 0x02;

struct Side {
    const char *name;      // "r2" or "l2", which is also the config key stem
    size_t position;       // where its pull sits in the report
    uint8_t button;        // what it clicks
};

struct State {
    bool engaged  = false;   // off its rest stop: the cursor should be frozen
    bool down     = false;   // the button is held
    bool dragging = false;   // held long enough that the cursor came back
    long long downAtMs = 0;
};

struct Pad {
    State r2;
    State l2;
};

inline std::mutex g_mutex;
inline std::map<const void *, Pad> g_pads;

inline long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Percent of the pull to the raw byte the report carries.
inline int raw_from_percent(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return (percent * kFullPull) / 100;
}

// One trigger's whole state machine. Adds its button bit to `buttons` and says
// whether it wants the cursor frozen.
inline void step_side(const std::string &section, const Side &side, State &st,
                      const uint8_t *data, long long nowMs, int engageRaw,
                      int holdMs, uint8_t *buttons, bool *freeze)
{
    const std::string stem = std::string("trigger_") + side.name;
    if (!device_config_bool(section.c_str(), (stem + "_click").c_str(), false)) {
        st = State();
        return;
    }

    const int clickRaw = raw_from_percent(
        device_config_int(section.c_str(), (stem + "_click_at").c_str(), 90));
    const int pos  = data[side.position];
    const bool past = pos >= clickRaw && clickRaw > 0;

    st.engaged = pos >= engageRaw;

    if (!st.engaged) {
        // Home. Everything lets go, including a drag: this is the ONLY thing
        // that ends one, which is what makes the gesture predictable.
        st.down = false;
        st.dragging = false;
        return;
    }

    if (past && !st.down) {
        st.down = true;
        st.dragging = false;
        st.downAtMs = nowMs;
    } else if (!past && st.down && !st.dragging) {
        // Lifted back over the point without going home. The button releases,
        // the cursor does NOT: the finger is still on the trigger, and the next
        // press is very likely the second half of a double click.
        st.down = false;
    } else if (st.down && !st.dragging && holdMs > 0 && nowMs - st.downAtMs >= holdMs) {
        // ⭐ Held past the window, so this was never a click. Hand the cursor
        // back and keep the button down: that is a drag.
        st.dragging = true;
    }

    if (st.down) *buttons = static_cast<uint8_t>(*buttons | side.button);
    if (!st.dragging) *freeze = true;
}

inline void on_ds5_input(const void *deviceKey,
                         const std::vector<unsigned char> &descriptor,
                         const std::string &linkedConfig,
                         const uint8_t *data, size_t len)
{
    if (data == nullptr || len <= kR2Position) return;

    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) return;                    // not a DualSense
    const std::string section = device_settings_section(kind, linkedConfig);

    const bool anyOn =
        device_config_bool(section.c_str(), "trigger_r2_click", false) ||
        device_config_bool(section.c_str(), "trigger_l2_click", false);

    // ⓘ The common case costs two config lookups and touches nothing else, so
    // an install that never turns this on behaves exactly as it did before.
    if (!anyOn) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pads.erase(deviceKey) != 0) {
            ctm_mouse_device::set_trigger_buttons(0);
            ctm_gyro_mouse::set_gyro_hold(deviceKey, false);
        }
        return;
    }

    // ⚠️ ONE number for both triggers. "Starting to pull" is a property of the
    // hand, not of which trigger it is, and two of them could disagree.
    const int engageRaw = raw_from_percent(
        device_config_int(section.c_str(), "trigger_engage_at", 5));
    const int holdMs = device_config_int(section.c_str(), "trigger_click_hold_ms", 200);
    const long long nowMs = now_ms();

    static const Side kR2{ "r2", kR2Position, kButtonLeft };
    static const Side kL2{ "l2", kL2Position, kButtonRight };

    uint8_t buttons = 0;
    bool freeze = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Pad &pad = g_pads[deviceKey];
        step_side(section, kR2, pad.r2, data, nowMs, engageRaw, holdMs, &buttons, &freeze);
        step_side(section, kL2, pad.l2, data, nowMs, engageRaw, holdMs, &buttons, &freeze);
    }

    ctm_mouse_device::set_trigger_buttons(buttons);
    ctm_gyro_mouse::set_gyro_hold(deviceKey, freeze);
    if (buttons != 0) ctm_gyro_mouse_ensure_mouse_started();
}

// ⛔ A pad that unbridges mid-click must not leave a button held: nothing else
// can release it, and no controller is left to try.
inline void forget(const void *deviceKey)
{
    bool had = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        had = g_pads.erase(deviceKey) != 0;
    }
    if (had) ctm_mouse_device::set_trigger_buttons(0);
    ctm_gyro_mouse::set_gyro_hold(deviceKey, false);
}

}  // namespace trigger_click

// Defined out here for the forward declarations in main.cpp, the same shape the
// touchpad hook uses: device.inl calls both on the input path, and this file is
// included long after it.
void trigger_click_apply(const void *deviceKey,
                         const std::vector<unsigned char> &descriptor,
                         const std::string &linkedConfig,
                         const uint8_t *data, size_t len)
{
    trigger_click::on_ds5_input(deviceKey, descriptor, linkedConfig, data, len);
}

void trigger_click_forget(const void *deviceKey)
{
    trigger_click::forget(deviceKey);
}
