// The rumble floor: the weakest motor value a game can actually feel.
//
// ⭐ WHY IT EXISTS (T-145). A straight multiply at low gain turns weak rumble
// into nothing -- 5 at 30% is 1, and 1 does not move a weight. The motors have
// a real start-up threshold, reported by others as "the lowest few DS4 rumble
// values barely start the motors", so below it a game's faint cue is not faint,
// it is absent. This lifts it back to the weakest value that does spin.
//
// ⭐ 12% IS DS4WINDOWS' NUMBER, checked online 2026-09-19, not one invented
// here: the most-used tool in this space will not let a pad's rumble go below
// it. ➡️ A place to start from and dial by feel, which is the only way a number
// about how something FEELS can be settled. 0 turns the floor off.
//
// ⛔⛔ SILENCE MUST STAY SILENT, TWICE OVER, and this is the whole trap. A
// game claims the
// rumble bits and then sends 0 to both motors to END a rumble. A floor applied
// to that report would leave the pad buzzing until the game happened to claim
// the bits again -- a pad that never stops. ➡️ So the floor reads the value the
// game ASKED for, not the scaled one, and a motor the game asked to stop is
// left alone. ⭐ The second half is the PERSON: a gain of zero is someone
// asking for no rumble, and a floor that overrode it would make "off" the one
// rumble setting that does not work. ⓘ The same shape of trap as audio's:
// zero is a legal value and means silent, so it can never be the sentinel for
// "not configured".
//
// ⚠️ ITS REACH IS ONE PAD ON ONE TRANSPORT. Every override in
// ds5_output_overrides.inl begins `if (data[0] != 0x02) return;`, and 0x02 is
// the WIRED DualSense report id -- over Bluetooth the host sends 0x36. The
// audio settings solved that by TRAVELLING to the TV; the rumble gain never
// joined them. So this reaches a cabled DualSense and nothing else, which is
// the rest of T-145 rather than a fault in this file.

#pragma once

#include <cstdint>

// ⓘ DS4Windows' floor. The default rather than a limit: the setting's range is
// the full 0 to 100.
static const int kRumbleFloorDefaultPercent = 12;

static uint8_t rumble_apply_floor(uint8_t scaled, uint8_t requested,
                                  int floorPercent, int gainPercent)
{
    // ⛔ The game asked for nothing on this motor, so nothing is the answer.
    if (requested == 0 || floorPercent <= 0) {
        return scaled;
    }
    // ⛔⛔ AND A GAIN OF ZERO IS A PERSON ASKING FOR SILENCE. Caught by the
    // suite on 2026-09-19: `master_rumble_gain = 0` with a floor of 12 turned
    // a silenced pad back on, which would have made OFF the one rumble setting
    // that does not work. ⓘ The floor corrects OUR arithmetic, never the
    // person's decision.
    if (gainPercent <= 0) {
        return scaled;
    }
    const int floor = (floorPercent * 255) / 100;
    return scaled < floor ? static_cast<uint8_t>(floor) : scaled;
}
