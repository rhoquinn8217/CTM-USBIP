// A console that is closing waits for the listener to stop: the wait itself.
//
// ⭐ WHAT THESE PROTECT. Closing the terminal a listener was typed into ended
// it before it had stopped anything: the settings window stayed open and the
// log said nothing. The handler now waits for the program to say it is done,
// and these pin the three orders that can happen in -- done never said, done
// said during the wait, and done said before the wait began. The last is the
// one a quick stop gives, and the one a careless wait would sit through.
//
// WHAT THESE CANNOT DO. They close no console. That the handler is called,
// that Windows holds off while it waits, and that the listener is really
// stopped by the end is checked by typing it into a terminal and closing the
// window.

#include "harness.h"

#include <chrono>
#include <thread>

#include "app/stop_wait.inl"

using namespace ctmtest;

namespace {

unsigned elapsed_ms(std::chrono::steady_clock::time_point since)
{
    return static_cast<unsigned>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - since).count());
}

} // namespace

int run_stop_wait_tests()
{
    section("stop wait: nobody says done, and the wait gives up when its time is out");
    {
        stop_wait::Waiter waiter;
        const auto start = std::chrono::steady_clock::now();
        CTM_CHECK(!waiter.until_done(80));
        // It waited, rather than answering at once.
        CTM_CHECK(elapsed_ms(start) >= 50);
    }

    section("stop wait: done said while it waits lets it go, long before its time");
    {
        stop_wait::Waiter waiter;
        const auto start = std::chrono::steady_clock::now();
        std::thread stopper([&waiter] {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            waiter.done();
        });
        CTM_CHECK(waiter.until_done(5000));
        CTM_CHECK(elapsed_ms(start) < 2500);
        stopper.join();
    }

    section("stop wait: done said BEFORE the wait began is not lost");
    {
        // ⛔ The order a quick stop gives: the program is finished before the
        // console's handler has got as far as waiting. A wait that only heard
        // a done said after it started would sit out its whole time, and the
        // listener would be ended by Windows having already stopped.
        stop_wait::Waiter waiter;
        waiter.done();
        const auto start = std::chrono::steady_clock::now();
        CTM_CHECK(waiter.until_done(5000));
        CTM_CHECK(elapsed_ms(start) < 2500);
        // And it stays done: a second asker is answered the same, at once.
        CTM_CHECK(waiter.until_done(0));
    }

    section("stop wait: one waiter is not another");
    {
        stop_wait::Waiter first;
        stop_wait::Waiter second;
        first.done();
        CTM_CHECK(first.until_done(0));
        CTM_CHECK(!second.until_done(0));
    }

    section("stop wait: the wait and the grace fit inside what Windows allows");
    {
        // ⓘ Five seconds is what a closing console gives a program. Past it
        // the program is ended whatever it is doing.
        CTM_CHECK(stop_wait::kCloseWaitMs + stop_wait::kGraceMs <= 5000);
        // ⓘ And not so short that an ordinary stop is cut off: one with pads
        // bridged takes a second or two.
        CTM_CHECK(stop_wait::kCloseWaitMs >= 3000);
    }

    return 0;
}
