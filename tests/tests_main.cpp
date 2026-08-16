// Single entry point for the test binary. Each suite lives in its own file and
// exposes a run_*_tests() function; only this file has main(), since two of
// them would not link.
#include "harness.h"

int run_map_defaults_tests(int argc, char **argv);
int run_device_config_tests();
int run_iso_in_pacing_tests(int argc, char **argv);
int run_gyro_mouse_tests();

int main(int argc, char **argv)
{
    run_map_defaults_tests(argc, argv);
    run_device_config_tests();
    run_iso_in_pacing_tests(argc, argv);
    run_gyro_mouse_tests();
    return ctmtest::summary();
}
