// Touchpad-to-mouse tests: cursor anchoring and carry, scroll ticks and
// direction, tap detection with a synthetic clock, and the off-control.
//
// touch_mouse.inl relies on its includer for its dependencies -- main.cpp has
// them; this translation unit brings its own stand-ins, the same pattern as
// gyro_mouse_test.cpp and rebind_test.cpp. Tests drive ctm_touch_mouse::step
// directly so time is a parameter, not a race.

#include "harness.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ⓘ Every pad's layout: where its fingers sit, which touch_mouse.inl now asks.
#include "input/button_layout.inl"

using namespace ctmtest;

// ---- Stand-ins for what touch_mouse.inl calls -------------------------------

namespace {

std::map<std::string, std::string> g_cfg;
bool g_configModeEffective = false;

int32_t g_pushedX = 0;      // summed cursor deltas pushed to the mailbox
int32_t g_pushedY = 0;
int g_pushCount = 0;
int g_wheelSum = 0;
uint8_t g_lastClick = 0;
uint8_t g_dragMask = 0;
// ⓘ Each pad's drag, kept apart as the mouse keeps it; g_dragMask is every
// pad's OR'd, which is what the host would see held.
std::map<const void *, uint8_t> g_dragFor;
int g_clickCount = 0;
int g_ensureCalls = 0;

void reset_stubs()
{
    g_cfg.clear();
    g_configModeEffective = false;
    g_pushedX = 0;
    g_pushedY = 0;
    g_pushCount = 0;
    g_wheelSum = 0;
    g_lastClick = 0;
    g_dragMask = 0;
    g_dragFor.clear();
    g_clickCount = 0;
    g_ensureCalls = 0;
}

} // namespace

static std::string device_config_str(const char *, const char *key)
{
    auto it = g_cfg.find(key);
    return it == g_cfg.end() ? std::string() : it->second;
}
static int device_config_int(const char *, const char *key, int fallback)
{
    auto it = g_cfg.find(key);
    if (it == g_cfg.end() || it->second.empty()) return fallback;
    return std::atoi(it->second.c_str());
}
static bool device_config_bool(const char *, const char *key, bool fallback)
{
    auto it = g_cfg.find(key);
    if (it == g_cfg.end()) return fallback;
    return it->second == "true";
}
static std::string device_settings_section(const char *kind, const std::string &linkedConfig)
{
    if (kind == nullptr) return std::string();
    if (linkedConfig.empty()) return std::string(kind);
    return "cfg:" + linkedConfig;
}
// ⓘ The real resolver lives in ds5_output_overrides.inl. Every descriptor here
// is a DualSense, as the stub it replaced always answered; the DS4 checks drive
// step() with the DS4's layout directly.
struct InputPad {
    const char *kind = nullptr;
    const ctm_rebind::Layout *layout = nullptr;
};
static InputPad device_input_pad_for(const std::vector<unsigned char> &)
{
    InputPad pad;
    pad.kind = "ds5";
    pad.layout = &ctm_rebind::kDs5Layout;
    return pad;
}
static bool ctm_rebind_config_mode_effective() { return g_configModeEffective; }

namespace device_log {
struct msg {
    template <typename T> msg &operator<<(const T &) { return *this; }
};
inline void input(const msg &) {}
} // namespace device_log
static void ctm_gyro_mouse_ensure_mouse_started() { ++g_ensureCalls; }

