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

#include <cmath>

// ⓘ Every pad's layout, which gyro_mouse.inl reads motion and gates through.
#include "input/button_layout.inl"

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
    return "cfg:" + linkedConfig;
}

// ⭐ The resolver gyro_mouse.inl asks for a pad's settings kind AND layout,
// mirroring the real one in ds5_output_overrides.inl: Sony 0ce6, 0df2 and
// 09cc/05c4 are a DualSense, an Edge and a DS4.
//
// ⚠️ The test binary is its own translation unit, so it sees nothing that
// main.cpp includes. Anything the real code gains has to be stubbed here too.
struct InputPad {
    const char *kind = nullptr;
    const ctm_rebind::Layout *layout = nullptr;
};

static InputPad device_input_pad_for(const std::vector<unsigned char> &d)
{
    InputPad pad;
    if (d.size() < 12) return pad;
    const uint16_t v = static_cast<uint16_t>(d[8] | (d[9] << 8));
    const uint16_t p = static_cast<uint16_t>(d[10] | (d[11] << 8));
    if (v != 0x054c) return pad;
    const char *kind = nullptr;
    if (p == 0x0ce6) kind = "ds5";
    else if (p == 0x0df2) kind = "ds5_edge";
    else if (p == 0x09cc || p == 0x05c4) kind = "ds4";
    pad.layout = ctm_rebind::layout_for(kind);
    if (pad.layout != nullptr) pad.kind = kind;
    return pad;
}

// ⭐ The calibration half that gyro_mouse.inl READS. Not the fetch half -- that
// needs a backend, which this harness has no business knowing about. Without
// this the scale type is undefined and nothing below compiles.
//
// ⚠️ This is the second time an include added to main.cpp was not added here.
// The test binary assembles its own translation unit, so main.cpp's include
// list is not a substitute for this one.
// ⭐ A device_log stub, matching how this test stubs everything else.
//
// Each test file is its own translation unit, so the real header being included
// by another one does not help here. And including it would pull in a file
// mutex, an output stream and a log file -- none of which belongs in a unit
// test, and it would print over the test results.
//
// ⓘ Only the tags gyro_mouse.inl actually uses. Adding one it does not use
// would be dead code that quietly rots.
namespace device_log {
struct sink {
    template <typename T> sink &operator<<(const T &) { return *this; }
    sink &operator<<(std::ostream &(*)(std::ostream &)) { return *this; }
};
inline sink input_s() { return sink(); }
}  // namespace device_log

