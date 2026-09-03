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
    return "cfg:" + linkedConfig + "/" + kind;
}
static const char *device_section_for(const std::vector<unsigned char> &)
{
    return "ds5";
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

inline Gate parse_gate(const std::string &raw)
{
    if (raw == "always") return Gate::Always;
    if (raw == "L2" || raw == "l2") return Gate::L2;
    return Gate::Off;
}

inline bool gate_open(Gate gate, const uint8_t *d, size_t len)
{
    switch (gate) {
        case Gate::Always: return true;
        case Gate::L2: return len > 5 && d[5] >= 30;
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
inline void set_drag(uint8_t mask) { g_dragMask = mask; }
}
} // namespace ctm_mouse_device

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

    return 0;
}
