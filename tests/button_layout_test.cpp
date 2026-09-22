// Button layout tests: which BYTE a standard button index lives in, per pad.
//
// ⭐ WHAT THESE PROTECT, and why they exist at all. These four functions decide
// where a button is read from and, for a rebound button, which byte gets
// REWRITTEN before the game sees the report. A wrong offset does not fail --
// it quietly acts on the wrong button, or scribbles on a neighbouring field.
// Nothing downstream would report it.
//
// ⛔ AND THEY HAD NO TESTS. rebind.inl cannot be included by the test binary:
// it reaches into the overlay, the on-screen keyboard, config_move and the
// keyboard device. So the layout was split into button_layout.inl, which
// depends on nothing, precisely so this file could exist.
//
// ⓘ The DualSense rows are the REGRESSION half: they are today's behaviour
// written down, so the per-layout rewrite can be proved not to have moved a
// single DualSense bit. The Xbox rows are the NEW half, derived from
// maps/xbox_gip_usb_over_xbox_bt.map.
//
// WHAT THESE CANNOT DO. They cannot say a real Xbox pad sends what the map
// claims. That is a hardware question and it is still open -- the map's own
// header calls itself a first pass.

#include "harness.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "input/button_layout.inl"

using namespace ctmtest;
using namespace ctm_rebind;

namespace {

// A report long enough for either layout, zeroed.
std::vector<uint8_t> blank_report(size_t len)
{
    return std::vector<uint8_t>(len, 0);
}

// ⛔ THE OLD CONFIG-MODE LINES, KEPT VERBATIM from rebind.inl as the reference
// for blank_to_rest()'s DualSense half. They wrote these positions into EVERY
// gated report, whatever the pad -- which is the fault on an Xbox one.
void old_config_blank(uint8_t *data, bool passOptions)
{
    data[1] = data[2] = data[3] = data[4] = 0x80;   // LX LY RX RY
    data[5] = data[6] = 0x00;                       // L2 R2 analog
    data[9] = passOptions ? static_cast<uint8_t>(data[9] & 0x20) : 0x00;
    data[10] = static_cast<uint8_t>(data[10] & ~0x07);   // PS, touchpad, mute
    data[8] = 0x08;                                 // faces clear, hat centred
}

}  // namespace

