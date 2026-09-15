// Tests for the trigger gesture: freeze, press, double press, drag.
//
// ⭐ WHAT THESE PROTECT. The whole design rests on one rule -- the cursor is
// frozen for as long as the finger is committed to pressing, and only a FULL
// release hands it back. Every outcome falls out of that, so the tests are
// written as transitions rather than as states: what a press does, what letting
// back up without going home does, what holding does.
//
// ⛔ THE ONE THAT MATTERS MOST is the double press. A press, a partial lift and
// a second press must keep the cursor frozen throughout. Thawing in the gap is
// exactly what stops a double click landing on a gyro pointer, and it would not
// show up as a failure anywhere else.
//
// WHAT THESE CANNOT DO. They cannot say the click point is comfortable, that
// the window feels right, or that a real trigger reaches the values used here.
// Those are hardware questions. These protect the state machine.
//
// ⓘ Time and thresholds are passed in rather than read, so nothing here sleeps
// and the window can be crossed exactly rather than approximately.

#include "harness.h"

#include <chrono>
#include <ostream>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ⓘ Every pad's layout: where its triggers are, which trigger_click.inl asks.
#include "input/button_layout.inl"

using namespace ctmtest;

namespace {

// Config stubs standing in for device_config_*. The real ones read a file.
std::map<std::string, std::string> g_strings;
std::map<std::string, int> g_ints;
std::map<std::string, bool> g_bools;

std::string device_config_str(const char *section, const char *key)
{
    auto it = g_strings.find(std::string(section) + "." + key);
    return it == g_strings.end() ? std::string() : it->second;
}

int device_config_int(const char *section, const char *key, int fallback)
{
    auto it = g_ints.find(std::string(section) + "." + key);
    return it == g_ints.end() ? fallback : it->second;
}

// The probe reads this; the tests leave it false so the probe stays silent.
bool device_config_bool(const char *section, const char *key, bool fallback)
{
    auto it = g_bools.find(std::string(section) + "." + key);
    return it == g_bools.end() ? fallback : it->second;
}

// ⓘ The real resolver lives in ds5_output_overrides.inl. A blank descriptor is a
// DualSense here, as the stub this replaced answered for every descriptor, so
// the end-to-end checks below still drive a DualSense; a DS4 and an Xbox pad
// are named by their real ids.
struct InputPad {
    const char *kind = nullptr;
    const ctm_rebind::Layout *layout = nullptr;
};
static InputPad device_input_pad_for(const std::vector<unsigned char> &d)
{
    InputPad pad;
    uint16_t vendor = 0;
    uint16_t product = 0;
    if (d.size() >= 12) {
        vendor = static_cast<uint16_t>(d[8] | (d[9] << 8));
        product = static_cast<uint16_t>(d[10] | (d[11] << 8));
    }
    const char *kind = "ds5";
    if (vendor == 0x054c && (product == 0x09cc || product == 0x05c4)) kind = "ds4";
    else if (vendor == 0x045e && product == 0x0b12) kind = "xbox";
    pad.layout = ctm_rebind::layout_for(kind);
    pad.kind = kind;
    return pad;
}

std::string device_settings_section(const char *kind, const std::string &linked)
{
    return linked.empty() ? std::string(kind) : "cfg:" + linked;
}

// What the module drives, recorded rather than performed.
// ⓘ Each pad's trigger buttons, kept apart as the mouse keeps them; g_buttons is
// every pad's OR'd, which is what the host would see held.
std::map<const void *, uint8_t> g_buttonsFor;
uint8_t g_buttons = 0;
std::map<const void *, bool> g_held;
std::map<const void *, std::vector<uint8_t>> g_keys;
int g_mouseStarts = 0;
int g_keyboardStarts = 0;

}  // namespace

// Standing in for the rebinder's name lookup, with just enough vocabulary to
// prove the binding is resolved rather than assumed.
namespace ctm_rebind {
enum MouseAction { kMouseNone = 0, kMouseLeft, kMouseRight, kMouseMiddle,
                   kMouseWheelUp, kMouseWheelDown };

inline MouseAction mouse_action_for(const std::string &code)
{
    if (code == "MouseLeft")      return kMouseLeft;
    if (code == "MouseRight")     return kMouseRight;
    if (code == "MouseMiddle")    return kMouseMiddle;
    if (code == "MouseWheelUp")   return kMouseWheelUp;
    return kMouseNone;
}

struct KeyName { const char *code; uint8_t usage; uint8_t modifier; };

inline const KeyName *key_for(const std::string &code)
{
    static const KeyName kEnter{ "Enter", 0x28, 0x00 };
    static const KeyName kShiftA{ "ShiftA", 0x04, 0x02 };
    if (code == "Enter") return &kEnter;
    if (code == "ShiftA") return &kShiftA;
    return nullptr;
}
}  // namespace ctm_rebind

