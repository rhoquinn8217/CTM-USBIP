// Single entry point for the test binary. Each suite lives in its own file and
// exposes a run_*_tests() function; only this file has main(), since two of
// them would not link.
#include "harness.h"

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

// ⛔ THE TEST BINARY RUNS FROM THE SAME DIRECTORY AS THE AGENT.
//
// out/x64/Debug holds ctm-device-config.txt and configs/ -- the files a running
// agent actually reads. Several suites write them, and one deletes the shared
// config outright when it finishes. From inside a suite that looks like tidying
// up; from outside it silently destroys a real configuration.
//
// ⚠️ Not hypothetical. A test writing speaker_volume=33 and walking away
// attenuated a real DualSense on 2026-08-21, and the value came back after
// every test run -- which made it look like something was rewriting the file
// on its own. Hours went into that.
//
// So the snapshot lives HERE, not in any one suite: whatever any of them does
// to those files, the state a person left behind is put back.
namespace {

std::string g_shared_backup;
bool g_had_shared = false;

void save_runtime_state()
{
    std::ifstream in("ctm-device-config.txt", std::ios::binary);
    g_had_shared = in.is_open();
    if (g_had_shared) {
        std::ostringstream all;
        all << in.rdbuf();
        g_shared_backup = all.str();
    }
}

void restore_runtime_state()
{
    if (g_had_shared) {
        std::ofstream out("ctm-device-config.txt", std::ios::binary | std::ios::trunc);
        out << g_shared_backup;
    } else {
        std::error_code ignored;
        std::filesystem::remove("ctm-device-config.txt", ignored);
    }
    // configs/ is created only by the tests, so removing it is always right.
    std::error_code ignored;
    std::filesystem::remove_all("configs", ignored);
}

} // namespace

int main(int argc, char **argv)
{
    save_runtime_state();

    run_map_defaults_tests(argc, argv);
    run_device_config_tests();
    run_iso_in_pacing_tests(argc, argv);
    run_rest_parser_tests();
    run_config_store_tests();
    run_gyro_mouse_tests();
    restore_runtime_state();
    return ctmtest::summary();
}
