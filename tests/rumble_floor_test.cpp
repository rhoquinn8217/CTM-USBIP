// Rumble-floor tests: the weakest motor value a game can actually feel.
//
// ⭐ The real function, included rather than mirrored. A mirrored copy of this
// would pass while the shipped clamp was wrong, and the whole point of the
// floor is a number that nobody can check by reading it.

#include "harness.h"

#include <cstdint>

#include "audio/rumble_floor.inl"

using namespace ctmtest;

namespace {

// 12% of 255. The value every "lifted" case below must land on.
const int kFloorByte = (kRumbleFloorDefaultPercent * 255) / 100;

// A gain of 100 -- the ordinary case, where the floor is free to act.
uint8_t floored(int scaled, int requested)
{
    return rumble_apply_floor(static_cast<uint8_t>(scaled), static_cast<uint8_t>(requested),
                              kRumbleFloorDefaultPercent, 100);
}

} // namespace

int run_rumble_floor_tests()
{
    section("rumble floor: silence stays silent");
    {
        // ⛔⛔ THE TRAP. A game claims the rumble bits and sends 0 to STOP a
        // rumble. Lifting that would leave the pad buzzing until the game
        // claimed the bits again -- a pad that never stops.
        CTM_CHECK(floored(0, 0) == 0);
        // ⓘ Even a scaled value that survived: what the GAME asked for decides.
        CTM_CHECK(floored(40, 0) == 40);
    }

    section("rumble floor: a cue the multiply erased comes back");
    {
        // The case it exists for: 5 at 30% is 1, and 1 does not move a weight.
        CTM_CHECK(floored(1, 5) == kFloorByte);
        CTM_CHECK(floored(0, 5) == kFloorByte);
        CTM_CHECK(floored(kFloorByte - 1, 200) == kFloorByte);
    }

    section("rumble floor: anything already above it is untouched");
    {
        CTM_CHECK(floored(kFloorByte, 100) == kFloorByte);
        CTM_CHECK(floored(kFloorByte + 1, 100) == kFloorByte + 1);
        CTM_CHECK(floored(255, 255) == 255);
        // ⭐ A floor is not a gain: it never pulls a strong rumble DOWN.
        CTM_CHECK(floored(200, 10) == 200);
    }

    section("rumble floor: a person who asked for silence gets it");
    {
        // ⛔⛔ THE REGRESSION THE SUITE CAUGHT, 2026-09-19. A gain of 0 is
        // someone turning rumble OFF. A floor that lifted it anyway would make
        // off the one rumble setting that does not work.
        CTM_CHECK(rumble_apply_floor(0, 255, kRumbleFloorDefaultPercent, 0) == 0);
        CTM_CHECK(rumble_apply_floor(0, 200, kRumbleFloorDefaultPercent, 0) == 0);
        // ⓘ And the motor beside it, gained normally, is unaffected by that.
        CTM_CHECK(rumble_apply_floor(1, 200, kRumbleFloorDefaultPercent, 50) == kFloorByte);
    }

    section("rumble floor: zero percent turns it off");
    {
        // ⓘ Off has to be reachable, and 0 is the natural way to ask for it --
        // the same reason audio_latency_ms's range was opened to its silent end.
        CTM_CHECK(rumble_apply_floor(1, 5, 0, 100) == 1);
        CTM_CHECK(rumble_apply_floor(0, 5, 0, 100) == 0);
        // A negative can only arrive from a malformed config; treat it as off.
        CTM_CHECK(rumble_apply_floor(1, 5, -20, 100) == 1);
    }

    section("rumble floor: the whole range is usable");
    {
        CTM_CHECK(rumble_apply_floor(1, 5, 100, 100) == 255);   // everything at full
        CTM_CHECK(rumble_apply_floor(1, 5, 1, 100) == 2);       // 1% of 255
        // ⓘ DS4Windows' 12% is the default, not the limit.
        CTM_CHECK(kRumbleFloorDefaultPercent == 12);
    }
    return 0;
}
