// Buttons a touchpad gesture is pressing on the pad's own report.
//
// ⭐ WHAT THESE PROTECT. Two things a gesture must never do: leave a button
// down, and press one on a pad that did not make the gesture. A held button
// comes up when it is released or its pad goes away; a tapped one comes up by
// the clock, read as the pad's next report passes.
//
// WHAT THESE CANNOT DO. They do not write a report. That the rebinder asks
// this on every report, and presses what it is told, is the rebinder's.

#include "harness.h"

#include <cstdint>

#include "input/pad_press.inl"

using namespace ctmtest;

int run_pad_press_tests()
{
    using pad_press::kTapHoldMs;

    // ⓘ Stand-ins for two bridged pads. Only the addresses matter.
    int padA = 0;
    int padB = 0;
    const uint32_t cross = 1u << 0;
    const uint32_t r2 = 1u << 7;

    section("pad press: a tapped button is down for its time, then up");
    {
        pad_press::Presses presses;
        CTM_CHECK_EQ(presses.pressed(&padA, 0), 0u);
        presses.tap(&padA, 0, 1000);
        CTM_CHECK_EQ(presses.pressed(&padA, 1000), cross);
        CTM_CHECK_EQ(presses.pressed(&padA, 1000 + kTapHoldMs - 1), cross);
        // ⛔ UP, with nothing having released it: the clock.
        CTM_CHECK_EQ(presses.pressed(&padA, 1000 + kTapHoldMs), 0u);
        CTM_CHECK_EQ(presses.pressed(&padA, 1000 + kTapHoldMs + 5000), 0u);
    }

    section("pad press: a held button stays down whatever the clock says, until it is let go");
    {
        pad_press::Presses presses;
        presses.hold(&padA, 7);
        CTM_CHECK_EQ(presses.pressed(&padA, 0), r2);
        CTM_CHECK_EQ(presses.pressed(&padA, 60000), r2);    // a minute on
        presses.release(&padA, 7);
        CTM_CHECK_EQ(presses.pressed(&padA, 60001), 0u);
        // ⓘ Letting go twice, or of something never held, is harmless.
        presses.release(&padA, 7);
        presses.release(&padB, 3);
        CTM_CHECK_EQ(presses.pressed(&padA, 60002), 0u);
    }

    section("pad press: one pad's press is not another pad's");
    {
        pad_press::Presses presses;
        presses.tap(&padA, 0, 0);
        presses.hold(&padB, 7);
        CTM_CHECK_EQ(presses.pressed(&padA, 10), cross);
        CTM_CHECK_EQ(presses.pressed(&padB, 10), r2);
        // ⓘ And one pad's time running out leaves the other's hold alone.
        CTM_CHECK_EQ(presses.pressed(&padA, kTapHoldMs), 0u);
        CTM_CHECK_EQ(presses.pressed(&padB, kTapHoldMs), r2);
    }

    section("pad press: a tap and a hold on one pad are both down, and end separately");
    {
        pad_press::Presses presses;
        presses.hold(&padA, 7);
        presses.tap(&padA, 0, 0);
        CTM_CHECK_EQ(presses.pressed(&padA, 1), cross | r2);
        CTM_CHECK_EQ(presses.pressed(&padA, kTapHoldMs), r2);   // the tap is over
        presses.release(&padA, 7);
        CTM_CHECK_EQ(presses.pressed(&padA, kTapHoldMs + 1), 0u);
    }

    section("pad press: a second tap before the first is up is one longer press");
    {
        pad_press::Presses presses;
        presses.tap(&padA, 0, 0);
        presses.tap(&padA, 0, 40);
        CTM_CHECK_EQ(presses.pressed(&padA, kTapHoldMs), cross);         // past the first's time
        CTM_CHECK_EQ(presses.pressed(&padA, 40 + kTapHoldMs - 1), cross);
        CTM_CHECK_EQ(presses.pressed(&padA, 40 + kTapHoldMs), 0u);
    }

    section("pad press: a pad going away takes its presses with it, and only its");
    {
        pad_press::Presses presses;
        presses.hold(&padA, 7);
        presses.tap(&padA, 0, 0);
        presses.hold(&padB, 0);
        presses.forget(&padA);
        CTM_CHECK_EQ(presses.pressed(&padA, 1), 0u);
        CTM_CHECK_EQ(presses.pressed(&padB, 1), cross);
        // ⓘ A device that arrives at the same address starts with nothing down.
        presses.release(&padA, 7);
        CTM_CHECK_EQ(presses.pressed(&padA, 2), 0u);
        presses.forget(&padA);
        presses.forget(nullptr);
        CTM_CHECK_EQ(presses.pressed(&padB, 3), cross);
    }

    section("pad press: a button that is not a button is ignored");
    {
        pad_press::Presses presses;
        presses.hold(&padA, -1);
        presses.tap(&padA, -1, 0);
        presses.hold(&padA, pad_press::kMaxButtons);
        presses.tap(&padA, 99, 0);
        presses.release(&padA, 99);
        CTM_CHECK_EQ(presses.pressed(&padA, 1), 0u);
    }

    return 0;
}
