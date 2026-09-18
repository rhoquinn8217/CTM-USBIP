// Same-controller tests: which older bridge session a new one retires.

#include "harness.h"

#include <string>

#include "app/same_controller.inl"

using namespace ctmtest;

int run_same_controller_tests()
{
    section("same controller: a re-bridge of one device retires the old session");
    {
        // The 2026-08-27 case: the same pad, bridged again from the same node.
        CTM_CHECK(same_controller::matches("7c:66:ef:82:10:ed", "/dev/hidraw3",
                                           "7c:66:ef:82:10:ed", "/dev/hidraw3"));
    }

    section("same controller: two halves of one device both stay");
    {
        // ⛔ The 2026-09-14 fault: the GameSir's pad and its keyboard interface
        // share one USB serial, and the pad retired the keyboard.
        CTM_CHECK(!same_controller::matches("3286967D", "/dev/input/event13",
                                            "3286967D", "/dev/hidraw2"));
        // Two devices that happen to share a serial.
        CTM_CHECK(!same_controller::matches("3286967D", "/dev/hidraw4",
                                            "3286967D", "/dev/hidraw5"));
    }

    section("same controller: different serials never match");
    {
        CTM_CHECK(!same_controller::matches("a0:fa:9c:ef:9b:30", "/dev/hidraw3",
                                            "7c:66:ef:82:10:ed", "/dev/hidraw3"));
    }

    section("same controller: no serial is no identity");
    {
        CTM_CHECK(!same_controller::matches("", "/dev/hidraw1", "", "/dev/hidraw1"));
        CTM_CHECK(!same_controller::matches("", "", "", ""));
    }

    section("same controller: a TV that sends no node matches on the serial");
    {
        CTM_CHECK(same_controller::matches("3286967D", "", "3286967D", "/dev/hidraw2"));
        CTM_CHECK(same_controller::matches("3286967D", "/dev/hidraw2", "3286967D", ""));
    }
    return 0;
}
