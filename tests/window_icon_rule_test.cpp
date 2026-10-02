// The two rules behind the settings window's taskbar icon.
//
// ⓘ What these CANNOT cover: whether the taskbar actually shows a sharp icon.
// That needs a desktop, a browser and somebody's eyes. What they cover is the
// part that would be wrong SILENTLY -- an icon asked for at the wrong size,
// which is the blur itself, and our icon landing on a window that is not ours.

#include "harness.h"

#include "app/window_icon_rule.inl"

using namespace ctmtest;

int run_window_icon_rule_tests()
{
    using namespace window_icon_rule;

    section("window icon: the pixels Windows wants at each display scale");
    {
        // ⭐ EVERY ANSWER HERE IS A FRAME installer\make-icon-ds5.ps1 DRAWS.
        // If one of these changes, that script's size list changes with it.
        CTM_CHECK_EQ(big_px(96), 32);    CTM_CHECK_EQ(small_px(96), 16);     // 100%
        CTM_CHECK_EQ(big_px(120), 40);   CTM_CHECK_EQ(small_px(120), 20);    // 125%
        // 150%, where the blur was reported: the browser's 48 px icon was a
        // small frame stretched, and ours is the 48 px frame itself.
        CTM_CHECK_EQ(big_px(144), 48);   CTM_CHECK_EQ(small_px(144), 24);
        CTM_CHECK_EQ(big_px(168), 56);   CTM_CHECK_EQ(small_px(168), 28);    // 175%
        CTM_CHECK_EQ(big_px(192), 64);   CTM_CHECK_EQ(small_px(192), 32);    // 200%
        CTM_CHECK_EQ(big_px(216), 72);   CTM_CHECK_EQ(small_px(216), 36);    // 225%
        CTM_CHECK_EQ(big_px(240), 80);   CTM_CHECK_EQ(small_px(240), 40);    // 250%, a TV across a room
        CTM_CHECK_EQ(big_px(288), 96);   CTM_CHECK_EQ(small_px(288), 48);    // 300%
    }

    section("window icon: a scale between the stock ones rounds, it does not truncate");
    {
        // ⓘ Windows lets a custom scale be typed in. 16 at 110 dpi is 18.33,
        // and 16 at 130 dpi is 21.67: the nearer whole pixel each time.
        CTM_CHECK_EQ(small_px(110), 18);
        CTM_CHECK_EQ(small_px(130), 22);
        CTM_CHECK_EQ(big_px(110), 37);
        CTM_CHECK_EQ(big_px(130), 43);
    }

    section("window icon: a window that has just gone cannot ask for an icon of no size");
    {
        // ⚠️ GetDpiForWindow answers zero for a handle that is no longer a
        // window, and this runs four times a second against a window that
        // closes whenever someone presses Circle.
        CTM_CHECK_EQ(big_px(0), 32);
        CTM_CHECK_EQ(small_px(0), 16);
    }

    section("window icon: only the window we opened");
    {
        const wchar_t *marker = L"[ctm-app]";

        // The window --app= makes: the page's title is the whole title.
        CTM_CHECK(is_app_window_title(L"Advanced - DS5-USBIP Device Config - v0.1.0 [ctm-app]", marker));
        CTM_CHECK(is_app_window_title(L"Quick - DS5-USBIP Device Config - v0.1.0 [ctm-app]", marker));
        CTM_CHECK(is_app_window_title(L"[ctm-app]", marker));

        // ⛔ THE CASE THIS RULE EXISTS FOR. The same page in a tab of someone's
        // own browser, opened with ?app typed by hand. The marker is in the
        // title, the browser's name follows it, and that window is theirs.
        CTM_CHECK(!is_app_window_title(
            L"Advanced - DS5-USBIP Device Config - v0.1.0 [ctm-app] - Google Chrome", marker));
        CTM_CHECK(!is_app_window_title(
            L"Advanced - DS5-USBIP Device Config - v0.1.0 [ctm-app] - Microsoft Edge", marker));

        // The page before its script has run, and a tab that never had ?app.
        CTM_CHECK(!is_app_window_title(L"DS5-USBIP Device Config", marker));
        CTM_CHECK(!is_app_window_title(L"", marker));

        // A title shorter than the marker, and a marker that is only part of one.
        CTM_CHECK(!is_app_window_title(L"app]", marker));
        CTM_CHECK(!is_app_window_title(L"[ctm-app] ", marker));

        // ⚠️ Nothing to compare is "no", never a crash and never "yes": an
        // empty marker would otherwise match every window on the desktop.
        CTM_CHECK(!is_app_window_title(nullptr, marker));
        CTM_CHECK(!is_app_window_title(L"anything", nullptr));
        CTM_CHECK(!is_app_window_title(L"anything", L""));
    }

    return 0;
}
