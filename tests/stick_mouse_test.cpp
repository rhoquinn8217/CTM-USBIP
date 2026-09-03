// Stick-to-mouse tests: deadzone and rescaling, response curves, time-based
// speed, the gate, and the off-control.
//
// Time is a parameter to step(), so the rate behaviour is tested directly
// rather than raced against a real clock -- which is the whole point of the
// module: a stick reports a held position, so what it produces is speed x time.

#include "harness.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace ctmtest;

namespace {

std::map<std::string, std::string> g_cfg;
bool g_configModeEffective = false;
int32_t g_pushedX = 0;
int32_t g_pushedY = 0;
int g_pushCount = 0;
int g_wheelSum = 0;

void reset_stubs()
{
    g_wheelSum = 0;
    g_cfg.clear();
    g_configModeEffective = false;
    g_pushedX = 0;
    g_pushedY = 0;
    g_pushCount = 0;
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
static std::string device_settings_section(const char *kind, const std::string &linkedConfig)
{
    if (kind == nullptr) return std::string();
    if (linkedConfig.empty()) return std::string(kind);
    return "cfg:" + linkedConfig + "/" + kind;
}
static const char *device_section_for(const std::vector<unsigned char> &) { return "ds5"; }
static bool device_config_bool(const char *, const char *key, bool fallback)
{
    auto it = g_cfg.find(key);
    if (it == g_cfg.end()) return fallback;
    return it->second == "true";
}
static bool ctm_rebind_config_mode_effective() { return g_configModeEffective; }
static void ctm_gyro_mouse_ensure_mouse_started() {}

// The gate machinery the stick shares with gyro. Mirrors the real enum and the
// two evaluations the tests use; the full parser lives in gyro_mouse.inl and is
// covered by its own suite.
// ⛔ THE STUBS BELOW LIVE IN AN UNNAMED NAMESPACE, and that is load-bearing.
// Each test file defines its own stand-in ctm_gyro_mouse::shared_mailbox() and
// friends. As plain inline functions those have EXTERNAL linkage with identical
// signatures across files -- a one-definition-rule violation -- so the linker
// keeps one and every suite silently shares it. On 2026-08-31 that sent the
// stick suite's movement into the touch suite's counters, and eight stick
// checks failed reporting no movement at all while the code was correct.
// Nesting in an unnamed namespace gives them internal linkage; qualified names
// still resolve inside this file, and nothing can be folded across files.
namespace ctm_mouse_device {
namespace {   // internal linkage, same reason as the note above
inline void add_wheel(int ticks) { g_wheelSum += ticks; }
}
} // namespace ctm_mouse_device

namespace ctm_gyro_mouse {
namespace {

enum class Gate { Off, Always, L2, R2, L1, R1, Touchpad, NotTouchpad, TouchpadClick, PS };

inline Gate parse_gate(const std::string &raw)
{
    if (raw == "always") return Gate::Always;
    if (raw == "L2" || raw == "l2") return Gate::L2;
    if (raw == "R2" || raw == "r2") return Gate::R2;
    return Gate::Off;
}

inline bool gate_open(Gate gate, const uint8_t *d, size_t len)
{
    switch (gate) {
        case Gate::Always: return true;
        case Gate::L2: return len > 5 && d[5] >= 30;
        case Gate::R2: return len > 6 && d[6] >= 30;
        default: return false;
    }
}

struct MouseDelta { int32_t dx = 0; int32_t dy = 0; };

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

#include "input/stick_mouse.inl"

namespace {

std::vector<uint8_t> rest_report()
{
    std::vector<uint8_t> r(64, 0);
    r[0] = 0x01;
    r[1] = 128; r[2] = 128;       // left stick centred
    r[3] = 128; r[4] = 128;       // right stick centred
    r[8] = 0x08;
    r[33] = 0x80;
    r[37] = 0x80;
    return r;
}

const void *kDev = reinterpret_cast<const void *>(0x3);

void run_step(std::vector<uint8_t> &r, long long nowMs)
{
    ctm_stick_mouse::step(kDev, "ds5", r.data(), r.size(), nowMs);
}

void fresh_device() { ctm_stick_mouse::forget(kDev); }

// ⚠️ Scroll must be driven at a REPORT CADENCE, not in one long jump: a single
// step of 500ms is clamped to 50ms by design (the stall guard), so a test that
// leaps produces one twentieth of the travel and looks like a broken feature.
void scroll_for(std::vector<uint8_t> &r, long long ms, long long everyMs = 8)
{
    for (long long at = 0; at <= ms; at += everyMs) {
        ctm_stick_mouse::scroll_step(kDev, "ds5", r.data(), r.size(), at);
    }
}

} // namespace

int run_stick_mouse_tests()
{
    section("stick: nothing configured moves nothing");
    {
        reset_stubs();
        fresh_device();
        auto r = rest_report();
        r[3] = 255;
        run_step(r, 0);
        run_step(r, 100);
        CTM_CHECK_EQ(g_pushCount, 0);
    }

    section("stick: a centred stick produces no movement");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "right";
        auto r = rest_report();
        run_step(r, 0);
        run_step(r, 100);
        run_step(r, 200);
        CTM_CHECK_EQ(g_pushCount, 0);
    }

    section("stick: inside the deadzone is still nothing");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "right";
        g_cfg["stick_mouse_deadzone"] = "20";
        auto r = rest_report();
        r[3] = 148;                        // ~16% right, inside 20%
        run_step(r, 0);
        run_step(r, 100);
        CTM_CHECK_EQ(g_pushCount, 0);
    }

    section("stick: full deflection moves at the configured speed");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "right";
        g_cfg["stick_mouse_speed"] = "1000";
        g_cfg["stick_mouse_curve"] = "linear";
        auto r = rest_report();
        r[3] = 255;                        // hard right
        run_step(r, 0);                    // first report only sets the clock
        run_step(r, 50);                   // 50ms at 1000px/s = ~50px
        CTM_CHECK(g_pushedX >= 48 && g_pushedX <= 52);
        CTM_CHECK_EQ(g_pushedY, 0);
    }

    section("stick: speed is time-based, not per-report");
    {
        // ⭐ The property that matters: the same held stick over the same
        // elapsed time produces the same travel however many reports arrive.
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "right";
        g_cfg["stick_mouse_speed"] = "1000";
        g_cfg["stick_mouse_curve"] = "linear";
        auto r = rest_report();
        r[3] = 255;
        run_step(r, 0);
        for (int ms = 4; ms <= 100; ms += 4) run_step(r, ms);   // 25 reports
        const int32_t many = g_pushedX;

        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "right";
        g_cfg["stick_mouse_speed"] = "1000";
        g_cfg["stick_mouse_curve"] = "linear";
        auto r2 = rest_report();
        r2[3] = 255;
        run_step(r2, 0);
        for (int ms = 20; ms <= 100; ms += 20) run_step(r2, ms);  // 5 reports
        const int32_t few = g_pushedX;

        CTM_CHECK(many >= few - 2 && many <= few + 2);
    }

    section("stick: a long stall is clamped, not banked");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "right";
        g_cfg["stick_mouse_speed"] = "1000";
        g_cfg["stick_mouse_curve"] = "linear";
        auto r = rest_report();
        r[3] = 255;
        run_step(r, 0);
        run_step(r, 10000);                // ten seconds of nothing
        CTM_CHECK(g_pushedX <= 55);        // one 50ms step, not 10000px
    }

