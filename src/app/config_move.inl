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
// ⭐ WHERE A TAP PUTS IT DEPENDS ON THE LAYOUT (rhoquinn8217, 2026-09-09).
// ADVANCED has ONE place, the centre: it is most of the screen, so left and
// right were a shuffle rather than a choice, and a tap now means "put it back
// in the middle". SIMPLE and QUICK have three, along the BOTTOM -- left,
// centre, right -- which is where a small window belongs while a game is
// running: out of the way, and reachable without crossing the screen.
// ⓘ The outer two are held in from the edge by a margin, and all three sit
// the same margin up from the bottom of the work area.
// ⭐ ALWAYS THE MAIN MONITOR: this streams to one large screen, and the
// keyboard already snaps to the primary display.
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
#include <string>
#include <thread>
#include <unordered_map>

namespace config_move {

// ⭐ One mover PER PAD (T-162): see window_move::Movers for why.
inline window_move::Movers g_movers;

// ⭐ TWO SIZES ON R3, and THREE FOR ADVANCED, as a share of the work area
// rather than pixels so 250% scaling and an unscaled 4K screen get the same
// proportion.
//
//   small   0.55 x 0.66   the page's first size, before it grew
//   medium  0.68 x 0.73   the size Advanced opens at
//   large   0.90 x 0.94   nearly the screen
//
// ⓘ Three had been one too many and Advanced went to two (rhoquinn8217,
// 2026-09-09); the large came back the same day, from the test run: at 250%
// scaling on a television the medium is CRAMPED, and Advanced is the layout
// with a whole page to show. ⛔ Simple and Quick keep their two -- they show
// one controller and one selector, and a third size would only be a bigger
// version of the same three lines.
//
// ⓘ It still OPENS at medium, and R3's index lives here for the life of the
// listener, so a large chosen once survives every close until the listener
// restarts.
struct SizeShare { double w; double h; };
inline const SizeShare kSizes[3] = { { 0.55, 0.66 }, { 0.68, 0.73 }, { 0.90, 0.94 } };
inline std::atomic_int g_size{1};

// ⭐ COMPACT HAS SIZES OF ITS OWN (rhoquinn8217, 2026-09-09): at 250% scaling on
// a large screen the compact view still needs to be sized to the room, so R3
// cycles a second table while the page is compact, with a position of its own.
//
//   medium  0.45 x 0.45   what the page opens Simple at, so the two agree
//   large   0.56 x 0.56
// ⓘ Simple keeps its medium and large (rhoquinn8217, 2026-09-09); the 0.34
// small is gone. ⛔ A quarter came off the width and a sixth off the height
// the same evening and both went straight back: Simple holds the preset
// description, which is sized by its content and not by the window.
//
// ⓘ The page says which view it is in (ui/view); the listener cannot tell by
// looking. Each switch resets that view's position to its entry size -- small
// for compact (rhoquinn8217, 2026-09-09: smallest first), large for full --
// which is exactly the size the page resizes to on the switch, so R3 always
// cycles from where the window actually is.
inline const SizeShare kCompactSizes[2] = { { 0.45, 0.45 }, { 0.56, 0.56 } };
inline std::atomic_int  g_sizeCompact{0};
inline std::atomic_bool g_compact{false};

// ⭐ QUICK HAS SIZES OF ITS OWN (rhoquinn8217, 2026-09-09), so the window
// hugs the little it shows -- the ring, a selector, a footer. ⓘ Its medium
// and large, by rhoquinn8217's naming: the half-of-Simple heights, with an
// eighth taken off the width.
//
//   medium  0.185 x 0.150   what the page opens Quick at
//   large   0.230 x 0.187
// ⓘ A third shorter than first set (rhoquinn8217, 2026-09-09): the gap above
// the footer was the window's, not the content's. The width went down a
// quarter and back up a quarter the same evening, for a button in the
// bottom-right corner.
inline const SizeShare kQuickSizes[2] = { { 0.185, 0.150 }, { 0.230, 0.187 } };
inline std::atomic_int  g_sizeQuick{0};
inline std::atomic_bool g_quick{false};

inline HWND page_window();              // all four are defined below, with the placing
inline void apply_size(HWND hwnd);
inline void place_default(HWND hwnd);
inline void place_exact(HWND hwnd, int x, int y);
inline void place_centre(HWND hwnd);

// ⭐⭐ THIS FILE IS THE MEMORY (rhoquinn8217, 2026-09-09: "when closing
// remember advance/simple/quick and size"). The window is closed and made
// again by the chord, so nothing inside it can remember anything; the LISTENER
// outlives it, and holds the layout, the size, the place and the controller
// that was on screen. The page asks on load (GET ui/view) rather than telling.
//
// ⛔ THE PAGE'S OWN localStorage IS NOT ENOUGH, and was the first attempt:
// a window killed rather than closed -- which is how this one usually ends,
// on a rebuild or a token mismatch -- can lose the last write, and a fresh
// window then starts from nothing. It stays as the fallback for a listener
// that has just started and knows nothing yet.
inline std::atomic_bool g_known{false};
inline std::mutex g_ordinalMutex;
inline std::string g_ordinal;

// ⭐ AND THE EXACT PLACE, per layout (rhoquinn8217, 2026-09-09). Three snap
// spots are what a CONTROLLER can reach; a mouse puts the window wherever it
// likes, and coming back to a snap spot instead would be the same surprise as
// coming back at the wrong size. ⓘ Position only -- the size belongs to R3's
// index, and restoring a stale size would fight it.
//
// ⓘ Written from what we are ABOUT to set rather than read back afterwards:
// SetWindowPos on another process's window need not have landed by the time
// GetWindowRect answers, and a memory one step behind is worse than none.
// The one read-back is remember_pos_now(), for a window someone dragged by
// its title bar -- nothing is in flight then, and it is the last thing done
// before the window is closed.
struct Pos { int x = 0; int y = 0; bool have = false; };
inline std::mutex g_posMutex;
inline Pos g_posAdvanced, g_posSimple, g_posQuick;

inline Pos &pos_slot()
{
    if (!g_compact.load()) return g_posAdvanced;
    return g_quick.load() ? g_posQuick : g_posSimple;
}

inline void note_pos(int x, int y)
{
    std::lock_guard<std::mutex> lock(g_posMutex);
    Pos &p = pos_slot();
    p.x = x;
    p.y = y;
    p.have = true;
}

inline bool last_pos(int *x, int *y)
{
    std::lock_guard<std::mutex> lock(g_posMutex);
    const Pos &p = pos_slot();
    if (!p.have) return false;
    if (x) *x = p.x;
    if (y) *y = p.y;
    return true;
}

// The window as it stands, dragged or not. Called while it still exists, on
// the way out.
inline void remember_pos_now()
{
    HWND h = page_window();
    if (h == nullptr) return;
    RECT rc;
    if (!GetWindowRect(h, &rc)) return;
    note_pos((int)rc.left, (int)rc.top);
}

inline void note_ordinal(const std::string &ordinal)
{
    std::lock_guard<std::mutex> lock(g_ordinalMutex);
    g_ordinal = ordinal;
}

inline bool view_get(bool *compact, bool *quick, std::string *ordinal)
{
    if (compact) *compact = g_compact.load();
    if (quick) *quick = g_quick.load();
    if (ordinal) {
        std::lock_guard<std::mutex> lock(g_ordinalMutex);
        *ordinal = g_ordinal;
    }
    return g_known.load();
}

// ⭐ THE WINDOW IS NOT THERE YET when the page says it has come back. It is
// found by the marker in its TITLE, and a brand new window has not always
// finished carrying it to the desktop by the time the page's first request
// lands -- measured 2026-09-09, when a restore silently did nothing and the
// window sat at the size Chrome had opened it. So: look for it for three
// seconds, and stop at the first sight of it.
inline void restore_geometry_soon()
{
    std::thread([] {
        for (int i = 0; i < 30; ++i) {
            if (HWND h = page_window()) {
                // ⛔⛔ READ THE PLACE BEFORE RESIZING (rhoquinn8217,
                // 2026-09-09: a window dragged somewhere came back where
                // OPTIONS had last put it, not where the mouse had). Resizing
                // moves a window -- it grows about its centre -- and while
                // apply_size() was also writing the memory, the restore's own
                // first act overwrote the place it was about to read. The
                // write is gone from apply_size(); reading first as well
                // means no future one can do it again.
                int x = 0, y = 0;
                const bool had = last_pos(&x, &y);
                apply_size(h);
                if (had) place_exact(h, x, y); else place_default(h);
                return;
            }
            Sleep(100);
        }
    }).detach();
}

// The page names its layout: Advanced, Simple or Quick.
//
// ⭐ A SWITCH resets that layout's slot to its entry size, which is what the
// page resizes the window to; the two then agree, and R3 cycles from where the
// window actually is.
//
// ⭐ A RESTORE -- the window closed and came back -- resets NOTHING. The
// layout is remembered by the page, the SIZE by this file (rhoquinn8217,
// 2026-09-09: "when closing remember advance/simple/quick and size"), so the
// window is put back at the size that layout was left at, in that layout's
// home place. ⓘ The page cannot do this half: R3 never reaches it, so it does
// not know the index. It sends `restore` and leaves the window alone.
inline void set_view(bool compact, bool quick, bool restore)
{
    quick = compact && quick;
    const bool changed = (compact != g_compact.load()) || (quick != g_quick.load());
    g_compact.store(compact);
    g_quick.store(quick);

    g_known.store(true);

    if (restore) {
        restore_geometry_soon();
        return;
    }
    if (!changed) return;
    if (!compact) g_size.store(1);
    else if (quick) g_sizeQuick.store(0);
    else g_sizeCompact.store(0);
}

// ⓘ Own edge tracking rather than ctm_overlay::edge(): that table's slots
// are the keyboard's. ⚠️ Named for R3 because it was R3 until 2026-09-20 --
// it watches Create now, and the mechanism is the same either way.
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
    note_pos(x, y);
}

