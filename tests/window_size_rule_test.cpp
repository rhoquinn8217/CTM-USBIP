// T-253: the arithmetic behind remembering a window someone resized by hand.
//
// ⓘ What these CANNOT cover: whether the window actually comes back that size.
// That needs a desktop and a real window, and it is checked by hand. What they
// cover is the part that would be wrong SILENTLY -- the rounding, the bounds,
// and the decision about whether a size was chosen by a person at all.

#include "harness.h"

#include "app/window_size_rule.inl"

using namespace ctmtest;

int run_window_size_rule_tests()
{
    using namespace ctm_window_size;

    section("window size: a share and its pixels are the same thing");
    {
        const int waW = 1920, waH = 1080;
        CTM_CHECK_EQ(from_thousandths(550, waW), 1056);
        CTM_CHECK_EQ(to_thousandths(1056, waW), 550);

        // ⛔ AND THE DIRECTION THAT IS *NOT* EXACT, stated so nobody
        // "fixes" it later. A thousandth of 1080 is 1.08 px, so a share turned
        // into whole pixels and back can land a thousandth low:
        //   660 -> 712 px -> 659
        // That is rounding to the pixel grid, not a bug, and it does not
        // matter because the stored share is always DERIVED FROM PIXELS --
        // never from another share. The invariant that counts is the one
        // below, in the other order.
        CTM_CHECK_EQ(from_thousandths(660, waH), 712);
        CTM_CHECK_EQ(to_thousandths(712, waH), 659);
    }

    section("window size: pixels survive the trip, which is the way it is used");
    {
        // ⭐ THE REAL INVARIANT. remember_size_now() measures a window in
        // pixels and stores a share; apply_size() turns that share back into
        // pixels. Those are the only two steps that ever run, and a window
        // must come back the size it was left.
        const int waW = 1920, waH = 1080;
        const int widths[] = { 400, 713, 1056, 1234, 1900 };
        const int heights[] = { 300, 517, 712, 1079 };
        for (int i = 0; i < 5; ++i) {
            const int back = from_thousandths(to_thousandths(widths[i], waW), waW);
            CTM_CHECK(back > widths[i] - 2 && back < widths[i] + 2);
        }
        for (int i = 0; i < 4; ++i) {
            const int back = from_thousandths(to_thousandths(heights[i], waH), waH);
            CTM_CHECK(back > heights[i] - 2 && back < heights[i] + 2);
        }
    }

    section("window size: rounding does not eat a window");
    {
        // ⛔ THE FAULT TRUNCATION WOULD CAUSE: a window loses a pixel on every
        // close. Invisible once, obvious after twenty, and impossible to
        // attribute to anything.
        const int waW = 1920;
        int px = 1234;
        for (int i = 0; i < 20; ++i) px = from_thousandths(to_thousandths(px, waW), waW);
        CTM_CHECK(px > 1230 && px < 1238);
    }

    section("window size: an absent work area cannot divide");
    {
        // ⚠️ work_area() can answer nonsense while a display is being
        // reconfigured, and this runs on the path that closes the window.
        CTM_CHECK_EQ(to_thousandths(800, 0), 0);
        CTM_CHECK_EQ(from_thousandths(550, 0), 0);
    }

    section("window size: what is sane enough to restore");
    {
        CTM_CHECK(share_sane(550, 660));
        CTM_CHECK(share_sane(1000, 1000));
        CTM_CHECK(!share_sane(1, 660));
        CTM_CHECK(!share_sane(1200, 660));
        CTM_CHECK(!share_sane(550, -3));
        CTM_CHECK(!share_sane(0, 0));
    }

    section("window size: a preset, or a size a person chose");
    {
        // ⭐ THE DECISION THE WHOLE TICKET TURNS ON. Call a preset "custom" and
        // the preset tables never apply again, because a custom size outranks
        // them.
        CTM_CHECK(is_preset(1056, 712, 1056, 712));
        CTM_CHECK(is_preset(1054, 714, 1056, 712));
        CTM_CHECK(!is_preset(900, 712, 1056, 712));
        CTM_CHECK(!is_preset(1056, 500, 1056, 712));
        CTM_CHECK(!is_preset(800, 500, 1056, 712));

        // The boundary, stated rather than left to chance.
        CTM_CHECK(!is_preset(1056 + kPresetSlop, 712, 1056, 712));
        CTM_CHECK(is_preset(1056 + kPresetSlop - 1, 712, 1056, 712));
    }

    return 0;
}