int run_button_layout_tests()
{
    const Layout *ds5 = layout_for("ds5");
    const Layout *edge = layout_for("ds5_edge");
    const Layout *ds4 = layout_for("ds4");
    const Layout *xbox = layout_for("xbox");

    section("layout: a pad resolves to a layout, and an unknown one to none");
    {
        CTM_CHECK(ds5 != nullptr);
        CTM_CHECK(edge != nullptr);
        CTM_CHECK(xbox != nullptr);
        // An Edge reads the same bytes as a DualSense.
        CTM_CHECK(ds5 == edge);
        // ✅ A DS4 HAS ITS OWN LAYOUT NOW. This line used to assert that it got
        // the DualSense's, with a note saying it should fail the day someone
        // corrected that. 2026-09-14 was the day.
        CTM_CHECK(ds4 != nullptr);
        CTM_CHECK(ds4 != ds5);
        CTM_CHECK(layout_for("puck") == nullptr);
        CTM_CHECK(layout_for("") == nullptr);
        CTM_CHECK(layout_for(nullptr) == nullptr);
    }

    section("layout: the DualSense bits are exactly where they always were");
    {
        // ⛔ This is the regression guard for the rewrite. Every one of these
        // was read off kDs5Spots before the layout split.
        std::vector<uint8_t> r = blank_report(11);
        r[8] = 0x20;                       // cross
        CTM_CHECK(is_pressed(*ds5, r.data(), r.size(), kBtnFaceDown));
        CTM_CHECK(!is_pressed(*ds5, r.data(), r.size(), kBtnFaceRight));

        r = blank_report(11);
        r[9] = 0x01;                       // L1
        CTM_CHECK(is_pressed(*ds5, r.data(), r.size(), kBtnL1));

        r = blank_report(11);
        r[10] = 0x01;                      // PS / home
        CTM_CHECK(is_pressed(*ds5, r.data(), r.size(), kBtnHome));
    }

    section("layout: the DualSense d-pad is a hat, and diagonals hold two ways");
    {
        std::vector<uint8_t> r = blank_report(11);
        r[8] = 8;                          // centred
        CTM_CHECK(!is_pressed(*ds5, r.data(), r.size(), kBtnDpadUp));

        r[8] = 0;                          // N
        CTM_CHECK(is_pressed(*ds5, r.data(), r.size(), kBtnDpadUp));
        CTM_CHECK(!is_pressed(*ds5, r.data(), r.size(), kBtnDpadRight));

        r[8] = 1;                          // NE -- up AND right
        CTM_CHECK(is_pressed(*ds5, r.data(), r.size(), kBtnDpadUp));
        CTM_CHECK(is_pressed(*ds5, r.data(), r.size(), kBtnDpadRight));

        // ⭐ Clearing one direction of a diagonal must leave the OTHER held. A
        // hat cannot express "up released, right still down" as a bitmask can,
        // so it steps to the remaining pure direction instead of centring.
        clear_button(*ds5, r.data(), r.size(), kBtnDpadUp);
        CTM_CHECK(!is_pressed(*ds5, r.data(), r.size(), kBtnDpadUp));
        CTM_CHECK(is_pressed(*ds5, r.data(), r.size(), kBtnDpadRight));

        // Clearing the only direction centres it.
        r[8] = 0;
        clear_button(*ds5, r.data(), r.size(), kBtnDpadUp);
        CTM_CHECK_EQ(static_cast<int>(r[8] & 0x0f), 8);

        // ⛔ The high nibble is not ours and must survive the write.
        r[8] = static_cast<uint8_t>(0xA0 | 0);   // N, with face bits set above
        clear_button(*ds5, r.data(), r.size(), kBtnDpadUp);
        CTM_CHECK_EQ(static_cast<int>(r[8] & 0xf0), 0xA0);
    }

    section("layout: clearing a DualSense button clears only its own bit");
    {
        std::vector<uint8_t> r = blank_report(11);
        r[9] = 0xFF;
        clear_button(*ds5, r.data(), r.size(), kBtnL1);   // 0x01
        CTM_CHECK_EQ(static_cast<int>(r[9]), 0xFE);
    }

    section("layout: the DS4 buttons sit in bytes 5, 6 and 7");
    {
        std::vector<uint8_t> r = blank_report(16);
        r[5] = 0x20;                       // cross
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnFaceDown));
        CTM_CHECK(!is_pressed(*ds4, r.data(), r.size(), kBtnFaceRight));

        r = blank_report(16);
        r[5] = 0x80;                       // triangle -- the TOP face button
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnFaceUp));
        r[5] = 0x10;                       // square -- the LEFT one
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnFaceLeft));

        r = blank_report(16);
        r[6] = 0x01;                       // L1
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnL1));
        r[6] = 0x80;                       // R3
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnR3));
        r[6] = 0x10;                       // share, which is select
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnSelect));

        r = blank_report(16);
        r[7] = 0x01;                       // PS / home
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnHome));
    }

    section("layout: the DS4 d-pad is a hat in the low nibble of byte 5");
    {
        std::vector<uint8_t> r = blank_report(16);
        r[5] = 8;                          // centred
        CTM_CHECK(!is_pressed(*ds4, r.data(), r.size(), kBtnDpadUp));
        CTM_CHECK(!is_pressed(*ds4, r.data(), r.size(), kBtnDpadLeft));

        r[5] = 0;                          // up
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnDpadUp));
        r[5] = 1;                          // up-right holds BOTH
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnDpadUp));
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnDpadRight));

        // ⭐ The face buttons share this byte, so a held direction must survive
        // a face press and vice versa.
        r[5] = static_cast<uint8_t>(2 | 0x20);   // right, plus cross
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnDpadRight));
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnFaceDown));
    }

    section("layout: the DS4 counter in byte 7 is not a home button");
    {
        // ⛔⛔ THE EXACT FAULT THE OLD TABLE CAUSED, in one check. A DS4 read
        // with the DualSense layout put home on byte 10 -- a DS4 timestamp --
        // and byte 7's top six bits are a counter that advances every report.
        // Either way a home button fires continuously. Neither may happen here.
        std::vector<uint8_t> r = blank_report(16);
        r[7] = 0xFC;                       // counter at maximum, PS bit clear
        CTM_CHECK(!is_pressed(*ds4, r.data(), r.size(), kBtnHome));
        r[7] = 0xFD;                       // same counter, PS bit set
        CTM_CHECK(is_pressed(*ds4, r.data(), r.size(), kBtnHome));
    }

    section("layout: a DS4 trigger pull is not a face button");
    {
        // ⭐ WHY THE OLD MISTAKE WAS INVISIBLE RATHER THAN OBVIOUS. The two
        // pads' fields overlap: byte 8 is an analog trigger on a DS4 and the
        // face buttons on a DualSense. So pulling L2 on a DS4 looked like
        // pressing face buttons, and no button was reported wrong until then.
        std::vector<uint8_t> r = blank_report(16);
        r[5] = 8;                          // hat centred, no faces
        r[8] = 0x80;                       // L2 pulled halfway
        CTM_CHECK(!is_pressed(*ds4, r.data(), r.size(), kBtnFaceUp));
        CTM_CHECK(!is_pressed(*ds4, r.data(), r.size(), kBtnFaceDown));
        // The DualSense table, on the very same bytes, disagrees -- which is
        // the bug, written down.
        CTM_CHECK(is_pressed(*ds5, r.data(), r.size(), kBtnFaceUp));
    }

    section("layout: a DS4 blanked to rest matches what real hardware sends");
    {
        // ✅ Measured: 1,468,405 reports from a wired DS4 were every one of
        // them [5]=0x08 and [6]=0x00 while nothing was touched.
        std::vector<uint8_t> r(16, 0xFF);
        blank_to_rest(*ds4, r.data(), r.size(), false);
        CTM_CHECK(r[5] == 0x08);           // hat centred, faces clear
        CTM_CHECK(r[6] == 0x00);           // shoulders, start, stick clicks
        CTM_CHECK(r[1] == 0x80 && r[2] == 0x80 && r[3] == 0x80 && r[4] == 0x80);
        CTM_CHECK(r[8] == 0x00 && r[9] == 0x00);   // analog L2 and R2
        CTM_CHECK((r[7] & 0x03) == 0x00);  // PS and touchpad-click cleared
        // ⛔ AND THE COUNTER IS LEFT ALONE. It is not a button, and blanking it
        // would be writing over a field the game may be counting on.
        CTM_CHECK((r[7] & 0xFC) == 0xFC);
    }

    section("layout: the Xbox buttons sit in bytes 4 and 5, behind the GIP header");
    {
        std::vector<uint8_t> r = blank_report(48);
        r[4] = 0x10;                       // A
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnFaceDown));
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnFaceRight));

        r = blank_report(48);
        r[4] = 0x80;                       // Y -- the TOP face button
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnFaceUp));

        r = blank_report(48);
        r[5] = 0x10;                       // LB
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnL1));
        r[5] = 0x80;                       // RS
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnR3));
    }

    section("layout: View and Menu are not swapped");
    {
        // ⚠️ THE ONE MOST LIKELY TO BE WRONG. The map CROSSES these over:
        // Bluetooth View is 0x04 and becomes 0x08; Menu is 0x08 and becomes
        // 0x04. Reading the source masks instead of the destination masks would
        // swap them, and nothing else in the system would notice.
        std::vector<uint8_t> r = blank_report(48);
        r[4] = 0x08;                       // View -> select
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnSelect));
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnStart));

        r[4] = 0x04;                       // Menu -> start
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnStart));
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnSelect));
    }

    section("layout: the Xbox d-pad is four bits, so diagonals are independent");
    {
        std::vector<uint8_t> r = blank_report(48);
        r[5] = 0x01 | 0x08;                // up AND right, both bits at once
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnDpadUp));
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnDpadRight));

        // ⭐ Unlike the hat, clearing one leaves the other exactly as it was --
        // no stepping, no centring.
        clear_button(*xbox, r.data(), r.size(), kBtnDpadUp);
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnDpadUp));
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnDpadRight));

        // ⛔ And the bumpers share byte 5 with the d-pad. Clearing a direction
        // must not disturb them.
        r = blank_report(48);
        r[5] = 0x01 | 0x10;                // up + LB
        clear_button(*xbox, r.data(), r.size(), kBtnDpadUp);
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnL1));
    }

    section("layout: a button the pad does not have is never pressed");
    {
        // ⛔ Guide has no byte in this report at all. A whole report of 0xFF must
        // still report it as up, because "absent" is not "look at byte 0".
        // ⓘ The triggers were checked here too while they were absent. They have
        // no bit, but since 2026-09-15 they press by travel -- see the trigger
        // sections at the end of this file.
        std::vector<uint8_t> r(48, 0xFF);
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnHome));

        // And clearing it must not write anywhere.
        std::vector<uint8_t> before = r;
        clear_button(*xbox, r.data(), r.size(), kBtnHome);
        CTM_CHECK(std::memcmp(r.data(), before.data(), r.size()) == 0);
    }

    section("layout: a short report is refused rather than read past");
    {
        // ⓘ Each layout declares the shortest report its own spots can be read
        // from, so a truncated one is refused per pad.
        CTM_CHECK_EQ(static_cast<int>(ds5->minLength), 11);
        CTM_CHECK_EQ(static_cast<int>(xbox->minLength), 6);

        std::vector<uint8_t> tiny = blank_report(5);
        CTM_CHECK(!is_pressed(*xbox, tiny.data(), tiny.size(), kBtnDpadUp));
        CTM_CHECK(!is_pressed(*ds5, tiny.data(), tiny.size(), kBtnHome));

        // An out-of-range index is refused too, rather than indexing the table.
        std::vector<uint8_t> r = blank_report(48);
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), -1));
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kButtonCount));
    }

    section("last press: any button at all counts, on every pad (T-240)");
    {
        // ⭐⭐ THE PREDICATE THE LEGEND TURNS ON. rebind.inl records WHICH
        // device pressed something, so the settings page can show the legend
        // for the pad in the hand, and it asks exactly this of every report.
        //
        // ⛔ Mirrored here rather than called, for the reason at the top of
        // this file: rebind.inl cannot be included by the test binary. ⚠️ If
        // the loop in apply() changes, change this one with it.
        auto any_pressed = [](const Layout &lay, const uint8_t *data, size_t len) {
            for (int i = 0; i < kButtonCount; ++i) {
                if (is_pressed(lay, data, len, i)) return true;
            }
            return false;
        };

        // ⓘ A PAD AT REST IS NOT PRESSING ANYTHING. The case that matters
        // most: it runs on every report from every idle pad, and a false
        // positive here would freeze the legend on whichever pad is plugged in.
        for (const Layout *lay : { ds5, ds4, xbox }) {
            std::vector<uint8_t> rest = blank_report(48);
            blank_to_rest(*lay, rest.data(), rest.size(), false);
            CTM_CHECK(!any_pressed(*lay, rest.data(), rest.size()));
        }

        // ⭐ EVERY BUTTON COUNTS, not the ten the page navigates with -- a
        // trigger, a stick click or Options is a press (rhoquinn8217 asked for
        // "the input of the last controller press"). ⓘ The plain bit spots
        // here; hats and trigger travel have their own sections above.
        int covered = 0;
        for (const Layout *lay : { ds5, ds4, xbox }) {
            for (int i = 0; i < kButtonCount; ++i) {
                const BitSpot &spot = lay->spots[i];
                if (spot.how != kSpotBit) continue;
                std::vector<uint8_t> r = blank_report(48);
                blank_to_rest(*lay, r.data(), r.size(), false);
                r[spot.byteIndex] = static_cast<uint8_t>(r[spot.byteIndex] | spot.mask);
                CTM_CHECK(any_pressed(*lay, r.data(), r.size()));
                ++covered;
            }
        }
        // ⚠️ A guard on the guard: if the spot tables were ever emptied the
        // loop above would pass by doing nothing at all.
        CTM_CHECK(covered > 20);

        // ⛔ A truncated report is refused rather than read past, so it can
        // never report a phantom press.
        std::vector<uint8_t> tiny = blank_report(4);
        CTM_CHECK(!any_pressed(*xbox, tiny.data(), tiny.size()));
        CTM_CHECK(!any_pressed(*ds5, tiny.data(), tiny.size()));
    }

    section("config mode rest: a DualSense report comes out exactly as the old lines left it");
    {
        // ⛔ The regression guard for the rewrite: every byte of every report,
        // against the old lines kept above. Byte 8 runs through all 256 values,
        // so every hat ordinal -- the out-of-range ones included -- and every
        // face combination is covered, each with Options kept and not.
        // ⓘ The on-screen keyboard's blank_report() (overlay_window.inl) wrote
        // these same lines with Options not kept, and now calls
        // blank_to_rest(..., false) -- so the pass == 0 half pins it too.
        const uint8_t b9s[] = {0x00, 0x20, 0xDF, 0xFF};
        const uint8_t b10s[] = {0x00, 0x07, 0xF8, 0xFF};
        for (int pattern = 0; pattern < 3; ++pattern) {
            for (int pass = 0; pass < 2; ++pass) {
                int mismatches = 0;
                for (int b8 = 0; b8 < 256; ++b8) {
                    for (uint8_t b9 : b9s) {
                        for (uint8_t b10 : b10s) {
                            std::vector<uint8_t> r(64);
                            for (size_t i = 0; i < r.size(); ++i) {
                                r[i] = pattern == 0 ? 0x00
                                     : pattern == 1 ? 0xFF
                                     : static_cast<uint8_t>(i * 37 + 11);
                            }
                            r[8] = static_cast<uint8_t>(b8);
                            r[9] = b9;
                            r[10] = b10;
                            std::vector<uint8_t> expected = r;
                            old_config_blank(expected.data(), pass != 0);
                            blank_to_rest(*ds5, r.data(), r.size(), pass != 0);
                            if (r != expected) ++mismatches;
                        }
                    }
                }
                CTM_CHECK_EQ(mismatches, 0);
            }
        }
    }

    section("config mode rest: an Xbox report keeps its GIP header and rests at its own offsets");
    {
        // A 0x20 report's shape: header, buttons, triggers, sticks, then bytes
        // this hook has no business with.
        const uint8_t header[4] = {0x20, 0x00, 0x2a, 0x2c};
        std::vector<uint8_t> r(48, 0x5A);
        std::memcpy(r.data(), header, sizeof(header));
        r[4] = 0xFC;  r[5] = 0xFF;         // every mapped button
        r[6] = 0xFF;  r[7] = 0x03;         // LT full
        r[8] = 0xFF;  r[9] = 0x03;         // RT full
        r[10] = 0xFF; r[11] = 0x7F;        // LX full right
        r[12] = 0x00; r[13] = 0x80;        // LY full the other way
        r[14] = 0x34; r[15] = 0x12;        // RX
        r[16] = 0xCD; r[17] = 0xAB;        // RY

        // ⛔ What the old lines did to it, written down so this section fails
        // loudly if they come back: the flags gain the fragment bit, and the
        // length byte gains a continuation bit.
        std::vector<uint8_t> old = r;
        old_config_blank(old.data(), false);
        CTM_CHECK_EQ(static_cast<int>(old[1]), 0x80);
        CTM_CHECK_EQ(static_cast<int>(old[3]), 0x80);

        blank_to_rest(*xbox, r.data(), r.size(), false);
        CTM_CHECK(std::memcmp(r.data(), header, sizeof(header)) == 0);
        CTM_CHECK_EQ(static_cast<int>(r[4]), 0x00);
        CTM_CHECK_EQ(static_cast<int>(r[5]), 0x00);
        int notAtRest = 0;
        for (size_t i = 6; i <= 17; ++i) if (r[i] != 0) ++notAtRest;
        CTM_CHECK_EQ(notAtRest, 0);
        int touched = 0;
        for (size_t i = 18; i < r.size(); ++i) if (r[i] != 0x5A) ++touched;
        CTM_CHECK_EQ(touched, 0);
    }

    section("config mode rest: keeping Options holds back Menu only, and only if it is down");
    {
        std::vector<uint8_t> r = blank_report(48);
        r[4] = 0xFC;                       // A B X Y View Menu
        r[5] = 0xFF;
        blank_to_rest(*xbox, r.data(), r.size(), true);
        CTM_CHECK_EQ(static_cast<int>(r[4]), 0x04);   // Menu is the Xbox Options
        CTM_CHECK_EQ(static_cast<int>(r[5]), 0x00);

        // ⓘ Keeping is not pressing.
        r = blank_report(48);
        r[4] = 0xF8;                       // everything but Menu
        blank_to_rest(*xbox, r.data(), r.size(), true);
        CTM_CHECK_EQ(static_cast<int>(r[4]), 0x00);
    }

    section("config mode rest: nothing is written past the report's length");
    {
        // ⓘ An Xbox pad's rest runs reach past its minLength, so each byte is
        // checked on its own. A 12-byte report inside a 48-byte buffer must leave
        // byte 12 onwards alone.
        std::vector<uint8_t> buf(48, 0xEE);
        blank_to_rest(*xbox, buf.data(), 12, false);
        CTM_CHECK_EQ(static_cast<int>(buf[11]), 0x00);
        int past = 0;
        for (size_t i = 12; i < buf.size(); ++i) if (buf[i] != 0xEE) ++past;
        CTM_CHECK_EQ(past, 0);

        std::vector<uint8_t> small(16, 0xEE);
        blank_to_rest(*ds5, small.data(), 6, false);
        past = 0;
        for (size_t i = 6; i < small.size(); ++i) if (small[i] != 0xEE) ++past;
        CTM_CHECK_EQ(past, 0);
    }

    // ---- Beyond the buttons: sticks, triggers, motion, touchpad -----------------

    section("layout: every pad's sensor and touch offsets are the verified ones");
    {
        // DualSense: the numbers every hook hardcoded before they asked a layout.
        CTM_CHECK(ds5->sticks.format == kAxisU8 && ds5->sticks.lx == 1 && ds5->sticks.ry == 4 &&
                  !ds5->sticks.upIsPositive);
        CTM_CHECK(ds5->triggers.format == kTriggerU8 && ds5->triggers.l2 == 5 && ds5->triggers.r2 == 6);
        CTM_CHECK(ds5->triggers.statusR2 == 42 && ds5->triggers.statusL2 == 43);
        CTM_CHECK(ds5->motion.present && ds5->motion.gyroPitch == 16 && ds5->motion.accelZ == 26);
        CTM_CHECK(ds5->touch.present && ds5->touch.finger1 == 33 && ds5->touch.finger2 == 37);
        CTM_CHECK(ds5->touch.clickByte == 10 && ds5->touch.clickMask == 0x02);
        // DS4: from the Linux driver's struct, confirmed on a real pad.
        CTM_CHECK(ds4->triggers.l2 == 8 && ds4->triggers.r2 == 9 && ds4->triggers.statusR2 < 0);
        CTM_CHECK(ds4->motion.gyroPitch == 13 && ds4->motion.gyroYaw == 15 && ds4->motion.gyroRoll == 17);
        CTM_CHECK(ds4->motion.accelX == 19 && ds4->motion.accelY == 21 && ds4->motion.accelZ == 23);
        CTM_CHECK(ds4->touch.finger1 == 35 && ds4->touch.finger2 == 39);
        CTM_CHECK(ds4->touch.older[0] == 44 && ds4->touch.older[3] == 57);
        CTM_CHECK(ds4->touch.clickByte == 7 && ds4->touch.clickMask == 0x02);
        // Xbox: 16-bit sticks with Y inverted by the map; no motion, no touchpad.
        CTM_CHECK(xbox->sticks.format == kAxisS16 && xbox->sticks.lx == 10 && xbox->sticks.ry == 16 &&
                  xbox->sticks.upIsPositive);
        CTM_CHECK(xbox->triggers.format == kTriggerU16 && xbox->triggers.fullScale == 1023);
        CTM_CHECK(!xbox->motion.present && !xbox->touch.present);
        CTM_CHECK(!has_digital_triggers(*xbox) && has_digital_triggers(*ds4) && has_digital_triggers(*ds5));
        // Minimum lengths match the checks the hooks made before.
        CTM_CHECK_EQ(static_cast<int>(motion_min_len(*ds5)), 28);   // gyro_mouse: len < 28
        CTM_CHECK_EQ(static_cast<int>(touch_min_len(*ds5)), 41);    // touch: len <= 40
        CTM_CHECK_EQ(static_cast<int>(stick_min_len(*ds5)), 5);     // stick: len <= 4
        CTM_CHECK_EQ(static_cast<int>(stick_min_len(*xbox)), 18);
    }

    section("layout: a DualSense's mouse-exclusive blanking is byte-for-byte the old lines");
    {
        // ⛔ The hook these replaced wrote exactly this. Every byte must match for
        // every input, or a DualSense would have changed under a DS4 change.
        std::vector<uint8_t> seed(64);
        for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(0x5A + i * 7);

        std::vector<uint8_t> want = seed;
        for (size_t i = 16; i <= 27; ++i) want[i] = 0;                // gyro and accel
        for (size_t i = 33; i <= 40; i += 4) {                       // both touch points
            want[i] = 0x80; want[i + 1] = 0; want[i + 2] = 0; want[i + 3] = 0;
        }
        want[10] = static_cast<uint8_t>(want[10] & ~0x02);           // the click
        want[3] = 0x80; want[4] = 0x80;                              // right stick
        want[1] = 0x80; want[2] = 0x80;                              // left stick

        std::vector<uint8_t> got = seed;
        blank_motion(*ds5, got.data(), got.size());
        blank_touch(*ds5, got.data(), got.size());
        blank_stick(*ds5, got.data(), got.size(), false);
        blank_stick(*ds5, got.data(), got.size(), true);
        int differ = 0;
        for (size_t i = 0; i < got.size(); ++i) if (got[i] != want[i]) ++differ;
        CTM_CHECK_EQ(differ, 0);
    }

    // A DS4 USB report at rest, read off a real wired pad on 2026-09-15.
    const uint8_t kDs4Rest[64] = {
        0x01, 0x7c, 0x80, 0x85, 0x81, 0x08, 0x00, 0xe4, 0x00, 0x00, 0x85, 0x95, 0x16, 0xfd, 0xff, 0x02,
        0x00, 0xfd, 0xff, 0xa5, 0xff, 0x7b, 0x1f, 0x79, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1b, 0x00,
        0x00, 0x01, 0x8b, 0xa4, 0x26, 0xf0, 0x22, 0xa2, 0x84, 0x60, 0x16, 0x00, 0x80, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
    };

    section("layout: a real DS4 at rest reads as a pad at rest");
    {
        MotionSample m;
        CTM_CHECK(read_motion(*ds4, kDs4Rest, sizeof(kDs4Rest), &m));
        // ✅ Gravity: -91, 8059, 2169 -- about 1.02 g at 8192 per g.
        const double ax = m.accelX, ay = m.accelY, az = m.accelZ;
        const double g = std::sqrt(ax * ax + ay * ay + az * az) / 8192.0;
        CTM_CHECK(g > 0.95 && g < 1.10);
        CTM_CHECK(m.gyroPitch > -10 && m.gyroPitch < 10);
        CTM_CHECK(m.gyroYaw > -10 && m.gyroYaw < 10);
        CTM_CHECK(m.gyroRoll > -10 && m.gyroRoll < 10);
        // No finger, no press, no chord -- and the packet count is not a finger.
        CTM_CHECK(!two_fingers_down(*ds4, kDs4Rest, sizeof(kDs4Rest)));
        CTM_CHECK(touch_finger_up(*ds4, kDs4Rest, sizeof(kDs4Rest), 0));
        CTM_CHECK(touch_finger_up(*ds4, kDs4Rest, sizeof(kDs4Rest), 1));
        CTM_CHECK(!touch_pressed(*ds4, kDs4Rest, sizeof(kDs4Rest)));
        CTM_CHECK_EQ(trigger_travel(*ds4, kDs4Rest, sizeof(kDs4Rest), true), 0);
        // ⛔ Read with a DualSense's layout the same pad looks busy: a finger down
        // for good, and the PS button pressed.
        CTM_CHECK(touch_finger_down(*ds5, kDs4Rest, sizeof(kDs4Rest), 0));
        CTM_CHECK(is_pressed(*ds5, kDs4Rest, sizeof(kDs4Rest), kBtnHome));
    }

    section("layout: a DS4's chord needs both fingers of the newest packet");
    {
        std::vector<uint8_t> r(kDs4Rest, kDs4Rest + 64);
        r[35] = 0x10;                                // finger 1 down
        CTM_CHECK(!two_fingers_down(*ds4, r.data(), r.size()));
        r[39] = 0x11;                                // finger 2 down
        CTM_CHECK(two_fingers_down(*ds4, r.data(), r.size()));
        CTM_CHECK(!two_fingers_down(*ds4, r.data(), 39));   // too short to hold finger 2's byte
    }

    section("layout: a DS4's touch blanking reaches every packet and spares the counter");
    {
        std::vector<uint8_t> r(kDs4Rest, kDs4Rest + 64);
        r[35] = 0x10; r[39] = 0x11; r[44] = 0x12; r[57] = 0x13;   // fingers everywhere
        r[7] = static_cast<uint8_t>(r[7] | 0x02);                 // pressed in
        blank_touch(*ds4, r.data(), r.size());
        CTM_CHECK_EQ(static_cast<int>(r[35]), 0x80);
        CTM_CHECK_EQ(static_cast<int>(r[39]), 0x80);
        CTM_CHECK_EQ(static_cast<int>(r[44]), 0x80);
        CTM_CHECK_EQ(static_cast<int>(r[57]), 0x80);
        CTM_CHECK_EQ(static_cast<int>(r[7] & 0x02), 0);
        CTM_CHECK_EQ(static_cast<int>(r[7] & 0xfc), 0xe4);       // counter untouched
        CTM_CHECK_EQ(static_cast<int>(r[33]), 0x01);             // count untouched
        // Motion blanking lands on [13..24] and nowhere else.
        std::vector<uint8_t> m(64, 0x77);
        blank_motion(*ds4, m.data(), m.size());
        int zeroed = 0;
        for (size_t i = 0; i < m.size(); ++i) if (m[i] == 0) ++zeroed;
        CTM_CHECK_EQ(zeroed, 12);
        CTM_CHECK_EQ(static_cast<int>(m[12]), 0x77);
        CTM_CHECK_EQ(static_cast<int>(m[13]), 0);
        CTM_CHECK_EQ(static_cast<int>(m[24]), 0);
        CTM_CHECK_EQ(static_cast<int>(m[25]), 0x77);
    }

    section("layout: an Xbox stick is signed 16-bit, and up comes back negative");
    {
        std::vector<uint8_t> r(48, 0);
        r[0] = 0x20;
        auto put = [&r](int at, int16_t v) {
            r[at] = static_cast<uint8_t>(v & 0xff);
            r[at + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
        };
        put(10, -32768);                             // LX hard left
        put(12, 32767);                              // LY hard UP -- positive on GIP
        float v = 0.0f;
        CTM_CHECK(stick_axis(*xbox, r.data(), r.size(), kStickLX, &v) && v < -0.99f);
        CTM_CHECK(stick_axis(*xbox, r.data(), r.size(), kStickLY, &v) && v < -0.99f);   // up is negative
        CTM_CHECK(stick_axis(*xbox, r.data(), r.size(), kStickRX, &v) && v == 0.0f);    // zero is centre
        CTM_CHECK(!stick_axis(*xbox, r.data(), 17, kStickRY, &v));                      // too short
        // A DualSense byte uses the formula the stick mouse always did.
        std::vector<uint8_t> d(8, 0x80);
        d[2] = 0;                                    // LY hard up
        CTM_CHECK(stick_axis(*ds5, d.data(), d.size(), kStickLY, &v) &&
                  v == (0.0f - 128.0f) / 127.0f);
        // Resting an Xbox stick writes zeros, not 0x80.
        put(14, 1234);
        put(16, -4321);
        blank_stick(*xbox, r.data(), r.size(), false);
        CTM_CHECK(r[14] == 0 && r[15] == 0 && r[16] == 0 && r[17] == 0);
    }

    section("layout: the window-steering stick is raw - 128 on a one-byte stick, and that scale on a 16-bit one");
    {
        // ⓘ window_move.inl steers with lround(stick_axis() * 127), where it used
        // to read (int)data[1] - 128. Its deadzone of 18 was tuned in those
        // steps, so the conversion must give back exactly that on every byte.
        auto steps = [](const Layout &lay, const std::vector<uint8_t> &r, int axis) {
            float f = 0.0f;
            return stick_axis(lay, r.data(), r.size(), axis, &f)
                 ? static_cast<int>(std::lround(f * 127.0f)) : 9999;
        };
        int mismatches = 0;
        for (int raw = 0; raw < 256; ++raw) {
            std::vector<uint8_t> r(64, 0x80);
            r[1] = static_cast<uint8_t>(raw);
            r[2] = static_cast<uint8_t>(255 - raw);
            if (steps(*ds5, r, kStickLX) != raw - 128) ++mismatches;
            if (steps(*ds5, r, kStickLY) != (255 - raw) - 128) ++mismatches;
            if (steps(*ds4, r, kStickLX) != raw - 128) ++mismatches;
            if (steps(*ds4, r, kStickLY) != (255 - raw) - 128) ++mismatches;
        }
        CTM_CHECK_EQ(mismatches, 0);

        // An Xbox stick: hard right is +127, hard UP is -127 like a DualSense's,
        // and a small rest drift stays inside the deadzone.
        std::vector<uint8_t> x(48, 0);
        auto put = [&x](int at, int16_t v) {
            x[at] = static_cast<uint8_t>(v & 0xff);
            x[at + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
        };
        put(10, 32767);
        put(12, 32767);
        CTM_CHECK_EQ(steps(*xbox, x, kStickLX), 127);
        CTM_CHECK_EQ(steps(*xbox, x, kStickLY), -127);
        put(10, 1500);                               // about 5% off centre
        CTM_CHECK(steps(*xbox, x, kStickLX) < 18);
        // Too short to hold the stick: nothing to steer with.
        std::vector<uint8_t> tiny(1, 0);
        CTM_CHECK_EQ(steps(*ds5, tiny, kStickLX), 9999);
    }

    section("layout: a 16-bit trigger is scaled onto the DualSense's 0..255");
    {
        std::vector<uint8_t> r(48, 0);
        r[6] = 0xff; r[7] = 0x03;                    // LT 1023, fully pulled
        r[8] = 0x00; r[9] = 0x02;                    // RT 512, about half
        CTM_CHECK_EQ(trigger_travel(*xbox, r.data(), r.size(), true), 255);
        CTM_CHECK_EQ(trigger_travel(*xbox, r.data(), r.size(), false), 127);
        CTM_CHECK_EQ(trigger_travel(*xbox, r.data(), 8, false), -1);      // too short
    }

    // ---- A trigger with no bit, as a button ---------------------------------------
    //
    // ⭐ rhoquinn8217, 2026-09-15: "A trigger past a threshold should count as a
    // press." Until then an Xbox pad's LT and RT were kSpotAbsent, so
    // stick-to-mouse -- offered to that pad -- bound two clicks nothing fired.

    // An Xbox 0x20 report with LT and RT at these raw values, 0 to 1023.
    auto xbox_triggers = [](int lt, int rt) {
        std::vector<uint8_t> r(48, 0);
        r[0] = 0x20;
        r[6] = static_cast<uint8_t>(lt & 0xff);
        r[7] = static_cast<uint8_t>((lt >> 8) & 0xff);
        r[8] = static_cast<uint8_t>(rt & 0xff);
        r[9] = static_cast<uint8_t>((rt >> 8) & 0xff);
        return r;
    };

    section("trigger as a button: an Xbox trigger presses at the threshold, and not one step before");
    {
        // ⓘ The gyro gate's number, on the DualSense's 0..255 scale: raw 120 of
        // 1023 is 29 there, and raw 121 is 30.
        CTM_CHECK_EQ(kTriggerPulledTravel, 30);
        std::vector<uint8_t> r = xbox_triggers(120, 0);
        CTM_CHECK_EQ(trigger_travel(*xbox, r.data(), r.size(), true), kTriggerPulledTravel - 1);
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnL2));
        r = xbox_triggers(121, 0);
        CTM_CHECK_EQ(trigger_travel(*xbox, r.data(), r.size(), true), kTriggerPulledTravel);
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnL2));
        r = xbox_triggers(1023, 0);
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnL2));

        // The right trigger the same, from its own bytes.
        r = xbox_triggers(0, 120);
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnR2));
        r = xbox_triggers(0, 121);
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnR2));

        // At rest, neither.
        r = xbox_triggers(0, 0);
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnL2));
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnR2));

        // ⛔ Each answers for itself: one trigger pulled is not both.
        r = xbox_triggers(1023, 0);
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnR2));
        r = xbox_triggers(0, 1023);
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnL2));

        // ⛔ And a trigger is no other button: both pulled all the way, and only
        // indices 6 and 7 read as held.
        r = xbox_triggers(1023, 1023);
        int others = 0;
        for (int i = 0; i < kButtonCount; ++i) {
            if (i != kBtnL2 && i != kBtnR2 && is_pressed(*xbox, r.data(), r.size(), i)) ++others;
        }
        CTM_CHECK_EQ(others, 0);

        // A report must hold the WHOLE value: LT needs [6] and [7], RT [8] and [9].
        CTM_CHECK(is_pressed(*xbox, r.data(), 8, kBtnL2));
        CTM_CHECK(!is_pressed(*xbox, r.data(), 7, kBtnL2));
        CTM_CHECK(!is_pressed(*xbox, r.data(), 9, kBtnR2));
    }

    section("trigger as a button: clearing an Xbox trigger zeroes its two bytes and nothing else");
    {
        // ⭐ A rebound trigger must not ALSO reach the game, and there is no bit to
        // take away -- so its whole 16-bit value goes to rest.
        std::vector<uint8_t> seed(48);
        for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(0x5A + i * 7);
        CTM_CHECK(is_pressed(*xbox, seed.data(), seed.size(), kBtnL2) &&
                  is_pressed(*xbox, seed.data(), seed.size(), kBtnR2));

        std::vector<uint8_t> r = seed;
        clear_button(*xbox, r.data(), r.size(), kBtnR2);
        int wrong = 0;
        for (size_t i = 0; i < r.size(); ++i) {
            const bool rt = (i == 8 || i == 9);
            if (rt ? r[i] != 0 : r[i] != seed[i]) ++wrong;
        }
        CTM_CHECK_EQ(wrong, 0);
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnR2));
        CTM_CHECK(is_pressed(*xbox, r.data(), r.size(), kBtnL2));       // LT untouched

        r = seed;
        clear_button(*xbox, r.data(), r.size(), kBtnL2);
        wrong = 0;
        for (size_t i = 0; i < r.size(); ++i) {
            const bool lt = (i == 6 || i == 7);
            if (lt ? r[i] != 0 : r[i] != seed[i]) ++wrong;
        }
        CTM_CHECK_EQ(wrong, 0);

        // ⛔ A report too short to hold the whole value is not written at all.
        std::vector<uint8_t> buf(48, 0xEE);
        clear_button(*xbox, buf.data(), 9, kBtnR2);
        clear_button(*xbox, buf.data(), 7, kBtnL2);
        int written = 0;
        for (uint8_t b : buf) if (b != 0xEE) ++written;
        CTM_CHECK_EQ(written, 0);

        // ⓘ The one-byte form rests one byte, for a pad whose travel is a byte.
        std::vector<uint8_t> d(16, 0xEE);
        blank_trigger(*ds5, d.data(), d.size(), false);                  // R2 at [6]
        written = 0;
        for (size_t i = 0; i < d.size(); ++i) if (d[i] != (i == 6 ? 0x00 : 0xEE)) ++written;
        CTM_CHECK_EQ(written, 0);
    }

    section("trigger as a button: an Xbox report rests exactly as it did while its triggers were absent");
    {
        // ⛔ blank_to_rest() now clears LT and RT as buttons before the rest run
        // zeroes [6..9] again. The result must not move by a byte: checked against
        // the same table with the triggers put back to kSpotAbsent, at every
        // length up to a whole report, over three fills, with Options kept and not.
        Layout before = *xbox;
        before.spots[kBtnL2] = BitSpot{ kSpotAbsent, 0, 0x00 };
        before.spots[kBtnR2] = BitSpot{ kSpotAbsent, 0, 0x00 };
        for (int pass = 0; pass < 2; ++pass) {
            int mismatches = 0;
            for (int pattern = 0; pattern < 3; ++pattern) {
                for (size_t len = 0; len <= 48; ++len) {
                    std::vector<uint8_t> want(48);
                    for (size_t i = 0; i < want.size(); ++i) {
                        want[i] = pattern == 0 ? 0x00
                                : pattern == 1 ? 0xFF
                                : static_cast<uint8_t>(i * 37 + 11);
                    }
                    std::vector<uint8_t> got = want;
                    blank_to_rest(before, want.data(), len, pass != 0);
                    blank_to_rest(*xbox, got.data(), len, pass != 0);
                    if (got != want) ++mismatches;
                }
            }
            CTM_CHECK_EQ(mismatches, 0);
        }
    }

    section("trigger as a button: a DualSense and a DS4 still read the bit, not the travel");
    {
        // ⛔ Only the Xbox rows changed. These pads report each trigger as a bit
        // AND a travel, and as buttons they read the bit alone, as they always
        // did: pulled all the way with the bit clear is not pressed, and clearing
        // one leaves its travel where it was.
        CTM_CHECK(ds5->spots[kBtnL2].how == kSpotBit && ds5->spots[kBtnL2].byteIndex == 9 &&
                  ds5->spots[kBtnL2].mask == 0x04);
        CTM_CHECK(ds5->spots[kBtnR2].how == kSpotBit && ds5->spots[kBtnR2].byteIndex == 9 &&
                  ds5->spots[kBtnR2].mask == 0x08);
        CTM_CHECK(ds4->spots[kBtnL2].how == kSpotBit && ds4->spots[kBtnL2].byteIndex == 6 &&
                  ds4->spots[kBtnL2].mask == 0x04);
        CTM_CHECK(ds4->spots[kBtnR2].how == kSpotBit && ds4->spots[kBtnR2].byteIndex == 6 &&
                  ds4->spots[kBtnR2].mask == 0x08);

        // DualSense: travel at [5] and [6] all the way, no bits -- nothing held.
        std::vector<uint8_t> r(64, 0);
        r[1] = r[2] = r[3] = r[4] = 0x80;
        r[8] = 0x08;                                  // hat centred
        r[5] = 0xFF;
        r[6] = 0xFF;
        int held = 0;
        for (int i = 0; i < kButtonCount; ++i) if (is_pressed(*ds5, r.data(), r.size(), i)) ++held;
        CTM_CHECK_EQ(held, 0);
        r[9] = 0x04;                                  // the L2 bit
        CTM_CHECK(is_pressed(*ds5, r.data(), r.size(), kBtnL2));
        CTM_CHECK(!is_pressed(*ds5, r.data(), r.size(), kBtnR2));
        clear_button(*ds5, r.data(), r.size(), kBtnL2);
        CTM_CHECK_EQ(static_cast<int>(r[9]), 0x00);
        CTM_CHECK_EQ(static_cast<int>(r[5]), 0xFF);   // the travel is not touched

        // DS4, from the real pad at rest: travel at [8] and [9] all the way.
        std::vector<uint8_t> d(kDs4Rest, kDs4Rest + 64);
        d[8] = 0xFF;
        d[9] = 0xFF;
        held = 0;
        for (int i = 0; i < kButtonCount; ++i) if (is_pressed(*ds4, d.data(), d.size(), i)) ++held;
        CTM_CHECK_EQ(held, 0);
        d[6] = 0x08;                                  // the R2 bit
        CTM_CHECK(is_pressed(*ds4, d.data(), d.size(), kBtnR2));
        clear_button(*ds4, d.data(), d.size(), kBtnR2);
        CTM_CHECK_EQ(static_cast<int>(d[6]), 0x00);
        CTM_CHECK_EQ(static_cast<int>(d[9]), 0xFF);

        // ⭐ And the answers the trigger click stands on. An Xbox trigger presses,
        // and it is still not a digital trigger the gesture could take.
        CTM_CHECK(has_digital_triggers(*ds5) && has_digital_triggers(*ds4));
        CTM_CHECK(!has_digital_triggers(*xbox));
        CTM_CHECK(trigger_click_can_take(*ds5) && trigger_click_can_take(*ds4));
        CTM_CHECK(!trigger_click_can_take(*xbox));
    }

    section("button layout: the battery byte, on reports read off the real pads");
    {
        using namespace ctm_rebind;
        const Layout *ds5 = layout_for("ds5");
        const Layout *ds4 = layout_for("ds4");
        const Layout *xbox = layout_for("xbox");

        // ⭐⭐ CAPTURED 2026-09-17 from the pads on the rooted monitor, read
        // straight off /dev/hidraw with both on a cable. These are the whole
        // 64-byte reports, not a hand-built fixture, so the offset is proved
        // against hardware rather than against a driver header.
        const uint8_t realDs5[64] = {
            0x01,0x82,0x85,0x83,0x84,0x00,0x00,0xe3,0x08,0x00,0x00,0x00,0x9a,0x08,0xf4,0x9e,
            0xfd,0xff,0xfc,0xff,0x01,0x00,0x0c,0xff,0x4e,0x20,0xcf,0x06,0x38,0x88,0x65,0x05,
            0x19,0xa2,0x22,0x17,0x3e,0x85,0x7c,0x90,0x18,0xc4,0x09,0x09,0x00,0x00,0x00,0x00,
            0x00,0x10,0xa3,0x65,0x05,0x28,0x18,0x00,0x3f,0x81,0x69,0xaf,0x4a,0xd8,0xfe,0x6c,
        };
        const uint8_t realDs4[64] = {
            0x01,0x78,0x7a,0x82,0x80,0x08,0x00,0x2c,0x00,0x00,0x17,0xb2,0x14,0x00,0x00,0x03,
            0x00,0x02,0x00,0x49,0xfa,0x83,0x1e,0x09,0xf8,0x00,0x00,0x00,0x00,0x00,0x1b,0x00,
            0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x00,0x80,0x00,0x00,0x00,
            0x80,0x00,0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x00,0x80,0x00,
        };

        // The DualSense read 0x28 at [53]: state 2, charge complete.
        BatteryReading r = battery_reading(*ds5, realDs5, sizeof(realDs5));
        CTM_CHECK(r.known);
        CTM_CHECK_EQ(r.percent, 100);
        CTM_CHECK(r.state == kBatteryFull);

        // The DS4 read 0x1b at [30]: cable attached, level 11, so full.
        r = battery_reading(*ds4, realDs4, sizeof(realDs4));
        CTM_CHECK(r.known);
        CTM_CHECK_EQ(r.percent, 100);
        CTM_CHECK(r.state == kBatteryFull);

        // ⭐ The states neither pad could show while plugged in, built by
        // changing ONLY that byte of the real report.
        uint8_t pad[64];
        std::memcpy(pad, realDs5, sizeof(pad));
        pad[53] = 0x08;                       // discharging, level 8
        r = battery_reading(*ds5, pad, sizeof(pad));
        CTM_CHECK(r.known && r.state == kBatteryDischarging);
        CTM_CHECK_EQ(r.percent, 85);
        pad[53] = 0x18;                       // charging, level 8
        r = battery_reading(*ds5, pad, sizeof(pad));
        CTM_CHECK(r.known && r.state == kBatteryCharging);
        CTM_CHECK_EQ(r.percent, 85);
        pad[53] = 0x00;                       // discharging, flat
        r = battery_reading(*ds5, pad, sizeof(pad));
        CTM_CHECK(r.known && r.percent == 5);
        pad[53] = 0x0a;                       // discharging, level 10: capped
        r = battery_reading(*ds5, pad, sizeof(pad));
        CTM_CHECK_EQ(r.percent, 100);

        // ⛔ A fault state carries no level, so it is NOT a reading. Zero and
        // "did not say" mean opposite things and must not share a value.
        for (uint8_t fault : { 0xa0, 0xb0, 0xf0 }) {
            pad[53] = static_cast<uint8_t>(fault | 0x07);
            r = battery_reading(*ds5, pad, sizeof(pad));
            CTM_CHECK(!r.known);
        }

        std::memcpy(pad, realDs4, sizeof(pad));
        pad[30] = 0x05;                       // no cable, level 5
        r = battery_reading(*ds4, pad, sizeof(pad));
        CTM_CHECK(r.known && r.state == kBatteryDischarging);
        CTM_CHECK_EQ(r.percent, 55);
        pad[30] = 0x15;                       // cable, level 5: charging
        r = battery_reading(*ds4, pad, sizeof(pad));
        CTM_CHECK(r.known && r.state == kBatteryCharging);
        CTM_CHECK_EQ(r.percent, 55);

        // ⛔ An Xbox pad says nothing anywhere, whatever the bytes hold.
        r = battery_reading(*xbox, realDs5, sizeof(realDs5));
        CTM_CHECK(!r.known);

        // ⛔ And a report too short to hold the byte is not a zero reading.
        r = battery_reading(*ds5, realDs5, 20);
        CTM_CHECK(!r.known);
        r = battery_reading(*ds5, nullptr, 64);
        CTM_CHECK(!r.known);
    }

    section("button layout: the overlay chord is taken out of a bridged Xbox report");
    {
        using namespace ctm_rebind;
        const Layout *xbox = layout_for("xbox");
        const Layout *ds5 = layout_for("ds5");

        // A GIP 0x20 report: a 4-byte header, then buttons at [4] and [5].
        uint8_t r[48] = {0x20, 0x00, 0x11, 0x2c, 0x00, 0x00};

        // ⛔ Select and Start ALONE are not touched: that is an ordinary press.
        r[4] = 0x08 | 0x04;            // View and Menu
        r[5] = 0x00;                   // no bumpers
        CTM_CHECK(!chord_gate_apply(*xbox, r, sizeof(r)));
        CTM_CHECK_EQ(static_cast<int>(r[4]), 0x0c);

        // ⭐ With BOTH bumpers held they belong to the chord, and they go.
        r[4] = 0x08 | 0x04;
        r[5] = 0x10 | 0x20;            // LB and RB
        CTM_CHECK(chord_gate_apply(*xbox, r, sizeof(r)));
        CTM_CHECK_EQ(static_cast<int>(r[4]), 0x00);
        // ⚠️ The bumpers themselves are LEFT ALONE: the game still gets them.
        CTM_CHECK_EQ(static_cast<int>(r[5]), 0x30);

        // One bumper is not the chord.
        r[4] = 0x08; r[5] = 0x10;
        CTM_CHECK(!chord_gate_apply(*xbox, r, sizeof(r)));
        CTM_CHECK_EQ(static_cast<int>(r[4]), 0x08);

        // ⛔ Face buttons and the d-pad are never touched, even mid-chord.
        r[4] = 0x10 | 0x08;            // A, and View
        r[5] = 0x10 | 0x20 | 0x01;     // both bumpers, and d-pad up
        CTM_CHECK(chord_gate_apply(*xbox, r, sizeof(r)));
        CTM_CHECK_EQ(static_cast<int>(r[4]), 0x10);
        CTM_CHECK_EQ(static_cast<int>(r[5]), 0x31);

        // ⓘ Nothing is reported as gated when there was nothing to clear.
        r[4] = 0x00; r[5] = 0x30;
        CTM_CHECK(!chord_gate_apply(*xbox, r, sizeof(r)));

        // ⛔ A DualSense is left alone. It reaches the TV by hidraw and IS
        // grabbed, so its reports do not go both ways, and this has never been
        // seen on one.
        uint8_t ds[64] = {0x01};
        ds[9] = 0x01 | 0x02 | 0x10 | 0x20;   // L1, R1, create, options
        CTM_CHECK(!chord_gate_apply(*ds5, ds, sizeof(ds)));
        CTM_CHECK_EQ(static_cast<int>(ds[9]), 0x33);

        // ⛔ A report too short to hold those bytes is left alone.
        CTM_CHECK(!chord_gate_apply(*xbox, r, 4));
        CTM_CHECK(!chord_gate_apply(*xbox, nullptr, sizeof(r)));
    }

    return 0;
}