// ⓘ The margin between the window and the screen edge on a snap, as a share
// of the work area's width so 1080p and 4K get the same proportion: 5% is
// about 96px on a 1080p screen and about 192px at 4K.
inline int snap_margin(const RECT &wa)
{
    return (wa.right - wa.left) / 20;
}

// ⓘ How far the bottom three sit above the taskbar: a hundredth of the work
// area, about ten pixels on a 1080p screen (rhoquinn8217, 2026-09-09: "just a
// little above the windows task bar"). It was the side margin, a twentieth of
// the WIDTH, which left them floating well clear of it.
inline int bottom_gap(const RECT &wa)
{
    return (wa.bottom - wa.top) / 100;
}

// A place asked for outright, kept on screen. ⓘ place() itself does not
// clamp: nudge() steers a pixel at a time and has already done it.
inline void place_exact(HWND hwnd, int x, int y)
{
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    const RECT wa = work_area();
    clamp_into(wa, rc.right - rc.left, rc.bottom - rc.top, x, y);
    place(hwnd, x, y);
}

// One of the three places along the bottom, for Simple and Quick.
inline void place_at(HWND hwnd, int idx)
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
    int x = targets[(idx % 3 + 3) % 3];
    int y = wa.bottom - bottom_gap(wa) - h;
    clamp_into(wa, w, h, x, y);
    place(hwnd, x, y);
}

