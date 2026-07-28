// Pins the upstream map defaults that the wired ISO passthrough path depends
// on. See process_iso_output_wired() in src/usbip/device.inl.
//
// The wired path bypasses the audio reservoir entirely, so it is only correct
// while these two settings stay inert:
//   ack_min_fill_ms  = 0     -> the fill gate in server.inl can never fire
//   underrun_silence = false -> the keep-alive silence lane stays inactive
//
// Neither is set by the wired map. If an upstream merge changes either default,
// or adds either setting to that map, the wired path changes behaviour with no
// error and no build failure. Nothing but this test detects that.

#include "harness.h"
#include "ctm/map/runtime.h"

#include <fstream>
#include <string>

using namespace ctmtest;

static std::wstring widen(const std::string &s) { return std::wstring(s.begin(), s.end()); }

int main(int argc, char **argv)
{
    const std::string mapPath =
        (argc > 1) ? argv[1] : "maps/ds5_usb_over_ds5_usb.map";

    // --- Test 1: the assumption the wired path actually relies on ----------
    section("wired map leaves reservoir settings inert");
    {
        CtmMapRuntime map;
        std::wstring err;
        const bool loaded = map.load(widen(mapPath), &err);
        CTM_CHECK(loaded);
        if (loaded) {
            CTM_CHECK_EQ(map.iso_passthrough_enabled(), true);
            CTM_CHECK_EQ(map.iso_ack_min_fill_ms(), 0u);
            CTM_CHECK_EQ(map.underrun_silence(), false);
        } else {
            std::printf("  could not load %s\n", mapPath.c_str());
        }
    }

    // --- Test 2: control. Proves test 1 can fail. --------------------------
    // Without this, test 1 would still pass if the parser were broken and
    // returned 0/false for everything.
    section("control: parser does read these settings when present");
    {
        std::ifstream in(mapPath, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        in.close();
        CTM_CHECK(!body.empty());

        const std::string tmpPath = "tests_tmp_control.map";
        {
            std::ofstream out(tmpPath, std::ios::binary);
            out << body
                << "\n[path.iso.virtual_to_physical_stream]\n"
                << "ack_min_fill_ms = 25\n"
                << "underrun_silence = true\n";
        }

        CtmMapRuntime map;
        std::wstring err;
        const bool loaded = map.load(widen(tmpPath), &err);
        CTM_CHECK(loaded);
        if (loaded) {
            CTM_CHECK_EQ(map.iso_ack_min_fill_ms(), 25u);
            CTM_CHECK_EQ(map.underrun_silence(), true);
        }
        std::remove(tmpPath.c_str());
    }

    return summary();
}
