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

// What a GAME sends: the DualSense output report id, its claim byte, and a
// mode byte in each trigger block. Everything else zero.
std::vector<uint8_t> host_report(uint8_t claims, uint8_t r2Mode, uint8_t l2Mode)
{
    std::vector<uint8_t> r(kL2Offset + kBlockLen + 15, 0);   // 48, as on the wire
    r[kHostReportId] = kHostOutputId;
    r[kHostFlag0] = claims;
    r[kR2Offset] = r2Mode;
    r[kL2Offset] = l2Mode;
    return r;
}

// The block the settings report would carry for this section, so a test can
// say "the host's block became exactly ours" rather than re-deriving the bytes.
std::vector<uint8_t> our_block(const std::string &section, size_t offset)
{
    std::vector<uint8_t> scratch = blank_report();
    apply_to_report(section, scratch.data(), scratch.size());
    return std::vector<uint8_t>(scratch.begin() + offset,
                                scratch.begin() + offset + kBlockLen);
}

std::vector<uint8_t> block_of(const std::vector<uint8_t> &r, size_t offset)
{
    return std::vector<uint8_t>(r.begin() + offset, r.begin() + offset + kBlockLen);
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

    section("trigger effect: the snap, with its force in its own byte");
    // ⛔ SECOND ATTEMPT. The first packed both forces into byte 3, three bits
    // each. On hardware the trigger never returned and the break got HARDER as
    // the snap force rose, which is what a byte read as ONE number would do.
    // So the snap force now has byte 4 to itself.
    build_snap(block, 2, 8, 7, 3);
    CTM_CHECK_EQ((int)block[0], 0x22);
    CTM_CHECK_EQ((int)(block[1] | (block[2] << 8)), (1 << 2) | (1 << 8));
    CTM_CHECK_EQ((int)block[3], 7);   // resistance, on its own
    CTM_CHECK_EQ((int)block[4], 3);   // the snap back, on its own
    // ⭐ The point of the retry: the resistance byte must not move when only the
    // snap force changes.
    {
        uint8_t soft[kBlockLen], hard[kBlockLen];
        build_snap(soft, 2, 8, 7, 1);
        build_snap(hard, 2, 8, 7, 7);
        CTM_CHECK_EQ((int)soft[3], (int)hard[3]);
        CTM_CHECK(soft[4] != hard[4]);
    }
    // The bow keeps the documented zone limits, so 9 is refused where a click
    // would take it. One unknown at a time.
    build_snap(block, 8, 9, 7, 3);
    CTM_CHECK_EQ((int)(block[1] | (block[2] << 8)), (1 << 7) | (1 << 8));
    // Neither force clamps to zero, which is the hardware\'s "none".
    build_snap(block, 2, 4, 0, 0);
    CTM_CHECK_EQ((int)block[3], 1);
    CTM_CHECK_EQ((int)block[4], 1);

    section("trigger effect: reading the config");
    CTM_CHECK(shape_from("") == Shape::Absent);
    CTM_CHECK(shape_from("off") == Shape::Off);
    CTM_CHECK(shape_from("click") == Shape::Click);
    CTM_CHECK(shape_from("weapon") == Shape::Click);
    CTM_CHECK(shape_from("wall") == Shape::Wall);
    CTM_CHECK(shape_from("feedback") == Shape::Wall);
    CTM_CHECK(shape_from("notch") == Shape::Notch);
    CTM_CHECK(shape_from("both") == Shape::Notch);
    // ⛔ A typo must leave the trigger alone. Treating it as an effect would
    // put resistance on a trigger nobody asked to change.
    CTM_CHECK(shape_from("clik") == Shape::Absent);
    CTM_CHECK(shape_from("snap") == Shape::Snap);
    CTM_CHECK(shape_from("bow") == Shape::Snap);

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
    g_strings["ds5.right_trigger_effect"] = "click";
    g_ints["ds5.right_trigger_effect_at"] = 50;
    g_ints["ds5.right_trigger_effect_strength"] = 6;
    {
        std::vector<uint8_t> report = blank_report();
        CTM_CHECK(wants_anything("ds5"));
        const uint8_t claim = apply_to_report("ds5", report.data(), report.size());
        // Both are claimed: a config that sets one trigger owns both, so L2
        // is put into a known state rather than inheriting one.
        CTM_CHECK_EQ((int)claim, (int)(kClaimR2 | kClaimL2));
        // ⭐ 50 percent is where it GIVES WAY, so the end zone is 4 and the
        // resistance runs from 3. ⛔ This asserted zones 4 and 5 until
        // 2026-09-11, when a zone was being read as a point: the pad does not
        // report the crossing until the trigger is into the zone AFTER the end
        // one, so naming zone 5 as the end put the break at 60 and a break
        // asked for at 80 landed on the hard stop. Measured on hardware.
        CTM_CHECK_EQ((int)report[kR2Offset], 0x25);
        CTM_CHECK_EQ((int)(report[kR2Offset + 1] | (report[kR2Offset + 2] << 8)),
                     (1 << 3) | (1 << 4));
        CTM_CHECK_EQ((int)report[kR2Offset + 3], 6);
        CTM_CHECK_EQ((int)report[kL2Offset], 0x05);         // and the other is turned off
    }

    section("trigger effect: off clears the trigger every time it is asked for");
    reset_config();
    g_strings["ds5.right_trigger_effect"] = "off";
    {
        std::vector<uint8_t> report = blank_report();
        const uint8_t claim = apply_to_report("ds5", report.data(), report.size());
        // Off owns BOTH, the same as any other shape: a section with an opinion
        // about one trigger puts the other into a known state rather than
        // leaving it holding whatever came before.
        CTM_CHECK_EQ((int)claim, (int)(kClaimR2 | kClaimL2));
        CTM_CHECK_EQ((int)report[kR2Offset], 0x05);
        CTM_CHECK_EQ((int)report[kL2Offset], 0x05);
    }
    // ⛔ AND AGAIN, IDENTICALLY. This asserted the opposite until 2026-09-11 --
    // that a second off did nothing, because a record of what this process had
    // set said there was nothing left to undo. That record was keyed on the
    // CONFIG NAME, so linking a fresh config whose effect is off found no
    // record under its name and sent nothing: the trigger kept the effect the
    // previous config had given it. rhoquinn8217 caught it switching from a
    // notch to an off: *"doesn't appear to turn off the adaptive trigger
    // feeling."*
    {
        std::vector<uint8_t> report = blank_report();
        CTM_CHECK_EQ((int)apply_to_report("ds5", report.data(), report.size()),
                     (int)(kClaimR2 | kClaimL2));
        CTM_CHECK(wants_anything("ds5"));
    }
    // ⭐ What still touches nothing is a section that never mentions a trigger.
    // That is the protection against stamping on a game, and it does not need a
    // record to work: silence by default does it.
    reset_config();
    {
        std::vector<uint8_t> report = blank_report();
        CTM_CHECK_EQ((int)apply_to_report("ds5", report.data(), report.size()), 0);
        CTM_CHECK(!wants_anything("ds5"));
    }

    section("trigger effect: both triggers, and a short report");
    reset_config();
    g_strings["ds5.right_trigger_effect"] = "click";
    g_strings["ds5.left_trigger_effect"] = "wall";
    g_ints["ds5.left_trigger_effect_at"] = 30;
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

    section("trigger effect: a config that sets one trigger owns both");
    // A wall left on L2 by one config was still there after switching to a
    // config that says nothing about L2 (2026-09-10). An effect lives on the
    // controller until something changes it, so "absent means leave alone" let
    // one config's setting follow the pad into the next one.
    reset_config();
    g_strings["ds5.right_trigger_effect"] = "click";
    {
        std::vector<uint8_t> report = blank_report();
        const uint8_t claim = apply_to_report("ds5", report.data(), report.size());
        CTM_CHECK_EQ((int)claim, (int)(kClaimR2 | kClaimL2));   // both claimed
        CTM_CHECK_EQ((int)report[kR2Offset], 0x25);             // R2 as asked
        CTM_CHECK_EQ((int)report[kL2Offset], 0x05);             // L2 put to off
    }
    // The same the other way round.
    reset_config();
    g_strings["ds5.left_trigger_effect"] = "wall";
    {
        std::vector<uint8_t> report = blank_report();
        const uint8_t claim = apply_to_report("ds5", report.data(), report.size());
        CTM_CHECK_EQ((int)claim, (int)(kClaimR2 | kClaimL2));
        CTM_CHECK_EQ((int)report[kR2Offset], 0x05);
        CTM_CHECK_EQ((int)report[kL2Offset], 0x21);
    }
    // A config that asks for NOTHING still touches nothing, so an install that
    // never uses this keeps a game's own trigger effects.
    reset_config();
    {
        std::vector<uint8_t> report = blank_report();
        CTM_CHECK_EQ((int)apply_to_report("ds5", report.data(), report.size()), 0);
        CTM_CHECK_EQ((int)report[kR2Offset], 0);
        CTM_CHECK_EQ((int)report[kL2Offset], 0);
    }

    section("trigger effect: sections do not leak into each other");
    reset_config();
    g_strings["ds5.right_trigger_effect"] = "click";
    g_strings["edge.right_trigger_effect"] = "off";
    {
        std::vector<uint8_t> report = blank_report();
        apply_to_report("ds5", report.data(), report.size());
        CTM_CHECK_EQ((int)report[kR2Offset], 0x25);        // the DS5 got its click
        // ⭐ And the Edge gets what the EDGE asked for -- an off -- rather than
        // the click sitting in the other section. Each section is read on its
        // own; neither reaches into the other.
        std::vector<uint8_t> other = blank_report();
        CTM_CHECK_EQ((int)apply_to_report("edge", other.data(), other.size()),
                     (int)(kClaimR2 | kClaimL2));
        CTM_CHECK_EQ((int)other[kR2Offset], 0x05);
    }

    section("trigger effect: a section with no trigger keys is left alone");
    reset_config();
    g_strings["ds5.right_trigger_effect"] = "click";
    {
        // The Edge says nothing about triggers, so nothing is sent for it even
        // while another section is holding an effect.
        std::vector<uint8_t> other = blank_report();
        CTM_CHECK_EQ((int)apply_to_report("edge", other.data(), other.size()), 0);
        CTM_CHECK(!wants_anything("edge"));
    }

    section("trigger effect: a game turning the triggers OFF does not win");
    // ⛔ THE CASE THAT WAS REPORTED. A DualSense-aware game claims both
    // triggers and sends mode Off, which replaced the config's click -- and with
    // no break left to report, the trigger's remap stopped pressing too.
    reset_config();
    g_strings["ds5.right_trigger_effect"] = "click";
    g_strings["ds5.left_trigger_effect"] = "click";
    {
        std::vector<uint8_t> r = host_report(kClaimR2 | kClaimL2, kModeOff, kModeOff);
        const uint8_t replaced = defend_host_report("ds5", r.data(), r.size());
        CTM_CHECK_EQ((int)replaced, (int)(kClaimR2 | kClaimL2));
        CTM_CHECK(block_of(r, kR2Offset) == our_block("ds5", kR2Offset));
        CTM_CHECK(block_of(r, kL2Offset) == our_block("ds5", kL2Offset));
        CTM_CHECK_EQ((int)r[kR2Offset], (int)kModeWeapon);   // a click, not off
        // ⭐ The claim byte is left as the host sent it: it was already
        // claiming, which is exactly why it had to be answered.
        CTM_CHECK_EQ((int)r[kHostFlag0], (int)(kClaimR2 | kClaimL2));
    }

    section("trigger effect: a game's own effect on one trigger is replaced there only");
    reset_config();
    g_strings["ds5.right_trigger_effect"] = "click";
    {
        // The game sets a weapon effect on R2 and says nothing about L2.
        std::vector<uint8_t> r = host_report(kClaimR2, kModeWeapon, 0);
        r[kR2Offset + 1] = 0x0c;                               // its own zones
        const std::vector<uint8_t> hostL2 = block_of(r, kL2Offset);
        const uint8_t replaced = defend_host_report("ds5", r.data(), r.size());
        CTM_CHECK_EQ((int)replaced, (int)kClaimR2);
        CTM_CHECK(block_of(r, kR2Offset) == our_block("ds5", kR2Offset));
        // ⛔ L2 was not claimed, so it is not ours to write -- the pad keeps
        // whatever it holds, which is the config's from the settings report.
        CTM_CHECK(block_of(r, kL2Offset) == hostL2);
    }

    section("trigger effect: a report that does not claim a trigger is left alone");
    reset_config();
    g_strings["ds5.right_trigger_effect"] = "click";
    {
        // Rumble only, say. The same rule the rumble override follows in reverse:
        // if the host is not claiming the field, the field is not ours.
        std::vector<uint8_t> r = host_report(0x03, 0, 0);
        const std::vector<uint8_t> before = r;
        CTM_CHECK_EQ((int)defend_host_report("ds5", r.data(), r.size()), 0);
        CTM_CHECK(r == before);
    }

    section("trigger effect: a config that sets no effect lets the game's stand");
    // ⭐ This is what keeps the guard from costing anyone who never asked for
    // an effect: their games keep their adaptive triggers.
    reset_config();
    {
        std::vector<uint8_t> r = host_report(kClaimR2 | kClaimL2, kModeWeapon, kModeWeapon);
        const std::vector<uint8_t> before = r;
        CTM_CHECK_EQ((int)defend_host_report("ds5", r.data(), r.size()), 0);
        CTM_CHECK(r == before);
    }

    section("trigger effect: only a DualSense output report, and a whole one");
    reset_config();
    g_strings["ds5.right_trigger_effect"] = "click";
    {
        std::vector<uint8_t> notOutput = host_report(kClaimR2, kModeOff, 0);
        notOutput[kHostReportId] = 0x31;                     // a Bluetooth report id
        const std::vector<uint8_t> before = notOutput;
        CTM_CHECK_EQ((int)defend_host_report("ds5", notOutput.data(), notOutput.size()), 0);
        CTM_CHECK(notOutput == before);

        std::vector<uint8_t> shortReport = host_report(kClaimR2, kModeOff, 0);
        CTM_CHECK_EQ((int)defend_host_report("ds5", shortReport.data(),
                                             kL2Offset + kBlockLen - 1), 0);
        CTM_CHECK_EQ((int)shortReport[kR2Offset], (int)kModeOff);  // untouched
        CTM_CHECK_EQ((int)defend_host_report("ds5", nullptr, 48), 0);
    }

    section("trigger effect: a report already carrying ours reports no change");
    reset_config();
    g_strings["ds5.right_trigger_effect"] = "click";
    {
        // A game echoing the effect back must not be counted as a fight.
        std::vector<uint8_t> r = host_report(kClaimR2, 0, 0);
        const std::vector<uint8_t> ours = our_block("ds5", kR2Offset);
        std::copy(ours.begin(), ours.end(), r.begin() + kR2Offset);
        CTM_CHECK_EQ((int)defend_host_report("ds5", r.data(), r.size()), 0);
        CTM_CHECK(block_of(r, kR2Offset) == ours);
    }

    section("trigger effect: the ownership rule holds against a game too");
    reset_config();
    g_strings["ds5.right_trigger_effect"] = "click";
    {
        // The config sets R2 only, so by the ownership rule it put L2 into a
        // known state -- off -- rather than inheriting one. A game turning L2's
        // effect ON must not undo that either.
        std::vector<uint8_t> r = host_report(kClaimL2, 0, kModeWeapon);
        const uint8_t replaced = defend_host_report("ds5", r.data(), r.size());
        CTM_CHECK_EQ((int)replaced, (int)kClaimL2);
        CTM_CHECK_EQ((int)r[kL2Offset], (int)kModeOff);
    }

    reset_config();
    return 0;
}