// Where this layout goes when the window comes back: exactly where it was
// left, whether that was one of the three or somewhere a mouse dragged it.
// ⛔ CLAMPED, always: a remembered place is only as good as the screen it was
// remembered on, and a display that changed resolution or scaling in between
// would otherwise put the window out of reach. Failing that -- a layout not
// yet seen -- Advanced takes the middle of the screen and the other two the
// bottom centre.
inline void place_default(HWND hwnd)
{
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const RECT wa = work_area();

    int x = 0, y = 0;
    if (last_pos(&x, &y)) {
        clamp_into(wa, w, h, x, y);
        place(hwnd, x, y);
        return;
    }
    if (g_compact.load()) { place_at(hwnd, 1); return; }
    place_centre(hwnd);
}

// The middle of the screen, whatever is remembered. Advanced's one place.
inline void place_centre(HWND hwnd)
{
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const RECT wa = work_area();
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    clamp_into(wa, w, h, x, y);
    place(hwnd, x, y);
}

// A tap. In Advanced there is one place and this is "put it back in the
// middle". In Simple and Quick it is the NEXT of bottom left, bottom centre,
// bottom right -- judged from where the window IS, so one that was steered
// somewhere still goes somewhere sensible rather than to whatever a stale
// counter said. ⓘ The choice is kept, so the window comes back to it.
inline void snap_next(HWND hwnd)
{
    // ⛔ ADVANCED CENTRES, always -- never "back to where it was remembered"
    // (rhoquinn8217, 2026-09-09: "advanced should only center"). ⓘ The two
    // pulled apart once the exact place was remembered: coming BACK to a
    // dragged window is the memory doing its job, but a tap is a person
    // asking for the one place this layout has.
    if (!g_compact.load()) { place_centre(hwnd); return; }

    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    const int w = rc.right - rc.left;
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
    place_at(hwnd, (nearest + 1) % 3);
}

