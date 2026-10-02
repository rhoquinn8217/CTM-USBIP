// A key pressed and released by the clock: what a tap sends.
//
// ⭐ WHAT THESE PROTECT. A tap has nothing held under it, so the key it
// presses comes back up only because this says so. The fault to keep out is a
// key left down on someone's PC: every section below ends with the key up, and
// the first asks for nothing else.
//
// WHAT THESE CANNOT DO. They do not see the keyboard's pump call step(), nor
// what Windows received. The clock here is a number passed in.

#include "harness.h"

#include <cstdint>

#include "input/key_pulse.inl"

using namespace ctmtest;

int run_key_pulse_tests()
{
    using key_pulse::kGapMs;
    using key_pulse::kHoldMs;

    section("key pulse: a tapped key goes down, and comes back up by the clock alone");
    {
        key_pulse::Pulses pulses;
        CTM_CHECK(pulses.down().none());
        pulses.add(0, 0x28);                       // Enter
        // ⓘ Queued, not down: nothing changes until the clock moves.
        CTM_CHECK(pulses.down().none());

        CTM_CHECK(pulses.step(1000));              // the pump's next pass
        CTM_CHECK_EQ(static_cast<int>(pulses.down().usage), 0x28);

        // Held for its time, with nothing else happening at all.
        CTM_CHECK(!pulses.step(1000 + kHoldMs - 1));
        CTM_CHECK_EQ(static_cast<int>(pulses.down().usage), 0x28);

        // ⛔ AND UP. No report from any pad, no second call to add: the clock.
        CTM_CHECK(pulses.step(1000 + kHoldMs));
        CTM_CHECK(pulses.down().none());
        CTM_CHECK(!pulses.step(1000 + kHoldMs + 500));
        CTM_CHECK(pulses.down().none());
    }

    section("key pulse: two taps are two presses, with the key up between them");
    {
        key_pulse::Pulses pulses;
        pulses.add(0, 0x04);                       // KeyA
        pulses.add(0, 0x04);                       // and again
        CTM_CHECK(pulses.step(0));
        CTM_CHECK_EQ(static_cast<int>(pulses.down().usage), 0x04);
        CTM_CHECK(pulses.step(kHoldMs));           // the first comes up
        CTM_CHECK(pulses.down().none());
        // ⛔ NOT STRAIGHT BACK DOWN. A key that came up and went down in the
        // same moment reads as one long press to anything that asks "is it
        // down" between frames.
        CTM_CHECK(!pulses.step(kHoldMs + kGapMs - 1));
        CTM_CHECK(pulses.down().none());
        CTM_CHECK(pulses.step(kHoldMs + kGapMs));  // the second goes down
        CTM_CHECK_EQ(static_cast<int>(pulses.down().usage), 0x04);
        CTM_CHECK(pulses.step(kHoldMs + kGapMs + kHoldMs));
        CTM_CHECK(pulses.down().none());
        CTM_CHECK(!pulses.step(kHoldMs + kGapMs + kHoldMs + kGapMs + 1000));
    }

    section("key pulse: different keys keep their order");
    {
        key_pulse::Pulses pulses;
        pulses.add(0, 0x28);                       // Enter
        pulses.add(0, 0x29);                       // then Escape
        long long now = 0;
        CTM_CHECK(pulses.step(now));
        CTM_CHECK_EQ(static_cast<int>(pulses.down().usage), 0x28);
        now += kHoldMs;
        CTM_CHECK(pulses.step(now));
        now += kGapMs;
        CTM_CHECK(pulses.step(now));
        CTM_CHECK_EQ(static_cast<int>(pulses.down().usage), 0x29);
        now += kHoldMs;
        CTM_CHECK(pulses.step(now));
        CTM_CHECK(pulses.down().none());
    }

    section("key pulse: a modifier alone is a press too");
    {
        key_pulse::Pulses pulses;
        pulses.add(0x02, 0);                       // ShiftLeft, no key slot
        CTM_CHECK(pulses.step(0));
        CTM_CHECK_EQ(static_cast<int>(pulses.down().modifier), 0x02);
        CTM_CHECK_EQ(static_cast<int>(pulses.down().usage), 0);
        CTM_CHECK(!pulses.down().none());
        CTM_CHECK(pulses.step(kHoldMs));
        CTM_CHECK(pulses.down().none());
    }

    section("key pulse: nothing to press is not queued");
    {
        key_pulse::Pulses pulses;
        pulses.add(0, 0);
        CTM_CHECK(!pulses.step(0));
        CTM_CHECK(pulses.down().none());
    }

    section("key pulse: a pump that was away brings the key up on its first pass back");
    {
        // ⓘ The pump sleeps a few milliseconds between passes and can be
        // starved for longer. Late is fine; never is not.
        key_pulse::Pulses pulses;
        pulses.add(0, 0x2C);                       // Space
        CTM_CHECK(pulses.step(0));
        CTM_CHECK(pulses.step(5000));              // five seconds late
        CTM_CHECK(pulses.down().none());
    }

    section("key pulse: the queue is bounded, so a fault cannot type for ever");
    {
        key_pulse::Pulses pulses;
        for (int i = 0; i < 100; ++i) pulses.add(0, 0x04);
        int presses = 0;
        long long now = 0;
        for (int pass = 0; pass < 1000; ++pass) {
            if (pulses.step(now) && !pulses.down().none()) ++presses;
            now += 10;
        }
        CTM_CHECK_EQ(presses, static_cast<int>(key_pulse::kMaxWaiting));
        CTM_CHECK(pulses.down().none());
    }

    section("key pulse: clearing brings everything up and forgets what was waiting");
    {
        key_pulse::Pulses pulses;
        pulses.add(0, 0x04);
        pulses.add(0, 0x05);
        CTM_CHECK(pulses.step(0));
        pulses.clear();
        CTM_CHECK(pulses.down().none());
        CTM_CHECK(!pulses.step(10000));
        // ⓘ And it works again afterwards, from a clean start.
        pulses.add(0, 0x06);
        CTM_CHECK(pulses.step(10001));
        CTM_CHECK_EQ(static_cast<int>(pulses.down().usage), 0x06);
        CTM_CHECK(pulses.step(10001 + kHoldMs));
        CTM_CHECK(pulses.down().none());
    }

    return 0;
}
