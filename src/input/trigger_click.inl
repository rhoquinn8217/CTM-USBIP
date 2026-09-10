// The trigger as a bound button, with the cursor held still for the whole press.
//
// ⭐⭐ THE GESTURE (rhoquinn8217, 2026-09-10), once the adaptive trigger effect
// gave the click point somewhere a finger could find it:
//
//   R2 at rest                    the gyro moves the cursor
//   starting to pull              the cursor FREEZES
//   past the click point          the binding goes DOWN
//   let back up, not to rest      the binding goes UP, the cursor stays frozen
//   fully released                the cursor comes back
//   held past the window          the cursor comes back with the binding DOWN
//
// ⭐ ONE RULE MAKES ALL THREE OUTCOMES. The cursor is frozen for exactly as long
// as the finger is committed to pressing, and only a FULL release hands it back:
//
//   1. a single click   -- release straight away, and the cursor returns.
//   2. a double click   -- lift and press again without going home, and the
//                          cursor stays still across BOTH presses.
//   3. a drag           -- keep holding, and past the window the cursor returns
//                          while the binding stays down until the trigger goes
//                          home.
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
// ⭐⭐ AND IT IS A BINDING, NOT A SWITCH (rhoquinn8217, 2026-09-10): *"it
// shouldn't be a true/false setting. You should be able to remap this like the
// face buttons."* Quite right -- a trigger hardwired to the left mouse button
// is the only button on the pad that could not be pointed somewhere else. It
// takes the same names as `rebind_*` and resolves them the same way.
//
// ⓘ Reads the report and never modifies it. If a game should not also see the
// trigger, that is `mouse_exclusive.inl`'s job, not this file's.
//
// ⚠️ Binding the SAME trigger with `rebind_6`/`rebind_7` as well would fire
// twice, once here and once through the rebinder. A config that uses this
// should leave those blank.

#pragma once

namespace trigger_click {

// L2 and R2 analog positions in the DualSense input report: 0 at rest, 255 at
// the stop. The same bytes the gyro's L2/R2 gate reads.
constexpr size_t kL2Position = 5;
constexpr size_t kR2Position = 6;
constexpr int    kFullPull   = 255;

struct Side {
    const char *name;      // "r2" or "l2", which is also the config key stem
    size_t position;       // where its pull sits in the report
};

// What a trigger's press should hold down. Resolved per report, so a config
// change lands without a re-bridge like every other setting here.
struct Bound {
    uint8_t mouseBit    = 0;   // a mouse button bit, or 0
    uint8_t keyUsage    = 0;   // a keyboard usage, or 0
    uint8_t keyModifier = 0;
    bool set() const { return mouseBit != 0 || keyUsage != 0; }
};

inline Bound bound_for(const std::string &code)
{
    Bound out;
    if (code.empty()) return out;

    const ctm_rebind::MouseAction ma = ctm_rebind::mouse_action_for(code);
    if (ma == ctm_rebind::kMouseLeft)   { out.mouseBit = 0x01; return out; }
    if (ma == ctm_rebind::kMouseRight)  { out.mouseBit = 0x02; return out; }
    if (ma == ctm_rebind::kMouseMiddle) { out.mouseBit = 0x04; return out; }
    // ⛔ A wheel tick is a PULSE and this whole gesture is built on holding, so
    // there is nothing sensible to do with one. Left unbound rather than half
    // working: a scroll that fired once on press and never again would be a
    // stranger fault than a binding that plainly does nothing.
    if (ma != ctm_rebind::kMouseNone) return out;

    const ctm_rebind::KeyName *k = ctm_rebind::key_for(code);
    if (k != nullptr) {
        out.keyUsage    = k->usage;
        out.keyModifier = k->modifier;
    }
    return out;
}

struct State {
    bool engaged  = false;   // off its rest stop: the cursor should be frozen
    bool down     = false;   // the binding is held
    bool dragging = false;   // held long enough that the cursor came back
    long long downAtMs = 0;
};

struct Pad {
    State r2;
    State l2;
    bool heldKeys = false;   // so an idle pad never touches the keyboard
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

// One trigger's whole state machine. Returns true while its binding should be
// held, and raises *freeze when it wants the cursor still.
//
// ⓘ Takes its thresholds rather than reading them, so the rule can be tested
// without a config and time can be supplied rather than observed.
inline bool step_side(const Side &side, State &st, const uint8_t *data,
                      long long nowMs, int engageRaw, int clickRaw, int holdMs,
                      bool bound, bool *freeze)
{
    if (!bound) {
        st = State();
        return false;
    }

    const int pos = data[side.position];
    const bool past = clickRaw > 0 && pos >= clickRaw;

    st.engaged = pos >= engageRaw;

    if (!st.engaged) {
        // Home. Everything lets go, including a drag: this is the ONLY thing
        // that ends one, which is what makes the gesture predictable.
        st.down = false;
        st.dragging = false;
        return false;
    }

    if (past && !st.down) {
        st.down = true;
        st.dragging = false;
        st.downAtMs = nowMs;
    } else if (!past && st.down && !st.dragging) {
        // Lifted back over the point without going home. The binding releases,
        // the cursor does NOT: the finger is still on the trigger, and the next
        // press is very likely the second half of a double click.
        st.down = false;
    } else if (st.down && !st.dragging && holdMs > 0 && nowMs - st.downAtMs >= holdMs) {
        // ⭐ Held past the window, so this was never a click. Hand the cursor
        // back and keep the binding down: that is a drag.
        st.dragging = true;
    }

    if (!st.dragging) *freeze = true;
    return st.down;
}

inline std::string bind_key_for(const char *sideName)
{
    return std::string("trigger_") + sideName + "_click";
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

    static const Side kR2{ "r2", kR2Position };
    static const Side kL2{ "l2", kL2Position };

    const Bound boundR2 =
        bound_for(device_config_str(section.c_str(), bind_key_for(kR2.name).c_str()));
    const Bound boundL2 =
        bound_for(device_config_str(section.c_str(), bind_key_for(kL2.name).c_str()));

    // ⓘ The common case costs two config lookups and touches nothing else, so
    // an install that never binds a trigger behaves exactly as it did before.
    if (!boundR2.set() && !boundL2.set()) {
        bool had = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_pads.find(deviceKey);
            if (it != g_pads.end()) {
                had = it->second.heldKeys;
                g_pads.erase(it);
            } else {
                return;
            }
        }
        ctm_mouse_device::set_trigger_buttons(0);
        if (had) ctm_keyboard_device::set_trigger_keys_for(deviceKey, 0, nullptr, 0);
        ctm_gyro_mouse::set_gyro_hold(deviceKey, false);
        return;
    }

