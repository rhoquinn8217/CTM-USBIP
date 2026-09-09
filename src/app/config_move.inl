// Options moves the settings page, as it moves the keyboard.
//
// ⭐ WHY THE LISTENER AND NOT THE PAGE (rhoquinn8217, 2026-09-08). The page is
// a web page in a browser window, and a web page only sees the mouse while the
// mouse is INSIDE its window. Steering with the mouse -- the point of the hold
// -- needs the system cursor, which only this side can read. So the gesture
// lives here, the page is not touched, and the two windows share one
// implementation (window_move.inl) rather than two that drift.
//
// ⭐ WHEN OPTIONS MEANS "MOVE". Only while the page is up AND in front:
// ctm_rebind_config_mode_effective() is config mode plus the [ctm-app] window
// holding the foreground. Any other time this returns false at once and the
// report goes on to whatever was going to get it. That is the same protection
// the keyboard has, where the overlay swallows the pad while it is visible --
// the page had no equivalent, and this is it.
//
// ⓘ Options was free on the page. Measured 2026-09-08: the page reads gamepad
// indices 0-5 and 12-15 and never consults 9, so nothing there loses a button.
//
// ⭐ THREE POSITIONS, LEFT, CENTRE, RIGHT, at whatever size the window is,
// the outer two held in from the edge by a margin, all centred vertically.
// ⓘ It went to two positions and back in one evening (rhoquinn8217,
// 2026-09-08): at Windows 250% scaling, the likely setting on a large TV, the
// page cannot be a side panel and is four fifths of the screen both ways, so
// centre is where it lives and left and right are a small shift to glance at
// the game. ⭐ ALWAYS THE MAIN MONITOR: this streams to one large screen, and
// the keyboard already snaps to the primary display.
// ⛔ FULLY ON SCREEN, unlike the keyboard, which is allowed to overhang by a
// third: a keyboard is parked, a page is read.
//
// ⓘ The page is a FOREIGN window, so it is moved directly with SetWindowPos.
// The keyboard posts messages to itself only because it owns its window on a
// thread of its own; nothing here needs that.

#pragma once

#include <windows.h>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace config_move {

inline window_move::Mover g_mover;

// ⭐ THREE SIZES ON R3, as the keyboard has (rhoquinn8217, 2026-09-08), as a
// share of the work area rather than pixels so 250% scaling and an unscaled 4K
// screen get the same proportion.
//
//   small   0.55 x 0.66   the page's first size, before it grew
//   medium  0.68 x 0.73   halfway
//   large   0.80 x 0.80   four fifths both ways -- the size it opens at, and
//                         the size the page's own Fit window restores
//
// ⓘ The index lives here for the life of the listener, like the keyboard's
// g_size. The page does not know it: Fit window means "large, centred" and
// says so. ⛔ Starts at LARGE, unlike the keyboard's medium, because large is
// what was on screen when the sizes were asked for.
struct SizeShare { double w; double h; };
inline const SizeShare kSizes[3] = { { 0.55, 0.66 }, { 0.68, 0.73 }, { 0.80, 0.80 } };
inline std::atomic_int g_size{2};

// ⓘ Own edge tracking for R3 rather than ctm_overlay::edge(): that table's
// slots are the keyboard's, and slot 11 is already R3 there.
inline std::mutex g_r3Mutex;
inline std::unordered_map<const void *, bool> g_r3Down;

inline bool r3_edge(const void *deviceKey, bool downNow)
{
    std::lock_guard<std::mutex> lock(g_r3Mutex);
    bool &was = g_r3Down[deviceKey];
    const bool rising = downNow && !was;
    was = downNow;
    return rising;
}

// The settings page's window, found the way focus_existing() finds it: by the
// marker the page appends to its own title when we opened it. nullptr when
// there is none.
inline HWND page_window()
{
    ctm_open_ui::FindState state;
    EnumWindows(ctm_open_ui::find_window_proc, reinterpret_cast<LPARAM>(&state));
    return state.found;
}

