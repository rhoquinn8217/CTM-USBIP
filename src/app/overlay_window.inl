// The overlay window.
//
// ⭐ WHAT IT IS FOR. An on-screen keyboard that a bridged controller can drive
// while a GAME keeps focus. Steam's keyboard cannot be navigated by our pad --
// a rebound button is stripped before Steam sees it -- and the Windows touch
// keyboard ignores our bridged DS5 entirely (measured 2026-08-31). The TV has a
// usable keyboard of its own, but using it means TV-side code, a protocol
// message and an upstream merge; T-063 is about contributing a THIN bridge, so
// anything solved here is TV-side code never written.
//
// ⛔ IT MUST NEVER TAKE FOCUS. That is the whole point: the game keeps focus and
// keeps receiving the keystrokes we synthesise. WS_EX_NOACTIVATE is what makes
// that structural rather than something we have to remember.
//
// ⚠️ AND IT CANNOT WORK OVER AN EXCLUSIVE-FULLSCREEN GAME. "Always on top" is a
// hint to the desktop compositor about stacking order; an exclusive-fullscreen
// game bypasses the compositor and paints the display directly, so there is no
// stack for the hint to act on. Borderless windowed works -- which is why every
// overlay you have ever used works there and nowhere else. Do not spend time
// trying to beat this; document it.
//
// ⓘ THIS FILE IS STEP ONE OF TWO. It draws a placeholder with plain GDI, on
// purpose: the question worth answering first is whether a window with these
// styles behaves over the games actually played, and that question needs no
// browser. WebView2 replaces the painting once the window is proven, and if the
// window is NOT proven we will not have spent anything on a rendering engine.

#pragma once

