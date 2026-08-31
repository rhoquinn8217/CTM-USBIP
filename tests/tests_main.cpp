// Single entry point for the test binary. Each suite lives in its own file and
// exposes a run_*_tests() function; only this file has main(), since two of
// them would not link.
#include "harness.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

int run_map_defaults_tests(int argc, char **argv);
int run_device_config_tests();
int run_iso_in_pacing_tests(int argc, char **argv);
int run_rest_parser_tests();
int run_config_store_tests();
int run_gyro_mouse_tests();
int run_touch_mouse_tests();
int run_stick_mouse_tests();
int run_osk_tests();
int run_host_audio_settings_tests();

// ⛔⛔ THE TEST BINARY RUNS FROM THE SAME DIRECTORY AS THE AGENT.
//
// out/x64/Debug holds ctm-device-config.txt and configs/ -- the files a running
// agent actually reads. Several suites write them.
//
// ⚠️ This was handled by snapshot-and-restore, which REPAIRED the damage
// instead of preventing it, and carried the line "configs/ is created only by
// the tests, so removing it is always right". That was true when written and
// became false the moment per-controller configs shipped: the agent creates
// configs/ for the USER now, and every test run deleted them (2026-08-31 --
// an evening of "why aren't my configs being saved", with nothing in any log
// because the deletion happened in a different process). The 2026-08-21
// speaker_volume=33 incident was the same trap one file over.
//
// ⭐ So the tests now run somewhere else entirely: a scratch directory beside
// the binary. Nothing outside it is opened, written or removed, and there is
// no restore step because there is nothing to put back.
namespace {

const char *const kScratchDir = "test-scratch";

// Returns false if the scratch directory cannot be made -- the caller then
// stops rather than falling back to writing beside the agent, because that
// fallback is exactly the behaviour being removed.
bool enter_scratch()
{
    std::error_code ec;
    std::filesystem::remove_all(kScratchDir, ec);        // last run's leftovers
    std::filesystem::create_directories(kScratchDir, ec);
    if (ec) return false;
    std::filesystem::current_path(kScratchDir, ec);
    return !ec;
}

} // namespace

int main(int argc, char **argv)
{
    // ⛔ MAP TESTS FIRST, AND BEFORE THE SCRATCH SWITCH. They read
    // maps/*.map relative to the working directory, so they need the real one.
    // They only READ, which is why they are safe there.
    // ⚠️ Do not move this below enter_scratch() -- the map files are not in
    // the scratch directory and the suite would fail to find them.
    run_map_defaults_tests(argc, argv);

    if (!enter_scratch()) {
        std::fprintf(stderr, "could not create the scratch directory -- "
                             "refusing to run tests beside the agent's own "
                             "configs\n");
        return 1;
    }

    // Everything below writes files. All of it lands in the scratch directory.
    run_device_config_tests();
    run_iso_in_pacing_tests(argc, argv);
    run_rest_parser_tests();
    run_config_store_tests();
    run_gyro_mouse_tests();
    run_touch_mouse_tests();
    run_stick_mouse_tests();
    run_osk_tests();
    run_host_audio_settings_tests();
    return ctmtest::summary();
}
