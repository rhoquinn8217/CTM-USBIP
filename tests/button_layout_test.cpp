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

}  // namespace

int run_button_layout_tests()
{
    const Layout *ds5 = layout_for("ds5");
    const Layout *edge = layout_for("ds5_edge");
    const Layout *xbox = layout_for("xbox");

    section("layout: a pad resolves to a layout, and an unknown one to none");
    {
        CTM_CHECK(ds5 != nullptr);
        CTM_CHECK(edge != nullptr);
        CTM_CHECK(xbox != nullptr);
        // An Edge reads the same bytes as a DualSense.
        CTM_CHECK(ds5 == edge);
        // ⓘ A DS4 is deliberately given the DualSense layout today, which is
        // KNOWN WRONG and has its own ticket. Written down so the day it is
        // corrected, this line fails and says why.
        CTM_CHECK(layout_for("ds4") == ds5);
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

    return 0;
}
