// Tests for the trigger-click gesture: freeze, click, double click, drag.
//
// ⭐ WHAT THESE PROTECT. The whole design rests on one rule -- the cursor is
// frozen for as long as the finger is committed to clicking, and only a FULL
// release hands it back. Every outcome falls out of that, so the tests are
// written as transitions rather than as states: what a press does, what letting
// back up without going home does, what holding does.
//
// ⛔ THE ONE THAT MATTERS MOST is the double click. A press, a partial lift and
// a second press must keep the cursor frozen throughout. Thawing in the gap is
// exactly what stops a double click landing on a gyro pointer, and it would not
// show up as a failure anywhere else.
//
// WHAT THESE CANNOT DO. They cannot say the click point is comfortable, that
// the window feels right, or that a real trigger reaches the values used here.
// Those are hardware questions. These protect the state machine.
//
// ⓘ Time is passed in rather than read, so nothing here sleeps and the window
// can be crossed exactly rather than approximately.

#include "harness.h"

#include <chrono>
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

bool device_config_bool(const char *section, const char *key, bool fallback)
{
    auto it = g_bools.find(std::string(section) + "." + key);
    return it == g_bools.end() ? fallback : it->second;
}

int device_config_int(const char *section, const char *key, int fallback)
{
    auto it = g_ints.find(std::string(section) + "." + key);
    return it == g_ints.end() ? fallback : it->second;
}

const char *device_section_for(const std::vector<unsigned char> &) { return "ds5"; }

std::string device_settings_section(const char *kind, const std::string &linked)
{
    return linked.empty() ? std::string(kind) : linked + "/" + kind;
}

// What the module drives, recorded rather than performed.
uint8_t g_buttons = 0;
std::map<const void *, bool> g_held;
int g_mouseStarts = 0;

}  // namespace

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

inline void ctm_gyro_mouse_ensure_mouse_started() { ++g_mouseStarts; }

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
    g_mouseStarts = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pads.clear();
}

// A report long enough to carry both trigger positions.
std::vector<uint8_t> report_with(int l2, int r2)
{
    std::vector<uint8_t> d(16, 0);
    d[kL2Position] = static_cast<uint8_t>(l2);
    d[kR2Position] = static_cast<uint8_t>(r2);
    return d;
}

bool held_for(const void *key)
{
    return g_held.find(key) != g_held.end();
}

// One step of the R2 side, with time supplied. Returns the button mask and
// whether the cursor was asked to freeze.
struct Out { uint8_t buttons; bool freeze; };

Out pull(State &st, int r2, long long nowMs, int holdMs = 200, int clickAt = 90)
{
    g_ints["ds5.trigger_r2_click_at"] = clickAt;
    static const Side side{ "r2", kR2Position, kButtonLeft };
    const std::vector<uint8_t> d = report_with(0, r2);
    Out out{ 0, false };
    step_side("ds5", side, st, d.data(), nowMs,
              raw_from_percent(5), holdMs, &out.buttons, &out.freeze);
    return out;
}

}  // namespace