// ⓘ Named explicitly rather than relying on the project inheriting MSVC's
// default library set. It does today -- user32 calls elsewhere link fine -- but
// this file is the first to draw anything, and a missing gdi32 would surface as
// a link error a long way from its cause.
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace ctm_overlay {

// ⭐ THE BUTTON THAT OPENED IT ALSO CLOSES IT.
//
// ⛔ Without this the overlay could not be dismissed at all: it swallows every
// button while it is up, so the OSKeyboard binding that opened it never runs a
// second time. Found on hardware 2026-09-01 -- Square opened it and nothing
// closed it.
//
// ⓘ Remembered rather than hardcoded to Square, because the binding is the
// person's to choose. -1 means it was opened some other way (the test flag),
// and then only Circle closes it.
inline std::atomic_int g_openedBy{-1};

// ⛔ THE CLOSE IS NOT ARMED UNTIL THE OPENING BUTTON HAS BEEN LET GO.
//
// ⚠️ Found on hardware 2026-09-01: Square opened the overlay and it vanished
// again instantly. The button is still HELD when the window comes up, so the
// next report -- four milliseconds later -- saw that same press as a fresh edge
// and closed it. Opening and closing on one button means the press that opened
// it must not count.
inline std::atomic_bool g_closeArmed{false};

// ⓘ Declared here: handle_report closes the window, and is defined above hide.
inline void hide();

inline HWND g_hwnd = nullptr;
inline std::thread g_thread;
inline std::atomic_bool g_running{false};

inline const wchar_t *const kClassName = L"CtmOverlayKeyboard";

// ⭐ TWO POSITIONS, because the keyboard will sometimes be over the very box
// you are typing into (rhoquinn8217, 2026-09-01). Triangle moves it.
//
// ⓘ Two, not free movement: a cursor you have to drive to a corner is a worse
// answer than a single button that puts it out of the way, and this is a
// keyboard for people with no mouse.
inline std::atomic_bool g_atTop{false};

// ⓘ Held, not latched: L1 shows capitals while you hold it, L2 shows the F
// keys. A held layer needs no state to get out of -- letting go is the exit.
inline std::atomic_bool g_shiftHeld{false};
inline std::atomic_bool g_fnHeld{false};

// ⓘ 0 is invisible, 255 is solid. 140 is roughly 55% -- enough to read the
// letters over a bright game, little enough to see what is behind it.
// ⭐ A constant for now; it becomes a setting once the layout settles.
inline const BYTE kAlpha = 140;

// The window is owned by its own thread, so moving it is posted rather than
// done from the controller's report thread.
inline const UINT WM_CTM_REPOSITION = WM_APP + 1;
inline const UINT WM_CTM_RESIZE     = WM_APP + 2;

// ⭐ THREE SIZES, as a share of the screen rather than pixels: 1080p and 4K
// want very different pixel counts and the same proportion.
// ⓘ Cycled with Create -- every face button and shoulder was already spoken
// for, and Create is out of the way of typing.
inline std::atomic_int g_size{1};                     // 0 small, 1 medium, 2 large
inline const float kSizeShare[3] = { 0.46f, 0.62f, 0.80f };

inline void size_for(int *w, int *h)
{
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    *w = (int)(screenW * kSizeShare[g_size.load() % 3]);
    *h = *w * 5 / 16;                                 // five rows, roughly
}

inline int overlay_y(int height)
{
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    return g_atTop.load() ? 80 : (screenH - height - 80);
}

// ⓘ COPIED FROM rebind.inl's kDs5Spots (this repo, src/input/rebind.inl, as of
// 2026-09-01) rather than shared: rebind.inl calls into this file and is
// included after it, so reaching back would be a cycle. It must stay in step
// with the original -- if the report layout changes, both change.
struct Spot { int byteIndex; uint8_t mask; };
inline const Spot kSpots[12] = {
    { 8, 0x20 },   // 0  cross
    { 8, 0x40 },   // 1  circle
    { 8, 0x10 },   // 2  square
    { 8, 0x80 },   // 3  triangle
    { 9, 0x01 },   // 4  L1
    { 9, 0x02 },   // 5  R1
    { 9, 0x04 },   // 6  L2
    { 9, 0x08 },   // 7  R2
    { 9, 0x10 },   // 8  create
    { 9, 0x20 },   // 9  options
    { 9, 0x40 },   // 10 L3
    { 9, 0x80 },   // 11 R3
};

inline bool button_down(const uint8_t *data, size_t len, int index)
{
    if (index < 0 || index >= 12) return false;
    const Spot &s = kSpots[index];
    return len > (size_t)s.byteIndex && (data[s.byteIndex] & s.mask) != 0;
}

// ⭐⭐ THE LAYOUT IS A TABLE, NOT A DRAWING.
//
// ⓘ The same table drew the mockups (tools/osk_mockups.py), which is the point:
// what was approved and what gets built cannot drift, and moving a key is one
// line in one place rather than a rectangle and a hit-test.
//
// ⭐ Staggered like a physical keyboard -- esc, tab, ctrl and shift get wider
// down the left, so each row starts further right. rhoquinn8217, 2026-09-01:
// "my goal is to mimic a real keyboard since it's almost there anyway". The
// cost is that rows no longer line up in columns, which is why moving up and
// down is nearest-neighbour rather than an array index.
//
// `wide` is in key-widths: 1.0 is an ordinary letter.
// ⓘ KK_ACTION keys do something to the KEYBOARD rather than sending anything:
// moving it out of the way, and closing it. They are on the face because a
// keyboard whose only exit is a controller button nobody told you about is a
// keyboard people get stuck in.
enum KeyKind { KK_NORMAL, KK_MOD, KK_FN, KK_ACTION };

// What a KK_ACTION key does, carried in the usage field, which is unused there.
inline const uint8_t ACT_MOVE = 1, ACT_CLOSE = 2;

struct Key {
    const wchar_t *label;
    const wchar_t *shifted;   // label while shift is on; null means the same
    uint8_t        usage;     // HID usage, or 0 for a modifier
    uint8_t        mod;       // which modifier bit, for KK_MOD
    KeyKind        kind;
    float          wide;
};

// Modifier bits as a HID keyboard reports them: left ctrl, shift, alt, GUI.
//
// ⛔ NOT NAMED MOD_*. winuser.h already defines MOD_SHIFT, MOD_ALT and MOD_WIN
// for RegisterHotKey, so `inline const uint8_t MOD_SHIFT = 0x02` expanded to
// `inline const uint8_t 0x0004 = 0x02` and failed with "syntax error:
// constant" -- an error pointing at a line that looks perfectly fine.
inline const uint8_t KBD_CTRL = 0x01, KBD_SHIFT = 0x02, KBD_ALT = 0x04, KBD_WIN = 0x08;

inline const Key kRow0[] = {
    { L"esc", nullptr, 0x29, 0, KK_NORMAL, 1.0f },
    { L"1", L"!", 0x1e, 0, KK_FN, 1.0f }, { L"2", L"@", 0x1f, 0, KK_FN, 1.0f },
    { L"3", L"#", 0x20, 0, KK_FN, 1.0f }, { L"4", L"$", 0x21, 0, KK_FN, 1.0f },
    { L"5", L"%", 0x22, 0, KK_FN, 1.0f }, { L"6", L"^", 0x23, 0, KK_FN, 1.0f },
    { L"7", L"&", 0x24, 0, KK_FN, 1.0f }, { L"8", L"*", 0x25, 0, KK_FN, 1.0f },
    { L"9", L"(", 0x26, 0, KK_FN, 1.0f }, { L"0", L")", 0x27, 0, KK_FN, 1.0f },
    { L"-", L"_", 0x2d, 0, KK_FN, 1.0f }, { L"=", L"+", 0x2e, 0, KK_FN, 1.0f },
    { L"back", nullptr, 0x2a, 0, KK_NORMAL, 2.1f },
};
inline const Key kRow1[] = {
    { L"tab", nullptr, 0x2b, 0, KK_NORMAL, 1.4f },
    { L"q", nullptr, 0x14, 0, KK_NORMAL, 1.0f }, { L"w", nullptr, 0x1a, 0, KK_NORMAL, 1.0f },
    { L"e", nullptr, 0x08, 0, KK_NORMAL, 1.0f }, { L"r", nullptr, 0x15, 0, KK_NORMAL, 1.0f },
    { L"t", nullptr, 0x17, 0, KK_NORMAL, 1.0f }, { L"y", nullptr, 0x1c, 0, KK_NORMAL, 1.0f },
    { L"u", nullptr, 0x18, 0, KK_NORMAL, 1.0f }, { L"i", nullptr, 0x0c, 0, KK_NORMAL, 1.0f },
    { L"o", nullptr, 0x12, 0, KK_NORMAL, 1.0f }, { L"p", nullptr, 0x13, 0, KK_NORMAL, 1.0f },
    { L"[", L"{", 0x2f, 0, KK_NORMAL, 1.0f }, { L"]", L"}", 0x30, 0, KK_NORMAL, 1.0f },
    { L"\\", L"|", 0x31, 0, KK_NORMAL, 1.7f },
};
inline const Key kRow2[] = {
    { L"ctrl", nullptr, 0, KBD_CTRL, KK_MOD, 1.65f },
    { L"a", nullptr, 0x04, 0, KK_NORMAL, 1.0f }, { L"s", nullptr, 0x16, 0, KK_NORMAL, 1.0f },
    { L"d", nullptr, 0x07, 0, KK_NORMAL, 1.0f }, { L"f", nullptr, 0x09, 0, KK_NORMAL, 1.0f },
    { L"g", nullptr, 0x0a, 0, KK_NORMAL, 1.0f }, { L"h", nullptr, 0x0b, 0, KK_NORMAL, 1.0f },
    { L"j", nullptr, 0x0d, 0, KK_NORMAL, 1.0f }, { L"k", nullptr, 0x0e, 0, KK_NORMAL, 1.0f },
    { L"l", nullptr, 0x0f, 0, KK_NORMAL, 1.0f },
    { L";", L":", 0x33, 0, KK_NORMAL, 1.0f }, { L"'", L"\"", 0x34, 0, KK_NORMAL, 1.0f },
    { L"enter", nullptr, 0x28, 0, KK_NORMAL, 2.5f },
};
inline const Key kRow3[] = {
    { L"shift", nullptr, 0, KBD_SHIFT, KK_MOD, 2.15f },
    { L"z", nullptr, 0x1d, 0, KK_NORMAL, 1.0f }, { L"x", nullptr, 0x1b, 0, KK_NORMAL, 1.0f },
    { L"c", nullptr, 0x06, 0, KK_NORMAL, 1.0f }, { L"v", nullptr, 0x19, 0, KK_NORMAL, 1.0f },
    { L"b", nullptr, 0x05, 0, KK_NORMAL, 1.0f }, { L"n", nullptr, 0x11, 0, KK_NORMAL, 1.0f },
    { L"m", nullptr, 0x10, 0, KK_NORMAL, 1.0f },
    { L",", L"<", 0x36, 0, KK_NORMAL, 1.0f }, { L".", L">", 0x37, 0, KK_NORMAL, 1.0f },
    { L"/", L"?", 0x38, 0, KK_NORMAL, 1.0f },
    // ⓘ Up sits directly above down, and del beside it -- rhoquinn8217 asked
    // for these two swapped so the arrow cluster reads as a cluster.
    { L"\u2191", nullptr, 0x52, 0, KK_NORMAL, 1.0f },
    { L"del", nullptr, 0x4c, 0, KK_NORMAL, 1.0f },
    { L"top", nullptr, ACT_MOVE, 0, KK_ACTION, 1.1f },
};
inline const Key kRow4[] = {
    { L"alt", nullptr, 0, KBD_ALT, KK_MOD, 1.3f },
    { L"win", nullptr, 0, KBD_WIN, KK_MOD, 1.3f },
    { L"space", nullptr, 0x2c, 0, KK_NORMAL, 9.3f },
    { L"\u2190", nullptr, 0x50, 0, KK_NORMAL, 1.0f },
    { L"\u2193", nullptr, 0x51, 0, KK_NORMAL, 1.0f },
    { L"\u2192", nullptr, 0x4f, 0, KK_NORMAL, 1.0f },
    { L"close", nullptr, ACT_CLOSE, 0, KK_ACTION, 1.1f },
};

struct Row { const Key *keys; int count; };
inline const Row kRows[] = {
    { kRow0, (int)(sizeof(kRow0) / sizeof(kRow0[0])) },
    { kRow1, (int)(sizeof(kRow1) / sizeof(kRow1[0])) },
    { kRow2, (int)(sizeof(kRow2) / sizeof(kRow2[0])) },
    { kRow3, (int)(sizeof(kRow3) / sizeof(kRow3[0])) },
    { kRow4, (int)(sizeof(kRow4) / sizeof(kRow4[0])) },
};
inline const int kRowCount = (int)(sizeof(kRows) / sizeof(kRows[0]));

// ⓘ F1..F12 replace the digits while L2 is held. Same positions, so the row you
// are looking at is the row that changes -- there is no key to travel to and
// nothing to navigate back from.
inline const wchar_t *const kFnLabels[12] = {
    L"F1", L"F2", L"F3", L"F4", L"F5", L"F6",
    L"F7", L"F8", L"F9", L"F10", L"F11", L"F12",
};
inline const uint8_t kFnUsages[12] = {
    0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45,
};

// ⭐ MODIFIERS LATCH, AND SHIFT ALSO LOCKS (rhoquinn8217, 2026-09-01).
//
// ⓘ One press latches: it stays on until the next ordinary key, then releases.
// That is what nearly every real use needs -- Ctrl+C, Alt+Tab, one capital --
// and it cleans up after itself. ⛔ Lock-only would make every shortcut three
// presses, and forgetting the third leaves the modifier held, which turns the
// next letter into a shortcut nobody meant.
//
// ⭐ Shift alone goes on to lock, for a run of capitals. Every platform does
// exactly this: once latches, twice locks, a third time clears.
enum LatchState { LATCH_OFF = 0, LATCH_ON = 1, LATCH_LOCKED = 2 };
inline std::map<uint8_t, int> g_mods;

inline int mod_state(uint8_t bit)
{
    auto it = g_mods.find(bit);
    return it == g_mods.end() ? LATCH_OFF : it->second;
}

inline uint8_t active_mods()
{
    uint8_t m = 0;
    for (const auto &e : g_mods) if (e.second != LATCH_OFF) m |= e.first;
    return m;
}

inline bool shift_showing()
{
    return mod_state(KBD_SHIFT) != LATCH_OFF || g_shiftHeld.load();
}

// Where the highlight is, and where each key ended up on screen.
inline int g_row = 1, g_col = 1;
inline int g_anchorX = 0;          // see move_v

struct Placed { RECT r; int row, col; };
inline std::vector<Placed> g_placed;
inline int g_placedW = 0, g_placedH = 0;

inline const Key &key_at(int row, int col)
{
    return kRows[row].keys[col];
}

// ⓘ Laid out once per size change rather than per frame: the walking needs the
// same rectangles the drawing uses, and computing them twice is how the two
// quietly disagree.
inline void layout(int w, int h)
{
    if (w == g_placedW && h == g_placedH && !g_placed.empty()) return;
    g_placedW = w; g_placedH = h;
    g_placed.clear();

    const int pad = 10;
    const int rowGap = 6;
    const int keyH = (h - pad * 2 - rowGap * (kRowCount - 1)) / kRowCount;

    float widest = 0;
    for (int r = 0; r < kRowCount; ++r) {
        float units = 0;
        for (int c = 0; c < kRows[r].count; ++c) units += kRows[r].keys[c].wide;
        if (units > widest) widest = units;
    }
    const int gap = 4;
    const float unit = (float)(w - pad * 2 - gap * 13) / widest;

    int y = pad;
    for (int r = 0; r < kRowCount; ++r) {
        float x = (float)pad;
        for (int c = 0; c < kRows[r].count; ++c) {
            const float kw = kRows[r].keys[c].wide * unit;
            Placed pl;
            pl.r.left = (int)x; pl.r.top = y;
            pl.r.right = (int)(x + kw); pl.r.bottom = y + keyH;
            pl.row = r; pl.col = c;
            g_placed.push_back(pl);
            x += kw + gap;
        }
        y += keyH + rowGap;
    }
}

inline const Placed *placed_of(int row, int col)
{
    for (const Placed &p : g_placed) if (p.row == row && p.col == col) return &p;
    return nullptr;
}

inline void invalidate()
{
    if (g_hwnd != nullptr) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ⓘ Sideways WRAPS. Clamping doubled the worst journey -- p to q was twelve
// presses and is now one -- and a row is short enough that wrapping within it
// stays predictable.
inline void move_h(int dir)
{
    const int n = kRows[g_row].count;
    g_col = (g_col + dir + n) % n;
    const Placed *p = placed_of(g_row, g_col);
    if (p) g_anchorX = (p->r.left + p->r.right) / 2;
    invalidate();
}

// ⭐⭐ UP AND DOWN GO TO THE NEAREST KEY, not to the same index.
//
// ⛔ The rows are staggered, so column 3 of one row is nowhere near column 3 of
// the next. Nearest-by-centre is what makes pressing up from shift land on
// ctrl, which is what a person expects from looking at it.
//
// ⚠️ AND IT REMEMBERS THE COLUMN IT STARTED FROM. Nearest-above and
// nearest-below are not symmetric: up from v then down again could land on c
// rather than v, which feels broken even though each step was right. The
// anchor is set by SIDEWAYS moves only, so a run of up-and-down keeps its line.
inline void move_v(int dir)
{
    const int next = g_row + dir;
    if (next < 0 || next >= kRowCount) return;       // clamped: rows are few

    // ⛔ DISTANCE TO THE KEY, NOT TO ITS CENTRE. Measuring to centres broke on
    // the wide keys: space is nine units across, so its centre sits far to the
    // right and pressing down from z landed on win -- even though z is
    // directly above the space bar. A key you are standing over is zero away.
    int best = 0, bestDist = 1 << 30;
    for (int c = 0; c < kRows[next].count; ++c) {
        const Placed *p = placed_of(next, c);
        if (!p) continue;
        int d = 0;
        if (g_anchorX < p->r.left)       d = p->r.left - g_anchorX;
        else if (g_anchorX > p->r.right) d = g_anchorX - p->r.right;
        if (d < bestDist) { bestDist = d; best = c; }
    }
    g_row = next;
    g_col = best;

    // ⭐⭐ THE ANCHOR MOVES WITH THE DIAGONAL (rhoquinn8217, 2026-09-01: down
    // from 1 should walk q, a, z).
    //
    // ⛔ Holding the ORIGINAL x looked right and was wrong: a staggered
    // keyboard drifts right as it descends, so a fixed anchor falls further
    // behind each row -- 1 q a landed on SHIFT instead of z, because shift's
    // wide key had crept under the line the anchor was still holding.
    //
    // ⚠️ The cost is that up-then-down is no longer guaranteed to return to the
    // same key when two keys are equally near. That is true of a real keyboard
    // too -- above v is as much f as g -- and following the stagger is what
    // was asked for.
    const Placed *landed = placed_of(g_row, g_col);
    if (landed) g_anchorX = (landed->r.left + landed->r.right) / 2;
    invalidate();
}

inline void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    layout(rc.right, rc.bottom);

    HBRUSH back = CreateSolidBrush(RGB(0x08, 0x09, 0x0c));
    FillRect(dc, &rc, back);
    DeleteObject(back);

    HBRUSH edge = CreateSolidBrush(RGB(0x2c, 0x2e, 0x36));
    FrameRect(dc, &rc, edge);
    DeleteObject(edge);

    const int keyH = g_placed.empty() ? 30
                   : (g_placed[0].r.bottom - g_placed[0].r.top);
    HFONT font = CreateFontW(keyH * 5 / 9, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    HBRUSH fillNormal = CreateSolidBrush(RGB(0x12, 0x14, 0x1a));
    HBRUSH fillMod    = CreateSolidBrush(RGB(0x19, 0x1c, 0x24));
    HBRUSH fillFn     = CreateSolidBrush(RGB(0x16, 0x1d, 0x1b));
    HBRUSH fillLatch  = CreateSolidBrush(RGB(0x2a, 0x35, 0x66));
    HBRUSH fillLock   = CreateSolidBrush(RGB(0x3a, 0x33, 0x20));
    HBRUSH fillHot    = CreateSolidBrush(RGB(0x3a, 0x4d, 0x9c));
    HBRUSH edgeHot    = CreateSolidBrush(RGB(0x8f, 0xa8, 0xff));

    const bool fnNow = g_fnHeld.load();
    const bool shiftNow = shift_showing();

    for (const Placed &p : g_placed) {
        const Key &k = key_at(p.row, p.col);
        const bool hot = (p.row == g_row && p.col == g_col);

        HBRUSH fill = fillNormal;
        if (k.kind == KK_MOD) {
            const int st = mod_state(k.mod);
            fill = st == LATCH_LOCKED ? fillLock : st == LATCH_ON ? fillLatch : fillMod;
        } else if (k.kind == KK_FN && fnNow) {
            fill = fillFn;
        } else if (k.kind == KK_ACTION) {
            fill = fillMod;
        }
        RECT r = p.r;
        FillRect(dc, &r, hot ? fillHot : fill);
        if (hot) FrameRect(dc, &r, edgeHot);

        // ⓘ One label decided in one place: F-keys win over shifted, which wins
        // over the plain one.
        const wchar_t *label = k.label;
        if (k.kind == KK_ACTION && k.usage == ACT_MOVE) {
            // ⓘ Says where it will GO, not where it is -- a button labelled
            // with the state you are already in tells you nothing.
            label = g_atTop.load() ? L"bottom" : L"top";
        } else if (k.kind == KK_FN && fnNow) {
            const int idx = p.col - 1;
            if (idx >= 0 && idx < 12) label = kFnLabels[idx];
        } else if (shiftNow && k.shifted != nullptr) {
            label = k.shifted;
        } else if (shiftNow && k.usage >= 0x04 && k.usage <= 0x1d) {
            static wchar_t up[2];
            up[0] = (wchar_t)(L'A' + (k.usage - 0x04));
            up[1] = 0;
            label = up;
        }

        SetTextColor(dc, hot ? RGB(0xff, 0xff, 0xff) : RGB(0xe8, 0xea, 0xf2));
        DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    DeleteObject(fillNormal); DeleteObject(fillMod); DeleteObject(fillFn);
    DeleteObject(fillLatch); DeleteObject(fillLock);
    DeleteObject(fillHot); DeleteObject(edgeHot);
    SelectObject(dc, oldFont);
    DeleteObject(font);

    EndPaint(hwnd, &ps);
}

inline LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint(hwnd);
        return 0;
    // ⛔ REFUSE ACTIVATION EVEN IF ASKED. WS_EX_NOACTIVATE covers clicks, but a
    // stray SetForegroundWindow from elsewhere would still hand us focus -- and
    // the moment this window has focus, the game stops receiving the keystrokes
    // it exists to send.
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_APP + 2: {                 // WM_CTM_RESIZE
        int w = 0, h = 0;
        size_for(&w, &h);
        // ⓘ Re-centred as it resizes, or growing would push it off one edge.
        SetWindowPos(hwnd, HWND_TOPMOST, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                     overlay_y(h), w, h, SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_APP + 1: {                 // WM_CTM_REPOSITION
        RECT rc;
        GetWindowRect(hwnd, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        // ⓘ SWP_NOACTIVATE with the move, for the same reason the window was
        // shown that way: repositioning must never hand it focus.
        SetWindowPos(hwnd, HWND_TOPMOST, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                     overlay_y(h), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        g_hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ⛔ ITS OWN THREAD, AND NOT ANY EXISTING ONE. A window needs a message loop,
// and a message loop owns the thread it runs on. Putting one on a thread that
// relays controller reports is how those reports queue up behind it -- measured
// 2026-09-01, when opening the settings window on a session thread made every
// button appear to rapid-fire.
inline void thread_main(int width, int height)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // Centred, and at whichever end overlay_y says -- bottom to start with,
    // like a keyboard, and Triangle moves it to the top.
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int x = (screenW - width) / 2;
    const int y = overlay_y(height);

    // ⭐ THE STYLES ARE THE FEATURE.
    //   WS_EX_NOACTIVATE  -- clicking it never gives it focus
    //   WS_EX_TOPMOST     -- above ordinary windows
    //   WS_EX_TOOLWINDOW  -- keeps it out of the taskbar and Alt+Tab
    //   WS_POPUP          -- no title bar, no border, no system menu
    g_hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kClassName, L"CTM overlay", WS_POPUP,
        x, y, width, height,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (g_hwnd == nullptr) {
        device_log::session_w() << L"overlay: could not create the window";
        g_running.store(false);
        return;
    }

    // ⭐⭐ AS SEE-THROUGH AS IT CAN BE AND STILL BE READ (rhoquinn8217,
    // 2026-09-01: "it needs to be as transparent as we can make it").
    //
    // ⓘ WS_EX_LAYERED plus one alpha for the whole window. Uniform, which means
    // the LABELS fade with the keys -- so the drawing compensates: the key
    // fills went darker and the text brighter, and the lit key stays solid
    // enough to find at a glance.
    //
    // ⚠️ Whole-window alpha is the simple half of this. Per-pixel alpha would
    // let the gaps between keys vanish entirely while the letters stayed
    // opaque, but it means drawing into a DIB with premultiplied alpha and
    // pushing it with UpdateLayeredWindow -- a bigger change, and worth doing
    // only if this is not transparent enough.
    SetLayeredWindowAttributes(g_hwnd, 0, kAlpha, LWA_ALPHA);

    // ⓘ SW_SHOWNOACTIVATE, not SW_SHOW. The ordinary show would activate it.
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    // ⭐ Asserted after showing as well as in the styles: HWND_TOPMOST here is
    // what puts it above other topmost windows rather than merely among them.
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    device_log::session_w() << L"overlay: window up at " << x << L"," << y
                            << L" (" << width << L"x" << height << L")";

    MSG m;
    while (g_running.load() && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_hwnd != nullptr) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    UnregisterClassW(kClassName, wc.hInstance);
    g_running.store(false);
    device_log::session_w() << L"overlay: window down";
}

inline bool visible() { return g_running.load() && g_hwnd != nullptr; }

// ⭐⭐ THE PAD MOVES THE HIGHLIGHT.
//
// ⓘ The listener owns this, not the window. It is the thing already reading
// every report from every bridged controller, so there is no transport, no
// second process and nothing to keep in step -- the code that sees the press is
// the code that decides what it means.
//
// ⛔ EDGE STATE IS PER DEVICE. Two bridged pads report independently at ~250Hz
// each; a single shared "was it down last time" would see the idle pad's zero
// between every one of the held pad's reports and fire an edge on each. That is
// exactly the fault that made every button appear to rapid-fire on 2026-09-01,
// and it is the same shape here.
//
// ⓘ BOTH pads drive the ONE highlight, deliberately (rhoquinn8217): the
// keyboard is shared, so they share the cursor on it.
inline std::map<const void *, uint32_t> g_padPrev;
inline std::mutex g_padMutex;

inline bool edge(const void *deviceKey, int index, bool downNow)
{
    const uint32_t bit = 1u << index;
    std::lock_guard<std::mutex> lock(g_padMutex);
    uint32_t &prev = g_padPrev[deviceKey];
    const bool wasDown = (prev & bit) != 0;
    if (downNow) prev |= bit; else prev &= ~bit;
    return downNow && !wasDown;
}

inline void forget_device(const void *deviceKey)
{
    std::lock_guard<std::mutex> lock(g_padMutex);
    g_padPrev.erase(deviceKey);
}

// ⓘ Everything neutral. Copied in shape from the gate's own blanking in
// rebind.inl (ctm_rebind::apply, config-mode branch) so the two agree about
// what "the game sees nothing" means: sticks centred, triggers released, no
// buttons, hat centred.
inline void blank_report(uint8_t *data, size_t len)
{
    if (data == nullptr || len < 11) return;
    data[1] = data[2] = data[3] = data[4] = 0x80;   // LX LY RX RY
    data[5] = data[6] = 0x00;                       // L2 R2 analog
    data[8] = 0x08;                                 // faces clear, hat centred
    data[9] = 0x00;
    data[10] = static_cast<uint8_t>(data[10] & ~0x07);
}

// Returns true when the overlay consumed the input, so the game does not also
// see it. ⛔ False when the overlay is down -- it must cost nothing when unused.
inline bool handle_report(const void *deviceKey, const uint8_t *data, size_t len)
{
    if (!visible() || data == nullptr || len < 11) return false;

    // ⛔ READ HERE, NOT THROUGH rebind's helper. rebind.inl calls into this
    // file, so depending on it back would be a cycle -- and this file is
    // included first precisely so the call above resolves.
    //
    // ⓘ The DualSense hat is the low nibble of byte 8: 0 is up and it goes
    // clockwise, 8 is centred. Same encoding rebind.inl reads.
    const uint8_t hat = static_cast<uint8_t>(data[8] & 0x0f);
    const bool up    = (hat == 7 || hat == 0 || hat == 1);
    const bool right = (hat == 1 || hat == 2 || hat == 3);
    const bool down  = (hat == 3 || hat == 4 || hat == 5);
    const bool left  = (hat == 5 || hat == 6 || hat == 7);

    if (edge(deviceKey, 0, up))    move_v(-1);
    if (edge(deviceKey, 1, down))  move_v(1);
    if (edge(deviceKey, 2, left))  move_h(-1);
    if (edge(deviceKey, 3, right)) move_h(1);

    // ⭐ THE HELD LAYERS. L1 shows capitals and shifted punctuation, L2 turns
    // the digit row into F1-F12 in place.
    const bool l1 = button_down(data, len, 4);
    const bool l2 = button_down(data, len, 6);
    if (l1 != g_shiftHeld.load()) { g_shiftHeld.store(l1); invalidate(); }
    if (l2 != g_fnHeld.load())    { g_fnHeld.store(l2);   invalidate(); }

    // ⭐ TRIANGLE MOVES IT out of the way of whatever is being typed into.
    if (edge(deviceKey, 6, button_down(data, len, 3))) {
        g_atTop.store(!g_atTop.load());
        if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CTM_REPOSITION, 0, 0);
    }

    // ⭐ CREATE CYCLES THE THREE SIZES. It is free, out of the way of typing,
    // and its own thing rather than a mode.
    if (edge(deviceKey, 7, button_down(data, len, 8))) {
        g_size.store((g_size.load() + 1) % 3);
        if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CTM_RESIZE, 0, 0);
    }

    // ⭐⭐ CROSS PRESSES THE HIGHLIGHTED KEY.
    const Key &k = key_at(g_row, g_col);
    const bool cross = (data[8] & 0x20) != 0;

    // ⓘ A MODIFIER is a press, not a hold: it cycles its own state and sends
    // nothing on its own. Latch, then lock for shift, then off.
    if (cross && k.kind == KK_MOD && edge(deviceKey, 8, true)) {
        const int st = mod_state(k.mod);
        const int next = (st == LATCH_OFF)     ? LATCH_ON
                       : (st == LATCH_ON && k.mod == KBD_SHIFT) ? LATCH_LOCKED
                       : LATCH_OFF;
        g_mods[k.mod] = next;
        invalidate();
    } else if (!cross) {
        edge(deviceKey, 8, false);          // keep the modifier edge honest
    }

    // ⭐ AN ACTION KEY ACTS ON THE KEYBOARD and sends nothing. Handled before
    // the sending path so it cannot also type.
    if (cross && k.kind == KK_ACTION && edge(deviceKey, 9, true)) {
        if (k.usage == ACT_CLOSE) { hide(); return true; }
        if (k.usage == ACT_MOVE) {
            g_atTop.store(!g_atTop.load());
            if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CTM_REPOSITION, 0, 0);
        }
    } else if (!cross) {
        edge(deviceKey, 9, false);
    }

    // ⓘ An ordinary key rides with whatever modifiers are active, and the
    // shoulders are shortcuts to keys that are also ON the keyboard -- Steam
    // does the same, and a shortcut to something invisible is folklore.
    uint8_t usage = 0;
    if (cross && k.kind != KK_MOD && k.kind != KK_ACTION) {
        usage = k.usage;
        if (g_fnHeld.load() && k.kind == KK_FN) {
            const int idx = g_col - 1;
            if (idx >= 0 && idx < 12) usage = kFnUsages[idx];
        }
    }
    else if (button_down(data, len, 5))  usage = 0x2c;   // R1: space
    else if (button_down(data, len, 7))  usage = 0x28;   // R2: enter

    uint8_t mods = active_mods();
    if (g_shiftHeld.load()) mods |= KBD_SHIFT;

    uint8_t keys[6] = { usage, 0, 0, 0, 0, 0 };
    ctm_keyboard_device::set_state_for(deviceKey, usage != 0 ? mods : 0,
                                       keys, usage != 0 ? 1 : 0);

    // ⛔ A LATCH RELEASES ON THE KEY IT MODIFIED, and a LOCK does not. That is
    // the whole difference between the two, and it happens on the RELEASE so
    // the modifier is still applied to the key that was just sent.
    static bool sentLast = false;
    if (usage != 0) {
        sentLast = true;
    } else if (sentLast) {
        sentLast = false;
        bool changed = false;
        for (auto &e : g_mods) {
            if (e.second == LATCH_ON) { e.second = LATCH_OFF; changed = true; }
        }
        if (changed) invalidate();
    }

    // ⓘ Every button is swallowed while the overlay is up, not just the ones it
    // uses -- a keyboard on screen that lets stray presses through to the game
    // is worse than one that does nothing.
    return true;
}

inline void show(int width = 0, int height = 0, int openedByButton = -1)
{
    if (width <= 0 || height <= 0) size_for(&width, &height);
    g_openedBy.store(openedByButton);
    g_closeArmed.store(false);          // the opening press must not close it
    if (g_running.exchange(true)) return;      // already up
    g_thread = std::thread(thread_main, width, height);
    g_thread.detach();
}

inline void hide()
{
    if (!g_running.exchange(false)) return;
    // ⛔ Whatever was held goes up with the window. A key left down repeats
    // forever and looks like a stuck keyboard -- the same class of fault as a
    // stuck mouse button, and the same reason unbridging releases its keys.
    {
        std::lock_guard<std::mutex> lock(g_padMutex);
        for (const auto &entry : g_padPrev) {
            ctm_keyboard_device::forget_device(entry.first);
        }
        g_padPrev.clear();
    }
    // ⓘ Posting rather than destroying from here: the window belongs to the
    // thread that created it, and destroying it from another one is undefined.
    if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

} // namespace ctm_overlay
