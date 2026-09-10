// Adaptive trigger effects: giving a threshold somewhere the finger can find.
//
// ⭐ WHY THIS EXISTS. A trigger rebound to a mouse button fires at a position
// the finger cannot feel, so the only way to know where the click is, is to
// pull until something happens. The DualSense can put a physical resistance
// break at a chosen point in the travel. Put that break ON the click point and
// the trigger tells the finger where it is, the way a camera shutter's half
// press does.
//
// ⓘ WHERE THE LAYOUT COMES FROM, and how much of it is ours. The block
// positions are this project's own, from the 2026-08-24 on-wire capture in
// trigger_watch.inl. The PARAMETER packing below is published community
// knowledge, and it was checked against that same capture before anything here
// was written:
//
//     shotgun      25 04 01 07   ->  zones 0x0104 = bits 2 and 8, strength 7
//     machine gun  25 0c 00 ..   ->  zones 0x000c = bits 2 and 3
//
// ⭐ Both decode as sensible weapons under this packing and as nothing at all
// under any other reading, which is what makes it believed rather than
// assumed: a shotgun resists across most of the pull, a machine gun gives one
// short bump. NO CODE WAS COPIED -- the encoders below are written from the
// byte layout, the same posture ds5_apply_settings.inl takes toward the same
// project's field positions.
//
// ⛔ FEEDBACK MODE IS NOT CROSS-CHECKED. Only weapon mode appears in a capture
// we took. The feedback encoder is written from the published layout alone, so
// it is the one to distrust if something feels wrong.
//
// ---- THE WHOLE MODE SET, read up 2026-09-10 so the next person need not -----
//
// Eleven bytes per trigger, always. Byte 0 is the mode, the rest are its
// parameters, and the pull is addressed as ten ZONES rather than as the 0-255
// the input report gives back.
//
//   0x05  off          all zeros after the mode
//   0x21  feedback     zones bitmap (2 bytes) + 3 bits of force per zone (4)
//   0x25  weapon       start and stop as one bitmap (2) + one strength byte
//   0x26  vibration    like feedback, plus a frequency byte at index 9
//   0x22  bow          weapon plus a snap-back force that resets the trigger
//   0x23  galloping    two feet and a frequency, an oscillation
//   0x27  machine      two amplitudes, a frequency and a period
//   0x01/0x02/0x06     the simple forms of feedback, weapon and vibration
//
// ⭐ ONLY 0x21 AND 0x25 ARE USED HERE, and between them they cover everything
// this project wants: a break somewhere (weapon), and any shape at all made out
// of ten per-zone forces (feedback). The oscillating modes are for guns.
//
// ⚠️ WHERE WE KNOWINGLY DIVERGE FROM THE PUBLISHED FACTORIES:
//   1. **Strength.** They take 1-8 and send one less, so their 1 stores 0. We
//      take 1-7 and send it verbatim. Same maximum, and no value in the range
//      stores a zero -- which on this hardware is an effect that can neither be
//      felt nor used to clear the last one. That cost a morning; see below.
//   2. **Weapon's deep end.** They cap the start at zone 7 and the stop at 8.
//      We allow 8 and 9, because rhoquinn8217 asked for a deeper click and it
//      was then confirmed on hardware at both 30% and 90% (2026-09-10).
//
// ⭐⭐ AND ONE THING WE DO NOT YET USE. The INPUT report carries a trigger
// STATUS nybble beside the stop zone -- the controller says whether the finger
// is before, inside, or PAST the effect's stop zone. ➡️ That means a click
// could fire exactly where the physical detent is, instead of at a percentage
// chosen to match it. See T-168; the byte offset still needs confirming against
// a real report before anything is built on it.

#pragma once

