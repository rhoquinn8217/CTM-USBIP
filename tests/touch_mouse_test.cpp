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
// ⓘ What a gesture that is not a mouse button reached for: a key tapped, a key
// held (per pad, as the keyboard keeps it), and an on-screen keyboard toggled.
int g_pulseCount = 0;
uint8_t g_pulseModifier = 0;
uint8_t g_pulseUsage = 0;
std::map<const void *, std::pair<uint8_t, std::vector<uint8_t>>> g_touchKeys;
int g_ensureKeyboardCalls = 0;
int g_oskCount = 0;
int g_oskButton = 0;
int g_oskProgram = -1;
// ⓘ A stand-in for the real cursor: movement pushed moves it, as delivery to
// Windows would, and the touchpad's put-back reads and sets it.
long g_cursorX = 0;
long g_cursorY = 0;
int g_placeCount = 0;

void reset_stubs()
{
    g_cursorX = 0;
    g_cursorY = 0;
    g_placeCount = 0;
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
    g_pulseCount = 0;
    g_pulseModifier = 0;
    g_pulseUsage = 0;
    g_touchKeys.clear();
    g_ensureKeyboardCalls = 0;
    g_oskCount = 0;
    g_oskButton = 0;
    g_oskProgram = -1;
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
        g_cursorX += d.dx;
        g_cursorY += d.dy;
    }
    void clear() {}   // nothing waits here: the stub delivers at once
};
inline MailboxStub &shared_mailbox()
{
    static MailboxStub m;
    return m;
}
inline bool cursor_read(long *x, long *y)
{
    *x = g_cursorX;
    *y = g_cursorY;
    return true;
}
inline void cursor_place(long x, long y)
{
    g_cursorX = x;
    g_cursorY = y;
    ++g_placeCount;
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

// ⓘ NO STAND-IN FOR READING A BINDING'S VALUE ANY MORE. The mouse names were
// restated here while the real ones lived in rebind.inl, which this binary
// cannot include. They are in binding_names.inl now, which touch_mouse.inl
// includes for itself, so the gestures below are read by the real reader.

// ⓘ The keyboard, for a gesture bound to a key: a tap asks for one press, the
// pad pressed in holds a level of its own.
namespace ctm_keyboard_device {
namespace {   // internal linkage, same reason as above
inline void pulse_key(uint8_t modifier, uint8_t usage)
{
    g_pulseModifier = modifier;
    g_pulseUsage = usage;
    ++g_pulseCount;
}
inline void set_touch_keys_for(const void *deviceKey, uint8_t modifiers,
                               const uint8_t *keys, size_t count)
{
    if (modifiers == 0 && (keys == nullptr || count == 0)) {
        g_touchKeys.erase(deviceKey);
        return;
    }
    auto &slot = g_touchKeys[deviceKey];
    slot.first = modifiers;
    slot.second.assign(keys, keys + (keys ? count : 0));
}
}
} // namespace ctm_keyboard_device
static void ctm_rebind_ensure_keyboard_started() { ++g_ensureKeyboardCalls; }
static void ctm_osk_toggle(const std::string &, int button, int program)
{
    g_oskButton = button;
    g_oskProgram = program;
    ++g_oskCount;
}

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

    // ⭐ THE RULE WENT ROUND ONCE ON 2026-09-30 AND CAME BACK. A hold that kept
    // every move back until a touch showed what it was made the pad jump and
    // feel unresponsive (rhoquinn8217). The cursor moves at once again, still
    // for the tap guard's first 15 units, and a tap, a late second finger or a
    // press PUTS IT BACK instead.
    section("touch: the cursor stays still for the first 15 units, then moves from there");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_tap_click"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 310, 300);    // inside 15 units: still
        run_step(r, 16);
        CTM_CHECK_EQ(g_pushCount, 0);
        set_point(r, 0, true, 1, 322, 300);    // past it: moving
        run_step(r, 32);
        CTM_CHECK_EQ(g_pushedX, 12);           // this report's step only --
        CTM_CHECK_EQ(g_pushedY, 0);            // no replayed 22-unit jump
    }

    section("touch: a tap that rolls clicks where the finger landed");
    {
        // ⓘ 84 units is the largest roll measured on 2026-09-30.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_tap_click"] = "true";
        g_cursorX = 1000;
        g_cursorY = 500;
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 350, 310);
        run_step(r, 40);
        set_point(r, 0, true, 1, 384, 312);
        run_step(r, 120);
        CTM_CHECK(g_pushCount > 0);            // the cursor did move, at once
        set_point(r, 0, false, 1, 384, 312);
        run_step(r, 180);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1000);   // and went back first
        CTM_CHECK_EQ(static_cast<int>(g_cursorY), 500);
        CTM_CHECK_EQ(g_clickCount, 0);         // the click waits for that to settle
        run_step(r, 215);
        CTM_CHECK_EQ(g_clickCount, 1);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1000);
    }

    section("touch: a double tap's second click lands exactly on the first");
    {
        // rhoquinn8217, 2026-09-30: "double tapping is very hard to do".
        // Windows wants both clicks within about 2 pixels.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_tap_click"] = "true";
        g_cursorX = 1000;
        g_cursorY = 500;
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);    // the first tap rolls
        run_step(r, 0);
        set_point(r, 0, true, 1, 350, 300);
        run_step(r, 40);
        set_point(r, 0, false, 1, 350, 300);
        run_step(r, 80);
        run_step(r, 115);
        CTM_CHECK_EQ(g_clickCount, 1);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1000);
        g_cursorX = 1003;                      // movement that landed late
        set_point(r, 0, true, 2, 310, 305);    // the second tap, still
        run_step(r, 200);
        set_point(r, 0, false, 2, 310, 305);
        run_step(r, 260);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1000);   // put on the first
        CTM_CHECK_EQ(g_clickCount, 1);         // and waits
        run_step(r, 295);
        CTM_CHECK_EQ(g_clickCount, 2);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1000);
        CTM_CHECK_EQ(static_cast<int>(g_cursorY), 500);
    }

    section("touch: a tap too late or too far for a double tap clicks where it is");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_tap_click"] = "true";
        g_cursorX = 1000;
        g_cursorY = 500;
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 80);
        CTM_CHECK_EQ(g_clickCount, 1);         // still, so at once
        g_cursorX = 1003;
        set_point(r, 0, true, 2, 300, 300);    // 680 ms later
        run_step(r, 700);
        set_point(r, 0, false, 2, 300, 300);
        run_step(r, 760);
        CTM_CHECK_EQ(g_clickCount, 2);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1003);
        g_cursorX = 1040;                      // 37 px away, soon after
        set_point(r, 0, true, 3, 300, 300);
        run_step(r, 900);
        set_point(r, 0, false, 3, 300, 300);
        run_step(r, 960);
        CTM_CHECK_EQ(g_clickCount, 3);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1040);
        CTM_CHECK_EQ(g_placeCount, 0);
    }

    section("touch: a slow tap under 400 ms still clicks");
    {
        // ⓘ Slow taps took up to 390 ms on 2026-09-30, and 250 failed them all.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_tap_click"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 306, 302);
        run_step(r, 200);
        set_point(r, 0, false, 1, 306, 302);
        run_step(r, 350);
        CTM_CHECK_EQ(g_clickCount, 1);
        CTM_CHECK_EQ(g_pushCount, 0);
    }

    section("touch: a late second finger puts the cursor back, and scrolls there");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_tap_click"] = "true";
        g_cfg["touchpad_scroll"] = "2";
        g_cursorX = 1000;
        g_cursorY = 500;
        auto r = rest_report();
        set_point(r, 0, true, 1, 400, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 400, 420);    // the first finger moves --
        run_step(r, 40);
        CTM_CHECK(g_cursorY != 500);           // and so does the cursor, at once
        set_point(r, 1, true, 2, 600, 300);    // the second finger, 80 ms late
        run_step(r, 80);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1000);   // back where it began
        CTM_CHECK_EQ(static_cast<int>(g_cursorY), 500);
        set_point(r, 0, true, 1, 400, 540);    // both scroll down
        set_point(r, 1, true, 2, 600, 420);
        run_step(r, 100);
        CTM_CHECK(g_wheelSum != 0);
        CTM_CHECK_EQ(static_cast<int>(g_cursorY), 500);    // and stays there
    }

    section("touch: a second finger long after the first leaves the move alone");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_scroll"] = "2";
        g_cursorX = 1000;
        g_cursorY = 500;
        auto r = rest_report();
        set_point(r, 0, true, 1, 400, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 400, 420);
        run_step(r, 40);
        run_step(r, 100);                      // the finger rests there
        set_point(r, 1, true, 2, 600, 300);    // 300 ms later: a move, then a scroll
        run_step(r, 300);
        CTM_CHECK_EQ(static_cast<int>(g_cursorY), 620);
        CTM_CHECK_EQ(g_placeCount, 0);
    }

    section("touch: a press goes back to a moment before it, then grabs, then holds still");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_press_touch_drag"] = "MouseLeft";
        g_cursorX = 1000;
        g_cursorY = 500;
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 310, 300);
        run_step(r, 10);
        set_point(r, 0, true, 1, 320, 300);
        run_step(r, 60);
        set_point(r, 0, true, 1, 330, 300);    // the finger rolls as it presses
        run_step(r, 120);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1030);
        set_point(r, 0, true, 1, 340, 300);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);   // the click
        run_step(r, 200);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x01);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1010);   // where it was 100 ms before
        set_point(r, 0, true, 1, 348, 300);    // the click bottoming out
        run_step(r, 220);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1010);   // still
        set_point(r, 0, true, 1, 380, 300);    // now a real drag
        run_step(r, 260);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1042);
    }

    section("touch: a press straight after a fast long move grabs where the cursor is");
    {
        // rhoquinn8217, 2026-09-30: going back along a fast move lands
        // somewhere nobody meant to grab.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_press_touch_drag"] = "MouseLeft";
        g_cursorX = 1000;
        g_cursorY = 500;
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        for (int i = 1; i <= 9; ++i) {         // 900 units in 45 ms
            set_point(r, 0, true, 1, 300 + 100 * i, 300);
            run_step(r, 5 * i);
        }
        set_point(r, 0, true, 1, 1210, 300);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);
        run_step(r, 50);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x01);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1900);
        CTM_CHECK_EQ(g_placeCount, 0);
    }

    section("touch: a finger still sliding as a scroll ends moves nothing");
    {
        // ⓘ Measured 2026-09-30: the last finger slid on for up to 113 ms and
        // moved the cursor up to 588 px.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_scroll"] = "2";
        auto r = rest_report();
        set_point(r, 0, true, 1, 400, 300);
        set_point(r, 1, true, 2, 600, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 400, 420);
        set_point(r, 1, true, 2, 600, 420);
        run_step(r, 50);
        set_point(r, 1, false, 2, 600, 420);   // one finger lifts first
        run_step(r, 100);
        for (int i = 1; i <= 24; ++i) {        // the other slides on for 120 ms
            set_point(r, 0, true, 1, 400, 420 + 20 * i);
            run_step(r, 100 + 5 * i);
        }
        set_point(r, 0, false, 1, 400, 900);
        run_step(r, 225);
        CTM_CHECK_EQ(g_pushCount, 0);
    }

    section("touch: after a scroll, the finger left behind points again once it rests");
    {
        // rhoquinn8217, 2026-09-30: lift one finger after a scroll and carry
        // on with the other, without lifting both first.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_scroll"] = "2";
        auto r = rest_report();
        set_point(r, 0, true, 1, 400, 300);
        set_point(r, 1, true, 2, 600, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 400, 420);    // a scroll
        set_point(r, 1, true, 2, 600, 420);
        run_step(r, 50);
        CTM_CHECK(g_wheelSum != 0);
        set_point(r, 1, false, 2, 600, 420);   // one finger lifts
        run_step(r, 300);
        set_point(r, 0, true, 1, 402, 421);    // the other rests a moment
        run_step(r, 320);
        run_step(r, 340);
        CTM_CHECK_EQ(g_pushCount, 0);
        set_point(r, 0, true, 1, 440, 421);    // and moves: it points again
        run_step(r, 360);
        set_point(r, 0, true, 1, 480, 421);
        run_step(r, 380);
        CTM_CHECK_EQ(static_cast<int>(g_pushedX), 40);   // from where it took over, no jump
    }

    section("touch: a finger that keeps sliding after a scroll stopped points after 150 ms, from where it is");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_scroll"] = "2";
        auto r = rest_report();
        set_point(r, 0, true, 1, 400, 300);
        set_point(r, 1, true, 2, 600, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 400, 420);
        set_point(r, 1, true, 2, 600, 420);
        run_step(r, 50);
        run_step(r, 100);                      // the scroll stops
        run_step(r, 180);
        set_point(r, 1, false, 2, 600, 420);   // and one finger lifts
        run_step(r, 200);
        int x = 400;
        for (long long t = 210; t < 350; t += 10) {   // the other slides right
            x += 20;
            set_point(r, 0, true, 1, x, 420);
            run_step(r, t);
        }
        CTM_CHECK_EQ(g_pushCount, 0);                 // not yet
        x += 20;
        set_point(r, 0, true, 1, x, 420);
        run_step(r, 350);                             // 150 ms: it takes over here
        CTM_CHECK_EQ(g_pushCount, 0);
        x += 20;
        set_point(r, 0, true, 1, x, 420);
        run_step(r, 360);
        CTM_CHECK_EQ(static_cast<int>(g_pushedX), 20);   // only what came after
    }

    section("touch: a two-finger tap whose fingers lift apart still right-clicks, and moves nothing");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_tap_click"] = "true";
        g_cfg["touchpad_scroll"] = "2";
        auto r = rest_report();
        set_point(r, 0, true, 1, 400, 300);
        set_point(r, 1, true, 2, 600, 300);
        run_step(r, 0);
        set_point(r, 1, false, 2, 600, 300);   // one lifts
        run_step(r, 100);
        for (int i = 1; i <= 8; ++i) {         // the other lingers, rolling a little
            set_point(r, 0, true, 1, 400 + i, 300);
            run_step(r, 100 + 10 * i);
        }
        set_point(r, 0, false, 1, 408, 300);
        run_step(r, 190);
        CTM_CHECK_EQ(g_clickCount, 1);
        CTM_CHECK_EQ(static_cast<int>(g_lastClick), 0x02);
        CTM_CHECK_EQ(g_pushCount, 0);
    }

    section("touch: a leap in one report is the pad, not a finger, and moves nothing");
    {
        // ⓘ Measured 2026-09-30: as a second finger came down, the pad reported
        // the first leaping 200 to 490 units in one report. The biggest real
        // step in 408 one-finger touches was 108.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 400, 300);
        run_step(r, 0);
        set_point(r, 0, true, 1, 410, 300);
        run_step(r, 4);
        set_point(r, 0, true, 1, 750, 310);    // the pad leaps 340 units
        run_step(r, 8);
        CTM_CHECK_EQ(static_cast<int>(g_pushedX), 10);
        set_point(r, 0, true, 1, 760, 310);    // and the finger carries on from there
        run_step(r, 12);
        CTM_CHECK_EQ(static_cast<int>(g_pushedX), 20);
        CTM_CHECK_EQ(static_cast<int>(g_pushedY), 0);
    }

    section("touch: a fast flick still moves the cursor all the way");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        auto r = rest_report();
        set_point(r, 0, true, 1, 100, 500);
        run_step(r, 0);
        for (int i = 1; i <= 9; ++i) {         // 190 units a report: the fastest measured was 188
            set_point(r, 0, true, 1, 100 + 190 * i, 500);
            run_step(r, 5 * i);
        }
        CTM_CHECK_EQ(static_cast<int>(g_pushedX), 1710);
    }

    section("touch: a second finger after a move puts back what the pad did as it came");
    {
        // rhoquinn8217, 2026-09-30: moving with one finger, then bringing the
        // second down to scroll, made the cursor jump a long way. ⓘ Measured:
        // for 30 to 75 ms before the pad reports a second finger, it drags the
        // first one's position toward it.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_scroll"] = "2";
        g_cursorX = 1000;
        g_cursorY = 500;
        auto r = rest_report();
        set_point(r, 0, true, 1, 400, 300);
        run_step(r, 0);
        for (int i = 1; i <= 50; ++i) {        // a real move, 100 units down
            set_point(r, 0, true, 1, 400, 300 + 2 * i);
            run_step(r, 4 * i);
        }
        for (long long t = 250; t <= 400; t += 50) run_step(r, t);   // it rests
        CTM_CHECK_EQ(static_cast<int>(g_cursorY), 600);
        set_point(r, 0, true, 1, 460, 400);    // the pad drags it toward the second
        run_step(r, 404);
        set_point(r, 0, true, 1, 550, 400);
        run_step(r, 408);
        set_point(r, 0, true, 1, 700, 400);
        run_step(r, 412);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1300);   // and the cursor followed
        set_point(r, 0, true, 1, 402, 401);    // then reports both
        set_point(r, 1, true, 2, 1000, 380);
        run_step(r, 420);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1000);   // the drag undone
        CTM_CHECK_EQ(static_cast<int>(g_cursorY), 600);    // the move kept
        CTM_CHECK_EQ(g_placeCount, 1);
    }

    section("touch: a finger held between scroll strokes moves nothing, even as the pad drags it");
    {
        // rhoquinn8217, 2026-09-30: holding one finger and scrolling with the
        // other made the cursor jump.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_to_mouse"] = "true";
        g_cfg["touchpad_scroll"] = "2";
        g_cursorX = 1000;
        g_cursorY = 500;
        auto r = rest_report();
        set_point(r, 0, true, 1, 400, 600);    // held
        set_point(r, 1, true, 2, 1200, 300);   // strokes
        run_step(r, 0);
        set_point(r, 1, true, 2, 1200, 420);
        run_step(r, 50);
        CTM_CHECK(g_wheelSum != 0);
        set_point(r, 1, false, 2, 1200, 420);  // the stroking finger lifts
        run_step(r, 100);
        for (long long t = 110; t <= 290; t += 10) {       // the held finger stays
            set_point(r, 0, true, 1, 400 + static_cast<int>((t / 10) % 2), 600);
            run_step(r, t);
        }
        set_point(r, 0, true, 1, 405, 600);    // a small roll, well past 150 ms
        run_step(r, 295);
        CTM_CHECK_EQ(g_pushCount, 0);          // no take-over by time
        set_point(r, 0, true, 1, 440, 580);    // the pad drags it as the other returns
        run_step(r, 304);
        set_point(r, 0, true, 1, 500, 550);
        run_step(r, 308);
        set_point(r, 0, true, 1, 600, 500);
        run_step(r, 312);
        CTM_CHECK_EQ(g_pushCount, 0);          // lifted mid-stroke: it holds
        set_point(r, 0, true, 1, 402, 600);    // the next stroke lands
        set_point(r, 1, true, 3, 1200, 300);
        run_step(r, 320);
        CTM_CHECK_EQ(static_cast<int>(g_cursorX), 1000);   // where the stroke left it
        CTM_CHECK_EQ(static_cast<int>(g_cursorY), 500);
        CTM_CHECK_EQ(g_placeCount, 0);         // with nothing to put back
    }

    section("touch: a short scroll is not a two-finger tap");
    {
        // ⛔ With the tap distance at 100, a one-tick scroll ends inside it, and
        // without this rule it right-clicked as it lifted.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_tap_click"] = "true";
        g_cfg["touchpad_scroll"] = "2";
        auto r = rest_report();
        set_point(r, 0, true, 1, 400, 200);
        set_point(r, 1, true, 2, 600, 200);
        run_step(r, 0);
        set_point(r, 0, true, 1, 400, 260);    // 60 units: one wheel tick
        set_point(r, 1, true, 2, 600, 260);
        run_step(r, 40);
        set_point(r, 0, false, 1, 400, 260);
        set_point(r, 1, false, 2, 600, 260);
        run_step(r, 100);
        CTM_CHECK(g_wheelSum != 0);
        CTM_CHECK_EQ(g_clickCount, 0);
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
        set_point(r, 0, true, 1, 150, 150);    // a finger's step, not a leap
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

    // ---- A gesture that is not a mouse button --------------------------------
    //
    // ⭐ WHAT THESE PROTECT. The three gesture settings take any value a
    // button's remap takes. Each kind has its own way of being pressed and its
    // own way of being let go, and the checks below ask for both halves.
    //
    // WHAT THEY CANNOT DO. They stop at the stand-ins: that a key is released
    // by the keyboard's pump is key_pulse_test's, and that the rebinder
    // presses the button it is told about is not tested anywhere here.

    section("touch: a tap bound to a key presses that key once, and clicks nothing");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_one_finger_tap"] = "Enter";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        CTM_CHECK_EQ(g_pulseCount, 0);          // not while the finger is down
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 100);
        CTM_CHECK_EQ(g_pulseCount, 1);
        CTM_CHECK_EQ(static_cast<int>(g_pulseUsage), 0x28);
        CTM_CHECK_EQ(static_cast<int>(g_pulseModifier), 0);
        CTM_CHECK(g_ensureKeyboardCalls > 0);   // or the press goes nowhere
        CTM_CHECK_EQ(g_clickCount, 0);
        // ⓘ And only once: the reports that follow the lift add nothing.
        run_step(r, 108);
        run_step(r, 116);
        CTM_CHECK_EQ(g_pulseCount, 1);
    }

    section("touch: the two-finger tap has its own key, and a modifier is a key");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_one_finger_tap"] = "Enter";
        g_cfg["touchpad_two_finger_tap"] = "ShiftLeft";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 1, true, 2, 400, 300);
        run_step(r, 30);
        set_point(r, 0, false, 1, 300, 300);
        set_point(r, 1, false, 2, 400, 300);
        run_step(r, 110);
        CTM_CHECK_EQ(g_pulseCount, 1);
        CTM_CHECK_EQ(static_cast<int>(g_pulseModifier), 0x02);
        CTM_CHECK_EQ(static_cast<int>(g_pulseUsage), 0);
    }

    section("touch: a key name as the config reader hands it back, lowercased, still presses");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_one_finger_tap"] = "arrowup";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 100);
        CTM_CHECK_EQ(g_pulseCount, 1);
        CTM_CHECK_EQ(static_cast<int>(g_pulseUsage), 0x52);
    }

    section("touch: a tap bound to a pad button presses it for a moment, then lets go");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_one_finger_tap"] = "button_cross";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 50), 0u);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 100);
        // ⓘ Standard index 0 is Cross. What the rebinder would be told, report
        // by report, as the clock moves on.
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 100), 1u);
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 100 + pad_press::kTapHoldMs - 1), 1u);
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 100 + pad_press::kTapHoldMs), 0u);
        CTM_CHECK_EQ(g_clickCount, 0);
        CTM_CHECK_EQ(g_pulseCount, 0);
    }

    section("touch: a tap bound to an on-screen keyboard toggles it, with no button reserved");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_two_finger_tap"] = "KeyboardDS5_USBIP";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        set_point(r, 1, true, 2, 400, 300);
        run_step(r, 0);
        set_point(r, 0, false, 1, 300, 300);
        set_point(r, 1, false, 2, 400, 300);
        run_step(r, 90);
        CTM_CHECK_EQ(g_oskCount, 1);
        CTM_CHECK_EQ(g_oskProgram, static_cast<int>(binding::kOskOurs));
        // ⓘ -1: a button that opens the keyboard is kept for closing it, and a
        // tap is not a button.
        CTM_CHECK_EQ(g_oskButton, -1);
        // ⭐ The same tap again is the other half of the toggle.
        set_point(r, 0, true, 3, 300, 300);
        set_point(r, 1, true, 4, 400, 300);
        run_step(r, 600);
        set_point(r, 0, false, 3, 300, 300);
        set_point(r, 1, false, 4, 400, 300);
        run_step(r, 690);
        CTM_CHECK_EQ(g_oskCount, 2);
    }

    section("touch: a tap bound to the wheel is one click of it, and not a button");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_one_finger_tap"] = "MouseWheelUp";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 100);
        CTM_CHECK_EQ(g_wheelSum, 1);
        CTM_CHECK_EQ(g_clickCount, 0);
    }

    section("touch: a name nothing knows does nothing, and so does a pad button that does not exist");
    {
        const char *nothing[] = { "banana", "button_nosuch", "button_", "x" };
        for (const char *value : nothing) {
            reset_stubs();
            fresh_device();
            g_cfg["touchpad_one_finger_tap"] = value;
            auto r = rest_report();
            set_point(r, 0, true, 1, 300, 300);
            run_step(r, 0);
            set_point(r, 0, false, 1, 300, 300);
            run_step(r, 100);
            CTM_CHECK_EQ(g_clickCount, 0);
            CTM_CHECK_EQ(g_pulseCount, 0);
            CTM_CHECK_EQ(g_oskCount, 0);
            CTM_CHECK_EQ(g_wheelSum, 0);
            CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 100), 0u);
        }
    }

    section("touch: while the pad drives the settings page, only the mouse acts");
    {
        // ⛔ A button's remaps stand down there, so a pad with Cross bound to a
        // key can still press "select". A tap that typed into the page, or
        // pressed a button into it, would be the same fault by another route.
        const char *standsDown[] = { "Enter", "button_cross", "KeyboardSteam" };
        for (const char *value : standsDown) {
            reset_stubs();
            fresh_device();
            g_configModeEffective = true;
            g_cfg["touchpad_one_finger_tap"] = value;
            auto r = rest_report();
            set_point(r, 0, true, 1, 300, 300);
            run_step(r, 0);
            set_point(r, 0, false, 1, 300, 300);
            run_step(r, 100);
            CTM_CHECK_EQ(g_pulseCount, 0);
            CTM_CHECK_EQ(g_oskCount, 0);
            CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 100), 0u);
        }
        // ⭐ The click carries on: it is how the touchpad drives the page.
        reset_stubs();
        fresh_device();
        g_configModeEffective = true;
        g_cfg["touchpad_one_finger_tap"] = "MouseLeft";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 100);
        CTM_CHECK_EQ(g_clickCount, 1);
    }

    section("touch: the pad pressed in holds a key until the last finger leaves");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_press_touch_drag"] = "KeyW";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        CTM_CHECK(g_touchKeys.find(kDev) == g_touchKeys.end());   // a touch is not a press
        r[10] = static_cast<uint8_t>(r[10] | 0x02);   // the pad pressed in
        run_step(r, 8);
        CTM_CHECK(g_touchKeys.find(kDev) != g_touchKeys.end());
        CTM_CHECK_EQ(static_cast<int>(g_touchKeys[kDev].second.size()), 1);
        CTM_CHECK_EQ(static_cast<int>(g_touchKeys[kDev].second[0]), 0x1A);
        CTM_CHECK_EQ(static_cast<int>(g_touchKeys[kDev].first), 0);
        CTM_CHECK(g_ensureKeyboardCalls > 0);
        // ⭐ THE CLICK CAN GO AND THE KEY STAYS, as a drag does.
        r[10] = static_cast<uint8_t>(r[10] & ~0x02);
        run_step(r, 40);
        CTM_CHECK(g_touchKeys.find(kDev) != g_touchKeys.end());
        // ⓘ It ends when the FINGER leaves.
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 80);
        CTM_CHECK(g_touchKeys.find(kDev) == g_touchKeys.end());
        // ⓘ No mouse button and no tapped key were involved.
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0);
        CTM_CHECK_EQ(g_pulseCount, 0);
    }

    section("touch: the pad pressed in holds a modifier the same way");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_press_touch_drag"] = "ShiftLeft";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);
        run_step(r, 0);
        CTM_CHECK(g_touchKeys.find(kDev) != g_touchKeys.end());
        CTM_CHECK_EQ(static_cast<int>(g_touchKeys[kDev].first), 0x02);
        CTM_CHECK_EQ(static_cast<int>(g_touchKeys[kDev].second.size()), 0);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 50);
        CTM_CHECK(g_touchKeys.find(kDev) == g_touchKeys.end());
    }

    section("touch: the pad pressed in holds a pad button until the last finger leaves");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_press_touch_drag"] = "button_r2";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);
        run_step(r, 0);
        const uint32_t r2 = 1u << ctm_rebind::kBtnR2;
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 0), r2);
        // ⓘ A hold, not a tap: the clock does not end it.
        r[10] = static_cast<uint8_t>(r[10] & ~0x02);
        run_step(r, 5000);
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 5000), r2);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 5008);
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 5008), 0u);
    }

    section("touch: the pad pressed in toggles an on-screen keyboard once per press");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_press_touch_drag"] = "KeyboardWindows";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);
        run_step(r, 0);
        CTM_CHECK_EQ(g_oskCount, 1);
        CTM_CHECK_EQ(g_oskProgram, static_cast<int>(binding::kOskWindows));
        // ⛔ NOT ONCE PER REPORT. Held in for a second at 250 a second it would
        // open and close the keyboard two hundred and fifty times.
        for (int i = 1; i <= 250; ++i) run_step(r, i * 4);
        CTM_CHECK_EQ(g_oskCount, 1);
        // ⓘ And not again as the click is let go with the finger still down.
        r[10] = static_cast<uint8_t>(r[10] & ~0x02);
        run_step(r, 1010);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);
        run_step(r, 1020);
        CTM_CHECK_EQ(g_oskCount, 1);
        // A new press, after every finger has left, is the next toggle.
        r[10] = static_cast<uint8_t>(r[10] & ~0x02);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 1100);
        set_point(r, 0, true, 2, 300, 300);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);
        run_step(r, 1500);
        CTM_CHECK_EQ(g_oskCount, 2);
    }

    section("touch: what the press took hold of is what it lets go, whatever the setting says by then");
    {
        // ⛔ The setting can be changed with the pad still held in. A key
        // pressed under the old value must be the key that comes back up, or
        // it stays down with nothing left that knows about it.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_press_touch_drag"] = "KeyW";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);
        run_step(r, 0);
        CTM_CHECK(g_touchKeys.find(kDev) != g_touchKeys.end());
        g_cfg["touchpad_press_touch_drag"] = "button_cross";   // changed mid-press
        run_step(r, 8);
        // ⓘ Still the key: a press is one thing from start to finish.
        CTM_CHECK(g_touchKeys.find(kDev) != g_touchKeys.end());
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 8), 0u);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 50);
        CTM_CHECK(g_touchKeys.find(kDev) == g_touchKeys.end());
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 50), 0u);
    }

    section("touch: the setting cleared mid-press lets go at once");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_press_touch_drag"] = "button_cross";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);
        run_step(r, 0);
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 0), 1u);
        g_cfg.erase("touchpad_press_touch_drag");
        run_step(r, 8);                        // finger still down, pad still in
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 8), 0u);
    }

    section("touch: a gate that shuts mid-press lets go");
    {
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_press_touch_drag"] = "KeyW";
        g_cfg["touchpad_to_mouse_gate"] = "L2";
        auto r = rest_report();
        r[5] = 200;                            // L2 pulled: the gate is open
        set_point(r, 0, true, 1, 300, 300);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);
        run_step(r, 0);
        CTM_CHECK(g_touchKeys.find(kDev) != g_touchKeys.end());
        r[5] = 0;                              // L2 let go, finger still down
        run_step(r, 8);
        CTM_CHECK(g_touchKeys.find(kDev) == g_touchKeys.end());
    }

    section("touch: a pad that goes away mid-press leaves nothing held");
    {
        const char *held[] = { "KeyW", "button_cross", "MouseLeft" };
        for (const char *value : held) {
            reset_stubs();
            fresh_device();
            g_cfg["touchpad_press_touch_drag"] = value;
            auto r = rest_report();
            set_point(r, 0, true, 1, 300, 300);
            r[10] = static_cast<uint8_t>(r[10] | 0x02);
            run_step(r, 0);
            ctm_touch_mouse::forget(kDev);     // unbridged with the pad held in
            CTM_CHECK(g_touchKeys.find(kDev) == g_touchKeys.end());
            CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 1), 0u);
            CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0);
        }
        // ⓘ And a button a tap had down goes with the pad as well.
        reset_stubs();
        fresh_device();
        g_cfg["touchpad_one_finger_tap"] = "button_cross";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        run_step(r, 0);
        set_point(r, 0, false, 1, 300, 300);
        run_step(r, 100);
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 101), 1u);
        ctm_touch_mouse::forget(kDev);
        CTM_CHECK_EQ(pad_press::shared().pressed(kDev, 102), 0u);
    }

    section("touch: while the pad drives the settings page, a press takes hold of nothing but the mouse");
    {
        reset_stubs();
        fresh_device();
        g_configModeEffective = true;
        g_cfg["touchpad_press_touch_drag"] = "KeyW";
        auto r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);
        run_step(r, 0);
        CTM_CHECK(g_touchKeys.find(kDev) == g_touchKeys.end());
        // ⭐ A drag still works there.
        reset_stubs();
        fresh_device();
        g_configModeEffective = true;
        g_cfg["touchpad_press_touch_drag"] = "MouseLeft";
        r = rest_report();
        set_point(r, 0, true, 1, 300, 300);
        r[10] = static_cast<uint8_t>(r[10] | 0x02);
        run_step(r, 0);
        CTM_CHECK_EQ(static_cast<int>(g_dragMask), 0x01);
    }

    return 0;
}
