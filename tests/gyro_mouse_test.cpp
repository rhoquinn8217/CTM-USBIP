// Tests for gyro-to-mouse. Pure input->output on the gate parser, the gate
// evaluation, the mailbox clamp/remainder, and the end-to-end "gate off emits
// nothing / gate open with motion eventually emits" behaviour.
//
// WHAT THESE CANNOT DO. They cannot confirm the FEEL is right, the sensitivity
// divisor is good, or that the byte offsets match a real DS5 report -- those
// are hardware questions. They protect the logic: gate off is inert, unknown
// config is off not an error, the sub-pixel remainder is never lost, and fast
// movement clamps instead of wrapping.

#include "harness.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// gyro_mouse.inl relies on its includer for these -- main.cpp has them, so the
// product build is fine. A test translation unit has to bring its own:
//   <iostream>  the gate diagnostic's std::cout
//   <map>       the per-device motion registry
//   <memory>    std::unique_ptr in that registry
#include <iostream>
#include <map>
#include <memory>

// gyro_mouse.inl's cursor-recentre helper calls Win32 (GetSystemMetrics,
// SetCursorPos). main.cpp already has windows.h in scope; the test binary
// needs it explicitly.
#include <windows.h>

using namespace ctmtest;

namespace {

// Config stubs standing in for device_config_*. The real accessors read a file;
// here the tests set the values directly.
std::string g_gate;
int g_sens = 0;          // legacy multiplier; 0 = unset
int g_invert = 0;
bool g_player = true;
int g_px360 = 1920;

} // namespace

// These must be visible to gyro_mouse.inl at the names it calls. It is included
// into an anonymous namespace in main.cpp; in the test binary we give it the
// same free functions at file scope.
static std::string device_config_str(const char *, const char *key)
{
    if (std::string(key) == "gyro_to_mouse_gate") return g_gate;
    if (std::string(key) == "gyro_mouse_recenter_button") return "";
    return "";
}
static int device_config_int(const char *, const char *key, int fallback)
{
    const std::string k(key);
    if (k == "gyro_mouse_sens") return g_sens;
    if (k == "gyro_mouse_invert") return g_invert;
    if (k == "gyro_mouse_px_per_360") return g_px360;
    return fallback;
}
static bool device_config_bool(const char *, const char *key, bool fallback)
{
    if (std::string(key) == "gyro_mouse_player_space") return g_player;
    return fallback;
}
// Mirrors the real resolver: shared section unless a config is linked.
static std::string device_settings_section(const char *kind, const std::string &linkedConfig)
{
    if (kind == nullptr) return std::string();
    if (linkedConfig.empty()) return std::string(kind);
    return "cfg:" + linkedConfig + "/" + kind;
}

static const char *device_section_for(const std::vector<unsigned char> &d)
{
    if (d.size() < 12) return nullptr;
    const uint16_t v = static_cast<uint16_t>(d[8] | (d[9] << 8));
    const uint16_t p = static_cast<uint16_t>(d[10] | (d[11] << 8));
    if (v != 0x054c) return nullptr;
    if (p == 0x0ce6) return "ds5";
    if (p == 0x0df2) return "ds5_edge";
    return nullptr;
}

// ⭐ The calibration half that gyro_mouse.inl READS. Not the fetch half -- that
// needs a backend, which this harness has no business knowing about. Without
// this the scale type is undefined and nothing below compiles.
//
// ⚠️ This is the second time an include added to main.cpp was not added here.
// The test binary assembles its own translation unit, so main.cpp's include
// list is not a substitute for this one.
#include "input/gyro_calibration.inl"
#include "input/gyro_mouse.inl"

using namespace ctm_gyro_mouse;

namespace {

std::vector<uint8_t> make_report(int16_t yaw, int16_t pitch, uint8_t l2 = 0)
{
    std::vector<uint8_t> d(64, 0);
    d[0] = 0x01;
    d[5] = l2;
    d[16] = static_cast<uint8_t>(pitch & 0xff);
    d[17] = static_cast<uint8_t>((pitch >> 8) & 0xff);
    d[18] = static_cast<uint8_t>(yaw & 0xff);
    d[19] = static_cast<uint8_t>((yaw >> 8) & 0xff);
    const int16_t az = 8192; // 1g down, gives the filter a gravity vector
    d[26] = static_cast<uint8_t>(az & 0xff);
    d[27] = static_cast<uint8_t>((az >> 8) & 0xff);
    return d;
}

} // namespace