namespace trigger_effect {

// ---- where the fields live, within output report 0x02 -----------------------
//
// ⓘ Absolute within the report, WITH the report id at index 0. Confirmed
// against the capture in trigger_watch.inl, which records that skipping the id
// was tried first and matched nothing.
constexpr size_t kR2Offset = 11;
constexpr size_t kL2Offset = 22;
constexpr size_t kBlockLen = 11;

// Claim bits in ValidFlag0, beside the two rumble bits already named in
// ds5_output_overrides.inl. A field is applied ONLY when its bit is claimed,
// which is what lets an effect we set survive a game or a TV sending rumble.
constexpr uint8_t kClaimR2 = 0x04;
constexpr uint8_t kClaimL2 = 0x08;

// ---- the modes --------------------------------------------------------------
constexpr uint8_t kModeOff      = 0x05;
constexpr uint8_t kModeFeedback = 0x21;
constexpr uint8_t kModeWeapon   = 0x25;
// ⚠️ Listed as UNOFFICIAL where the others are not, and never seen in a capture
// of ours. It is a weapon that also pushes the trigger back toward rest.
constexpr uint8_t kModeBow      = 0x22;

// ---- the travel, in zones ---------------------------------------------------
//
// The pull is addressed as ten zones rather than as the 0-255 the input report
// gives back. They are not the same scale and must not be mixed.
constexpr int kZoneCount = 10;

// ⚠️ WEAPON MODE CANNOT PUT ITS BREAK ANYWHERE. Start is limited to zones 2-7
// and the end to at most 9, so a break can only land between zone 3 and zone 9,
// which is 30% to 90% of the pull. A request outside that is clamped, not
// refused: a trigger that quietly sits at 30% is better than one that does
// nothing at all while the config looks right.
//
// ⛔ THE DEEP END IS OURS, NOT A DOCUMENTED LIMIT. Two public descriptions of
// this encoding disagree -- one caps the end at zone 8, the other allows 9 --
// and the 2026-08-24 capture only ever shows a game using 8. The pull has ten
// zones and bit 9 is representable, so 9 is offered because rhoquinn8217 asked
// for a deeper click (2026-09-10) and trying it is the only way to know.
// ⚠️ If a break at 90% is ever found to do nothing, put these back to 7 and 8
// rather than assuming the report was lost -- that mistake has already cost
// this ticket a morning.
constexpr int kWeaponStartMin = 2;
constexpr int kWeaponStartMax = 8;
constexpr int kWeaponEndMax   = 9;

// ⛔ THE BOTTOM OF THE RANGE MUST BE A REAL FORCE. The hardware takes 0 to 7
// and treats 0 as no resistance at all, so an earlier scale of 1 to 8 sent one
// less than it was given and made 1 mean silence. That cost most of a morning
// on 2026-09-10: a wall set to 1 was read as "the change never arrived" when
// what actually went out was an effect asking for nothing, twice.
// ⭐ So the setting IS the hardware value, 1 to 7, sent verbatim. Off is a
// MODE, never a strength of zero.
// ⛔ THE BOW KEEPS THE DOCUMENTED LIMITS, deliberately, where weapon does not.
// Weapon was pushed to zone 9 and confirmed on hardware; this mode has never
// been seen working at all, so it gets ONE unknown rather than two. ➡️ If a
// snap does nothing, that is the mode failing, not the zone -- and if it works,
// trying 9 afterwards is a one-line change.
constexpr int kBowStartMin = 0;
constexpr int kBowStartMax = 7;
constexpr int kBowEndMax   = 8;

constexpr int kStrengthMin = 1;
constexpr int kStrengthMax = 7;

inline int clamp_to(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// Percent of the pull to a zone. 50% is zone 5.
inline int zone_from_percent(int percent)
{
    return clamp_to((clamp_to(percent, 0, 100) * kZoneCount) / 100, 0, kZoneCount - 1);
}

inline void build_off(uint8_t *block)
{
    memset(block, 0, kBlockLen);
    block[0] = kModeOff;
}

// Resistance that builds from startZone and GIVES WAY at endZone.
//
// ⭐ The break is at the END. That is why the caller places the click point
// there and puts the start one zone earlier: the moment the finger feels
// something let go is the moment the button should fire.
inline void build_weapon(uint8_t *block, int startZone, int endZone, int strength)
{
    startZone = clamp_to(startZone, kWeaponStartMin, kWeaponStartMax);
    endZone   = clamp_to(endZone, startZone + 1, kWeaponEndMax);
    strength  = clamp_to(strength, kStrengthMin, kStrengthMax);

    const uint16_t zones = static_cast<uint16_t>((1u << startZone) | (1u << endZone));

    memset(block, 0, kBlockLen);
    block[0] = kModeWeapon;
    block[1] = static_cast<uint8_t>(zones & 0xff);
    block[2] = static_cast<uint8_t>((zones >> 8) & 0xff);
    block[3] = static_cast<uint8_t>(strength);
}

// Resistance that begins at startZone and stays for the rest of the pull.
//
// ⓘ Ten zones of three bits each, so the strengths need four bytes. Every zone
// from the start down is given the same strength, a wall rather than a ramp.
inline void build_feedback(uint8_t *block, int startZone, int strength)
{
    startZone = clamp_to(startZone, 0, kZoneCount - 1);
    strength  = clamp_to(strength, kStrengthMin, kStrengthMax);

    uint16_t active = 0;
    uint32_t forces = 0;
    for (int zone = startZone; zone < kZoneCount; ++zone) {
        active |= static_cast<uint16_t>(1u << zone);
        forces |= static_cast<uint32_t>(strength) << (3 * zone);
    }

    memset(block, 0, kBlockLen);
    block[0] = kModeFeedback;
    block[1] = static_cast<uint8_t>(active & 0xff);
    block[2] = static_cast<uint8_t>((active >> 8) & 0xff);
    block[3] = static_cast<uint8_t>(forces & 0xff);
    block[4] = static_cast<uint8_t>((forces >> 8) & 0xff);
    block[5] = static_cast<uint8_t>((forces >> 16) & 0xff);
    block[6] = static_cast<uint8_t>((forces >> 24) & 0xff);
}

// A light wall the whole way down, with ONE firm zone in it: the finger meets
// steady resistance, hits a distinct detent where the point is, and pushes on
// through the same steady resistance.
//
// ⛔ TWO SHAPES WERE TRIED AND FELT WRONG FIRST, both on hardware 2026-09-10.
//   1. A wall with a lip three steps firmer. rhoquinn8217: *"wall and notch
//      feel exactly the same."* Seven against four across a tenth of the pull
//      is not a step a finger notices.
//   2. A weapon effect climbing from an early zone to the point. That DOES
//      move its break, but the climb means a deeper point is a longer fight:
//      *"trigger_r2_effect_at changes how hard you need to press the button
//      for it to bump."* ⭐ The setting is called _at, so it has to change
//      WHERE, and only where.
//
// ⭐ SO THE WALL IS FLAT AND ONLY THE DETENT MOVES. The effort is the same
// wherever the point sits, which is what leaves its position as the thing the
// finger actually reads.
//
// ⓘ One knob. The detent takes the configured strength and the wall is a third
// of it, floored at a real force. A ratio rather than a difference, because the
// first failure above was a difference that turned out to be too small.
inline void build_detent_wall(uint8_t *block, int detentZone, int strength)
{
    // ⓘ Zone 0 is left free so the trigger is not heavy at rest, which also
    // means the detent cannot sit at the very top of the pull.
    constexpr int kWallStart = 1;
    detentZone = clamp_to(detentZone, kWallStart, kZoneCount - 1);
    strength   = clamp_to(strength, kStrengthMin, kStrengthMax);
    const int wall = clamp_to(strength / 3, kStrengthMin, kStrengthMax);

    uint16_t active = 0;
    uint32_t forces = 0;
    for (int zone = kWallStart; zone < kZoneCount; ++zone) {
        active |= static_cast<uint16_t>(1u << zone);
        const int force = (zone == detentZone) ? strength : wall;
        forces |= static_cast<uint32_t>(force) << (3 * zone);
    }

    memset(block, 0, kBlockLen);
    block[0] = kModeFeedback;
    block[1] = static_cast<uint8_t>(active & 0xff);
    block[2] = static_cast<uint8_t>((active >> 8) & 0xff);
    block[3] = static_cast<uint8_t>(forces & 0xff);
    block[4] = static_cast<uint8_t>((forces >> 8) & 0xff);
    block[5] = static_cast<uint8_t>((forces >> 16) & 0xff);
    block[6] = static_cast<uint8_t>((forces >> 24) & 0xff);
}

// A break at the point, plus a push that carries the trigger back to rest.
//
// ⭐ WHY IT IS WORTH HAVING (rhoquinn8217, 2026-09-10). The R2 gesture only
// hands the cursor back when the trigger goes ALL THE WAY home. A finger that
// relaxes but rests part way leaves the cursor frozen with nothing on screen
// explaining why -- which is the exact complaint that made the touchpad version
// feel broken. A trigger that returns itself removes that state instead of
// asking the user to remember it.
//
// ⓘ Two forces, and they are different things: `strength` is the resistance you
// push through, `snapForce` is what pushes back afterwards. They share one byte,
// three bits each.
inline void build_snap(uint8_t *block, int startZone, int endZone,
                       int strength, int snapForce)
{
    startZone = clamp_to(startZone, kBowStartMin, kBowStartMax);
    endZone   = clamp_to(endZone, startZone + 1, kBowEndMax);
    strength  = clamp_to(strength, kStrengthMin, kStrengthMax);
    snapForce = clamp_to(snapForce, kStrengthMin, kStrengthMax);

    const uint16_t zones = static_cast<uint16_t>((1u << startZone) | (1u << endZone));

    memset(block, 0, kBlockLen);
    block[0] = kModeBow;
    block[1] = static_cast<uint8_t>(zones & 0xff);
    block[2] = static_cast<uint8_t>((zones >> 8) & 0xff);
    block[3] = static_cast<uint8_t>((strength & 0x07) | ((snapForce & 0x07) << 3));
}

// ---- what a config asks for -------------------------------------------------

enum class Shape {
    Absent,   // the key is not set: leave the triggers alone entirely
    Off,      // asked for nothing: clear an effect WE set, and only that
    Click,    // a break at the point, so the finger can find it
    Wall,     // resistance from the point down
    Notch,    // a flat wall with one firm zone in it, at the point
    Snap,     // a break at the point, and the trigger returns itself after
};

inline Shape shape_from(const std::string &value)
{
    if (value.empty()) return Shape::Absent;
    if (value == "off" || value == "none") return Shape::Off;
    if (value == "click" || value == "weapon") return Shape::Click;
    if (value == "wall" || value == "feedback") return Shape::Wall;
    if (value == "notch" || value == "both") return Shape::Notch;
    if (value == "snap" || value == "bow") return Shape::Snap;
    return Shape::Absent;     // a typo leaves the trigger alone, never breaks it
}

// ⭐ WHAT WE HAVE SET, so that turning it off can clear it WITHOUT claiming the
// triggers on installs that never asked for any of this.
//
// ⛔ Clearing unconditionally was the obvious first shape and it is wrong: it
// would make every settings push claim the trigger fields, stamping "off" over
// a game's own effect for everyone, including the people not using this. So we
// clear only what this process actually set.
inline std::mutex g_setMutex;
inline std::map<std::string, bool> g_setEffect;

inline bool has_set_effect(const std::string &marker)
{
    std::lock_guard<std::mutex> lock(g_setMutex);
    return g_setEffect.find(marker) != g_setEffect.end();
}

inline void note_set_effect(const std::string &marker, bool on)
{
    std::lock_guard<std::mutex> lock(g_setMutex);
    if (on) g_setEffect[marker] = true;
    else g_setEffect.erase(marker);
}

inline std::string stem_snap(const char *sideKey)
{
    return std::string("trigger_") + sideKey + "_snap_force";
}

// Writes one trigger's block and says which claim bit to raise. Returns 0 when
// the trigger is to be left alone, which is the common case.
inline uint8_t apply_one(const std::string &section, const char *sideKey,
                         uint8_t *report, size_t offset, uint8_t claimBit)
{
    const std::string effectKey   = std::string("trigger_") + sideKey + "_effect";
    const std::string atKey       = effectKey + "_at";
    const std::string strengthKey = effectKey + "_strength";
    const std::string marker      = section + "/" + sideKey;

    const Shape shape = shape_from(device_config_str(section.c_str(), effectKey.c_str()));
    if (shape == Shape::Absent) return 0;

    if (shape == Shape::Off) {
        if (!has_set_effect(marker)) return 0;      // nothing of ours to undo
        build_off(report + offset);
        note_set_effect(marker, false);
        return claimBit;
    }

    const int percent  = device_config_int(section.c_str(), atKey.c_str(), 50);
    const int strength = device_config_int(section.c_str(), strengthKey.c_str(), 5);
    const int zone     = zone_from_percent(percent);

    if (shape == Shape::Click) {
        // ⭐ The break lands at the END zone, so the requested point IS the end
        // and the resistance starts one zone earlier.
        build_weapon(report + offset, zone - 1, zone, strength);
    } else if (shape == Shape::Notch) {
        build_detent_wall(report + offset, zone, strength);
    } else if (shape == Shape::Snap) {
        const int snapForce =
            device_config_int(section.c_str(), (stem_snap(sideKey)).c_str(), 3);
        build_snap(report + offset, zone - 1, zone, strength, snapForce);
    } else {
        build_feedback(report + offset, zone, strength);
    }
    note_set_effect(marker, true);
    return claimBit;
}

// Is this side asking for an effect of its own?
inline bool side_wants_effect(const std::string &section, const char *sideKey)
{
    const std::string key = std::string("trigger_") + sideKey + "_effect";
    const Shape shape = shape_from(device_config_str(section.c_str(), key.c_str()));
    return shape == Shape::Click || shape == Shape::Wall ||
           shape == Shape::Notch || shape == Shape::Snap;
}

// Adds whatever the section asks for to an output report already being built.
// Returns the claim bits to OR into ValidFlag0, or 0 to touch nothing.
//
// ⭐⭐ A CONFIG THAT SETS ANY TRIGGER EFFECT OWNS BOTH TRIGGERS.
//
// ⛔ WHY, and it was a real fault (rhoquinn8217, 2026-09-10): *"I'm feeling a
// wall on the L2 trigger. Are you setting anything for L2 by mistake?"* We were
// not -- a DIFFERENT config had, an hour earlier. An effect lives on the
// controller until something changes it, and an absent key means "leave it
// alone", so a wall set by one config followed the pad into the next one and
// looked like a ghost.
//
// ➡️ "Leave it alone" is the right rule for a field somebody ELSE owns. It is
// the wrong rule for one we set ourselves. So a section that configures either
// trigger now puts the other into a known state instead of inheriting one.
//
// ⓘ Deliberately stateless. Tracking what we had set was the first shape and it
// was keyed on the config, which is exactly the thing that changes -- so the
// old config's note was never visited again to be undone.
inline uint8_t apply_to_report(const std::string &section, uint8_t *report, size_t len)
{
    if (report == nullptr || len < kL2Offset + kBlockLen) return 0;

    const bool ownsTriggers =
        side_wants_effect(section, "r2") || side_wants_effect(section, "l2");

    uint8_t claim = 0;
    claim = static_cast<uint8_t>(claim | apply_one(section, "r2", report, kR2Offset, kClaimR2));
    claim = static_cast<uint8_t>(claim | apply_one(section, "l2", report, kL2Offset, kClaimL2));

    if (ownsTriggers) {
        if ((claim & kClaimR2) == 0) {
            build_off(report + kR2Offset);
            claim = static_cast<uint8_t>(claim | kClaimR2);
        }
        if ((claim & kClaimL2) == 0) {
            build_off(report + kL2Offset);
            claim = static_cast<uint8_t>(claim | kClaimL2);
        }
    }
    return claim;
}

// Does this section want anything? Asked before the settings report decides it
// has nothing to send, so a trigger effect alone is enough to send one.
inline bool wants_anything(const std::string &section)
{
    const char *sides[] = { "r2", "l2" };
    for (const char *side : sides) {
        const std::string key = std::string("trigger_") + side + "_effect";
        const Shape shape = shape_from(device_config_str(section.c_str(), key.c_str()));
        if (shape == Shape::Click || shape == Shape::Wall ||
            shape == Shape::Notch || shape == Shape::Snap) return true;
        if (shape == Shape::Off && has_set_effect(section + "/" + side)) return true;
    }
    return false;
}

}  // namespace trigger_effect
