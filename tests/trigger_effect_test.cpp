// Tests for the adaptive trigger effect encoder.
//
// ⭐ THE ONE THAT MATTERS is the capture cross-check. The parameter packing is
// published community knowledge rather than something this project decoded, so
// the test that earns its keep is the one proving our encoder reproduces bytes
// a real game actually sent -- Stellar Blade, captured 2026-08-24 and recorded
// in trigger_watch.inl. If that ever fails, the packing is wrong and every
// other check here is measuring the wrong thing consistently.
//
// WHAT THESE CANNOT DO. They cannot say an effect FEELS like a threshold, that
// the break sits where a finger expects, or that the controller accepts the
// report at all. Those are hardware questions. These protect the arithmetic:
// the zone maths, the clamps at the edges of what weapon mode can express, and
// the rule that an unconfigured trigger is never claimed.

#include "harness.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace ctmtest;

namespace {

// Config stubs standing in for device_config_*. The real ones read a file.
std::map<std::string, std::string> g_strings;
std::map<std::string, int> g_ints;

std::string device_config_str(const char *section, const char *key)
{
    auto it = g_strings.find(std::string(section) + "." + key);
    return it == g_strings.end() ? std::string() : it->second;
}

int device_config_int(const char *section, const char *key, int fallback)
{
    auto it = g_ints.find(std::string(section) + "." + key);
    return it == g_ints.end() ? fallback : it->second;
}

void reset_config()
{
    g_strings.clear();
    g_ints.clear();
}

}  // namespace

#include "../src/input/trigger_effect.inl"

using namespace trigger_effect;

namespace {

// Renders a block as "25 04 01 07" so a failure prints something a person can
// compare against the capture by eye, rather than a byte index.
std::string hex_of(const uint8_t *p, size_t n)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        if (i) out += ' ';
        out += digits[p[i] >> 4];
        out += digits[p[i] & 0x0f];
    }
    return out;
}

// A report long enough to hold both blocks, zeroed.
std::vector<uint8_t> blank_report()
{
    return std::vector<uint8_t>(kL2Offset + kBlockLen + 8, 0);
}

}  // namespace