// The primary monitor minus the taskbar. ⓘ SPI_GETWORKAREA is the primary
// monitor by definition, which is the one rhoquinn8217 asked for.
inline RECT work_area()
{
    RECT r = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    RECT wa;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) r = wa;
    return r;
}

inline void clamp_into(const RECT &wa, int w, int h, int &x, int &y)
{
    if (x > wa.right - w)  x = wa.right - w;
    if (y > wa.bottom - h) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top)  y = wa.top;
}

inline void place(HWND hwnd, int x, int y)
{
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ⓘ The margin between the window and the screen edge on a snap, as a share
// of the work area's width so 1080p and 4K get the same proportion: 5% is
// about 96px on a 1080p screen and about 192px at 4K.
inline int snap_margin(const RECT &wa)
{
    return (wa.right - wa.left) / 20;
}

// A tap: the NEXT of left, centre, right -- judged from where the window IS,
// so one that was steered somewhere still goes somewhere sensible rather than
// to whatever a stale counter said; and centred vertically, which is what a
// snap to a defined place means.
inline void snap_next(HWND hwnd)
{
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const RECT wa = work_area();
    const int margin = snap_margin(wa);
    const int targets[3] = {
        wa.left + margin,
        wa.left + ((wa.right - wa.left) - w) / 2,
        wa.right - margin - w,
    };
    int nearest = 0;
    long best = LONG_MAX;
    for (int i = 0; i < 3; ++i) {
        const long d = labs((long)rc.left - (long)targets[i]);
        if (d < best) { best = d; nearest = i; }
    }
    int x = targets[(nearest + 1) % 3];
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    clamp_into(wa, w, h, x, y);
    place(hwnd, x, y);
}

// R3: the next size, applied about the window's CENTRE so it grows and
// shrinks in place, then clamped fully on screen.
inline void resize_next(HWND hwnd)
{
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    const int idx = (g_size.load() + 1) % 3;
    g_size.store(idx);
    const RECT wa = work_area();
    const int w = (int)((wa.right - wa.left) * kSizes[idx].w);
    const int h = (int)((wa.bottom - wa.top) * kSizes[idx].h);
    const int cx = rc.left + (rc.right - rc.left) / 2;
    const int cy = rc.top + (rc.bottom - rc.top) / 2;
    int x = cx - w / 2;
    int y = cy - h / 2;
    clamp_into(wa, w, h, x, y);
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

inline void nudge(HWND hwnd, int dx, int dy)
{
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    int x = rc.left + dx;
    int y = rc.top + dy;
    clamp_into(work_area(), w, h, x, y);
    place(hwnd, x, y);
}

// True when the report was consumed by the gesture and must be blanked, so
// that while steering nothing else on the pad acts -- the d-pad must not also
// be walking the page's settings.
inline bool handle_report(const void *deviceKey, const uint8_t *data, size_t len)
{
    if (data == nullptr || len < 11) return false;

    if (!ctm_rebind_config_mode_effective()) {
        // ⛔ Not our window in front. Forget any hold in progress, so a release
        // seen later -- with the page back in front -- is not read as a tap.
        if (g_mover.held.load()) g_mover.abandon();
        r3_edge(deviceKey, false);
        return false;
    }

    // ⓘ R3 is free on the page as Options is: it never reads index 10 or 11.
    if (r3_edge(deviceKey, ctm_overlay::button_down(data, len, 11))) {
        if (HWND h = page_window()) resize_next(h);
        // The press goes through; the page ignores R3.
    }

    const window_move::Step mv = g_mover.step(ctm_overlay::button_down(data, len, 9), data, len);

    if (mv.tapped) {
        if (HWND h = page_window()) snap_next(h);
        return false;   // the release itself goes through; the page ignores Options anyway
    }
    if (mv.holding) {
        if (mv.dx != 0 || mv.dy != 0) {
            if (HWND h = page_window()) nudge(h, mv.dx, mv.dy);
        }
        return true;
    }
    return false;
}

} // namespace config_move