// Config-mode stand-in: the real one lives in rebind.inl, which this binary
// does not compile. Tests run with the gate permanently off.
static bool ctm_rebind_config_mode_effective() { return false; }

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

    section("gyro-mouse: the steady is no longer a gate value");
    {
        // ⛔ It used to be one, which made the two mutually exclusive: choosing
        // L2 as the gate silently gave up the steady. It is a suppression on
        // top of whichever gate was chosen now.
        //
        // ⚠️ THE SUPPRESSION ITSELF IS NOT ASSERTED HERE, deliberately.
        // trigger_click_test.cpp defines its own ctm_gyro_mouse::set_gyro_hold
        // stub, this file links the real one, and both are inline with the same
        // signature -- so the linker picks one for the whole binary. Calling it
        // from here made trigger_click's own assertions start failing, because
        // its writes went to the real map while its reads came from the stub.
        // ⓘ The behaviour is covered end to end over there, where held_for()
        // reads whatever set_gyro_hold actually wrote.
        auto r = make_report(0, 0, /*l2*/ 40);
        // No hold in play, so every gate answers on its own terms.
        CTM_CHECK(gate_open(Gate::L2, r.data(), r.size()));
        CTM_CHECK(gate_open(Gate::Always, r.data(), r.size()));
        // ⓘ And the old value is now exactly Always rather than a special case.
        CTM_CHECK(gate_open(Gate::TriggerHold, r.data(), r.size()));
    }

    section("gyro-mouse: \"trigger\" still parses, as always");
    // ⓘ Configs written before 2026-09-11 carry it; it was never a gate, and
    // now it is spelled what it always meant.
    CTM_CHECK(parse_gate("trigger") == Gate::Always);

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

    // ---- DualShock 4: the same sensor, three bytes earlier ----------------------

    // A DS4 USB report: gyro pitch/yaw/roll at [13] [15] [17], accel at [19] [21]
    // [23], hat centred, both fingers up with the packet count at [33].
    auto ds4_report = [](int16_t yaw, int16_t pitch) {
        std::vector<uint8_t> d(64, 0);
        d[0] = 0x01;
        d[1] = d[2] = d[3] = d[4] = 0x80;
        d[5] = 0x08;
        d[7] = 0xe4;                                  // counter bits, PS and press clear
        d[10] = 0xff;                                 // a timestamp byte, all bits set
        d[13] = static_cast<uint8_t>(pitch & 0xff);
        d[14] = static_cast<uint8_t>((pitch >> 8) & 0xff);
        d[15] = static_cast<uint8_t>(yaw & 0xff);
        d[16] = static_cast<uint8_t>((yaw >> 8) & 0xff);
        const int16_t az = 8192;                      // 1 g, so the filter has a gravity vector
        d[23] = static_cast<uint8_t>(az & 0xff);
        d[24] = static_cast<uint8_t>((az >> 8) & 0xff);
        d[33] = 0x01;                                 // ONE touch packet -- not a finger
        d[35] = 0x80;                                 // finger 1 up
        d[39] = 0x80;                                 // finger 2 up
        return d;
    };

    section("gyro-mouse: a DS4 moves the cursor from its own motion bytes");
    {
        g_gate = "always";
        g_sens = 0;
        g_player = false;
        GyroMouse gm;
        MouseDelta out{};
        bool moved = false;
        for (int i = 0; i < 300 && !moved; ++i) {
            auto r = ds4_report(6000, 0);
            if (gm.on_report(ctm_rebind::kDs4Layout, r.data(), r.size(), "ds4", &out)) {
                moved = (out.dx != 0 || out.dy != 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CTM_CHECK(moved);
    }

    section("gyro-mouse: a DS4's PS gate is [7], not a timestamp byte");
    {
        // ⛔⛔ THE FAULT, pinned. The gyro preset's recenter button is a gate; at
        // DualSense offsets PS is [10] 0x01, which on a DS4 is a timestamp that
        // changes on most reports -- so recenter fired over and over.
        auto r = ds4_report(0, 0);
        CTM_CHECK(!gate_open(Gate::PS, ctm_rebind::kDs4Layout, r.data(), r.size()));
        CTM_CHECK(gate_open(Gate::PS, ctm_rebind::kDs5Layout, r.data(), r.size()));
        r[7] = static_cast<uint8_t>(r[7] | 0x01);
        CTM_CHECK(gate_open(Gate::PS, ctm_rebind::kDs4Layout, r.data(), r.size()));
    }

    section("gyro-mouse: a DS4's touchpad gates read its fingers, not the packet count");
    {
        auto r = ds4_report(0, 0);
        CTM_CHECK(!gate_open(Gate::Touchpad, ctm_rebind::kDs4Layout, r.data(), r.size()));
        CTM_CHECK(gate_open(Gate::NotTouchpad, ctm_rebind::kDs4Layout, r.data(), r.size()));
        // At DualSense offsets the count 0x01 reads as a finger that never lifts.
        CTM_CHECK(gate_open(Gate::Touchpad, ctm_rebind::kDs5Layout, r.data(), r.size()));
        r[35] = 0x05;                                 // a real finger down
        CTM_CHECK(gate_open(Gate::Touchpad, ctm_rebind::kDs4Layout, r.data(), r.size()));
        r[7] = static_cast<uint8_t>(r[7] | 0x02);     // and the pad pressed in
        CTM_CHECK(gate_open(Gate::TouchpadClick, ctm_rebind::kDs4Layout, r.data(), r.size()));
    }

    section("gyro-mouse: \"move unless a finger is down\" is open on a pad with no touchpad");
    {
        // ⛔ Found in review, 2026-09-15: touch_finger_up() rightly answers false
        // for a pad with no touchpad, which shut this gate forever on an Xbox pad
        // -- a stick mouse gated on !touchpad never moved. No finger can be down
        // on a pad with no touchpad, so the gate is open there.
        std::vector<uint8_t> xbox(48, 0);
        xbox[0] = 0x20;
        CTM_CHECK(gate_open(Gate::NotTouchpad, ctm_rebind::kXboxLayout, xbox.data(), xbox.size()));
        // ⓘ The gates that need a touchpad to open stay shut on one without.
        CTM_CHECK(!gate_open(Gate::Touchpad, ctm_rebind::kXboxLayout, xbox.data(), xbox.size()));
        CTM_CHECK(!gate_open(Gate::TouchpadClick, ctm_rebind::kXboxLayout, xbox.data(), xbox.size()));
        // ⓘ And a pad WITH a touchpad still pauses for a finger.
        auto r = ds4_report(0, 0);
        r[35] = 0x05;
        CTM_CHECK(!gate_open(Gate::NotTouchpad, ctm_rebind::kDs4Layout, r.data(), r.size()));
    }

    section("gyro-mouse: a DS4's L2 gate is [8], not a face button");
    {
        auto r = ds4_report(0, 0);
        r[5] = 0x28;                                  // cross held, hat centred
        CTM_CHECK(!gate_open(Gate::L2, ctm_rebind::kDs4Layout, r.data(), r.size()));
        CTM_CHECK(gate_open(Gate::L2, ctm_rebind::kDs5Layout, r.data(), r.size()));   // the fault
        r[8] = 40;
        CTM_CHECK(gate_open(Gate::L2, ctm_rebind::kDs4Layout, r.data(), r.size()));
        r[6] = 0x01;                                  // L1
        CTM_CHECK(gate_open(Gate::L1, ctm_rebind::kDs4Layout, r.data(), r.size()));
    }

    // ---- Calibration: three reports, two field orders ---------------------------

    // Distinct spans per axis, so a report read in the wrong order gives a
    // DIFFERENT scale instead of the same one by symmetry.
    auto calib = [](uint8_t id, bool grouped) {
        std::vector<uint8_t> d(41, 0);
        d[0] = id;
        auto put = [&d](size_t off, int16_t v) {
            d[off] = static_cast<uint8_t>(v & 0xff);
            d[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
        };
        // biases 0
        if (!grouped) {
            put(7, 8000);  put(9, -8000);             // pitch plus, minus
            put(11, 7000); put(13, -7000);            // yaw
            put(15, 6000); put(17, -6000);            // roll
        } else {
            put(7, 8000);  put(9, 7000);  put(11, 6000);    // every plus first
            put(13, -8000); put(15, -7000); put(17, -6000); // then every minus
        }
        put(19, 540); put(21, 540);                   // speed plus, minus
        return d;
    };
    auto closeTo = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
    const float wantPitch = 1080.0f / 16000.0f;
    const float wantYaw   = 1080.0f / 14000.0f;
    const float wantRoll  = 1080.0f / 12000.0f;

    section("gyro calibration: the DualSense's report reads as it always did");
    {
        const auto d = calib(0x05, false);
        ctm_gyro_calib::Scale s;
        CTM_CHECK(ctm_gyro_calib::parse(d.data(), d.size(), &s));
        CTM_CHECK(closeTo(s.pitch, wantPitch) && closeTo(s.yaw, wantYaw) && closeTo(s.roll, wantRoll));
    }

    section("gyro calibration: a cabled DS4's report is 0x02 in the same order");
    {
        const auto d = calib(0x02, false);
        ctm_gyro_calib::Scale s;
        CTM_CHECK(ctm_gyro_calib::parse(d.data(), d.size(), ctm_gyro_calib::kDs4UsbCalibration, &s));
        CTM_CHECK(closeTo(s.pitch, wantPitch) && closeTo(s.yaw, wantYaw) && closeTo(s.roll, wantRoll));
        // ⛔ And the DualSense's parser refuses it by id rather than misreading it.
        ctm_gyro_calib::Scale refused;
        CTM_CHECK(!ctm_gyro_calib::parse(d.data(), d.size(), &refused));
    }

    section("gyro calibration: a Bluetooth DS4 groups the plus values first");
    {
        const auto d = calib(0x05, true);
        ctm_gyro_calib::Scale s;
        CTM_CHECK(ctm_gyro_calib::parse(d.data(), d.size(), ctm_gyro_calib::kDs4BtCalibration, &s));
        CTM_CHECK(closeTo(s.pitch, wantPitch) && closeTo(s.yaw, wantYaw) && closeTo(s.roll, wantRoll));
        // ⚠️ THE ORDER MATTERS: the same bytes read as paired give another pitch.
        // ⓘ Pitch, not yaw: with these spans the misread yaw happens to land on
        // the same 14000, so only pitch and roll can show the difference.
        ctm_gyro_calib::Scale paired;
        ctm_gyro_calib::parse(d.data(), d.size(), ctm_gyro_calib::kDs5Calibration, &paired);
        CTM_CHECK(!closeTo(paired.pitch, wantPitch));
    }

    return 0;
}