namespace ctm_mouse_device {
inline void set_trigger_buttons_for(const void *key, uint8_t mask)
{
    if (mask != 0) g_buttonsFor[key] = mask;
    else g_buttonsFor.erase(key);
    g_buttons = 0;
    for (const auto &entry : g_buttonsFor) g_buttons = static_cast<uint8_t>(g_buttons | entry.second);
}
}

namespace ctm_gyro_mouse {
inline void set_gyro_hold(const void *key, bool held)
{
    if (held) g_held[key] = true;
    else g_held.erase(key);
}
}

namespace ctm_keyboard_device {
inline void set_trigger_keys_for(const void *key, uint8_t /*mods*/,
                                 const uint8_t *keys, size_t count)
{
    std::vector<uint8_t> v;
    for (size_t i = 0; i < count && keys != nullptr; ++i) v.push_back(keys[i]);
    if (v.empty()) g_keys.erase(key);
    else g_keys[key] = v;
}
}

// The state log writes here in the product. The tests only need it to compile:
// what it says is judged by eye in device.log, not asserted.
namespace device_log {
inline std::ostream &input_s()
{
    static std::ostringstream sink;
    sink.str(std::string());
    return sink;
}
}

inline void ctm_gyro_mouse_ensure_mouse_started() { ++g_mouseStarts; }
inline void ctm_rebind_ensure_keyboard_started() { ++g_keyboardStarts; }

// ⓘ The REAL shape parser rather than a stub. trigger_click asks it whether a
// trigger has an effect at all, and a stub would let the two drift: a shape
// added there and forgotten here would test as "no effect" and pass.
#include "../src/input/trigger_effect.inl"
#include "../src/input/trigger_click.inl"

using namespace trigger_click;

namespace {

void reset_all()
{
    g_strings.clear();
    g_ints.clear();
    g_bools.clear();
    g_buttonsFor.clear();
    g_buttons = 0;
    g_held.clear();
    g_keys.clear();
    g_mouseStarts = 0;
    g_keyboardStarts = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pads.clear();
}

std::vector<uint8_t> report_with(int l2, int r2)
{
    std::vector<uint8_t> d(16, 0);
    d[kL2Position] = static_cast<uint8_t>(l2);
    d[kR2Position] = static_cast<uint8_t>(r2);
    return d;
}

bool held_for(const void *key) { return g_held.find(key) != g_held.end(); }

// A full-length report, so the status bytes exist. `status` is the value the
// controller would put in the HIGH nybble: 0 short of the effect, 1 inside it.
std::vector<uint8_t> report_with_status(int r2, int status)
{
    std::vector<uint8_t> d(64, 0);
    d[kR2Position] = static_cast<uint8_t>(r2);
    // ⓘ The low nybble is a counter in the real report. Filling it with
    // something non-zero proves we mask it off rather than read the byte whole.
    d[kRightStatusByte] = static_cast<uint8_t>((status << 4) | 0x07);
    return d;
}

struct Out { bool down; bool freeze; };

// One step of the R2 side with time and thresholds supplied.
Out pull(State &st, int r2, long long nowMs, int holdMs = 200, int clickAt = 90,
         int doubleMs = 200, Steady steady = Steady::Immediate)
{
    static const Side side{ "right", kR2Position, kRightStatusByte, 7 };
    const std::vector<uint8_t> d = report_with(0, r2);
    Out out{ false, false };
    out.down = step_side(side, st, d.data(), d.size(), nowMs, raw_from_percent(5),
                         raw_from_percent(clickAt), holdMs, doubleMs, true, steady,
                         false, false, &out.freeze);
    return out;
}

}  // namespace