// This layout's slot, and the table it indexes.
inline std::atomic_int &size_slot()
{
    const bool compact = g_compact.load();
    const bool quick = g_quick.load();
    return !compact ? g_size : (quick ? g_sizeQuick : g_sizeCompact);
}

inline const SizeShare *size_table()
{
    const bool compact = g_compact.load();
    const bool quick = g_quick.load();
    return !compact ? kSizes : (quick ? kQuickSizes : kCompactSizes);
}

// The size this layout is at, applied about the window's CENTRE so it grows
// and shrinks in place, then clamped fully on screen.
inline void apply_size(HWND hwnd)
{
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    const SizeShare *table = size_table();
    const int idx = size_slot().load();
    const RECT wa = work_area();
    const int w = (int)((wa.right - wa.left) * table[idx].w);
    const int h = (int)((wa.bottom - wa.top) * table[idx].h);
    const int cx = rc.left + (rc.right - rc.left) / 2;
    const int cy = rc.top + (rc.bottom - rc.top) / 2;
    int x = cx - w / 2;
    int y = cy - h / 2;
    clamp_into(wa, w, h, x, y);
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    // ⛔ AND IT DOES NOT REMEMBER WHERE THAT LEFT IT. Growing about the centre
    // moves a window without anyone having chosen a place; recording it here
    // overwrote a place someone HAD chosen. Deliberate placings write the
    // memory -- place() below -- and so does the last look on the way out.
}

// How many sizes this layout cycles: three in Advanced, two in the others.
inline int size_count()
{
    return g_compact.load() ? 2 : 3;
}

// R3: the next of this layout's sizes.
inline void resize_next(HWND hwnd)
{
    std::atomic_int &slot = size_slot();
    slot.store((slot.load() + 1) % size_count());
    apply_size(hwnd);
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
//
// ⓘ Options, R3 and the stick are read through the pad's own layout, so a DS4
// or an Xbox pad moves the page as a DualSense does (2026-09-15).
inline bool handle_report(const void *deviceKey, const ctm_rebind::Layout &lay,
                          const uint8_t *data, size_t len)
{
    if (data == nullptr || len < lay.minLength) return false;

    if (!ctm_rebind_config_mode_effective()) {
        // ⛔ Not our window in front. Forget any hold in progress, on any pad,
        // so a release seen later -- with the page back in front -- is not
        // read as a tap.
        g_movers.abandon_all();
        r3_edge(deviceKey, false);
        return false;
    }

    // ⭐⭐ CREATE RESIZES, NOT R3 (rhoquinn8217, 2026-09-20: *"don't use R3
    // to change the window size. Use the create button instead since it's not
    // used anymore."*). Index 8, kBtnSelect -- Create on a DualSense, Select
    // or View elsewhere.
    // ⚠️ CREATE WAS NOT QUITE FREE, and what it did has been given up
    // knowingly. It sent KeyC, which since T-233 only moved the pad's focus
    // onto the Mode picker -- a shortcut the d-pad now reaches on its own, so
    // the cost is one convenience rather than a feature. Its key mapping and
    // the page's handler for it go with this change, or Create would resize
    // AND jump the focus on the same press.
    // ⓘ R3 is given back. It is a gyro gate as of T-235, and a button that
    // resizes a window while also aiming is a collision waiting to happen.
    if (r3_edge(deviceKey, ctm_overlay::button_down(lay, data, len, 8))) {
        if (HWND h = page_window()) resize_next(h);
        // The press goes through; the page no longer acts on Create.
    }

    // ⭐ THIS pad's mover, so another pad's reports -- Options up, as always on
    // the pad not being held -- cannot end this pad's hold (T-162).
    const window_move::Step mv =
        g_movers.for_key(deviceKey).step(ctm_overlay::button_down(lay, data, len, 9), lay, data, len);

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
