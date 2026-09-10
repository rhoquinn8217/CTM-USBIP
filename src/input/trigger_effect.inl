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

// ---- the travel, in zones ---------------------------------------------------
//
// The pull is addressed as ten zones rather than as the 0-255 the input report
// gives back. They are not the same scale and must not be mixed.
constexpr int kZoneCount = 10;

// ⚠️ WEAPON MODE CANNOT PUT ITS BREAK ANYWHERE. Start is limited to zones 2-7
// and the end to at most 8, so a break can only land between zone 3 and zone 8,
// which is 30% to 80% of the pull. A request outside that is clamped, not
// refused: a trigger that quietly sits at 30% is better than one that does
// nothing at all while the config looks right.
constexpr int kWeaponStartMin = 2;
constexpr int kWeaponStartMax = 7;
constexpr int kWeaponEndMax   = 8;

// Strength is sent one less than it is written, so 1 is the lightest the
// hardware will express and 0 would underflow rather than mean "none". Off is a
// MODE, never a strength of zero.
constexpr int kStrengthMin = 1;
constexpr int kStrengthMax = 8;

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
    block[3] = static_cast<uint8_t>(strength - 1);
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
        forces |= static_cast<uint32_t>(strength - 1) << (3 * zone);
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

// ---- what a config asks for -------------------------------------------------

enum class Shape {
    Absent,   // the key is not set: leave the triggers alone entirely
    Off,      // asked for nothing: clear an effect WE set, and only that
    Click,    // a break at the point, so the finger can find it
    Wall,     // resistance from the point down
};

inline Shape shape_from(const std::string &value)
{
    if (value.empty()) return Shape::Absent;
    if (value == "off" || value == "none") return Shape::Off;
    if (value == "click" || value == "weapon") return Shape::Click;
    if (value == "wall" || value == "feedback") return Shape::Wall;
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
    const int strength = device_config_int(section.c_str(), strengthKey.c_str(), 6);
    const int zone     = zone_from_percent(percent);

    if (shape == Shape::Click) {
        // ⭐ The break lands at the END zone, so the requested point IS the end
        // and the resistance starts one zone earlier.
        build_weapon(report + offset, zone - 1, zone, strength);
    } else {
        build_feedback(report + offset, zone, strength);
    }
    note_set_effect(marker, true);
    return claimBit;
}

// Adds whatever the section asks for to an output report already being built.
// Returns the claim bits to OR into ValidFlag0, or 0 to touch nothing.
inline uint8_t apply_to_report(const std::string &section, uint8_t *report, size_t len)
{
    if (report == nullptr || len < kL2Offset + kBlockLen) return 0;
    uint8_t claim = 0;
    claim = static_cast<uint8_t>(claim | apply_one(section, "r2", report, kR2Offset, kClaimR2));
    claim = static_cast<uint8_t>(claim | apply_one(section, "l2", report, kL2Offset, kClaimL2));
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
        if (shape == Shape::Click || shape == Shape::Wall) return true;
        if (shape == Shape::Off && has_set_effect(section + "/" + side)) return true;
    }
    return false;
}

}  // namespace trigger_effect
