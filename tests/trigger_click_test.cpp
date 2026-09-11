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

const char *device_section_for(const std::vector<unsigned char> &) { return "ds5"; }

std::string device_settings_section(const char *kind, const std::string &linked)
{
    return linked.empty() ? std::string(kind) : linked + "/" + kind;
}

// What the module drives, recorded rather than performed.
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
inline void set_trigger_buttons(uint8_t mask) { g_buttons = mask; }
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
Out pull(State &st, int r2, long long nowMs, int holdMs = 200, int clickAt = 90)
{
    static const Side side{ "right", kR2Position, kRightStatusByte, 7 };
    const std::vector<uint8_t> d = report_with(0, r2);
    Out out{ false, false };
    out.down = step_side(side, st, d.data(), d.size(), nowMs, raw_from_percent(5),
                         raw_from_percent(clickAt), holdMs, true, false, false, &out.freeze);
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
                                    raw_from_percent(90), 200, false, false, false, &freeze);
        CTM_CHECK(!down);
        CTM_CHECK(!freeze);       // fully pulled, and still inert
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
        const Out out = pull(st, 0, 20);
        CTM_CHECK(!out.down);
        CTM_CHECK(!out.freeze);                    // home, so the cursor is back
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
        // Only going home hands the cursor back.
        out = pull(st, 0, 80);
        CTM_CHECK(!out.down);
        CTM_CHECK(!out.freeze);
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
            step_side(side, st, d.data(), d.size(), 0, engage, raw_from_percent(80), 600, true, false, false, &freeze);
            CTM_CHECK(!freeze);
        }
        // Past the engage point it engages, and a drag follows a held press.
        freeze = false;
        {
            const std::vector<uint8_t> d = report_with(0, 240);
            step_side(side, st, d.data(), d.size(), 0, engage, raw_from_percent(80), 600, true, false, false, &freeze);
            step_side(side, st, d.data(), d.size(), 700, engage, raw_from_percent(80), 600, true, false, false, &freeze);
        }
        CTM_CHECK(st.dragging);
        // ⭐ Now let it come to rest HIGH, at the old threshold. It must still
        // read as released, or the drag survives into the next pull.
        freeze = false;
        {
            const std::vector<uint8_t> d = report_with(0, 12);
            step_side(side, st, d.data(), d.size(), 800, engage, raw_from_percent(80), 600, true, false, false, &freeze);
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
            step_side(side, st, up.data(), up.size(), 0, engage, raw_from_percent(80), 600, true, false, false, &freeze);
            CTM_CHECK(st.engaged);
            const std::vector<uint8_t> between = report_with(0, 25);
            freeze = false;
            step_side(side, st, between.data(), between.size(), 10, engage, raw_from_percent(80), 600, true, false, false, &freeze);
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
                              600, true, true, false, &freeze);
        CTM_CHECK(!down);        // travel would have fired; the effect did not
        CTM_CHECK(freeze);       // and the cursor is held either way

        // Shallower than the travel threshold, but the effect says crossed.
        const std::vector<uint8_t> shallowButIn = report_with_status(120, 1);
        freeze = false;
        down = step_side(side, st, shallowButIn.data(), shallowButIn.size(),
                         10, raw_from_percent(15), raw_from_percent(80),
                         600, true, true, false, &freeze);
        CTM_CHECK(down);         // ⭐ the controller's word wins outright

        // ⛔ Status 2 is the bottom of the travel and still counts as crossed:
        // anything but 0 means the finger is in or past the effect.
        st = State();
        freeze = false;
        const std::vector<uint8_t> bottomed = report_with_status(255, 2);
        down = step_side(side, st, bottomed.data(), bottomed.size(),
                         20, raw_from_percent(15), raw_from_percent(80),
                         600, true, true, false, &freeze);
        CTM_CHECK(down);

        // ⓘ And with useEffect off, the very same report goes back to travel.
        st = State();
        freeze = false;
        down = step_side(side, st, shallowButIn.data(), shallowButIn.size(),
                         30, raw_from_percent(15), raw_from_percent(80),
                         600, true, false, false, &freeze);
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
                                    600, true, true, false, &freeze);
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
                             600, true, true, true, &freeze));
        CTM_CHECK(freeze);      // held either way
        CTM_CHECK(step_side(side, br, past.data(), past.size(), 10,
                            raw_from_percent(15), raw_from_percent(80),
                            600, true, true, true, &freeze));

        // A shape that only resists: entering it IS the moment, because there
        // is nothing to come through.
        State wall;
        freeze = false;
        CTM_CHECK(step_side(side, wall, inside.data(), inside.size(), 0,
                            raw_from_percent(15), raw_from_percent(80),
                            600, true, true, false, &freeze));
    }

    section("trigger click: a mouse binding, end to end");
    reset_all();
    g_bools["ds5.right_trigger_freezes_cursor"] = true;
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
    g_bools["ds5.right_trigger_freezes_cursor"] = true;
    g_strings["ds5.rebind_7"] = "Enter";
    {
        const std::vector<unsigned char> descriptor(12, 0);
        int pad = 0;
        on_ds5_input(&pad, descriptor, "", report_with(0, 240).data(), 16);
        CTM_CHECK_EQ((int)g_buttons, 0);           // no mouse button held
        CTM_CHECK(g_keys.find(&pad) != g_keys.end());
        CTM_CHECK(g_keys[&pad].size() == 1);
        if (g_keys[&pad].size() == 1) CTM_CHECK_EQ((int)g_keys[&pad][0], 0x28);
        // Releasing home lets the key go.
        on_ds5_input(&pad, descriptor, "", report_with(0, 0).data(), 16);
        CTM_CHECK(g_keys.find(&pad) == g_keys.end());
        CTM_CHECK(!held_for(&pad));
    }

    section("trigger click: both triggers, bound differently");
    reset_all();
    g_bools["ds5.right_trigger_freezes_cursor"] = true;
    g_strings["ds5.rebind_7"] = "MouseLeft";
    g_bools["ds5.left_trigger_freezes_cursor"] = true;
    g_strings["ds5.rebind_6"] = "MouseRight";
    {
        const std::vector<unsigned char> descriptor(12, 0);
        int pad = 0;
        on_ds5_input(&pad, descriptor, "", report_with(240, 240).data(), 16);
        CTM_CHECK_EQ((int)g_buttons, 0x03);
    }

    section("trigger click: two pads do not freeze each other");
    reset_all();
    g_bools["ds5.right_trigger_freezes_cursor"] = true;
    g_strings["ds5.rebind_7"] = "MouseLeft";
    {
        const std::vector<unsigned char> descriptor(12, 0);
        int padA = 0, padB = 0;
        on_ds5_input(&padA, descriptor, "", report_with(0, 240).data(), 16);
        CTM_CHECK(held_for(&padA));
        on_ds5_input(&padB, descriptor, "", report_with(0, 0).data(), 16);
        CTM_CHECK(!held_for(&padB));
        CTM_CHECK(held_for(&padA));               // ⭐ A is untouched by B
        // ⛔ And a pad that goes away leaves nothing held behind it.
        forget(&padA);
        CTM_CHECK(!held_for(&padA));
        CTM_CHECK_EQ((int)g_buttons, 0);
    }

    reset_all();
    return 0;
}