int run_gyro_mouse_tests()
{
    section("gyro-mouse: gate parsing");
    CTM_CHECK(parse_gate("L2") == Gate::L2);
    CTM_CHECK(parse_gate("l2") == Gate::L2);
    CTM_CHECK(parse_gate("always") == Gate::Always);
    CTM_CHECK(parse_gate("!touchpad") == Gate::NotTouchpad);
    CTM_CHECK(parse_gate("touchpad_click") == Gate::TouchpadClick);
    CTM_CHECK(parse_gate("PS") == Gate::PS);
    CTM_CHECK(parse_gate("garbage") == Gate::Off);   // unknown -> off, never error
    CTM_CHECK(parse_gate("") == Gate::Off);

    section("gyro-mouse: gate evaluation");
    {
        auto r = make_report(0, 0, /*l2*/ 40);
        CTM_CHECK(gate_open(Gate::L2, r.data(), r.size()));
        CTM_CHECK(gate_open(Gate::Always, r.data(), r.size()));
        CTM_CHECK(!gate_open(Gate::Off, r.data(), r.size()));
        r[5] = 10;                                    // below ~12% threshold
        CTM_CHECK(!gate_open(Gate::L2, r.data(), r.size()));
    }

    section("gyro-mouse: gate off is inert");
    {
        g_gate = "";
        GyroMouse gm;
        MouseDelta out{};
        auto r = make_report(6000, 0);
        CTM_CHECK(!gm.on_report(r.data(), r.size(), "ds5", &out));
    }

    section("gyro-mouse: always-on motion eventually moves");
    {
        g_gate = "always";
        g_sens = 0;                                   // use the shipped defaults
        g_player = false;                             // simpler calibrated path
        GyroMouse gm;
        MouseDelta out{};
        bool moved = false;
        for (int i = 0; i < 300 && !moved; ++i) {
            auto r = make_report(6000, 0);
            if (gm.on_report(r.data(), r.size(), "ds5", &out)) {
                moved = (out.dx != 0 || out.dy != 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CTM_CHECK(moved);
    }

    section("gyro-mouse: calibration defaults produce sane cursor speed");
    {
        // Protects the Real World Calibration maths, not the feel. A steady
        // fast turn must move the cursor a plausible distance -- not zero
        // (the pre-2026-08-16 bug, where deltaTime was omitted and the result
        // was both wrong and report-rate dependent) and not absurdly far.
        g_gate = "always";
        g_sens = 0;
        g_px360 = 1920;
        g_player = false;
        GyroMouse gm;
        MouseDelta out{};
        long total = 0;
        for (int i = 0; i < 400; ++i) {
            auto r = make_report(6000, 0);            // ~5.9 deg/sec steady yaw
            if (gm.on_report(r.data(), r.size(), "ds5", &out)) {
                total += (out.dx < 0 ? -out.dx : out.dx);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CTM_CHECK(total > 0);                          // moved at all
        CTM_CHECK(total < 100000);                     // did not fly off
    }

    section("gyro-mouse: mailbox clamps and keeps the remainder");
    {
        MouseMailbox mb;
        mb.push({200, -200});
        int8_t dx = 0, dy = 0;
        CTM_CHECK(mb.drain(&dx, &dy));
        CTM_CHECK_EQ(static_cast<int>(dx), 127);
        CTM_CHECK_EQ(static_cast<int>(dy), -127);
        int8_t dx2 = 0, dy2 = 0;
        CTM_CHECK(mb.drain(&dx2, &dy2));              // overflow carried
        CTM_CHECK_EQ(static_cast<int>(dx2), 73);
        CTM_CHECK_EQ(static_cast<int>(dy2), -73);
        int8_t dx3 = 0, dy3 = 0;
        CTM_CHECK(!mb.drain(&dx3, &dy3));             // now empty
    }

    return 0;
}