// ⛔ THE STUBS BELOW LIVE IN AN UNNAMED NAMESPACE, and that is load-bearing.
// Each test file defines its own stand-in ctm_gyro_mouse::shared_mailbox() and
// friends. As plain inline functions those have EXTERNAL linkage with identical
// signatures across files -- a one-definition-rule violation -- so the linker
// keeps one and every suite silently shares it. On 2026-08-31 that sent the
// stick suite's movement into the touch suite's counters, and eight stick
// checks failed reporting no movement at all while the code was correct.
// Nesting in an unnamed namespace gives them internal linkage; qualified names
// still resolve inside this file, and nothing can be folded across files.
namespace ctm_gyro_mouse {
namespace {

// The gate the touchpad now shares with gyro and the stick. Enough of it to
// exercise the touch paths; the real parser has its own suite.
enum class Gate { Off, Always, L2, R2, L1, R1, Touchpad, NotTouchpad, TouchpadClick, PS };

// ⓘ T-241 renamed the real one's "always" to a call, because a gate is a pair
// now. The double keeps its enum -- it is a stand-in, not the thing -- and
// answers to the same name so the callers under test compile unchanged.
inline Gate gate_always() { return Gate::Always; }

inline Gate parse_gate(const std::string &raw)
{
    if (raw == "always") return Gate::Always;
    if (raw == "L2" || raw == "l2") return Gate::L2;
    return Gate::Off;
}

// ⓘ Reads L2 through the layout, as the real gate does now, so the L2 gate check
// below exercises a DualSense's [5] the same way it always did.
inline bool gate_open(Gate gate, const ctm_rebind::Layout &lay, const uint8_t *d, size_t len)
{
    switch (gate) {
        case Gate::Always: return true;
        case Gate::L2: return ctm_rebind::trigger_travel(lay, d, len, true) >= ctm_rebind::kTriggerPulledTravel;
        default: return false;
    }
}

struct MouseDelta {
    int32_t dx = 0;
    int32_t dy = 0;
};
struct MailboxStub {
    void push(const MouseDelta &d)
    {
        g_pushedX += d.dx;
        g_pushedY += d.dy;
        ++g_pushCount;
    }
};
inline MailboxStub &shared_mailbox()
{
    static MailboxStub m;
    return m;
}
}  // unnamed -- internal linkage, see the note above
} // namespace ctm_gyro_mouse

namespace ctm_mouse_device {
namespace {   // internal linkage, same reason as above
inline void add_wheel(int ticks) { g_wheelSum += ticks; }
inline void add_click(uint8_t mask) { g_lastClick = mask; ++g_clickCount; }
inline void set_drag_for(const void *deviceKey, uint8_t mask)
{
    if (mask != 0) g_dragFor[deviceKey] = mask;
    else g_dragFor.erase(deviceKey);
    g_dragMask = 0;
    for (const auto &entry : g_dragFor) g_dragMask = static_cast<uint8_t>(g_dragMask | entry.second);
}
}
} // namespace ctm_mouse_device

// ⓘ A STAND-IN, like the Gate one above. touch_mouse.inl asks the rebinder
// how to read a binding value since T-242, and rebind.inl cannot be included
// by this binary -- so the mapping it needs is restated here, matching the
// real one in rebind.inl. ⚠️ It folds case, because the config reader
// lowercases every value and a comparison that does not silently never
// matches, which rebind.inl records happening three times.
namespace ctm_rebind {
enum MouseAction { kMouseNone = 0, kMouseLeft, kMouseRight, kMouseMiddle,
                   kMouseWheelUp, kMouseWheelDown };
inline MouseAction mouse_action_for(const std::string &code)
{
    std::string want;
    for (char c : code) want.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    if (want == "mouseleft")      return kMouseLeft;
    if (want == "mouseright")     return kMouseRight;
    if (want == "mousemiddle")    return kMouseMiddle;
    if (want == "mousewheelup")   return kMouseWheelUp;
    if (want == "mousewheeldown") return kMouseWheelDown;
    return kMouseNone;
}
}  // namespace ctm_rebind

#include "input/touch_mouse.inl"

// ---- Report scaffolding -----------------------------------------------------

namespace {

std::vector<uint8_t> rest_report()
{
    std::vector<uint8_t> r(64, 0);
    r[0] = 0x01;
    r[8] = 0x08;
    r[33] = 0x80;    // point 1 up
    r[37] = 0x80;    // point 2 up
    return r;
}

void set_point(std::vector<uint8_t> &r, int slot, bool down, int id, int x, int y)
{
    const size_t base = (slot == 0) ? 33 : 37;
    r[base] = static_cast<uint8_t>((down ? 0x00 : 0x80) | (id & 0x7f));
    r[base + 1] = static_cast<uint8_t>(x & 0xff);
    r[base + 2] = static_cast<uint8_t>(((x >> 8) & 0x0f) | ((y & 0x0f) << 4));
    r[base + 3] = static_cast<uint8_t>((y >> 4) & 0xff);
}

const void *kDev = reinterpret_cast<const void *>(0x2);

void run_step(std::vector<uint8_t> &r, long long nowMs)
{
    ctm_touch_mouse::step(kDev, "ds5", r.data(), r.size(), nowMs);
}

void fresh_device()
{
    ctm_touch_mouse::forget(kDev);
}

} // namespace

