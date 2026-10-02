// How big the settings window's icons must be, and which window may have them.
//
// ⭐⭐ WHY THIS IS ITS OWN FILE. window_icon.inl reaches into Windows on every
// line -- another program's window, icon handles, messages -- so nothing in it
// can be tested without a desktop and a browser. The two RULES below are the
// part that can be wrong SILENTLY: an icon made at the wrong size is exactly
// the blur this exists to remove, and a window matched too loosely puts our
// icon on somebody's own browser.
// ⓘ The same split window_size_rule.inl makes for the window's size.
//
// ⛔ NOTHING HERE MAY INCLUDE windows.h, or the point of the split is lost.

#pragma once

#include <cwchar>

namespace window_icon_rule {

// ⭐ AN ICON IS ASKED FOR IN PIXELS, AND THE PIXELS DEPEND ON THE SCALE OF THE
// DISPLAY THE WINDOW IS ON. Windows' two icon sizes are 32 and 16 at 96 dpi,
// which is "100%", and they grow with it: 48 and 24 at 150%, 56 and 28 at
// 175%, 80 and 40 at 250%.
//
// ⛔ NOT GetSystemMetrics(SM_CXICON). That answers for the PRIMARY display,
// and the settings window may be sitting on another one at another scale.
//
// ⚠️ EVERY SIZE THIS CAN ANSWER FOR A STOCK SCALE IS A FRAME IN THE ICON,
// drawn for that size by installer\make-icon-ds5.ps1. Take a frame out there
// and Windows quietly stretches a neighbour to fit, which is the blur again at
// that one scale and nowhere else.
inline const int kBigAt96 = 32;
inline const int kSmallAt96 = 16;
inline const unsigned kDpiAt100 = 96;

inline int scaled(int at96, unsigned dpi)
{
    // ⚠️ Zero is what Windows answers for a window that has just gone. 100%
    // is the reading that cannot produce an icon of no size.
    if (dpi == 0) dpi = kDpiAt100;
    // ⓘ Rounded, as Windows' own MulDiv rounds: 16 at 120 dpi is 20, not 19.
    return (int)(((long long)at96 * dpi + kDpiAt100 / 2) / kDpiAt100);
}

inline int big_px(unsigned dpi)   { return scaled(kBigAt96, dpi); }
inline int small_px(unsigned dpi) { return scaled(kSmallAt96, dpi); }

// ⭐ IS THIS TITLE OUR APP WINDOW'S, and not a browser tab showing the page?
//
// The page puts the marker on the end of its own title when it was opened with
// ?app. In the window WE open (--app=) the page's title is the whole window
// title, so the marker is the END of it. An ordinary browser window showing
// the same page in a tab reads "... [ctm-app] - Google Chrome": the marker is
// there, and something follows it.
//
// ⛔ STRICTER THAN FINDING THE WINDOW, ON PURPOSE. Finding it asks only whether
// the marker is anywhere in the title. An icon put on someone's own browser
// window is a fault they would see on their taskbar for as long as this
// listener ran, so here the marker has to be the last thing in the title.
inline bool is_app_window_title(const wchar_t *title, const wchar_t *marker)
{
    if (title == nullptr || marker == nullptr) return false;
    const size_t t = wcslen(title), m = wcslen(marker);
    if (m == 0 || t < m) return false;
    return wcscmp(title + (t - m), marker) == 0;
}

} // namespace window_icon_rule