    section("stick: quadratic is gentler than linear at half push");
    {
        auto travel = [](const char *curve) {
            reset_stubs();
            fresh_device();
            g_cfg["stick_to_mouse"] = "right";
            g_cfg["stick_mouse_speed"] = "2000";
            g_cfg["stick_mouse_curve"] = curve;
            g_cfg["stick_mouse_deadzone"] = "0";
            auto r = rest_report();
            r[3] = 192;                    // ~half right
            run_step(r, 0);
            run_step(r, 50);
            return g_pushedX;
        };
        const int32_t lin = travel("linear");
        const int32_t quad = travel("quadratic");
        const int32_t cube = travel("cubic");
        CTM_CHECK(quad < lin);
        CTM_CHECK(cube < quad);
        CTM_CHECK(cube > 0);               // still moves -- gentler, not dead
    }

    section("stick: the left stick can drive it instead");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "left";
        g_cfg["stick_mouse_speed"] = "1000";
        g_cfg["stick_mouse_curve"] = "linear";
        auto r = rest_report();
        r[1] = 255;                        // left stick hard right
        r[3] = 128;                        // right stick centred
        run_step(r, 0);
        run_step(r, 50);
        CTM_CHECK(g_pushedX > 40);
    }

    section("stick: invert flips the axes");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "right";
        g_cfg["stick_mouse_speed"] = "1000";
        g_cfg["stick_mouse_curve"] = "linear";
        g_cfg["stick_mouse_invert"] = "1";
        auto r = rest_report();
        r[3] = 255;
        run_step(r, 0);
        run_step(r, 50);
        CTM_CHECK(g_pushedX < 0);
    }

    section("stick: a shut gate holds it still, opening does not lurch");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "right";
        g_cfg["stick_to_mouse_gate"] = "L2";
        g_cfg["stick_mouse_speed"] = "1000";
        g_cfg["stick_mouse_curve"] = "linear";
        auto r = rest_report();
        r[3] = 255;                        // held hard right throughout
        run_step(r, 0);
        run_step(r, 500);                  // half a second, gate shut
        CTM_CHECK_EQ(g_pushCount, 0);
        r[5] = 200;                        // L2 pulled
        run_step(r, 520);                  // 20ms of open gate
        CTM_CHECK(g_pushedX > 0);
        CTM_CHECK(g_pushedX <= 25);        // 20ms of travel, not 520ms
    }

    section("stick: an absent gate means always");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "right";
        g_cfg["stick_mouse_speed"] = "1000";
        g_cfg["stick_mouse_curve"] = "linear";
        auto r = rest_report();
        r[3] = 255;
        run_step(r, 0);
        run_step(r, 50);
        CTM_CHECK(g_pushedX > 0);
    }

    section("stick: both means whichever stick is pushed further");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "both";
        g_cfg["stick_mouse_speed"] = "1000";
        g_cfg["stick_mouse_curve"] = "linear";

        // Left pushed hard right, right stick barely off centre: the left wins.
        auto r = rest_report();
        r[1] = 255;
        r[3] = 140;
        run_step(r, 0);
        run_step(r, 50);
        const int32_t leftWins = g_pushedX;
        CTM_CHECK(leftWins > 40);

        // Now the right stick is pushed further, and it takes over.
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "both";
        g_cfg["stick_mouse_speed"] = "1000";
        g_cfg["stick_mouse_curve"] = "linear";
        auto r2 = rest_report();
        r2[1] = 140;
        r2[2] = 255;      // left barely right, right stick hard DOWN
        r2[3] = 128;
        r2[4] = 255;
        run_step(r2, 0);
        run_step(r2, 50);
        CTM_CHECK(g_pushedY > 40);
    }

    section("stick: opposite pushes do not cancel under both");
    {
        // \u2b50 The reason for further-wins over summing: opposite pushes would
        // sum to a dead cursor, which reads as the feature being broken.
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "both";
        g_cfg["stick_mouse_speed"] = "1000";
        g_cfg["stick_mouse_curve"] = "linear";
        auto r = rest_report();
        r[1] = 0;         // left hard LEFT
        r[3] = 255;       // right hard RIGHT
        run_step(r, 0);
        run_step(r, 50);
        CTM_CHECK(g_pushedX != 0);
    }

    section("stick scroll: nothing configured scrolls nothing");
    {
        reset_stubs();
        fresh_device();
        auto r = rest_report();
        r[2] = 0;                       // left stick hard up
        scroll_for(r, 500);
        CTM_CHECK_EQ(g_wheelSum, 0);
    }

    section("stick scroll: pushing up scrolls up, at the configured rate");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_scroll"] = "left";
        g_cfg["stick_scroll_speed"] = "10";        // 10 clicks a second
        g_cfg["stick_scroll_deadzone"] = "0";
        auto r = rest_report();
        r[2] = 0;                                  // hard up
        scroll_for(r, 500);
        CTM_CHECK(g_wheelSum >= 4 && g_wheelSum <= 6);
    }

    section("stick scroll: pushing down scrolls the other way");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_scroll"] = "left";
        g_cfg["stick_scroll_speed"] = "10";
        g_cfg["stick_scroll_deadzone"] = "0";
        auto r = rest_report();
        r[2] = 255;                                // hard down
        scroll_for(r, 500);
        CTM_CHECK(g_wheelSum <= -4);
    }

    section("stick scroll: a resting stick and the deadzone produce nothing");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_scroll"] = "left";
        g_cfg["stick_scroll_speed"] = "20";
        auto r = rest_report();                    // centred
        scroll_for(r, 500);
        CTM_CHECK_EQ(g_wheelSum, 0);

        r[2] = 110;                                // ~14% up, inside 25%
        ctm_stick_mouse::scroll_step(kDev, "ds5", r.data(), r.size(), 1000);
        CTM_CHECK_EQ(g_wheelSum, 0);
    }

    section("stick scroll: natural inverts, and a gate can hold it off");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_scroll"] = "left";
        g_cfg["stick_scroll_speed"] = "10";
        g_cfg["stick_scroll_deadzone"] = "0";
        g_cfg["stick_scroll_natural"] = "true";
        auto r = rest_report();
        r[2] = 0;                                  // hard up
        scroll_for(r, 500);
        CTM_CHECK(g_wheelSum <= -4);      // inverted

        reset_stubs();
        fresh_device();
        g_cfg["stick_to_scroll"] = "left";
        g_cfg["stick_to_scroll_gate"] = "L2";
        g_cfg["stick_scroll_speed"] = "10";
        g_cfg["stick_scroll_deadzone"] = "0";
        auto r2 = rest_report();
        r2[2] = 0;
        scroll_for(r2, 500);
        CTM_CHECK_EQ(g_wheelSum, 0);      // gate shut
    }

    section("stick scroll: the cursor stick and the scroll stick are separate");
    {
        // ⭐ The pairing the presets need: right stick moves the cursor, left
        // stick scrolls, neither disturbing the other.
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "right";
        g_cfg["stick_mouse_speed"] = "1000";
        g_cfg["stick_mouse_curve"] = "linear";
        g_cfg["stick_to_scroll"] = "left";
        g_cfg["stick_scroll_speed"] = "10";
        g_cfg["stick_scroll_deadzone"] = "0";
        auto r = rest_report();
        r[3] = 255;                                // right stick hard right
        r[2] = 0;                                  // left stick hard up
        for (long long at = 0; at <= 500; at += 8) {
            run_step(r, at);
            ctm_stick_mouse::scroll_step(kDev, "ds5", r.data(), r.size(), at);
        }
        CTM_CHECK(g_pushedX > 0);                              // cursor moved
        CTM_CHECK(g_wheelSum >= 4);          // and it scrolled
    }

    // ⭐ THE RULE CHANGED HERE (rhoquinn8217, 2026-09-02). The gate used to
    // stand the stick cursor down, which over-gated: pointer movement goes to
    // whatever has focus, and while the gate applies that is our own settings
    // window -- so it could never have reached a game.
    section("stick: the cursor still works while the gate is on");
    {
        reset_stubs();
        fresh_device();
        g_cfg["stick_to_mouse"] = "right";
        g_configModeEffective = true;
        auto r = rest_report();
        r[3] = 255;
        run_step(r, 0);
        run_step(r, 100);
        CTM_CHECK(g_pushCount > 0);
    }

    return 0;
}