int run_trigger_click_tests()
{
    section("trigger click: what a binding resolves to");
    CTM_CHECK(!bound_for("").set());
    CTM_CHECK_EQ((int)bound_for("MouseLeft").mouseBit, 0x01);
    CTM_CHECK_EQ((int)bound_for("MouseRight").mouseBit, 0x02);
    CTM_CHECK_EQ((int)bound_for("MouseMiddle").mouseBit, 0x04);
    CTM_CHECK_EQ((int)bound_for("Enter").keyUsage, 0x28);
    CTM_CHECK_EQ((int)bound_for("ShiftA").keyModifier, 0x02);
    // ⛔ A wheel tick is a pulse and this gesture is built on holding, so it is
    // left unbound rather than made to half work.
    CTM_CHECK(!bound_for("MouseWheelUp").set());
    // A name nobody recognises binds nothing, rather than guessing.
    CTM_CHECK(!bound_for("Bananas").set());

    section("trigger click: an unbound trigger does nothing");
    reset_all();
    {
        State st;
        static const Side side{ "right", kR2Position, kRightStatusByte, 7 };
        const std::vector<uint8_t> d = report_with(0, 255);
        bool freeze = false;
        const bool down = step_side(side, st, d.data(), d.size(), 0, raw_from_percent(5),
                                    raw_from_percent(90), 200, 200, false, Steady::Immediate, false, false, &freeze);
        CTM_CHECK(!down);
        CTM_CHECK(!freeze);       // fully pulled, and still inert
    }

    section("trigger click: where the steady engages, per mode");
    reset_all();
    {
        // Off and immediate both take the shared number; only before_press
        // derives its own, and it follows the press point rather than being a
        // second depth that can drift away from it.
        g_strings["ds5.right_trigger_steady_cursor_pull"] = "immediate";
        CTM_CHECK_EQ(engage_percent("ds5", "right", 6, 80), 6);
        g_strings["ds5.right_trigger_steady_cursor_pull"] = "before_press";
        CTM_CHECK_EQ(engage_percent("ds5", "right", 6, 80), 70);
        CTM_CHECK_EQ(engage_percent("ds5", "right", 6, 50), 40);
        // ⓘ Floored, so a shallow press cannot put the lock at or below rest.
        CTM_CHECK_EQ(engage_percent("ds5", "right", 6, 10), 4);
        // ⛔ And the OTHER side is unaffected: these are per-side on purpose.
        CTM_CHECK_EQ(engage_percent("ds5", "left", 6, 80), 6);
        g_strings["ds5.right_trigger_steady_cursor_pull"] = "off";
        CTM_CHECK(!steadies_cursor("ds5", "right"));
        g_strings["ds5.right_trigger_steady_cursor_pull"] = "immediate";
        CTM_CHECK(steadies_cursor("ds5", "right"));
    }
    reset_all();

    section("trigger click: the gesture takes a trigger exactly when the rebinder gives it up");
    {
        // ⛔⛔ The rebinder gives L2/R2 up when trigger_effect::steady_value_on says
        // the switch is on; the gesture takes them when steadies_cursor does. Any
        // value they disagree on is a trigger fired twice or not at all -- and
        // "off", which the page saves as its default, was not at all: the
        // rebinder's test was "the key has a value" (2026-09-15).
        const char *const values[] = { "", "off", "false", "0", "none", "imediate",
                                       "immediate", "true", "1", "before_press",
                                       "after_press" };
        for (const char *value : values) {
            g_strings["ds5.right_trigger_steady_cursor_pull"] = value;
            g_strings["ds4.left_trigger_steady_cursor_pull"] = value;
            CTM_CHECK_EQ(steadies_cursor("ds5", "right"), trigger_effect::steady_value_on(value));
            CTM_CHECK_EQ(steadies_cursor("ds4", "left"), trigger_effect::steady_value_on(value));
        }
        g_strings["ds5.right_trigger_steady_cursor_pull"] = "off";
        CTM_CHECK(!trigger_effect::steady_value_on(
            device_config_str("ds5", trigger_effect::steady_key("right").c_str())));
    }
    reset_all();

    section("trigger click: after_press leaves the pull alone and holds on the click");
    {
        // ⭐ For a trigger that is ALSO the gyro's gate. The cursor must stay
        // live all the way down so you can aim with it, and only stop once the
        // click has landed -- long enough that a second click lands in the same
        // place. rhoquinn8217, 2026-09-11: *"on the L2 click, it should stop
        // gyro for a duration that you would give to double click."*
        State st;
        // Engaged but short of the press: NOT held, unlike immediate.
        Out out = pull(st, 60, 0, 200, 90, 200, Steady::AfterPress);
        CTM_CHECK(!out.down);
        CTM_CHECK(!out.freeze);
        // The press lands, and the hold starts with it.
        out = pull(st, 240, 10, 200, 90, 200, Steady::AfterPress);
        CTM_CHECK(out.down);
        CTM_CHECK(out.freeze);
        // ⛔ And it runs from the PRESS, not the release -- a gate trigger may
        // never be released, so a window keyed on release would never start.
        out = pull(st, 240, 150, 200, 90, 200, Steady::AfterPress);
        CTM_CHECK(out.freeze);
        out = pull(st, 240, 260, 200, 90, 200, Steady::AfterPress);
        CTM_CHECK(!out.freeze);              // window spent, cursor live again
    }

    section("trigger click: off never touches the cursor");
    {
        State st;
        Out out = pull(st, 60, 0, 200, 90, 200, Steady::Off);
        CTM_CHECK(!out.freeze);
        out = pull(st, 240, 10, 200, 90, 200, Steady::Off);
        CTM_CHECK(out.down);                 // it still presses
        CTM_CHECK(!out.freeze);              // and still leaves the cursor alone
        out = pull(st, 0, 20, 200, 90, 200, Steady::Off);
        CTM_CHECK(!out.freeze);
    }

    section("trigger click: the freeze arrives before the press");
    {
        State st;
        // A tenth of the way in: past the engage point, nowhere near the click.
        Out out = pull(st, 26, 0);
        CTM_CHECK(out.freeze);                     // cursor already still
        CTM_CHECK(!out.down);                      // and nothing pressed yet
        // ⭐ That ordering is the whole design: the jolt of the press lands on a
        // cursor that stopped moving before the finger got there.
        out = pull(st, 240, 10);
        CTM_CHECK(out.freeze);
        CTM_CHECK(out.down);
    }

    section("trigger click: a single press, released home");
    {
        State st;
        pull(st, 240, 0);
        Out out = pull(st, 0, 20);
        CTM_CHECK(!out.down);
        // ⭐ STILL HELD, because a second click may be coming. This asserted the
        // cursor came back the instant the trigger went home until 2026-09-11;
        // see the double-click test below for why that was wrong.
        CTM_CHECK(out.freeze);
        // And back once the window has run out.
        out = pull(st, 0, 20 + 200);
        CTM_CHECK(!out.freeze);
    }

    section("trigger click: a double press with the lift going ALL THE WAY HOME");
    {
        // ⛔ THE CASE THAT SHIPPED BROKEN. The test below this one lifts back
        // over the click point but not home, which holds the cursor because the
        // trigger never disengages. On a WALL the finger goes home every time:
        // the effect pushes it out as it releases, and rhoquinn8217 measured two
        // double clicks seconds apart where the only difference was a lift
        // stopping at raw 92 rather than carrying on to 8. One landed, one went
        // wide, and nobody can feel that difference.
        State st;
        Out out = pull(st, 240, 0);
        CTM_CHECK(out.down);
        CTM_CHECK(out.freeze);
        // All the way home, well below the release threshold.
        out = pull(st, 0, 30);
        CTM_CHECK(!out.down);
        CTM_CHECK(out.freeze);                     // ⭐ the cursor did NOT move
        // The second press of the pair, from rest.
        out = pull(st, 240, 60);
        CTM_CHECK(out.down);
        CTM_CHECK(out.freeze);
        // Home again, and this time let the window run out.
        out = pull(st, 0, 90);
        CTM_CHECK(out.freeze);
        out = pull(st, 0, 90 + 200);
        CTM_CHECK(!out.freeze);
    }

    section("trigger click: a drag that ends hands the cursor straight back");
    {
        // ⚠️ A drag is excluded from the hold on purpose: letting go of one
        // means you are done, and a cursor that stayed put would feel stuck.
        State st;
        pull(st, 240, 0);
        Out out = pull(st, 240, 250);              // past the 200ms window
        CTM_CHECK(!out.freeze);                    // dragging: cursor already back
        out = pull(st, 0, 300);
        CTM_CHECK(!out.down);
        CTM_CHECK(!out.freeze);                    // and it stays back
    }

    section("trigger click: a double press keeps the cursor still throughout");
    {
        State st;
        Out out = pull(st, 240, 0);
        CTM_CHECK(out.down);
        // Lifted back over the click point but NOT home.
        out = pull(st, 60, 30);
        CTM_CHECK(!out.down);                      // the binding released
        CTM_CHECK(out.freeze);                     // ⭐ and the cursor did not move
        // The second press of the pair.
        out = pull(st, 240, 60);
        CTM_CHECK(out.down);
        CTM_CHECK(out.freeze);
        // Going home releases the binding, and the cursor comes back once the
        // double-click window has run out rather than immediately.
        out = pull(st, 0, 80);
        CTM_CHECK(!out.down);
        CTM_CHECK(out.freeze);
        out = pull(st, 0, 80 + 200);
        CTM_CHECK(!out.freeze);
    }

    section("trigger click: shoving a wall to the stop drags at once");
    {
        // A wall fires on ENTRY, so meeting it is light. Carrying on to the
        // stop against its resistance is a decision, and it should not have to
        // wait out the hold window to be read as one.
        static const Side side{ "right", kR2Position, kRightStatusByte, 7 };
        State st;
        bool freeze = false;
        // Inside the effect but nowhere near the stop: a press, cursor held.
        const std::vector<uint8_t> met = report_with_status(120, 1);
        CTM_CHECK(step_side(side, st, met.data(), met.size(), 0,
                            raw_from_percent(5), raw_from_percent(80),
                            600, 200, true, Steady::Immediate, true, false, &freeze));
        CTM_CHECK(freeze);
        // Bottomed out, one millisecond later. Far short of the 600ms window.
        freeze = false;
        const std::vector<uint8_t> floored = report_with_status(255, 1);
        CTM_CHECK(step_side(side, st, floored.data(), floored.size(), 1,
                            raw_from_percent(5), raw_from_percent(80),
                            600, 200, true, Steady::Immediate, true, false, &freeze));
        CTM_CHECK(!freeze);                // dragging already
    }

    section("trigger click: a BREAKING shape is not dragged by bottoming out");
    {
        // ⛔ The break throws the trigger to the stop by itself, so treating
        // that as a shove would turn every click into a drag.
        static const Side side{ "right", kR2Position, kRightStatusByte, 7 };
        State st;
        bool freeze = false;
        const std::vector<uint8_t> past = report_with_status(255, 2);
        CTM_CHECK(step_side(side, st, past.data(), past.size(), 0,
                            raw_from_percent(5), raw_from_percent(80),
                            600, 200, true, Steady::Immediate, true, true, &freeze));
        CTM_CHECK(freeze);                 // pressed, still a click
        freeze = false;
        CTM_CHECK(step_side(side, st, past.data(), past.size(), 1,
                            raw_from_percent(5), raw_from_percent(80),
                            600, 200, true, Steady::Immediate, true, true, &freeze));
        CTM_CHECK(freeze);                 // and it stays a click
    }

    section("trigger click: holding past the window becomes a drag");
    {
        State st;
        Out out = pull(st, 240, 1000, 200);
        CTM_CHECK(out.freeze);
        // One millisecond short of the window is still a press.
        out = pull(st, 240, 1199, 200);
        CTM_CHECK(out.freeze);
        CTM_CHECK(out.down);
        // ⭐ Exactly ON the window, the cursor comes back and the binding stays.
        out = pull(st, 240, 1200, 200);
        CTM_CHECK(!out.freeze);
        CTM_CHECK(out.down);
        // ⛔ Lifting over the click point mid-drag must NOT drop what is being
        // dragged. Only going home does.
        out = pull(st, 60, 1300, 200);
        CTM_CHECK(out.down);
        CTM_CHECK(!out.freeze);
        out = pull(st, 0, 1400, 200);
        CTM_CHECK(!out.down);
        CTM_CHECK(!out.freeze);
    }

    section("trigger click: a window of zero turns dragging off");
    {
        State st;
        pull(st, 240, 0, 0);
        const Out out = pull(st, 240, 100000, 0);  // held a very long time
        CTM_CHECK(out.down);
        CTM_CHECK(out.freeze);                     // never hands the cursor back
    }

    section("trigger click: the click point is honoured");
    {
        State st;
        // At 50%, a pull to 40% presses nothing but still freezes.
        Out out = pull(st, 102, 0, 200, 50);
        CTM_CHECK(out.freeze);
        CTM_CHECK(!out.down);
        out = pull(st, 130, 10, 200, 50);
        CTM_CHECK(out.down);
    }

    section("trigger click: a resting trigger cannot look like a finger");
    // ⛔ THE INTERMITTENCY THIS FIXES. One threshold sat a single unit above
    // where the trigger comes to rest, so a pull that ended high was never seen
    // as released -- and the NEXT pull began with a drag still set and the
    // cursor already handed back. Engaging takes the full threshold; letting go
    // takes a clearly lower one, and the gap is the margin.
    {
        State st;
        const int engage = raw_from_percent(12);       // 30
        const int release = (engage * 2) / 3;          // 20
        static const Side side{ "right", kR2Position, kRightStatusByte, 7 };
        bool freeze = false;

        // A trigger wandering below the engage point never engages at all.
        for (int rest : { 0, 11, 20, 29 }) {
            freeze = false;
            const std::vector<uint8_t> d = report_with(0, rest);
            step_side(side, st, d.data(), d.size(), 0, engage, raw_from_percent(80), 600, 200, true, Steady::Immediate, false, false, &freeze);
            CTM_CHECK(!freeze);
        }
        // Past the engage point it engages, and a drag follows a held press.
        freeze = false;
        {
            const std::vector<uint8_t> d = report_with(0, 240);
            step_side(side, st, d.data(), d.size(), 0, engage, raw_from_percent(80), 600, 200, true, Steady::Immediate, false, false, &freeze);
            step_side(side, st, d.data(), d.size(), 700, engage, raw_from_percent(80), 600, 200, true, Steady::Immediate, false, false, &freeze);
        }
        CTM_CHECK(st.dragging);
        // ⭐ Now let it come to rest HIGH, at the old threshold. It must still
        // read as released, or the drag survives into the next pull.
        freeze = false;
        {
            const std::vector<uint8_t> d = report_with(0, 12);
            step_side(side, st, d.data(), d.size(), 800, engage, raw_from_percent(80), 600, 200, true, Steady::Immediate, false, false, &freeze);
        }
        CTM_CHECK(!st.engaged);
        CTM_CHECK(!st.dragging);
        CTM_CHECK(!st.down);
        CTM_CHECK(!freeze);
        // ⓘ And between the two thresholds the state HOLDS rather than flapping,
        // which is the point of having two.
        st = State();
        freeze = false;
        {
            const std::vector<uint8_t> up = report_with(0, 40);
            step_side(side, st, up.data(), up.size(), 0, engage, raw_from_percent(80), 600, 200, true, Steady::Immediate, false, false, &freeze);
            CTM_CHECK(st.engaged);
            const std::vector<uint8_t> between = report_with(0, 25);
            freeze = false;
            step_side(side, st, between.data(), between.size(), 10, engage, raw_from_percent(80), 600, 200, true, Steady::Immediate, false, false, &freeze);
            CTM_CHECK(st.engaged);                     // above the release point
            CTM_CHECK(freeze);
        }
    }

    section("trigger click: firing on the effect instead of on travel");
    // ⭐ The controller reports when the finger has entered the effect, and that
    // is a better answer than a number. With a break at 80% the hardware called
    // the crossing at raw 215 while a travel threshold of 80% sits at 204, so
    // the press fired eleven units before anything gave way (2026-09-10).
    {
        State st;
        static const Side side{ "right", kR2Position, kRightStatusByte, 7 };
        bool freeze = false;

        // Deep enough to pass any travel threshold, but the effect says no.
        const std::vector<uint8_t> deepButShort = report_with_status(250, 0);
        bool down = step_side(side, st, deepButShort.data(), deepButShort.size(),
                              0, raw_from_percent(15), raw_from_percent(80),
                              600, 200, true, Steady::Immediate, true, false, &freeze);
        CTM_CHECK(!down);        // travel would have fired; the effect did not
        CTM_CHECK(freeze);       // and the cursor is held either way

        // Shallower than the travel threshold, but the effect says crossed.
        const std::vector<uint8_t> shallowButIn = report_with_status(120, 1);
        freeze = false;
        down = step_side(side, st, shallowButIn.data(), shallowButIn.size(),
                         10, raw_from_percent(15), raw_from_percent(80),
                         600, 200, true, Steady::Immediate, true, false, &freeze);
        CTM_CHECK(down);         // ⭐ the controller's word wins outright

        // ⛔ Status 2 is the bottom of the travel and still counts as crossed:
        // anything but 0 means the finger is in or past the effect.
        st = State();
        freeze = false;
        const std::vector<uint8_t> bottomed = report_with_status(255, 2);
        down = step_side(side, st, bottomed.data(), bottomed.size(),
                         20, raw_from_percent(15), raw_from_percent(80),
                         600, 200, true, Steady::Immediate, true, false, &freeze);
        CTM_CHECK(down);

        // ⓘ And with useEffect off, the very same report goes back to travel.
        st = State();
        freeze = false;
        down = step_side(side, st, shallowButIn.data(), shallowButIn.size(),
                         30, raw_from_percent(15), raw_from_percent(80),
                         600, 200, true, Steady::Immediate, false, false, &freeze);
        CTM_CHECK(!down);        // 120 is short of 204
    }
    // ⛔ A report too short to hold the status byte must not read past its end.
    {
        State st;
        static const Side side{ "right", kR2Position, kRightStatusByte, 7 };
        const std::vector<uint8_t> shortReport = report_with(0, 255);
        bool freeze = false;
        const bool down = step_side(side, st, shortReport.data(), shortReport.size(),
                                    0, raw_from_percent(15), raw_from_percent(80),
                                    600, 200, true, Steady::Immediate, true, false, &freeze);
        CTM_CHECK(!down);
    }

    section("trigger click: a break fires on 2, a wall on 1");
    // ⛔ Firing on anything non-zero read ENTERING the resistance as breaking
    // through it. Five pulls that never broke still clicked (2026-09-10), and
    // the status never reached 2 on any of them.
    {
        static const Side side{ "right", kR2Position, kRightStatusByte, 7 };
        const std::vector<uint8_t> inside = report_with_status(230, 1);
        const std::vector<uint8_t> past   = report_with_status(250, 2);

        // A shape that gives way: inside is not enough, past is.
        State br;
        bool freeze = false;
        CTM_CHECK(!step_side(side, br, inside.data(), inside.size(), 0,
                             raw_from_percent(15), raw_from_percent(80),
                             600, 200, true, Steady::Immediate, true, true, &freeze));
        CTM_CHECK(freeze);      // held either way
        CTM_CHECK(step_side(side, br, past.data(), past.size(), 10,
                            raw_from_percent(15), raw_from_percent(80),
                            600, 200, true, Steady::Immediate, true, true, &freeze));

        // A shape that only resists: entering it IS the moment, because there
        // is nothing to come through.
        State wall;
        freeze = false;
        CTM_CHECK(step_side(side, wall, inside.data(), inside.size(), 0,
                            raw_from_percent(15), raw_from_percent(80),
                            600, 200, true, Steady::Immediate, true, false, &freeze));
    }

    section("trigger click: a mouse binding, end to end");
    reset_all();
    g_strings["ds5.right_trigger_steady_cursor_pull"] = "immediate";
    g_strings["ds5.rebind_7"] = "MouseLeft";
    {
        const std::vector<unsigned char> descriptor(12, 0);
        int pad = 0;
        on_ds5_input(&pad, descriptor, "", report_with(0, 240).data(), 16);
        CTM_CHECK_EQ((int)g_buttons, 0x01);
        CTM_CHECK(held_for(&pad));
        CTM_CHECK(g_keys.empty());                 // the keyboard is untouched
    }

    section("trigger click: a keyboard binding, end to end");
    reset_all();
    g_strings["ds5.right_trigger_steady_cursor_pull"] = "immediate";
    g_strings["ds5.rebind_7"] = "Enter";
    {
        const std::vector<unsigned char> descriptor(12, 0);
        int pad = 0;
        on_ds5_input(&pad, descriptor, "", report_with(0, 240).data(), 16);
        CTM_CHECK_EQ((int)g_buttons, 0);           // no mouse button held
        CTM_CHECK(g_keys.find(&pad) != g_keys.end());
        CTM_CHECK(g_keys[&pad].size() == 1);
        if (g_keys[&pad].size() == 1) CTM_CHECK_EQ((int)g_keys[&pad][0], 0x28);
        // Releasing home lets the key go. ⭐ The CURSOR is a separate question:
        // it stays held for the rest of the double-click window, so held_for is
        // still true here and this used to assert the opposite. Real time runs
        // in this path, so the window cannot be waited out from a test -- the
        // timing itself is covered against a supplied clock further up.
        on_ds5_input(&pad, descriptor, "", report_with(0, 0).data(), 16);
        CTM_CHECK(g_keys.find(&pad) == g_keys.end());
        CTM_CHECK(held_for(&pad));
    }

    section("trigger click: both triggers, bound differently");
    reset_all();
    g_strings["ds5.right_trigger_steady_cursor_pull"] = "immediate";
    g_strings["ds5.rebind_7"] = "MouseLeft";
    g_strings["ds5.left_trigger_steady_cursor_pull"] = "immediate";
    g_strings["ds5.rebind_6"] = "MouseRight";
    {
        const std::vector<unsigned char> descriptor(12, 0);
        int pad = 0;
        on_ds5_input(&pad, descriptor, "", report_with(240, 240).data(), 16);
        CTM_CHECK_EQ((int)g_buttons, 0x03);
    }

    section("trigger click: two pads do not freeze each other");
    reset_all();
    g_strings["ds5.right_trigger_steady_cursor_pull"] = "immediate";
    g_strings["ds5.rebind_7"] = "MouseLeft";
    {
        const std::vector<unsigned char> descriptor(12, 0);
        int padA = 0, padB = 0;
        on_ds5_input(&padA, descriptor, "", report_with(0, 240).data(), 16);
        CTM_CHECK(held_for(&padA));
        on_ds5_input(&padB, descriptor, "", report_with(0, 0).data(), 16);
        CTM_CHECK(!held_for(&padB));
        CTM_CHECK(held_for(&padA));               // ⭐ A is untouched by B
        // ⛔ Nor is A's press. B at rest publishes "nothing held" on every report,
        // and until 2026-09-15 that went into the one level A was holding.
        CTM_CHECK_EQ((int)g_buttons, 0x01);
        // ⛔ And a pad that goes away leaves nothing held behind it.
        forget(&padA);
        CTM_CHECK(!held_for(&padA));
        CTM_CHECK_EQ((int)g_buttons, 0);
    }

    // ---- DualShock 4: triggers at [8] and [9], and no effect status -------------

    const std::vector<unsigned char> ds4Descriptor = { 0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
                                                      0x4c, 0x05, 0xc4, 0x05 };
    auto ds4_report = [](int l2, int r2) {
        std::vector<uint8_t> d(64, 0);
        d[0] = 0x01;
        d[1] = d[2] = d[3] = d[4] = 0x80;
        d[5] = 0x08;                                  // hat centred, no face buttons
        d[8] = static_cast<uint8_t>(l2);
        d[9] = static_cast<uint8_t>(r2);
        return d;
    };

    section("trigger click: a DS4's R2 at [9] clicks");
    reset_all();
    g_strings["ds4.right_trigger_steady_cursor_pull"] = "immediate";
    g_strings["ds4.rebind_7"] = "MouseLeft";
    {
        int pad = 0;
        on_ds5_input(&pad, ds4Descriptor, "", ds4_report(0, 240).data(), 64);
        CTM_CHECK_EQ((int)g_buttons, 0x01);
        CTM_CHECK(held_for(&pad));
        forget(&pad);
    }

    section("trigger click: a DS4 face button or Options is not a trigger pull");
    reset_all();
    g_strings["ds4.right_trigger_steady_cursor_pull"] = "immediate";
    g_strings["ds4.rebind_7"] = "MouseLeft";
    g_strings["ds4.left_trigger_steady_cursor_pull"] = "immediate";
    g_strings["ds4.rebind_6"] = "MouseRight";
    {
        // ⛔⛔ THE FAULT, pinned. At DualSense offsets a DS4's cross ([5] 0x28)
        // read as L2 at 40 of 255, and Options ([6] 0x20) as R2 at 32 -- both past
        // the default 6%, so both triggers engaged and froze the cursor.
        int pad = 0;
        std::vector<uint8_t> d = ds4_report(0, 0);
        d[5] = 0x28;                                  // cross
        d[6] = 0x20;                                  // Options
        on_ds5_input(&pad, ds4Descriptor, "", d.data(), 64);
        CTM_CHECK_EQ((int)g_buttons, 0);
        CTM_CHECK(!held_for(&pad));
        forget(&pad);
    }

    section("trigger click: a DS4 has no effect status, so an effect clicks on travel");
    reset_all();
    g_strings["ds4.right_trigger_steady_cursor_pull"] = "immediate";
    g_strings["ds4.rebind_7"] = "MouseLeft";
    g_strings["ds4.right_trigger_effect"] = "click";
    {
        // ⓘ [42] on a DS4 is a touch coordinate. Read as a DualSense status byte,
        // a high nybble of 2 would say "past the break" at a light pull.
        int pad = 0;
        std::vector<uint8_t> light = ds4_report(0, 100);
        light[42] = 0x27;
        on_ds5_input(&pad, ds4Descriptor, "", light.data(), 64);
        CTM_CHECK_EQ((int)g_buttons, 0);              // below the 90% press point
        on_ds5_input(&pad, ds4Descriptor, "", ds4_report(0, 240).data(), 64);
        CTM_CHECK_EQ((int)g_buttons, 0x01);           // past it on travel
        forget(&pad);
    }

    section("trigger click: an Xbox pad is left alone");
    reset_all();
    g_strings["xbox.right_trigger_steady_cursor_pull"] = "immediate";
    g_strings["xbox.rebind_7"] = "MouseLeft";
    {
        // ⓘ Deliberate: its triggers are 16-bit with no digital bit to take over.
        const std::vector<unsigned char> xboxDescriptor = { 0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
                                                           0x5e, 0x04, 0x12, 0x0b };
        std::vector<uint8_t> d(48, 0);
        d[0] = 0x20;
        d[8] = 0xff;                                  // RT low byte, fully pulled
        d[9] = 0x03;
        int pad = 0;
        on_ds5_input(&pad, xboxDescriptor, "", d.data(), 48);
        CTM_CHECK_EQ((int)g_buttons, 0);
        CTM_CHECK(!held_for(&pad));
    }

    reset_all();
    return 0;
}