int run_trigger_effect_tests()
{
    uint8_t block[kBlockLen];

    section("trigger effect: the 2026-08-24 capture, decoded and rebuilt");
    // Shotgun: resistance from zone 2 that gives way at zone 8, at full force.
    build_weapon(block, 2, 8, 7);
    CTM_CHECK_EQ(hex_of(block, 4), std::string("25 04 01 07"));
    // Machine gun: a single short bump, zones 2 to 3.
    build_weapon(block, 2, 3, 1);
    CTM_CHECK_EQ(hex_of(block, 3), std::string("25 0c 00"));
    // ⭐ Strength goes out exactly as it is written, and the bottom of the
    // range is a real force. It used to be sent one lower, which made a
    // strength of 1 mean no resistance at all -- an effect that looks set,
    // reads as set, and cannot be felt. That cost a morning on 2026-09-10.
    build_weapon(block, 2, 3, 1);
    CTM_CHECK_EQ((int)block[3], 1);
    build_weapon(block, 2, 3, 7);
    CTM_CHECK_EQ((int)block[3], 7);

    section("trigger effect: percent to zone");
    CTM_CHECK_EQ(zone_from_percent(0), 0);
    CTM_CHECK_EQ(zone_from_percent(50), 5);
    CTM_CHECK_EQ(zone_from_percent(80), 8);
    CTM_CHECK_EQ(zone_from_percent(100), 9);
    CTM_CHECK_EQ(zone_from_percent(-40), 0);      // nonsense clamps, never wraps
    CTM_CHECK_EQ(zone_from_percent(4000), 9);

    section("trigger effect: what weapon mode cannot express");
    // Asked for a break at 10%, which is below anything weapon mode can do.
    // It lands at the shallowest real break rather than doing nothing.
    build_weapon(block, zone_from_percent(10) - 1, zone_from_percent(10), 6);
    CTM_CHECK_EQ(hex_of(block, 3), std::string("25 0c 00"));   // zones 2 and 3
    // Asked for 100%. The deepest break the ten zones can express is 9, so it
    // lands there rather than being refused.
    build_weapon(block, zone_from_percent(100) - 1, zone_from_percent(100), 6);
    CTM_CHECK_EQ(hex_of(block, 3), std::string("25 00 03"));   // zones 8 and 9
    // An end at or before the start would encode one zone twice and mean
    // nothing, so it is pushed past the start instead.
    build_weapon(block, 5, 5, 6);
    CTM_CHECK_EQ((int)(block[1] | (block[2] << 8)), (1 << 5) | (1 << 6));
    build_weapon(block, 5, 2, 6);
    CTM_CHECK_EQ((int)(block[1] | (block[2] << 8)), (1 << 5) | (1 << 6));
    // ⛔ Strength outside the range clamps INTO it, and never to zero: zero
    // is the hardware's "no resistance", so clamping there would turn a
    // mistyped number into a silent effect.
    build_weapon(block, 3, 4, 0);
    CTM_CHECK_EQ((int)block[3], 1);
    build_weapon(block, 3, 4, 99);
    CTM_CHECK_EQ((int)block[3], 7);

    section("trigger effect: off, and feedback");
    memset(block, 0xaa, kBlockLen);
    build_off(block);
    CTM_CHECK_EQ((int)block[0], 0x05);
    CTM_CHECK_EQ(hex_of(block + 1, 3), std::string("00 00 00"));   // nothing left behind

    // A wall from zone 5 down: zones 5-9 active, each at strength 6.
    build_feedback(block, 5, 6);
    CTM_CHECK_EQ((int)block[0], 0x21);
    CTM_CHECK_EQ((int)(block[1] | (block[2] << 8)), 0x03e0);
    CTM_CHECK_EQ(hex_of(block + 3, 4), std::string("00 00 db 36"));
    // ⛔ And a wall of zero force is not reachable from any setting. The
    // controller ignores such a report, so it can neither set nor clear
    // anything -- which is exactly how a whole morning was lost to it.
    build_feedback(block, 5, 0);
    CTM_CHECK(block[3] != 0 || block[4] != 0 || block[5] != 0 || block[6] != 0);

    section("trigger effect: the notch is a flat wall with a detent in it");
    // Two shapes failed on hardware before this one. A lip three steps firmer
    // felt identical to a plain wall, and a climb to the point made a deeper
    // point mean a HARDER pull rather than a further one. The wall is flat so
    // that only the detent's POSITION changes with the setting.
    build_detent_wall(block, 5, 7);
    CTM_CHECK_EQ((int)block[0], 0x21);
    CTM_CHECK_EQ((int)(block[1] | (block[2] << 8)), 0x03fe);   // zones 1-9
    CTM_CHECK_EQ(hex_of(block + 3, 4), std::string("90 a4 4b 12"));
    {
        const uint32_t forces = (uint32_t)block[3] | ((uint32_t)block[4] << 8) |
                                ((uint32_t)block[5] << 16) | ((uint32_t)block[6] << 24);
        CTM_CHECK_EQ((int)((forces >> 15) & 0x7), 7);   // the detent, at zone 5
        CTM_CHECK_EQ((int)((forces >> 12) & 0x7), 2);   // the wall before it
        CTM_CHECK_EQ((int)((forces >> 18) & 0x7), 2);   // and after it
    }
    // Moving the point moves ONLY the detent: every other zone is unchanged, so
    // the pull takes the same effort wherever it sits.
    {
        uint8_t deep[kBlockLen];
        build_detent_wall(deep, 8, 7);
        CTM_CHECK_EQ((int)(deep[1] | (deep[2] << 8)), 0x03fe);   // the same wall
        const uint32_t forces = (uint32_t)deep[3] | ((uint32_t)deep[4] << 8) |
                                ((uint32_t)deep[5] << 16) | ((uint32_t)deep[6] << 24);
        CTM_CHECK_EQ((int)((forces >> 24) & 0x7), 7);   // the detent moved to 8
        CTM_CHECK_EQ((int)((forces >> 15) & 0x7), 2);   // and zone 5 is wall again
    }
    // The contrast is the feature, so the wall must never reach the detent's
    // own force, and must never fall to silence either.
    {
        uint8_t light[kBlockLen];
        build_detent_wall(light, 5, 3);
        const uint32_t forces = (uint32_t)light[3] | ((uint32_t)light[4] << 8) |
                                ((uint32_t)light[5] << 16) | ((uint32_t)light[6] << 24);
        CTM_CHECK_EQ((int)((forces >> 15) & 0x7), 3);
        CTM_CHECK_EQ((int)((forces >> 12) & 0x7), 1);
    }
    // Zone 0 stays free, so the trigger is not heavy at rest.
    CTM_CHECK_EQ((int)(block[1] & 0x01), 0);

    section("trigger effect: the snap breaks, then returns the trigger");
    // Two forces in one byte, three bits each: the resistance you push through
    // sits low, the push back sits above it.
    build_snap(block, 2, 8, 7, 3);
    CTM_CHECK_EQ((int)block[0], 0x22);
    CTM_CHECK_EQ((int)(block[1] | (block[2] << 8)), (1 << 2) | (1 << 8));
    CTM_CHECK_EQ((int)(block[3] & 0x07), 7);          // resistance
    CTM_CHECK_EQ((int)((block[3] >> 3) & 0x07), 3);   // the snap back
    // Neither force can reach the other's bits, whatever is asked for.
    build_snap(block, 2, 8, 7, 7);
    CTM_CHECK_EQ((int)block[3], 7 | (7 << 3));
    build_snap(block, 2, 8, 1, 1);
    CTM_CHECK_EQ((int)block[3], 1 | (1 << 3));
    // ⛔ The bow keeps the DOCUMENTED zone limits where weapon does not: this
    // mode has never been seen working, so a snap that does nothing should be
    // one unknown rather than two. Zone 9 is refused, unlike a click.
    build_snap(block, 8, 9, 7, 3);
    CTM_CHECK_EQ((int)(block[1] | (block[2] << 8)), (1 << 7) | (1 << 8));
    // A force outside the range clamps into it, never to zero.
    build_snap(block, 2, 4, 0, 99);
    CTM_CHECK_EQ((int)(block[3] & 0x07), 1);
    CTM_CHECK_EQ((int)((block[3] >> 3) & 0x07), 7);

    section("trigger effect: reading the config");
    CTM_CHECK(shape_from("") == Shape::Absent);
    CTM_CHECK(shape_from("off") == Shape::Off);
    CTM_CHECK(shape_from("click") == Shape::Click);
    CTM_CHECK(shape_from("weapon") == Shape::Click);
    CTM_CHECK(shape_from("wall") == Shape::Wall);
    CTM_CHECK(shape_from("feedback") == Shape::Wall);
    CTM_CHECK(shape_from("notch") == Shape::Notch);
    CTM_CHECK(shape_from("both") == Shape::Notch);
    CTM_CHECK(shape_from("snap") == Shape::Snap);
    CTM_CHECK(shape_from("bow") == Shape::Snap);
    // ⛔ A typo must leave the trigger alone. Treating it as an effect would
    // put resistance on a trigger nobody asked to change.
    CTM_CHECK(shape_from("clik") == Shape::Absent);

    section("trigger effect: an unconfigured trigger is never claimed");
    reset_config();
    {
        std::vector<uint8_t> report = blank_report();
        CTM_CHECK_EQ((int)apply_to_report("ds5", report.data(), report.size()), 0);
        CTM_CHECK_EQ((int)report[kR2Offset], 0);     // report untouched
        CTM_CHECK(!wants_anything("ds5"));
    }

    section("trigger effect: a click on R2 only");
    reset_config();
    g_strings["ds5.trigger_r2_effect"] = "click";
    g_ints["ds5.trigger_r2_effect_at"] = 50;
    g_ints["ds5.trigger_r2_effect_strength"] = 6;
    {
        std::vector<uint8_t> report = blank_report();
        CTM_CHECK(wants_anything("ds5"));
        const uint8_t claim = apply_to_report("ds5", report.data(), report.size());
        CTM_CHECK_EQ((int)claim, (int)kClaimR2);            // L2 left alone
        // Break at zone 5, so resistance runs 4 to 5.
        CTM_CHECK_EQ((int)report[kR2Offset], 0x25);
        CTM_CHECK_EQ((int)(report[kR2Offset + 1] | (report[kR2Offset + 2] << 8)),
                     (1 << 4) | (1 << 5));
        CTM_CHECK_EQ((int)report[kR2Offset + 3], 6);
        CTM_CHECK_EQ((int)report[kL2Offset], 0);            // the other block is untouched
    }

    section("trigger effect: off clears only what we set");
    // Still holding the R2 effect from the section above.
    reset_config();
    g_strings["ds5.trigger_r2_effect"] = "off";
    {
        std::vector<uint8_t> report = blank_report();
        const uint8_t claim = apply_to_report("ds5", report.data(), report.size());
        CTM_CHECK_EQ((int)claim, (int)kClaimR2);
        CTM_CHECK_EQ((int)report[kR2Offset], 0x05);
    }
    // ⭐ And a second off does nothing at all: there is no longer an effect of
    // ours to undo, so the triggers are not claimed. This is what keeps an
    // install that never uses the feature from stamping on a game.
    {
        std::vector<uint8_t> report = blank_report();
        CTM_CHECK_EQ((int)apply_to_report("ds5", report.data(), report.size()), 0);
        CTM_CHECK(!wants_anything("ds5"));
    }

    section("trigger effect: both triggers, and a short report");
    reset_config();
    g_strings["ds5.trigger_r2_effect"] = "click";
    g_strings["ds5.trigger_l2_effect"] = "wall";
    g_ints["ds5.trigger_l2_effect_at"] = 30;
    {
        std::vector<uint8_t> report = blank_report();
        const uint8_t claim = apply_to_report("ds5", report.data(), report.size());
        CTM_CHECK_EQ((int)claim, (int)(kClaimR2 | kClaimL2));
        CTM_CHECK_EQ((int)report[kL2Offset], 0x21);
        CTM_CHECK_EQ((int)(report[kL2Offset + 1] | (report[kL2Offset + 2] << 8)), 0x03f8);
    }
    // ⛔ A report too short to hold the L2 block must be refused whole rather
    // than half-written: a claimed effect with no bytes behind it is worse
    // than no effect.
    {
        std::vector<uint8_t> shortReport(kL2Offset + 2, 0);
        CTM_CHECK_EQ((int)apply_to_report("ds5", shortReport.data(), shortReport.size()), 0);
        CTM_CHECK_EQ((int)shortReport[kR2Offset], 0);
    }

    section("trigger effect: sections do not leak into each other");
    reset_config();
    g_strings["ds5.trigger_r2_effect"] = "click";
    g_strings["edge.trigger_r2_effect"] = "off";
    {
        std::vector<uint8_t> report = blank_report();
        apply_to_report("ds5", report.data(), report.size());
        // The Edge never had an effect set, so its off is a no-op even though
        // the DualSense section is holding one.
        std::vector<uint8_t> other = blank_report();
        CTM_CHECK_EQ((int)apply_to_report("edge", other.data(), other.size()), 0);
    }

    reset_config();
    return 0;
}