int run_trigger_click_tests()
{
    section("trigger click: an unconfigured trigger does nothing");
    reset_all();
    {
        State st;
        const Out out = pull(st, 255, 0);         // fully pulled, and still inert
        CTM_CHECK_EQ((int)out.buttons, 0);
        CTM_CHECK(!out.freeze);
    }

    // Everything below has R2 turned on.
    g_bools["ds5.trigger_r2_click"] = true;

    section("trigger click: the freeze arrives before the click");
    {
        State st;
        // A tenth of the way in: past the engage point, nowhere near the click.
        Out out = pull(st, 26, 0);
        CTM_CHECK(out.freeze);                     // cursor already still
        CTM_CHECK_EQ((int)out.buttons, 0);         // and nothing clicked yet
        // ⭐ That ordering is the whole design: the jolt of the press lands on a
        // cursor that stopped moving before the finger got there.
        out = pull(st, 240, 10);
        CTM_CHECK(out.freeze);
        CTM_CHECK_EQ((int)out.buttons, (int)kButtonLeft);
    }

    section("trigger click: a single click, released home");
    {
        State st;
        pull(st, 240, 0);
        const Out out = pull(st, 0, 20);
        CTM_CHECK_EQ((int)out.buttons, 0);
        CTM_CHECK(!out.freeze);                    // home, so the cursor is back
    }

    section("trigger click: a double click keeps the cursor still throughout");
    {
        State st;
        Out out = pull(st, 240, 0);
        CTM_CHECK_EQ((int)out.buttons, (int)kButtonLeft);
        // Lifted back over the click point but NOT home.
        out = pull(st, 60, 30);
        CTM_CHECK_EQ((int)out.buttons, 0);         // the button released
        CTM_CHECK(out.freeze);                     // ⭐ and the cursor did not move
        // The second press of the pair.
        out = pull(st, 240, 60);
        CTM_CHECK_EQ((int)out.buttons, (int)kButtonLeft);
        CTM_CHECK(out.freeze);
        // Only going home hands the cursor back.
        out = pull(st, 0, 80);
        CTM_CHECK_EQ((int)out.buttons, 0);
        CTM_CHECK(!out.freeze);
    }

    section("trigger click: holding past the window becomes a drag");
    {
        State st;
        Out out = pull(st, 240, 1000, 200);
        CTM_CHECK(out.freeze);
        // One millisecond short of the window is still a click.
        out = pull(st, 240, 1199, 200);
        CTM_CHECK(out.freeze);
        CTM_CHECK_EQ((int)out.buttons, (int)kButtonLeft);
        // ⭐ Exactly ON the window, the cursor comes back and the button stays.
        out = pull(st, 240, 1200, 200);
        CTM_CHECK(!out.freeze);
        CTM_CHECK_EQ((int)out.buttons, (int)kButtonLeft);
        // ⛔ Lifting over the click point mid-drag must NOT drop what is being
        // dragged. Only going home does.
        out = pull(st, 60, 1300, 200);
        CTM_CHECK_EQ((int)out.buttons, (int)kButtonLeft);
        CTM_CHECK(!out.freeze);
        out = pull(st, 0, 1400, 200);
        CTM_CHECK_EQ((int)out.buttons, 0);
        CTM_CHECK(!out.freeze);
    }

    section("trigger click: a window of zero turns dragging off");
    {
        State st;
        pull(st, 240, 0, 0);
        const Out out = pull(st, 240, 100000, 0);  // held a very long time
        CTM_CHECK_EQ((int)out.buttons, (int)kButtonLeft);
        CTM_CHECK(out.freeze);                     // never hands the cursor back
    }

    section("trigger click: the click point is honoured");
    {
        State st;
        // At 50%, a pull to 40% clicks nothing but still freezes.
        Out out = pull(st, 102, 0, 200, 50);
        CTM_CHECK(out.freeze);
        CTM_CHECK_EQ((int)out.buttons, 0);
        out = pull(st, 130, 10, 200, 50);
        CTM_CHECK_EQ((int)out.buttons, (int)kButtonLeft);
    }

    section("trigger click: two pads do not freeze each other");
    reset_all();
    g_bools["ds5.trigger_r2_click"] = true;
    {
        const std::vector<unsigned char> descriptor(12, 0);
        int padA = 0, padB = 0;
        // A is pulling; B is at rest.
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

    section("trigger click: L2 clicks the other button");
    reset_all();
    g_bools["ds5.trigger_l2_click"] = true;
    {
        const std::vector<unsigned char> descriptor(12, 0);
        int pad = 0;
        on_ds5_input(&pad, descriptor, "", report_with(240, 0).data(), 16);
        CTM_CHECK_EQ((int)g_buttons, (int)kButtonRight);
        CTM_CHECK(held_for(&pad));
    }

    reset_all();
    return 0;
}