int run_touch_mouse_tests()
{
    section("touch: point encoding round-trips");
    {
        auto r = rest_report();
        set_point(r, 0, true, 42, 1900, 1000);
        const auto p = ctm_touch_mouse::read_point(r.data(), 33);
        CTM_CHECK(p.down);
        CTM_CHECK_EQ(p.id, 42);
        CTM_CHECK_EQ(p.x, 1900);
        CTM_CHECK_EQ(p.y, 1000);
    }

    section("touch: nothing configured pushes nothing");
    {
        reset_stubs();
        fresh_device();
        auto r = rest_report();
        set_point(r, 0, true, 1, 100, 100);
        run_step(r, 0);
        set_point(r, 0, true, 1, 500, 500);
        run_step(r, 16);
        CTM_CHECK_EQ(g_pushCount, 0);
        CTM_CHECK_EQ(g_wheelSum, 0);
        CTM_CHECK_EQ(g_clickCount, 0);
    }

    section("touch: first contact anchors, movement moves");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 100, 100);
        run_step(r, 0);
        CTM_CHECK_EQ(g_pushCount, 0);          // anchor only, no jump
        set_point(r, 0, true, 1, 150, 130);
        run_step(r, 8);
        CTM_CHECK_EQ(g_pushedX, 50);
        CTM_CHECK_EQ(g_pushedY, 30);
    }

    section("touch: lift and retouch re-anchors instead of jumping");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 100, 100);
        run_step(r, 0);
        set_point(r, 0, false, 1, 100, 100);   // lift
        run_step(r, 8);
        set_point(r, 0, true, 2, 900, 900);    // retouch far away, new id
        run_step(r, 400);
        CTM_CHECK_EQ(g_pushCount, 0);          // no jump across the lift
        set_point(r, 0, true, 2, 910, 905);
        run_step(r, 408);
        CTM_CHECK_EQ(g_pushedX, 10);
        CTM_CHECK_EQ(g_pushedY, 5);
    }

    section("touch: slow movement carries the sub-pixel remainder");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_mouse_speed"] = "10";  // 0.1 px per unit
        auto r = rest_report();
        set_point(r, 0, true, 1, 100, 100);
        run_step(r, 0);
        set_point(r, 0, true, 1, 105, 100);    // 0.5 px -- below one pixel
        run_step(r, 8);
        CTM_CHECK_EQ(g_pushCount, 0);
        set_point(r, 0, true, 1, 110, 100);    // now 1.0 px accumulated
        run_step(r, 16);
        CTM_CHECK_EQ(g_pushedX, 1);
    }

    section("touch: two-finger travel becomes wheel ticks, classic direction");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_scroll"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 400, 200);
        set_point(r, 1, true, 2, 600, 200);
        run_step(r, 0);                        // anchor
        set_point(r, 0, true, 1, 400, 320);    // both down 120 units
        set_point(r, 1, true, 2, 600, 320);
        run_step(r, 16);
        CTM_CHECK_EQ(g_wheelSum, -2);          // fingers down = wheel down
    }

    section("touch: natural scroll inverts");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_scroll"] = "true";
        g_cfg["touchpad_scroll_natural"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 400, 200);
        set_point(r, 1, true, 2, 600, 200);
        run_step(r, 0);
        set_point(r, 0, true, 1, 400, 320);
        set_point(r, 1, true, 2, 600, 320);
        run_step(r, 16);
        CTM_CHECK_EQ(g_wheelSum, 2);
    }

    section("touch: a quick still tap is a left click");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_tap_click"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 100);
        CTM_CHECK_EQ(g_clickCount, 1);
        CTM_CHECK_EQ(static_cast<int>(g_lastClick), 0x01);
    }

    section("touch: a two-finger tap is a right click, even with scroll on");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_tap_click"] = "true";
        g_cfg["touchpad_scroll"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 1, true, 2, 400, 300);    // second finger joins
        run_step(r, 30);
        set_point(r, 0, false, 1, 300, 300);   // both lift
        set_point(r, 1, false, 2, 400, 300);
        run_step(r, 110);
        CTM_CHECK_EQ(g_clickCount, 1);
        CTM_CHECK_EQ(static_cast<int>(g_lastClick), 0x02);
        CTM_CHECK_EQ(g_wheelSum, 0);           // resting fingers never scroll
    }

    // ⓘ The two above now double as the MIGRATION tests: they set the old
    // `touchpad_tap_click` bool and still expect left and right, which only
    // holds while the old spelling keeps meaning exactly what it meant.

    section("touch: T-242, a one-finger tap does what it is REMAPPED to");
    {
        reset_stubs();
        fresh_device();
        // ⛔ Right click from ONE finger -- impossible before, because the bool
        // hard-coded one finger to left.
        g_cfg["touchpad_one_finger_tap"] = "MouseRight";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 100);
        CTM_CHECK_EQ(g_clickCount, 1);
        CTM_CHECK_EQ(static_cast<int>(g_lastClick), 0x02);
    }

    section("touch: T-242, a finger count with no action set does NOTHING");
    {
        reset_stubs();
        fresh_device();
        // One finger acts; two is deliberately blank. ⚠️ It must NOT fall back
        // to the one-finger action -- "two fingers do nothing" has to be sayable.
        g_cfg["touchpad_one_finger_tap"] = "MouseLeft";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 1, true, 2, 400, 300);
        run_step(r, 30);
        set_point(r, 0, false, 1, 300, 300);
        set_point(r, 1, false, 2, 400, 300);
        run_step(r, 110);
        CTM_CHECK_EQ(g_clickCount, 0);
    }

    section("touch: T-242, the drag holds whichever button it is given");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_press_touch_drag"] = "MouseMiddle";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);   // DS5 pad pressed in, clickByte 10
        run_step(r, 0);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x04);
        // ⭐ THE CLICK CAN GO AND THE DRAG STAYS. That is the whole point of it.
        r[10] = static_cast<uint8_t>(r[10] & ~0x02);  // the click let go
        run_step(r, 40);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x04);
        // ⓘ It ends when the FINGER leaves, not when the click did.
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 80);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x00);
    }

    section("touch: a slow hold is not a tap");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_tap_click"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 600);                      // past kTapMaxMs
        CTM_CHECK_EQ(g_clickCount, 0);
    }

    section("touch: a moving finger is not a tap");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_tap_click"] = "true";
        g_cfg["touchpad_to_mouse"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 420, 300);    // well past the slop
        run_step(r, 40);
        set_point(r, 0, false, 1, 420, 300);
        run_step(r, 80);
        CTM_CHECK_EQ(g_clickCount, 0);
        CTM_CHECK(g_pushedX > 0);              // it was cursor movement instead
    }

    section("touch: a tap never nudges the cursor");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_tap_click"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 305, 302);    // the twitch a real tap makes
        run_step(r, 30);
        set_point(r, 0, false, 1, 305, 302);
        run_step(r, 90);
        CTM_CHECK_EQ(g_pushCount, 0);          // the hardware finding, fixed
        CTM_CHECK_EQ(g_clickCount, 1);         // and the tap still clicks
    }

    section("touch: a drag unlocks at the slop and moves from there");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_tap_click"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 310, 300);    // inside the slop: held still
        run_step(r, 16);
        CTM_CHECK_EQ(g_pushCount, 0);
        set_point(r, 0, true, 1, 322, 300);    // past the slop: unlocked
        run_step(r, 32);
        CTM_CHECK_EQ(g_pushedX, 12);           // this report's step only --
        CTM_CHECK_EQ(g_pushedY, 0);            // no replayed 22-unit jump
    }

    section("touch: a gate can hold the touchpad off, absent means always");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_to_mouse_gate"] = "L2";
        auto r = rest_report();
        set_point(r, 0, true, 1, 100, 100);
        run_step(r, 0);
        set_point(r, 0, true, 1, 400, 400);
        run_step(r, 16);
        CTM_CHECK_EQ(g_pushCount, 0);          // gate shut, nothing moves

        r[5] = 200;                            // L2 pulled
        set_point(r, 0, true, 1, 400, 400);
        run_step(r, 32);                       // re-anchors here
        set_point(r, 0, true, 1, 410, 400);
        run_step(r, 48);
        CTM_CHECK_EQ(g_pushedX, 10);           // from the re-anchor, no jump
    }

    section("touch drag: click to grab, LIFT to drop");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_click_drag"] = "true";
        auto r = rest_report();

        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0);   // finger alone: nothing

        r[10] |= 0x02;                                   // pad clicked in
        run_step(r, 16);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x01); // grabbed

        r[10] &= ~0x02;                                  // click released early
        set_point(r, 0, true, 1, 400, 380);
        run_step(r, 32);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x01); // ⭐ still held
        CTM_CHECK(g_pushedX > 0);                        // and still moving

        set_point(r, 0, false, 1, 400, 380);             // finger lifts
        run_step(r, 48);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0);   // dropped
    }

    section("touch drag: a click with no finger is not a drag");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_click_drag"] = "true";
        auto r = rest_report();
        r[10] |= 0x02;                                   // clicked, no touch
        run_step(r, 0);
        run_step(r, 16);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0);
    }

    section("touch drag: nothing is left held when it cannot continue");
    {
        // ⛔ Every way out of a drag must release the button, or the desktop is
        // left with a stuck mouse and nothing able to let go.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_click_drag"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        r[10] |= 0x02;
        run_step(r, 0);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x01);

        // ⭐ THE GATE NO LONGER DROPS A DRAG (rhoquinn8217, 2026-09-02). Safe
        // Edit Mode stops a pad MIRRORING INTO A GAME, and a drag cannot do
        // that -- it goes to whatever has focus, which while the gate applies
        // is our own settings window. So a drag in progress simply continues.
        g_configModeEffective = true;                    // settings page opens
        run_step(r, 16);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x01);

        // ⛔ But LIFTING still drops it, gate or no gate: that is the release
        // path that matters, and nothing may leave a button held.
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 32);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0);
        g_configModeEffective = false;

        // ...and an unbridge mid-drag.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_click_drag"] = "true";
        auto r2 = rest_report();
        set_point(r2, 0, true, 1, 300, 300);
        r2[10] |= 0x02;
        run_step(r2, 0);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x01);
        ctm_touch_mouse::forget(kDev);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0);
    }

    section("touch drag: one pad dropping its drag leaves another pad's held");
    {
        // ⛔ The drag was one level for every pad until 2026-09-15, so a second
        // pad lifting its finger let go of the first pad's drag mid-move. ⓘ The
        // stub keeps each pad's drag apart as the mouse does, so this also fails
        // if the hook stops passing its own pad.
        reset_stubs();
        fresh_device();
        const void *const kOtherDev = reinterpret_cast<const void *>(0x4);
        ctm_touch_mouse::forget(kOtherDev);
        g_cfg["touchpad_click_drag"] = "true";

        auto a = rest_report();
        set_point(a, 0, true, 1, 300, 300);
        a[10] |= 0x02;
        run_step(a, 0);                                  // this pad grabs
        auto b = rest_report();
        set_point(b, 0, true, 1, 900, 600);
        b[10] |= 0x02;
        ctm_touch_mouse::step(kOtherDev, "ds5", b.data(), b.size(), 0);   // so does the other
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x01);

        set_point(b, 0, false, 1, 900, 600);
        b[10] &= ~0x02;
        ctm_touch_mouse::step(kOtherDev, "ds5", b.data(), b.size(), 16);  // the other lifts
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x01); // ⭐ this pad's drag holds

        a[10] &= ~0x02;
        set_point(a, 0, false, 1, 300, 300);
        run_step(a, 32);                                 // this pad lifts too
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0);
        ctm_touch_mouse::forget(kOtherDev);
    }

    // ⭐ THE RULE CHANGED HERE (rhoquinn8217, 2026-09-02). This used to assert
    // that the gate stood the touchpad down entirely -- and that over-gated:
    // pointer movement cannot reach a game, so suspending it protected nothing
    // and made the settings page look as though the pad had died.
    section("touch: the cursor still works while the gate is on");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_configModeEffective = true;
        auto r = rest_report();
        set_point(r, 0, true, 1, 100, 100);
        run_step(r, 0);
        set_point(r, 0, true, 1, 500, 500);
        run_step(r, 16);
        CTM_CHECK(g_pushCount > 0);
    }

    // ---- DualShock 4: the same touchpad, at its own offsets --------------------

    // A DS4 USB report at rest, taken from a real pad on 2026-09-15: one touch
    // packet, both fingers up (0xa4 and 0xa2 carry stale coordinates), older
    // packets inactive.
    const uint8_t kDs4Rest[64] = {
        0x01, 0x7c, 0x80, 0x85, 0x81, 0x08, 0x00, 0xe4, 0x00, 0x00, 0x85, 0x95, 0x16, 0xfd, 0xff, 0x02,
        0x00, 0xfd, 0xff, 0xa5, 0xff, 0x7b, 0x1f, 0x79, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1b, 0x00,
        0x00, 0x01, 0x8b, 0xa4, 0x26, 0xf0, 0x22, 0xa2, 0x84, 0x60, 0x16, 0x00, 0x80, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
    };
    auto ds4_point = [](std::vector<uint8_t> &r, int slot, bool down, int id, int x, int y) {
        const size_t base = (slot == 0) ? 35 : 39;
        r[base] = static_cast<uint8_t>((down ? 0x00 : 0x80) | (id & 0x7f));
        r[base + 1] = static_cast<uint8_t>(x & 0xff);
        r[base + 2] = static_cast<uint8_t>(((x >> 8) & 0x0f) | ((y & 0x0f) << 4));
        r[base + 3] = static_cast<uint8_t>((y >> 4) & 0xff);
    };
    auto ds4_step = [](std::vector<uint8_t> &r, long long nowMs) {
        ctm_touch_mouse::step(kDev, "ds4", ctm_rebind::kDs4Layout, r.data(), r.size(), nowMs);
    };

    section("touch: a DS4 finger at [35] moves the cursor");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        std::vector<uint8_t> r(kDs4Rest, kDs4Rest + 64);
        ds4_point(r, 0, true, 5, 300, 200);
        ds4_step(r, 0);
        CTM_CHECK_EQ(g_pushCount, 0);          // anchor only
        ds4_point(r, 0, true, 5, 340, 225);
        ds4_step(r, 8);
        CTM_CHECK_EQ(g_pushedX, 40);
        CTM_CHECK_EQ(g_pushedY, 25);
    }

    section("touch: a DS4 at rest moves nothing, and its packet count is not a finger");
    {
        // ⛔⛔ THE FAULT, pinned. Read at DualSense offsets, a DS4's [33] -- the
        // count of touch packets, 0x01 here -- has its high bit clear, which is
        // "finger down". With the DS4 layout the same bytes are no fingers at all.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_tap_click"] = "true";
        std::vector<uint8_t> r(kDs4Rest, kDs4Rest + 64);
        ds4_step(r, 0);
        ds4_step(r, 8);
        ds4_step(r, 300);
        CTM_CHECK_EQ(g_pushCount, 0);
        CTM_CHECK_EQ(g_clickCount, 0);
        CTM_CHECK(!ctm_rebind::touch_finger_down(ctm_rebind::kDs4Layout, r.data(), r.size(), 0));
        CTM_CHECK(ctm_rebind::touch_finger_down(ctm_rebind::kDs5Layout, r.data(), r.size(), 0));
    }

    section("touch: a DS4 two-finger drag scrolls");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_scroll"] = "2";
        std::vector<uint8_t> r(kDs4Rest, kDs4Rest + 64);
        ds4_point(r, 0, true, 1, 800, 300);
        ds4_point(r, 1, true, 2, 1000, 300);
        ds4_step(r, 0);
        ds4_point(r, 0, true, 1, 800, 480);
        ds4_point(r, 1, true, 2, 1000, 480);
        ds4_step(r, 8);
        CTM_CHECK(g_wheelSum != 0);
    }

    section("touch: a DS4 press at [7] drags, and the counter bits beside it do not");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_click_drag"] = "true";
        std::vector<uint8_t> r(kDs4Rest, kDs4Rest + 64);
        // [7] is 0xe4 at rest: counter bits set, PS and press clear. No drag.
        ds4_point(r, 0, true, 1, 500, 500);
        ds4_step(r, 0);
        CTM_CHECK_EQ(g_dragMask, 0);
        r[7] = static_cast<uint8_t>(r[7] | 0x02);   // the pad pressed in
        ds4_step(r, 8);
        CTM_CHECK_EQ(g_dragMask, 1);
    }

    return 0;
}
