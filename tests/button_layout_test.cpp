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
        // ⛔ Guide has no byte in this report at all, and the triggers have no
        // digital bit -- they arrive as u16 values with nothing thresholding
        // them. A whole report of 0xFF must still report all three as up,
        // because "absent" is not "look at byte 0".
        std::vector<uint8_t> r(48, 0xFF);
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnHome));
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnL2));
        CTM_CHECK(!is_pressed(*xbox, r.data(), r.size(), kBtnR2));

        // And clearing one must not write anywhere.
        std::vector<uint8_t> before = r;
        clear_button(*xbox, r.data(), r.size(), kBtnHome);
        clear_button(*xbox, r.data(), r.size(), kBtnL2);
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

    section("config mode rest: a DualSense report comes out exactly as the old lines left it");
    {
        // ⛔ The regression guard for the rewrite: every byte of every report,
        // against the old lines kept above. Byte 8 runs through all 256 values,
        // so every hat ordinal -- the out-of-range ones included -- and every
        // face combination is covered, each with Options kept and not.
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

    return 0;
}