    // ⚠️ ONE number for both triggers. "Starting to pull" is a property of the
    // hand, not of which trigger it is, and two of them could disagree.
    const int engageRaw = raw_from_percent(
        device_config_int(section.c_str(), "trigger_engage_at", 5));
    const int holdMs = device_config_int(section.c_str(), "trigger_click_hold_ms", 200);
    const int clickR2 = raw_from_percent(
        device_config_int(section.c_str(), "trigger_r2_click_at", 90));
    const int clickL2 = raw_from_percent(
        device_config_int(section.c_str(), "trigger_l2_click_at", 90));
    const long long nowMs = now_ms();

    uint8_t buttons = 0;
    uint8_t keys[2] = { 0, 0 };
    uint8_t mods = 0;
    size_t keyCount = 0;
    bool freeze = false;
    bool wantsKeys = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Pad &pad = g_pads[deviceKey];

        const bool downR2 = step_side(kR2, pad.r2, data, nowMs, engageRaw,
                                      clickR2, holdMs, boundR2.set(), &freeze);
        const bool downL2 = step_side(kL2, pad.l2, data, nowMs, engageRaw,
                                      clickL2, holdMs, boundL2.set(), &freeze);

        const struct { bool down; const Bound *b; } held[2] = {
            { downR2, &boundR2 }, { downL2, &boundL2 }
        };
        for (const auto &h : held) {
            if (!h.down) continue;
            if (h.b->mouseBit != 0) {
                buttons = static_cast<uint8_t>(buttons | h.b->mouseBit);
            } else if (h.b->keyUsage != 0 && keyCount < 2) {
                keys[keyCount++] = h.b->keyUsage;
                mods = static_cast<uint8_t>(mods | h.b->keyModifier);
            }
        }
        // ⛔ Only touch the keyboard when this pad has something to say there,
        // or had something a moment ago. Writing an empty state 250 times a
        // second would take the keyboard's lock for nothing.
        wantsKeys = (keyCount > 0) || pad.heldKeys;
        pad.heldKeys = keyCount > 0;
    }

    ctm_mouse_device::set_trigger_buttons(buttons);
    if (wantsKeys) {
        ctm_keyboard_device::set_trigger_keys_for(deviceKey, mods, keys, keyCount);
        ctm_rebind_ensure_keyboard_started();
    }
    ctm_gyro_mouse::set_gyro_hold(deviceKey, freeze);
    if (buttons != 0) ctm_gyro_mouse_ensure_mouse_started();
}

// ⛔ A pad that unbridges mid-press must not leave anything held: nothing else
// can release it, and no controller is left to try.
inline void forget(const void *deviceKey)
{
    bool had = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_pads.find(deviceKey);
        if (it != g_pads.end()) {
            had = true;
            g_pads.erase(it);
        }
    }
    if (had) {
        ctm_mouse_device::set_trigger_buttons(0);
        ctm_keyboard_device::set_trigger_keys_for(deviceKey, 0, nullptr, 0);
    }
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
