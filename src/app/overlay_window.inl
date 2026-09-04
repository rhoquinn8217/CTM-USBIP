// The overlay window.
//
// ⭐ WHAT IT IS FOR. An on-screen keyboard that a bridged controller can drive
// while a GAME keeps focus. Steam's keyboard cannot be navigated by our pad --
// a rebound button is stripped before Steam sees it -- and the Windows touch
// keyboard ignores our bridged DS5 entirely (measured 2026-08-31). The TV has a
// usable keyboard of its own, but using it means TV-side code, a protocol
// message and an upstream merge; the bridge contributed upstream is meant to
// stay THIN, so
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

// ⓘ Defined in osk.inl, which already knows how to hand a steam:// URL to the
// shell. ⛔ Declared at GLOBAL scope, not inside the namespace below -- put
// inside, it becomes ctm_overlay::ctm_overlay_open_steam and never links.
void ctm_overlay_open_steam();
void ctm_rebind_swallow_held();

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
// ⛔ SOLID. rhoquinn8217, 2026-09-02: transparency made it hard to read and
// sometimes hard to tell it was there at all. A keyboard is a thing you look
// AT, not through -- and it has a place of its own at the edge of the screen,
// so there is nothing behind it worth seeing.
inline const BYTE kAlpha = 255;

// The window is owned by its own thread, so moving it is posted rather than
// done from the controller's report thread.
inline const UINT WM_CTM_REPOSITION = WM_APP + 1;
inline const UINT WM_CTM_RESIZE     = WM_APP + 2;
inline const UINT WM_CTM_NUDGE      = WM_APP + 3;

// ⭐ HOLD TRIANGLE AND STEER (rhoquinn8217, 2026-09-02). A tap still snaps it
// top or bottom; holding turns the left stick into a way to put it anywhere.
//
// ⓘ The deltas are accumulated here and applied on the window's own thread,
// because the window belongs to that thread and moving it from the report
// thread is the kind of thing that works until it does not.
inline std::atomic_int g_nudgeX{0}, g_nudgeY{0};
inline std::atomic_bool g_triHeld{false}, g_triMoved{false};

// ⓘ Where the cursor was when the hold began, so the window follows the mouse
// by its DELTA rather than jumping to wherever the pointer happens to be.
inline POINT g_dragFrom = { 0, 0 };
inline bool g_dragHaveFrom = false;

// ⭐ THREE SIZES, as a share of the screen rather than pixels: 1080p and 4K
// want very different pixel counts and the same proportion.
// ⓘ Cycled with Create -- every face button and shoulder was already spoken
// for, and Create is out of the way of typing.
// ⓘ Three faces now, cycled by the layout button. Compact is the DEFAULT:
// most typing from a couch is a search box or a password.
// ⛔ Declared here rather than beside the layouts, because the SIZE depends on
// which face is showing and size_for comes first.
enum Face { FACE_SUB = 0, FACE_COMPACT = 1, FACE_FULL = 2 };
inline std::atomic_int g_face{FACE_COMPACT};

// ⓘ How many columns each face is, which is what the size arithmetic needs.
inline float face_cols()
{
    switch (g_face.load()) {
    case FACE_SUB:  return 11.0f;
    case FACE_FULL: return 15.5f;
    default:        return 14.0f;
    }
}

inline std::atomic_int g_size{1};                     // 0 small, 1 medium, 2 large

// ⭐⭐ THE FULL FACE IS WIDER, not just fuller (rhoquinn8217, 2026-09-02).
//
// ⛔ Both faces used to share these shares, so Full simply squeezed more
// columns into the same width and every key got smaller. It has sixteen
// columns to Compact's fourteen, so at equal width its keys are 12% narrower
// before anything else.
//
// ⓘ Full's middle size is 4/5 of the screen, as asked. Compact stays where it
// was, because it was right.
// ⭐⭐ THE KEY SIZE IS THE SETTING, NOT THE WINDOW WIDTH (rhoquinn8217,
// 2026-09-02: the faces should look the same at the same size).
//
// ⛔ Each face used to pick a share of the SCREEN, so Compact's fourteen
// columns and Full's fifteen and a half divided that width differently and
// their letters came out different sizes. Only the largest happened to match.
//
// ⓘ Now a size chooses how wide one KEY is, and the window follows from how
// many columns the face has. Same keys, same font, either face -- and the
// height falls out identical too, because it depends only on the key size.
inline const float kKeyShare[3] = { 0.0426f, 0.0516f, 0.0606f };

inline void size_for(int *w, int *h)
{
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const float keyW = screenW * kKeyShare[g_size.load() % 3];
    const float cols0 = face_cols();
    *w = (int)(keyW * cols0);
    // ⓘ Height follows the COLUMN COUNT so the keys stay roughly square: a
    // wider face with more columns needs proportionally more height, and a
    // fixed ratio would have squashed Full's rows.
    const float cols = face_cols();
    // ⓘ Two thirds the height it was (rhoquinn8217, 2026-09-02): five rows of
    // tall keys plus a tab made the keyboard cover too much of the screen, and
    // a key does not need to be square to be easy to hit.
    *h = (int)(*w * 5.0f / (cols * 1.72f));
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
// ⓘ KK_SHOULDER_* only changes what is PRINTED on the key -- the shortcut
// itself is read from the pad directly. Steam prints them the same way, and it
// is what turns a shoulder shortcut from folklore into something visible.
// ⓘ KK_ESC is the backtick that becomes esc while L2 is held.
// ⛔ DT_NOPREFIX ON EVERY LABEL. DrawTextW treats & as an accelerator marker:
// it swallows the ampersand and underlines the next character instead, so the
// shifted 7 simply never appeared (rhoquinn8217, 2026-09-02).
// ⓘ KK_SPACER is not a key at all: it is the part of the tab you GRAB. It
// draws as bare frame and the highlight skips straight over it.
enum KeyKind { KK_NORMAL, KK_MOD, KK_FN, KK_ACTION, KK_ESC, KK_SPACER,
               KK_SHOULDER_L1, KK_SHOULDER_R1, KK_SHOULDER_R2 };

// What a KK_ACTION key does, carried in the usage field, which is unused there.
// ⛔ WELL CLEAR OF THE HID USAGES. These live in the same field as a key's
// usage, and ACT_PASTE was 4 -- which is the usage for the letter 'a', so the
// shift labelling turned the paste key into a capital A. Starting at 200 puts
// them past every usage we use.
inline const uint8_t ACT_MOVE = 200, ACT_CLOSE = 201, ACT_STEAM = 202,
                     ACT_PASTE = 203, ACT_COPY = 204, ACT_SIZE = 205,
                     ACT_LAYOUT = 206;

struct Key {
    const wchar_t *label;
    const wchar_t *shifted;   // label while shift is on; null means the same
    uint8_t        usage;     // HID usage, or 0 for a modifier
    uint8_t        mod;       // KK_MOD: which modifier this key IS.
                              // KK_NORMAL: modifiers it SENDS WITH, so a key
                              // like " can exist without latching shift.
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
// ⓘ NOT a real modifier bit: Fn exists only in this keyboard, so it is never
// sent. 0x80 is unused by the HID modifier byte's four left-hand keys, and it
// is masked out before anything is transmitted.
inline const uint8_t KBD_FN = 0x80;

inline const Key kRow0[] = {
    { L"esc", nullptr, 0x29, 0, KK_NORMAL, 1.0f },
    { L"`", L"~", 0x35, 0, KK_NORMAL, 1.0f },
    { L"1", L"!", 0x1e, 0, KK_FN, 1.0f }, { L"2", L"@", 0x1f, 0, KK_FN, 1.0f },
    { L"3", L"#", 0x20, 0, KK_FN, 1.0f }, { L"4", L"$", 0x21, 0, KK_FN, 1.0f },
    { L"5", L"%", 0x22, 0, KK_FN, 1.0f }, { L"6", L"^", 0x23, 0, KK_FN, 1.0f },
    { L"7", L"&", 0x24, 0, KK_FN, 1.0f }, { L"8", L"*", 0x25, 0, KK_FN, 1.0f },
    { L"9", L"(", 0x26, 0, KK_FN, 1.0f }, { L"0", L")", 0x27, 0, KK_FN, 1.0f },
    { L"-", L"_", 0x2d, 0, KK_FN, 1.0f }, { L"=", L"+", 0x2e, 0, KK_FN, 1.0f },
    { L"\u232b", nullptr, 0x2a, 0, KK_NORMAL, 1.5f },
};
inline const Key kRow1[] = {
    { L"tab", nullptr, 0x2b, 0, KK_NORMAL, 1.5f },
    { L"q", nullptr, 0x14, 0, KK_NORMAL, 1.0f }, { L"w", nullptr, 0x1a, 0, KK_NORMAL, 1.0f },
    { L"e", nullptr, 0x08, 0, KK_NORMAL, 1.0f }, { L"r", nullptr, 0x15, 0, KK_NORMAL, 1.0f },
    { L"t", nullptr, 0x17, 0, KK_NORMAL, 1.0f }, { L"y", nullptr, 0x1c, 0, KK_NORMAL, 1.0f },
    { L"u", nullptr, 0x18, 0, KK_NORMAL, 1.0f }, { L"i", nullptr, 0x0c, 0, KK_NORMAL, 1.0f },
    { L"o", nullptr, 0x12, 0, KK_NORMAL, 1.0f }, { L"p", nullptr, 0x13, 0, KK_NORMAL, 1.0f },
    { L"[", L"{", 0x2f, 0, KK_NORMAL, 1.0f }, { L"]", L"}", 0x30, 0, KK_NORMAL, 1.0f },
    { L"\\", L"|", 0x31, 0, KK_NORMAL, 1.0f },
    { L"del", nullptr, 0x4c, 0, KK_NORMAL, 1.0f },
};
inline const Key kRow2[] = {
    { L"caps", nullptr, 0x39, 0, KK_NORMAL, 1.8f },
    { L"a", nullptr, 0x04, 0, KK_NORMAL, 1.0f }, { L"s", nullptr, 0x16, 0, KK_NORMAL, 1.0f },
    { L"d", nullptr, 0x07, 0, KK_NORMAL, 1.0f }, { L"f", nullptr, 0x09, 0, KK_NORMAL, 1.0f },
    { L"g", nullptr, 0x0a, 0, KK_NORMAL, 1.0f }, { L"h", nullptr, 0x0b, 0, KK_NORMAL, 1.0f },
    { L"j", nullptr, 0x0d, 0, KK_NORMAL, 1.0f }, { L"k", nullptr, 0x0e, 0, KK_NORMAL, 1.0f },
    { L"l", nullptr, 0x0f, 0, KK_NORMAL, 1.0f },
    { L";", L":", 0x33, 0, KK_NORMAL, 1.0f }, { L"'", L"\"", 0x34, 0, KK_NORMAL, 1.0f },
    { L"enter", nullptr, 0x28, 0, KK_NORMAL, 2.7f },
};
inline const Key kRow3[] = {
    { L"shift", nullptr, 0, KBD_SHIFT, KK_MOD, 2.2f },
    { L"z", nullptr, 0x1d, 0, KK_NORMAL, 1.0f }, { L"x", nullptr, 0x1b, 0, KK_NORMAL, 1.0f },
    { L"c", nullptr, 0x06, 0, KK_NORMAL, 1.0f }, { L"v", nullptr, 0x19, 0, KK_NORMAL, 1.0f },
    { L"b", nullptr, 0x05, 0, KK_NORMAL, 1.0f }, { L"n", nullptr, 0x11, 0, KK_NORMAL, 1.0f },
    { L"m", nullptr, 0x10, 0, KK_NORMAL, 1.0f },
    { L",", L"<", 0x36, 0, KK_NORMAL, 1.0f }, { L".", L">", 0x37, 0, KK_NORMAL, 1.0f },
    { L"/", L"?", 0x38, 0, KK_NORMAL, 1.0f },
    { L"\u2191", nullptr, 0x52, 0, KK_NORMAL, 1.0f },
    // ⭐ A second shift beside the up arrow, and fn out on the edge
    // (rhoquinn8217, 2026-09-02). The move key that used to sit here has gone
    // to the tab, where it belongs with the other two that act on the window.
    { L"shift", nullptr, 0, KBD_SHIFT, KK_MOD, 1.0f },
    { L"fn", nullptr, 0, KBD_FN, KK_MOD, 1.3f },
};
inline const Key kRow4[] = {
    { L"ctrl", nullptr, 0, KBD_CTRL, KK_MOD, 1.3f },
    { L"win", nullptr, 0, KBD_WIN, KK_MOD, 1.3f },
    { L"alt", nullptr, 0, KBD_ALT, KK_MOD, 1.3f },
    { L"space", nullptr, 0x2c, 0, KK_NORMAL, 5.3f },
    // ⭐ Where the right alt and right ctrl would be -- duplicates of keys
    // already on this row, in the place a thumb can reach.
    { L"paste", L"copy", ACT_PASTE, 0, KK_ACTION, 2.0f },
    { L"\u2190", nullptr, 0x50, 0, KK_NORMAL, 1.0f },
    { L"\u2193", nullptr, 0x51, 0, KK_NORMAL, 1.0f },
    { L"\u2192", nullptr, 0x4f, 0, KK_NORMAL, 1.0f },
    { L"\u2328\u2938", nullptr, ACT_CLOSE, 0, KK_ACTION, 1.3f },
};

// ⭐⭐ THE TAB (rhoquinn8217, 2026-09-02). The window's frame is four pixels
// wide, which is a poor thing to aim at with a remote -- so the three functions
// that act on the KEYBOARD ITSELF get a strip of their own above it, with a
// wide grab area beside them.
//
// ⓘ Shared by both faces: they are the same three buttons whichever keys are
// below, and one table means they cannot drift apart.
//
// ⛔ The spacer is most of the width on purpose. It is the handle.
// ⭐⭐ SUB-COMPACT (rhoquinn8217, 2026-09-02). Compact with every symbol and
// punctuation mark taken out, backtick included -- eleven columns of letters,
// digits and the keys a controller cannot reach.
//
// ⓘ For typing a name or a search term, where , . / [ ] \ ; ' are dead weight
// and every column of them is a column of travel.
inline const Key kSub0[] = {
    { L"1", nullptr, 0x1e, 0, KK_FN, 1.0f }, { L"2", nullptr, 0x1f, 0, KK_FN, 1.0f },
    { L"3", nullptr, 0x20, 0, KK_FN, 1.0f }, { L"4", nullptr, 0x21, 0, KK_FN, 1.0f },
    { L"5", nullptr, 0x22, 0, KK_FN, 1.0f }, { L"6", nullptr, 0x23, 0, KK_FN, 1.0f },
    { L"7", nullptr, 0x24, 0, KK_FN, 1.0f }, { L"8", nullptr, 0x25, 0, KK_FN, 1.0f },
    { L"9", nullptr, 0x26, 0, KK_FN, 1.0f }, { L"0", nullptr, 0x27, 0, KK_FN, 1.0f },
    { L"\u232b", nullptr, 0x2a, 0, KK_NORMAL, 1.0f },
};
inline const Key kSub1[] = {
    { L"tab", nullptr, 0x2b, 0, KK_NORMAL, 1.0f },
    { L"q", nullptr, 0x14, 0, KK_NORMAL, 1.0f }, { L"w", nullptr, 0x1a, 0, KK_NORMAL, 1.0f },
    { L"e", nullptr, 0x08, 0, KK_NORMAL, 1.0f }, { L"r", nullptr, 0x15, 0, KK_NORMAL, 1.0f },
    { L"t", nullptr, 0x17, 0, KK_NORMAL, 1.0f }, { L"y", nullptr, 0x1c, 0, KK_NORMAL, 1.0f },
    { L"u", nullptr, 0x18, 0, KK_NORMAL, 1.0f }, { L"i", nullptr, 0x0c, 0, KK_NORMAL, 1.0f },
    { L"o", nullptr, 0x12, 0, KK_NORMAL, 1.0f }, { L"p", nullptr, 0x13, 0, KK_NORMAL, 1.0f },
};
inline const Key kSub2[] = {
    { L"ctrl", nullptr, 0, KBD_CTRL, KK_MOD, 1.0f },
    { L"a", nullptr, 0x04, 0, KK_NORMAL, 1.0f }, { L"s", nullptr, 0x16, 0, KK_NORMAL, 1.0f },
    { L"d", nullptr, 0x07, 0, KK_NORMAL, 1.0f }, { L"f", nullptr, 0x09, 0, KK_NORMAL, 1.0f },
    { L"g", nullptr, 0x0a, 0, KK_NORMAL, 1.0f }, { L"h", nullptr, 0x0b, 0, KK_NORMAL, 1.0f },
    { L"j", nullptr, 0x0d, 0, KK_NORMAL, 1.0f }, { L"k", nullptr, 0x0e, 0, KK_NORMAL, 1.0f },
    { L"l", nullptr, 0x0f, 0, KK_NORMAL, 1.0f },
    { L"enter", nullptr, 0x28, 0, KK_NORMAL, 1.0f },
};
inline const Key kSub3[] = {
    { L"shift", nullptr, 0, KBD_SHIFT, KK_MOD, 1.0f },
    { L"z", nullptr, 0x1d, 0, KK_NORMAL, 1.0f }, { L"x", nullptr, 0x1b, 0, KK_NORMAL, 1.0f },
    { L"c", nullptr, 0x06, 0, KK_NORMAL, 1.0f }, { L"v", nullptr, 0x19, 0, KK_NORMAL, 1.0f },
    { L"b", nullptr, 0x05, 0, KK_NORMAL, 1.0f }, { L"n", nullptr, 0x11, 0, KK_NORMAL, 1.0f },
    { L"m", nullptr, 0x10, 0, KK_NORMAL, 1.0f },
    { L"\u2191", nullptr, 0x52, 0, KK_NORMAL, 1.0f },
    { L"shift", nullptr, 0, KBD_SHIFT, KK_MOD, 1.0f },
    { L"fn", nullptr, 0, KBD_FN, KK_MOD, 1.0f },
};
inline const Key kSub4[] = {
    { L"alt", nullptr, 0, KBD_ALT, KK_MOD, 1.0f },
    { L"win", nullptr, 0, KBD_WIN, KK_MOD, 1.0f },
    { L"space", nullptr, 0x2c, 0, KK_NORMAL, 4.0f },
    { L"paste", L"copy", ACT_PASTE, 0, KK_ACTION, 1.0f },
    { L"\u2190", nullptr, 0x50, 0, KK_NORMAL, 1.0f },
    { L"\u2193", nullptr, 0x51, 0, KK_NORMAL, 1.0f },
    { L"\u2192", nullptr, 0x4f, 0, KK_NORMAL, 1.0f },
    { L"\u2328\u2938", nullptr, ACT_CLOSE, 0, KK_ACTION, 1.0f },
};
inline const Key kTabSub[] = {
    { L"", nullptr, 0, 0, KK_SPACER, 8.34f },
    { L"\u2328", nullptr, ACT_LAYOUT, 0, KK_ACTION, 0.67f },
    { L"\u21f3", nullptr, ACT_MOVE,   0, KK_ACTION, 0.67f },
    { L"\u2197", nullptr, ACT_SIZE,   0, KK_ACTION, 0.66f },
    { L"x", nullptr, ACT_CLOSE, 0, KK_ACTION, 0.66f },
};

inline const Key kTabCompact[] = {
    { L"", nullptr, 0, 0, KK_SPACER, 11.34f },
    { L"\u2328", nullptr, ACT_LAYOUT, 0, KK_ACTION, 0.67f },
    { L"\u21f3", nullptr, ACT_MOVE,   0, KK_ACTION, 0.67f },
    { L"\u2197", nullptr, ACT_SIZE,   0, KK_ACTION, 0.66f },
    // ⭐ Close here TOO, not instead. rhoquinn8217, 2026-09-02:
    // top-right is where a pointer goes to shut a window, and the
    // key on the face is where a thumb already is. Two ways out of
    // something is not redundancy worth removing.
    { L"x", nullptr, ACT_CLOSE, 0, KK_ACTION, 0.66f },
};

// ⓘ One per face, because the grab area has to fill whatever width that face
// is -- 14 units for Compact, 15.5 for Full. Only the spacer differs.
inline const Key kTabFull[] = {
    { L"", nullptr, 0, 0, KK_SPACER, 12.84f },
    { L"\u2328", nullptr, ACT_LAYOUT, 0, KK_ACTION, 0.67f },
    { L"\u21f3", nullptr, ACT_MOVE,   0, KK_ACTION, 0.67f },
    { L"\u2197", nullptr, ACT_SIZE,   0, KK_ACTION, 0.66f },
    // ⭐ Close here TOO, not instead. rhoquinn8217, 2026-09-02:
    // top-right is where a pointer goes to shut a window, and the
    // key on the face is where a thumb already is. Two ways out of
    // something is not redundancy worth removing.
    { L"x", nullptr, ACT_CLOSE, 0, KK_ACTION, 0.66f },
};

inline const Key kCompact0[] = {
    { L"`", L"~", 0x35, 0, KK_NORMAL, 1.0f },
    { L"1", L"!", 0x1e, 0, KK_FN, 1.0f }, { L"2", L"@", 0x1f, 0, KK_FN, 1.0f },
    { L"3", L"#", 0x20, 0, KK_FN, 1.0f }, { L"4", L"$", 0x21, 0, KK_FN, 1.0f },
    { L"5", L"%", 0x22, 0, KK_FN, 1.0f }, { L"6", L"^", 0x23, 0, KK_FN, 1.0f },
    { L"7", L"&", 0x24, 0, KK_FN, 1.0f }, { L"8", L"*", 0x25, 0, KK_FN, 1.0f },
    { L"9", L"(", 0x26, 0, KK_FN, 1.0f }, { L"0", L")", 0x27, 0, KK_FN, 1.0f },
    { L"-", L"_", 0x2d, 0, KK_FN, 1.0f }, { L"=", L"+", 0x2e, 0, KK_FN, 1.0f },
    { L"\u232b", nullptr, 0x2a, 0, KK_NORMAL, 1.0f },
};
inline const Key kCompact1[] = {
    { L"tab", nullptr, 0x2b, 0, KK_NORMAL, 1.0f },
    { L"q", nullptr, 0x14, 0, KK_NORMAL, 1.0f }, { L"w", nullptr, 0x1a, 0, KK_NORMAL, 1.0f },
    { L"e", nullptr, 0x08, 0, KK_NORMAL, 1.0f }, { L"r", nullptr, 0x15, 0, KK_NORMAL, 1.0f },
    { L"t", nullptr, 0x17, 0, KK_NORMAL, 1.0f }, { L"y", nullptr, 0x1c, 0, KK_NORMAL, 1.0f },
    { L"u", nullptr, 0x18, 0, KK_NORMAL, 1.0f }, { L"i", nullptr, 0x0c, 0, KK_NORMAL, 1.0f },
    { L"o", nullptr, 0x12, 0, KK_NORMAL, 1.0f }, { L"p", nullptr, 0x13, 0, KK_NORMAL, 1.0f },
    { L"[", L"{", 0x2f, 0, KK_NORMAL, 1.0f }, { L"]", L"}", 0x30, 0, KK_NORMAL, 1.0f },
    { L"\\", L"|", 0x31, 0, KK_NORMAL, 1.0f },
};
inline const Key kCompact2[] = {
    { L"ctrl", nullptr, 0, KBD_CTRL, KK_MOD, 1.0f },
    { L"a", nullptr, 0x04, 0, KK_NORMAL, 1.0f }, { L"s", nullptr, 0x16, 0, KK_NORMAL, 1.0f },
    { L"d", nullptr, 0x07, 0, KK_NORMAL, 1.0f }, { L"f", nullptr, 0x09, 0, KK_NORMAL, 1.0f },
    { L"g", nullptr, 0x0a, 0, KK_NORMAL, 1.0f }, { L"h", nullptr, 0x0b, 0, KK_NORMAL, 1.0f },
    { L"j", nullptr, 0x0d, 0, KK_NORMAL, 1.0f }, { L"k", nullptr, 0x0e, 0, KK_NORMAL, 1.0f },
    { L"l", nullptr, 0x0f, 0, KK_NORMAL, 1.0f },
    { L";", L":", 0x33, 0, KK_NORMAL, 1.0f }, { L"'", L"\"", 0x34, 0, KK_NORMAL, 1.0f },
    { L"enter", nullptr, 0x28, 0, KK_NORMAL, 2.0f },
};
inline const Key kCompact3[] = {
    { L"shift", nullptr, 0, KBD_SHIFT, KK_MOD, 1.0f },
    { L"z", nullptr, 0x1d, 0, KK_NORMAL, 1.0f }, { L"x", nullptr, 0x1b, 0, KK_NORMAL, 1.0f },
    { L"c", nullptr, 0x06, 0, KK_NORMAL, 1.0f }, { L"v", nullptr, 0x19, 0, KK_NORMAL, 1.0f },
    { L"b", nullptr, 0x05, 0, KK_NORMAL, 1.0f }, { L"n", nullptr, 0x11, 0, KK_NORMAL, 1.0f },
    { L"m", nullptr, 0x10, 0, KK_NORMAL, 1.0f },
    { L",", L"<", 0x36, 0, KK_NORMAL, 1.0f }, { L".", L">", 0x37, 0, KK_NORMAL, 1.0f },
    { L"/", L"?", 0x38, 0, KK_NORMAL, 1.0f },
    { L"\u2191", nullptr, 0x52, 0, KK_NORMAL, 1.0f },
    { L"shift", nullptr, 0, KBD_SHIFT, KK_MOD, 1.0f },
    { L"fn", nullptr, 0, KBD_FN, KK_MOD, 1.0f },
};
inline const Key kCompact4[] = {
    { L"alt", nullptr, 0, KBD_ALT, KK_MOD, 1.0f },
    { L"win", nullptr, 0, KBD_WIN, KK_MOD, 1.0f },
    { L"space", nullptr, 0x2c, 0, KK_NORMAL, 6.0f },
    { L"paste", L"copy", ACT_PASTE, 0, KK_ACTION, 2.0f },
    { L"\u2190", nullptr, 0x50, 0, KK_NORMAL, 1.0f },
    { L"\u2193", nullptr, 0x51, 0, KK_NORMAL, 1.0f },
    { L"\u2192", nullptr, 0x4f, 0, KK_NORMAL, 1.0f },
    { L"\u2328\u2938", nullptr, ACT_CLOSE, 0, KK_ACTION, 1.0f },
};

struct Row { const Key *keys; int count; };
#define CTM_ROW(a) { a, (int)(sizeof(a) / sizeof(a[0])) }

inline const Row kFullRows[] = {
    CTM_ROW(kTabFull),
    CTM_ROW(kRow0), CTM_ROW(kRow1), CTM_ROW(kRow2), CTM_ROW(kRow3), CTM_ROW(kRow4),
};
inline const Row kSubRows[] = {
    CTM_ROW(kTabSub),
    CTM_ROW(kSub0), CTM_ROW(kSub1), CTM_ROW(kSub2), CTM_ROW(kSub3), CTM_ROW(kSub4),
};
inline const Row kCompactRows[] = {
    CTM_ROW(kTabCompact),
    CTM_ROW(kCompact0), CTM_ROW(kCompact1), CTM_ROW(kCompact2),
    CTM_ROW(kCompact3), CTM_ROW(kCompact4),
};
#undef CTM_ROW


inline const Row *rows_now()
{
    switch (g_face.load()) {
    case FACE_SUB:  return kSubRows;
    case FACE_FULL: return kFullRows;
    default:        return kCompactRows;
    }
}
inline const int kRowCount = 6;          // the tab, then five rows of keys

// ⓘ The tab is shorter than a key row -- it holds icons, not letters.
inline float row_height_factor(int r) { return r == 0 ? 0.5f : 1.0f; }

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

// ⓘ Whether anything has been typed since a modifier was latched, and a tap
// waiting to be sent. See the note where a cleared latch becomes a tap.
inline bool g_latchUsed = false;
inline uint8_t g_tapMod = 0;
inline int g_tapFrames = 0;
// ⛔ g_escFrames is GONE (2026-09-04). It held Escape down for three frames
// after Circle sent it; Circle closes the keyboard now, so nothing sets it.
// ⓘ Removed rather than left -- unused state is what someone later wires back
// up, and T-079 keeps collecting exactly this.
// ⓘ How long a direction has been held, in reports. ⛔ Not per device: it is
// one highlight, and two pads holding opposite directions should fight over it
// exactly as two hands on one keyboard would.
inline int g_dirHeld = 0;

// ⭐⭐ WHERE YOU ARE AND WHAT IS PRESSED ARE DIFFERENT THINGS (rhoquinn8217,
// 2026-09-02). A filled key was being asked to mean three things at once --
// "the cursor is here", "this is held down" and "this is latched" -- so none of
// them read clearly.
//
// ⓘ Now: a HALO marks where the cursor is, a FILL means pressed, a fill that
// stays means latched, and amber means held down on a shoulder.
inline int g_pressRow = -1, g_pressCol = -1;

// ⓘ T-142. The key each pad latched when its Cross went down, kept until that
// Cross is released -- so moving the halo mid-press cannot change what is being
// typed. ⭐ Per device: two pads share this keyboard by design, and a single
// value would let one pad's press decide the other's key.
inline std::map<const void *, uint8_t> heldUsageFor;
inline const void *g_pasteHeld = nullptr;

inline int mod_state(uint8_t bit)
{
    auto it = g_mods.find(bit);
    return it == g_mods.end() ? LATCH_OFF : it->second;
}

inline uint8_t active_mods()
{
    uint8_t m = 0;
    for (const auto &e : g_mods) if (e.second != LATCH_OFF) m |= e.first;
    // ⛔ Fn is ours, not the host's. It picks a layer on this keyboard and must
    // never appear in a report.
    return static_cast<uint8_t>(m & ~KBD_FN);
}

// ⓘ Either way of asking for the F keys.
// ⭐ WHICH F KEY THIS IS, counted among the F-capable keys in its own row.
//
// ⛔ This used to be derived from the COLUMN INDEX, which quietly assumed the
// digits began at column 1. Adding esc pushed them right by one, so 1 became F2
// and F1 disappeared entirely (rhoquinn8217, 2026-09-02). Counting cannot go
// wrong when the layout moves.
inline int fn_index(int row, int col)
{
    int n = 0;
    for (int c = 0; c < rows_now()[row].count; ++c) {
        if (rows_now()[row].keys[c].kind != KK_FN) continue;
        if (c == col) return n;
        ++n;
    }
    return -1;
}

inline bool fn_showing()
{
    return g_fnHeld.load() || mod_state(KBD_FN) != LATCH_OFF;
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
    return rows_now()[row].keys[col];
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
    // ⓘ Rows are no longer all the same height: the tab is shorter than a row
    // of letters, so the unit height is worked out from the TOTAL of the
    // factors rather than from the row count.
    float rowsTall = 0.0f;
    for (int r = 0; r < kRowCount; ++r) rowsTall += row_height_factor(r);
    const int unitH = (int)((h - pad * 2 - rowGap * (kRowCount - 1)) / rowsTall);

    float widest = 0;
    for (int r = 0; r < kRowCount; ++r) {
        float units = 0;
        for (int c = 0; c < rows_now()[r].count; ++c) units += rows_now()[r].keys[c].wide;
        if (units > widest) widest = units;
    }
    // ⛔⛔ THE GAP IS PART OF THE COLUMN, NOT ADDED BETWEEN KEYS.
    //
    // ⚠️ This used to advance by width PLUS a gap, so a row of 8 keys got 7
    // gaps where a row of 14 got 13 -- and the rows drifted apart by the
    // difference. rhoquinn8217 saw it as space not reaching the keys above it
    // (2026-09-02), on a layout that is a perfect grid in the data.
    //
    // ⭐ Now every key is placed at its COLUMN, computed from the units before
    // it, and inset to leave the gap. Rows cannot drift however many keys they
    // hold, because nothing accumulates.
    const int gap = 4;
    const float unit = (float)(w - pad * 2) / widest;

    int y = pad;
    for (int r = 0; r < kRowCount; ++r) {
        const int rowH = (int)(unitH * row_height_factor(r));
        float col = 0.0f;                       // in units, not pixels
        for (int c = 0; c < rows_now()[r].count; ++c) {
            const float wide = rows_now()[r].keys[c].wide;
            Placed pl;
            pl.r.left  = pad + (int)(col * unit) + gap / 2;
            pl.r.right = pad + (int)((col + wide) * unit) - gap / 2;
            pl.r.top = y;
            pl.r.bottom = y + rowH;
            pl.row = r; pl.col = c;
            g_placed.push_back(pl);
            col += wide;
        }
        y += rowH + rowGap;
    }
}

inline const Placed *placed_of(int row, int col)
{
    for (const Placed &p : g_placed) if (p.row == row && p.col == col) return &p;
    return nullptr;
}

// ⭐⭐ THE MOUSE DRIVES IT TOO, always -- not as a mode.
//
// ⛔ rhoquinn8217, 2026-09-02: switching between a mouse and a controller is a
// PHYSICAL act, not a software one. Whichever you touched last should be the
// one that works; having to press a button first is exactly the thing that
// makes something feel broken.
//
// ⓘ -1 when the cursor is not over a key. The hover highlight is also what
// tells someone the keyboard is clickable at all.
inline int g_hoverRow = -1, g_hoverCol = -1;
inline bool g_mouseTracking = false;

// ⓘ A stand-in "device" for the mouse, so a clicked key goes through exactly
// the same per-device path a pad press does -- and cannot cancel a pad that is
// holding something down.
inline const void *const kMouseKey = &g_hoverRow;

inline void press_current(const void *who);
inline void release_current(const void *who);

inline bool key_under(int x, int y, int *row, int *col)
{
    for (const Placed &p : g_placed) {
        if (x >= p.r.left && x < p.r.right && y >= p.r.top && y < p.r.bottom) {
            *row = p.row; *col = p.col;
            return true;
        }
    }
    return false;
}

inline void invalidate()
{
    if (g_hwnd != nullptr) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ⓘ Sideways WRAPS. Clamping doubled the worst journey -- p to q was twelve
// presses and is now one -- and a row is short enough that wrapping within it
// stays predictable.
inline bool is_key(int row, int col)
{
    return rows_now()[row].keys[col].kind != KK_SPACER;
}

inline void move_h(int dir)
{
    const int n = rows_now()[g_row].count;
    // ⓘ Steps over the grab area rather than stopping on it -- it is padding,
    // not a button, and landing there would look like the highlight had stuck.
    for (int i = 0; i < n; ++i) {
        g_col = (g_col + dir + n) % n;
        if (is_key(g_row, g_col)) break;
    }
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
    for (int c = 0; c < rows_now()[next].count; ++c) {
        if (!is_key(next, c)) continue;          // never land on the grab area
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

// ⭐ What a press DOES, in one place, so the mouse and the pad cannot drift
// apart. The pad's own path calls the same thing.
// ⭐ Swapping faces, in one place: the tab's button and the Options button both
// do exactly this, so they cannot come to mean different things.
inline void switch_face()
{
    const wchar_t *was = key_at(g_row, g_col).label;
    // ⓘ Cycles sub -> compact -> full -> sub. One button, three faces.
    g_face.store((g_face.load() + 1) % 3);
    g_placed.clear(); g_placedW = 0;
    int fr = -1, fc = -1;
    for (int r = 0; r < kRowCount && fr < 0; ++r) {
        for (int c = 0; c < rows_now()[r].count; ++c) {
            if (!is_key(r, c)) continue;
            if (wcscmp(rows_now()[r].keys[c].label, was) == 0) { fr = r; fc = c; break; }
        }
    }
    g_row = fr >= 0 ? fr : 2;
    g_col = fc >= 0 ? fc : 1;
    if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CTM_RESIZE, 0, 0);
    invalidate();
}

inline void press_current(const void *who)
{
    const Key &k = key_at(g_row, g_col);

    if (k.kind == KK_ACTION) {
        if (k.usage == ACT_CLOSE) { hide(); return; }
        if (k.usage == ACT_STEAM) { ctm_overlay_open_steam(); hide(); return; }
        if (k.usage == ACT_PASTE) {
            // ⓘ Shift turns it into copy. The shifted label already prints on
            // the key, so it says so rather than needing to be known.
            // ⓘ Not a keystroke: ctrl+v, sent as one. The only entry on the
            // face that is a combination rather than a key.
            const bool asCopy = shift_showing();
            uint8_t pv[6] = { (uint8_t)(asCopy ? 0x06 : 0x19), 0, 0, 0, 0, 0 };
            ctm_keyboard_device::set_state_for(who, KBD_CTRL, pv, 1);
            return;
        }
        if (k.usage == ACT_MOVE) {
            g_atTop.store(!g_atTop.load());
            if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CTM_REPOSITION, 0, 0);
        }
        if (k.usage == ACT_SIZE) {
            g_size.store((g_size.load() + 1) % 3);
            if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CTM_RESIZE, 0, 0);
        }
        if (k.usage == ACT_LAYOUT) switch_face();
        return;
    }

    if (k.kind == KK_MOD) {
        const int st = mod_state(k.mod);
        const int next = (st == LATCH_OFF) ? LATCH_ON
                       : (st == LATCH_ON && k.mod == KBD_SHIFT) ? LATCH_LOCKED
                       : LATCH_OFF;
        // ⛔ NEVER FOR FN. Its bit is 0x80, which in a HID modifier byte is the
        // RIGHT WINDOWS KEY -- so clearing an unused fn latch opened the Start
        // menu (rhoquinn8217, 2026-09-02). active_mods masks fn out, but a tap
        // sends the bit straight through.
        if (next == LATCH_OFF && st == LATCH_ON && !g_latchUsed && k.mod != KBD_FN) {
            g_tapMod = k.mod;
            g_tapFrames = 3;
        }
        if (next != LATCH_OFF) g_latchUsed = false;
        g_mods[k.mod] = next;
        invalidate();                 // the three states are a colour each
        return;
    }

    uint8_t usage = k.usage;
    if (fn_showing() && k.kind == KK_FN) {
        const int idx = fn_index(g_row, g_col);
        if (idx >= 0 && idx < 12) usage = kFnUsages[idx];
    }
    uint8_t mods = active_mods();
    if (g_shiftHeld.load()) mods |= KBD_SHIFT;
    mods |= k.mod;

    uint8_t keys[6] = { usage, 0, 0, 0, 0, 0 };
    ctm_keyboard_device::set_state_for(who, mods, keys, usage != 0 ? 1 : 0);
    if (usage != 0) {
        g_latchUsed = true;
        g_pressRow = g_row; g_pressCol = g_col;      // this one is down
        invalidate();
    }
}

inline void release_current(const void *who)
{
    uint8_t none[6] = { 0, 0, 0, 0, 0, 0 };
    ctm_keyboard_device::set_state_for(who, 0, none, 0);
    if (g_pressRow >= 0) { g_pressRow = g_pressCol = -1; invalidate(); }
    // ⛔ ONLY IF A KEY ACTUALLY USED IT. This cleared every latch on any
    // release -- so clicking ctrl latched it and letting go of the mouse button
    // un-latched it a moment later, which is why mouse latching never appeared
    // to work (rhoquinn8217, 2026-09-02).
    //
    // ⓘ A latch is spent by the key it modified, not by the act of letting go.
    if (!g_latchUsed) return;
    g_latchUsed = false;
    bool changed = false;
    for (auto &e : g_mods) {
        if (e.second == LATCH_ON) { e.second = LATCH_OFF; changed = true; }
    }
    if (changed) invalidate();
}

inline void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC front = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    layout(rc.right, rc.bottom);

    // ⛔⛔ DRAW INTO A BITMAP, THEN BLIT IT ONCE (rhoquinn8217, 2026-09-02:
    // "there is a noticeable flicker when navigating").
    //
    // ⚠️ Painting straight to the window means the screen holds a half-drawn
    // keyboard for a moment on every highlight move -- background cleared,
    // keys appearing one at a time. Moving the highlight repaints the whole
    // surface, so it flickered on every press.
    //
    // ⓘ One buffer per paint rather than one kept around: this happens at the
    // rate a person presses keys, not per frame, and a cached bitmap would
    // need invalidating every time the window resized.
    HDC dc = CreateCompatibleDC(front);
    HBITMAP buffer = CreateCompatibleBitmap(front, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, buffer);

    HBRUSH back = CreateSolidBrush(RGB(0x08, 0x09, 0x0c));
    FillRect(dc, &rc, back);
    DeleteObject(back);

    HBRUSH edge = CreateSolidBrush(RGB(0x2c, 0x2e, 0x36));
    FrameRect(dc, &rc, edge);
    DeleteObject(edge);

    // ⛔ MEASURE A KEY ROW, NOT THE FIRST ENTRY. The first entry is the tab
    // now, which is half the height -- so every letter on the keyboard shrank
    // to fit a strip it is not drawn in (rhoquinn8217, 2026-09-02).
    int keyH = 30;
    for (const Placed &pl : g_placed) {
        if (pl.row == 1) { keyH = pl.r.bottom - pl.r.top; break; }
    }
    HFONT font = CreateFontW(keyH * 5 / 9, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT small = CreateFontW(keyH * 3 / 10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    // ⛔ THE TAB NEEDS ITS OWN FONT. Its buttons are half a key row tall, and
    // drawing them with the key font meant glyphs taller than the box they sat
    // in -- which is why they looked off-centre rather than centred
    // (rhoquinn8217, 2026-09-02).
    int tabH = keyH / 2;
    for (const Placed &pl : g_placed) {
        if (pl.row == 0) { tabH = pl.r.bottom - pl.r.top; break; }
    }
    HFONT tabFont = CreateFontW(tabH * 3 / 5, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    // ⭐ A LATCHED KEY IS BOLD. Colour alone says which of three states a
    // modifier is in; bold says it at a glance from across a room, which is
    // what this keyboard is for.
    HFONT boldSmall = CreateFontW(keyH * 2 / 5, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT bold = CreateFontW(keyH * 5 / 9, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
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
    // ⓘ Lighter than a key, dimmer than the halo: enough to say "this is
    // clickable" without competing with where the pad is.
    HBRUSH fillHover  = CreateSolidBrush(RGB(0x24, 0x28, 0x34));
    // ⓘ Amber for HELD, matching Safe Edit Mode on the settings page: the
    // same colour already means "on, and not because you latched it".
    HBRUSH fillHeld   = CreateSolidBrush(RGB(0x5a, 0x4a, 0x1c));
    HBRUSH edgeHot    = CreateSolidBrush(RGB(0x8f, 0xa8, 0xff));

    // ⓘ Held on R1, or latched from the fn key -- either shows the F row.
    const bool fnNow = g_fnHeld.load() || mod_state(KBD_FN) != LATCH_OFF;
    const bool shiftNow = shift_showing();

    for (const Placed &p : g_placed) {
        const Key &k = key_at(p.row, p.col);
        // ⓘ FOUR SEPARATE FACTS, each with its own mark now.
        const bool hot = (p.row == g_row && p.col == g_col);          // the halo
        const bool hover = (p.row == g_hoverRow && p.col == g_hoverCol);
        const bool pressed = (p.row == g_pressRow && p.col == g_pressCol);

        HBRUSH fill = fillNormal;
        if (k.kind == KK_MOD) {
            const int st = mod_state(k.mod);
            fill = st == LATCH_LOCKED ? fillLock : st == LATCH_ON ? fillLatch : fillMod;
        } else if (k.kind == KK_FN && fnNow) {
            fill = fillFn;
        } else if (k.kind == KK_ACTION) {
            fill = fillMod;
        }
        // ⭐ THE FILL SAYS WHAT THE KEY IS DOING, never where the cursor is.
        if ((k.kind == KK_MOD && k.mod == KBD_SHIFT && g_shiftHeld.load()) ||
            (k.kind == KK_MOD && k.mod == KBD_FN && g_fnHeld.load())) fill = fillHeld;
        if (pressed) fill = fillHot;
        RECT r = p.r;
        // ⓘ The grab area is drawn as nothing: no fill, no label. It reads as
        // part of the frame, which is exactly what it is for.
        if (k.kind == KK_SPACER) continue;
        FillRect(dc, &r, (hover && !pressed) ? fillHover : fill);

        // ⭐ THE HALO IS A RING, NOT A FILL. Where you ARE has to stay legible
        // on top of whatever the key is DOING, and a ring is the only mark
        // that does not compete with the colour underneath it.
        if (hot) {
            RECT ring = r;
            for (int i = 0; i < 3; ++i) {
                FrameRect(dc, &ring, edgeHot);
                ring.left += 1; ring.top += 1; ring.right -= 1; ring.bottom -= 1;
            }
        }

        // ⓘ One label decided in one place: F-keys win over shifted, which wins
        // over the plain one.
        const wchar_t *label = k.label;
        if (k.usage == 0x29 && fnNow) {
            label = L"`";                 // esc becomes the console key
        } else if (k.kind == KK_FN && fnNow) {
            const int idx = fn_index(p.row, p.col);
            if (idx >= 0 && idx < 12) label = kFnLabels[idx];
        } else if (shiftNow && k.shifted != nullptr) {
            label = k.shifted;
        } else if (shiftNow && k.kind != KK_ACTION && k.kind != KK_MOD
                   && k.usage >= 0x04 && k.usage <= 0x1d) {
            static wchar_t up[2];
            up[0] = (wchar_t)(L'A' + (k.usage - 0x04));
            up[1] = 0;
            label = up;
        }

        // ⭐ THE LABEL SAYS ITS OWN STATE. A latched modifier gains ⌵ and goes
        // bold; a shift being HELD on the bumper goes bold and shouts, because
        // that is what it is doing to every letter you press.
        wchar_t shown[24];
        const bool latched = (k.kind == KK_MOD && mod_state(k.mod) != LATCH_OFF);
        const bool heldNow = (k.kind == KK_MOD && k.mod == KBD_SHIFT
                              && g_shiftHeld.load());
        // ⭐ THREE APPEARANCES (rhoquinn8217, 2026-09-02):
        //   off      "shift"
        //   latched  bold "shift⌵"
        //   held     bold "⌵shift⌵"
        // ⓘ A countersink on BOTH sides for held, because held is the state
        // that is doing something to every key you press, not just the next.
        if (heldNow) {
            int i = 0;
            shown[i++] = L'\u2335';
            for (int j = 0; label[j] != 0 && i < 20; ++j) shown[i++] = label[j];
            shown[i++] = L'\u2335';
            shown[i] = 0;
            label = shown;
        } else if (latched) {
            int i = 0;
            for (; label[i] != 0 && i < 20; ++i) shown[i] = label[i];
            shown[i++] = L'\u2335';
            shown[i] = 0;
            label = shown;
        }

        // ⛔ A MARKED LABEL NEEDS A SMALLER FONT. "⌵shift⌵" is nearly twice the
        // width of "shift", and DrawText with DT_CENTER clips what will not
        // fit -- from BOTH ends, which ate the leading countersink and left
        // only the trailing one visible (rhoquinn8217, 2026-09-02).
        HGDIOBJ chosen = SelectObject(dc, p.row == 0 ? tabFont
                                        : heldNow ? boldSmall
                                        : latched ? bold : font);
        SetTextColor(dc, pressed ? RGB(0xff, 0xff, 0xff) : RGB(0xe8, 0xea, 0xf2));
        DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, chosen);

        // ⭐ THE SHIFTED CHARACTER, PRINTED SMALL IN THE CORNER -- Steam does
        // this and it is the best thing on their keyboard: you never have to
        // hold shift to find out what a key will give you.
        // ⓘ Skipped while shift is showing, because then the big label IS the
        // shifted one and printing it twice says nothing.
        if (k.shifted != nullptr && !shiftNow) {
            RECT s = r;
            s.left += 4; s.top += 2;
            SetTextColor(dc, RGB(0x8b, 0x8d, 0x96));
            HGDIOBJ prev = SelectObject(dc, small);
            DrawTextW(dc, k.shifted, -1, &s, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, prev);
        }

        // ⭐ AND THE SHOULDER SHORTCUT, on the key it belongs to. Without this
        // the shoulders are folklore -- discoverable only by being told.
        const wchar_t *hint = k.kind == KK_SHOULDER_L1 ? L"L1"
                            : k.kind == KK_SHOULDER_R1 ? L"R1"
                            : k.kind == KK_SHOULDER_R2 ? L"R2" : nullptr;
        if (hint != nullptr) {
            RECT s = r;
            s.right -= 5; s.top += 2;
            SetTextColor(dc, RGB(0x9d, 0xb4, 0xff));
            HGDIOBJ prev = SelectObject(dc, small);
            DrawTextW(dc, hint, -1, &s, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, prev);
        }
    }

    // ⭐ THE TITLE, in the space that is otherwise a handle. It says what this
    // is -- useful the first time someone meets it, and it costs a strip that
    // was empty anyway.
    for (const Placed &pl : g_placed) {
        if (pl.row != 0 || key_at(0, pl.col).kind != KK_SPACER) continue;
        RECT tr = pl.r;
        tr.left += 8;
        SelectObject(dc, tabFont);
        SetTextColor(dc, RGB(0x8b, 0x8d, 0x96));
        const wchar_t *title =
            g_face.load() == FACE_FULL ? L"DS5-USBIP Virtual Keyboard - FULL" :
            g_face.load() == FACE_SUB  ? L"DS5-USBIP Virtual Keyboard - SUB-COMPACT"
                                       : L"DS5-USBIP Virtual Keyboard - COMPACT";
        DrawTextW(dc, title, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // ⭐⭐ A LEGEND, because the buttons are not guessable (rhoquinn8217,
        // 2026-09-04 -- he asked what Circle did, having built it).
        //
        // ⭐ PlayStation glyphs ONLY. Combining them with Xbox letters --
        // "✕/A select" -- reads as a fraction rather than a button and is
        // unreadable at a distance.
        //
        // ⓘ Right-aligned in the SAME rectangle as the title, so it costs no
        // layout: the tab's spacer is 8.34 columns even on the narrowest face,
        // and the tab font fits roughly eleven characters per column.
        //
        // ⛔ The order is by how surprising each one is, not by button position:
        // Circle CLOSES here where it is Escape everywhere else, and Square is
        // backspace where it opened the keyboard. Those two are why this exists.
        // ⛔ ONLY IF IT FITS. The title is LONGEST on the sub-compact face and
        // the spacer is NARROWEST there, so a left-aligned title and a
        // right-aligned legend can collide in the middle. Measured rather than
        // assumed -- the estimate that said it would fit was wrong by 4x once.
        const wchar_t *legend =
            L"\u25cb close   \u25a1 \u232b   \u25b3 space   "
            L"\u2715 select   \u2699 move";
        SIZE ts = {0, 0}, ls = {0, 0};
        GetTextExtentPoint32W(dc, title, lstrlenW(title), &ts);
        GetTextExtentPoint32W(dc, legend, lstrlenW(legend), &ls);
        const LONG room = (pl.r.right - 8) - (pl.r.left + 8);
        if (ts.cx + ls.cx + 24 <= room) {
            RECT lr = pl.r;
            lr.right -= 8;
            DrawTextW(dc, legend, -1, &lr,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        break;
    }

    DeleteObject(fillNormal); DeleteObject(fillMod); DeleteObject(fillFn);
    DeleteObject(fillLatch); DeleteObject(fillLock);
    DeleteObject(fillHover);
    DeleteObject(fillHeld);
    DeleteObject(fillHot); DeleteObject(edgeHot);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteObject(small);
    DeleteObject(tabFont);
    DeleteObject(bold);
    DeleteObject(boldSmall);

    // ⓘ The finished picture arrives in one go.
    BitBlt(front, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(buffer);
    DeleteDC(dc);

    EndPaint(hwnd, &ps);
}

inline LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint(hwnd);
        return 0;

    // ⛔ NOTHING TO ERASE. The paint covers every pixel, so letting Windows
    // clear the window first only adds a flash of blank between the two.
    case WM_ERASEBKGND:
        return 1;
    // ⛔ REFUSE ACTIVATION EVEN IF ASKED. WS_EX_NOACTIVATE covers clicks, but a
    // stray SetForegroundWindow from elsewhere would still hand us focus -- and
    // the moment this window has focus, the game stops receiving the keystrokes
    // it exists to send.
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    // ⭐⭐ DRAG IT BY ITS BACKGROUND (rhoquinn8217, 2026-09-02).
    //
    // ⓘ Telling Windows a point is the CAPTION makes it drag the window from
    // there -- so the frame and the gaps between keys become a handle, and the
    // keys themselves stay clickable. No drag code of our own, and no mode.
    //
    // ⛔ Only where there is no key. Reporting the whole surface as caption
    // would make the keyboard undraggable in the useful sense: every click
    // would move it instead of typing.
    //
    // ⚠️ And this does NOT give it focus: a caption drag on a WS_EX_NOACTIVATE
    // window moves it without activating, which is the whole point.
    case WM_NCHITTEST: {
        POINT pt = { (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        int r = -1, c = -1;
        if (!key_under(pt.x, pt.y, &r, &c)) return HTCAPTION;
        // ⓘ The tab's padding is a handle, not a key -- so dragging from it
        // moves the window, which is the whole reason the tab exists.
        return is_key(r, c) ? HTCLIENT : HTCAPTION;
    }

    // ⓘ Triangle's two positions still work; dragging is the free-form version
    // of the same thing, and neither cancels the other.
    case WM_EXITSIZEMOVE:
        return 0;

    case WM_MOUSEMOVE: {
        // ⛔ ASK TO BE TOLD WHEN THE CURSOR LEAVES. Windows sends moves but no
        // "gone" message unless requested, so the last key stayed lit after the
        // cursor had left the window entirely.
        if (!g_mouseTracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_mouseTracking = true;
        }
        int r = -1, c = -1;
        const int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        if (!key_under(x, y, &r, &c)) { r = -1; c = -1; }
        if (r != g_hoverRow || c != g_hoverCol) {
            g_hoverRow = r; g_hoverCol = c;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_mouseTracking = false;
        if (g_hoverRow != -1) {
            g_hoverRow = g_hoverCol = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        int r = -1, c = -1;
        if (key_under((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), &r, &c)) {
            // ⓘ The click moves the PAD's highlight too. Two pointers
            // disagreeing about where you are is worse than either alone.
            g_row = r; g_col = c;
            const Placed *pl = placed_of(r, c);
            if (pl) g_anchorX = (pl->r.left + pl->r.right) / 2;
            // ⛔⛔ CAPTURE THE MOUSE. Without it the button-up goes to whatever
            // window is under the cursor, so releasing OUTSIDE the keyboard
            // meant we never heard it and the key stayed held down forever
            // (rhoquinn8217, 2026-09-02). Capture guarantees the release
            // comes back here wherever the cursor ends up.
            SetCapture(hwnd);
            press_current(kMouseKey);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        release_current(kMouseKey);
        return 0;

    // ⛔ AND IF THE CAPTURE IS TAKEN AWAY -- by another window, by Alt+Tab, by
    // anything -- the key must still come up. A stuck key is worse than a
    // missed one, and this is the path that has no button-up at all.
    case WM_CAPTURECHANGED:
        release_current(kMouseKey);
        return 0;
    case WM_APP + 3: {                 // WM_CTM_NUDGE
        const int dx = g_nudgeX.exchange(0);
        const int dy = g_nudgeY.exchange(0);
        if (dx == 0 && dy == 0) return 0;
        RECT rc;
        GetWindowRect(hwnd, &rc);
        // ⓘ Kept on screen: a keyboard dragged off the edge cannot be dragged
        // back, and there is no title bar to grab.
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        int x = rc.left + dx, y = rc.top + dy;
        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (x < -w / 3) x = -w / 3;
        if (y < 0) y = 0;
        if (x > screenW - w / 3) x = screenW - w / 3;
        if (y > screenH - h / 3) y = screenH - h / 3;
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    }

    case WM_APP + 2: {                 // WM_CTM_RESIZE
        int w = 0, h = 0;
        size_for(&w, &h);
        // ⭐ GROWS WHERE IT STANDS (rhoquinn8217, 2026-09-02). It used to jump
        // back to the centre, which threw away wherever you had just dragged
        // it to -- so resizing and placing fought each other.
        //
        // ⓘ Around its own MIDDLE, not its corner: growing from the top-left
        // pushes a keyboard that lives at the bottom of the screen off it.
        RECT rc;
        GetWindowRect(hwnd, &rc);
        int x = rc.left - (w - (rc.right - rc.left)) / 2;
        int y = rc.top  - (h - (rc.bottom - rc.top)) / 2;
        // ⓘ Still clamped: there is no title bar to grab it back by.
        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (x < -w / 3) x = -w / 3;
        if (y < 0) y = 0;
        if (x > screenW - w / 3) x = screenW - w / 3;
        if (y > screenH - h / 3) y = screenH - h / 3;
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
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

    // ⭐⭐ A HELD DIRECTION REPEATS (rhoquinn8217, 2026-09-02: "instinctually I
    // expect it to repeat"). Every keyboard does, and without it crossing this
    // grid means fourteen separate presses.
    //
    // ⓘ The same shape as a keyboard's own repeat: a pause first, so a single
    // press stays single, then a steady run. ⛔ Measured in REPORTS rather than
    // milliseconds -- they arrive at a known rate and it costs no clock.
    const int kFirst = 90;      // ~360ms at 250Hz
    const int kThen  = 14;      // ~56ms between repeats
    const bool anyDir = up || down || left || right;
    if (!anyDir) {
        g_dirHeld = 0;
    } else {
        ++g_dirHeld;
    }
    const bool repeatNow = (g_dirHeld > kFirst)
                        && ((g_dirHeld - kFirst) % kThen == 0);

    if (edge(deviceKey, 0, up)    || (repeatNow && up))    move_v(-1);
    if (edge(deviceKey, 1, down)  || (repeatNow && down))  move_v(1);
    if (edge(deviceKey, 2, left)  || (repeatNow && left))  move_h(-1);
    if (edge(deviceKey, 3, right) || (repeatNow && right)) move_h(1);

    // ⭐ THE HELD LAYERS. L1 shows capitals and shifted punctuation, L2 turns
    // the digit row into F1-F12 in place.
    // ⛔⛔ L1 AND R1 ONLY -- THE TRIGGERS ARE LEFT ALONE (rhoquinn8217,
    // 2026-09-02). L2 and R2 are the analog triggers, and they are already
    // spoken for by the gyro and stick mouse gates; R2 was also being caught
    // by accident on everything in Windows. The bumpers are a deliberate press
    // in a way a trigger is not.
    //
    // ⓘ And the shoulder SHORTCUTS are gone with them: L1 and R1 now shift the
    // layers, so they cannot also be space and backspace. Both are on the face.
    const bool l1 = button_down(data, len, 4);
    const bool r1 = button_down(data, len, 5);
    if (l1 != g_shiftHeld.load()) { g_shiftHeld.store(l1); invalidate(); }
    if (r1 != g_fnHeld.load())    { g_fnHeld.store(r1);   invalidate(); }

    // ⭐ THE BUTTON THAT OPENED IT ALSO CLOSES IT.
    //
    // ⛔ Restored 2026-09-02: this block was lost inside an earlier rewrite, so
    // only the on-screen key could dismiss the keyboard.
    //
    // ⛔ AND CIRCLE IS NOT ONE OF THEM ANY MORE (rhoquinn8217). Circle is esc,
    // which is the back-out key everywhere else in this project and the thing
    // you most often want when a dialog is in the way.
    //
    // ⓘ The close is not armed until the opening button has been released --
    // it is still held when the window appears, and acting on that press would
    // close what it just opened.
    // ⛔⛔ THE OPENING PRESS MUST NOT TYPE (2026-09-04).
    //
    // ⚠️ This guard used to stop the opening button CLOSING the keyboard it had
    // just opened. Circle closes now -- but the same trap moved rather than
    // went away: **Square opens the keyboard and is BACKSPACE once it is up**,
    // so the press that opened it would delete a character on arrival.
    //
    // ⓘ Armed only once that button has been seen released.
    const int openedBy = g_openedBy.load();
    const bool openBtnStillDown =
        (openedBy >= 0 && button_down(data, len, openedBy));
    if (!g_closeArmed.load() && !openBtnStillDown) g_closeArmed.store(true);
    const bool faceArmed = g_closeArmed.load();

    // ⭐ CIRCLE IS ESC. Sent straight through rather than moving the highlight,
    // so backing out of a dialog costs one press from wherever you are.
    // ⭐⭐ CIRCLE CLOSES THE KEYBOARD (2026-09-04). It used to type Escape.
    //
    // ⓘ Both Valve and Microsoft put close/cancel on this button: the Steam
    // Deck's keyboard closes with B, and B is Escape everywhere else in its
    // desktop layout. ⭐ And Escape is not lost -- there is an `esc` key on the
    // keyboard itself, which is the honest place for it: a keystroke that fires
    // into whatever is BEHIND the keyboard was a surprise, not a feature.
    if (edge(deviceKey, 5, button_down(data, len, 1))) {
        hide();
        return true;
    }
    // ⭐⭐ TRIANGLE: TAP TO SNAP, HOLD TO STEER.
    //
    // ⓘ The tap fires on RELEASE, not on press, and only when the stick was
    // never used -- otherwise every drag would end by also snapping the window
    // to the top or bottom, undoing the placing you just did.
    // ⭐⭐ OPTIONS MOVES THE WINDOW, NOT TRIANGLE (2026-09-04).
    //
    // ⭐ Triangle is SPACE on both the Steam Deck and Windows 11's gamepad
    // keyboard -- Y in their notation -- and space is around a fifth of
    // everything anyone types, so it earns a face button.
    // ⓘ Moving goes to Options, which is where the Deck community reports the
    // hamburger/menu button already repositions its keyboard.
    // ⚠️ Options previously cycled the FACE. That is not lost -- the tab's own
    // ⌨ key still does it -- but it no longer has a button.
    const bool tri = button_down(data, len, 9);
    if (tri && !g_triHeld.load()) {
        g_triHeld.store(true);
        g_triMoved.store(false);
        g_dragHaveFrom = (GetCursorPos(&g_dragFrom) != 0);
    } else if (!tri && g_triHeld.load()) {
        g_triHeld.store(false);
        g_dragHaveFrom = false;
        if (!g_triMoved.load()) {
            g_atTop.store(!g_atTop.load());
            if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CTM_REPOSITION, 0, 0);
        }
    }

    if (g_triHeld.load()) {
        // ⓘ The left stick, with a deadzone so a resting stick does not creep.
        // ⛔ Byte 1 and 2 are LX and LY, 0x80 at centre -- the same bytes the
        // stick mouse reads, and the same reason for the deadzone.
        const int lx = (int)data[1] - 128;
        const int ly = (int)data[2] - 128;
        const int dead = 18;
        int dx = 0, dy = 0;
        if (lx > dead || lx < -dead) dx = lx / 16;
        if (ly > dead || ly < -dead) dy = ly / 16;
        // ⭐⭐ AND THE MOUSE STEERS IT TOO, on the same hold. Whichever you
        // reach for works -- the pad or the mouse -- with no button to press
        // first, which is the rule the hover highlight already follows.
        //
        // ⓘ Read from the SYSTEM rather than from mouse messages: the pointer
        // is usually not over the keyboard while you are placing it, and a
        // window gets no moves for a cursor outside it.
        //
        // ⛔ By the DELTA since the last look, not to the cursor's position --
        // otherwise the keyboard would leap so its corner sat under the
        // pointer the moment you held Triangle.
        POINT now;
        if (g_dragHaveFrom && GetCursorPos(&now)) {
            const int mx = now.x - g_dragFrom.x;
            const int my = now.y - g_dragFrom.y;
            if (mx != 0 || my != 0) {
                dx += mx;
                dy += my;
                g_dragFrom = now;
            }
        }

        if (dx != 0 || dy != 0) {
            g_nudgeX.fetch_add(dx);
            g_nudgeY.fetch_add(dy);
            g_triMoved.store(true);
            if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CTM_NUDGE, 0, 0);
        }
        // ⛔ While steering, nothing else on the pad acts: the d-pad must not
        // also be walking the keys, and Cross must not be typing.
        return true;
    }

    // ⭐⭐ OPTIONS SWITCHES THE FACE. Create is how BIG; Options is how MUCH --
    // genuinely different questions, and a big compact keyboard is as
    // reasonable a thing to want as a small full one.
    // ⛔ Options no longer cycles the face -- it moves the window now. The
    // tab's ⌨ key still cycles, which is where someone looks for it anyway.
    //
    // ⓘ Create still cycles the SIZE, below.

    // ⭐ CREATE CYCLES THE THREE SIZES. It is free, out of the way of typing,
    // and its own thing rather than a mode.
    if (edge(deviceKey, 7, button_down(data, len, 8))) {
        g_size.store((g_size.load() + 1) % 3);
        if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CTM_RESIZE, 0, 0);
    }

    // ⭐⭐ CROSS PRESSES THE HIGHLIGHTED KEY.
    const Key &k = key_at(g_row, g_col);
    const bool cross = (data[8] & 0x20) != 0;

    // ⛔ AGAIN: ONE IMPLEMENTATION. This was a second copy of the latch rules,
    // and it drifted exactly as the actions did -- the guard that stops fn
    // being sent as a keystroke went into press_current and this copy kept
    // sending it, which is why unlatching fn opened the Start menu
    // (rhoquinn8217, 2026-09-02).
    if (cross && k.kind == KK_MOD && edge(deviceKey, 8, true)) {
        press_current(deviceKey);
    } else if (!cross) {
        edge(deviceKey, 8, false);          // keep the modifier edge honest
    }

    // ⭐ AN ACTION KEY ACTS ON THE KEYBOARD and sends nothing. Handled before
    // the sending path so it cannot also type.
    // ⛔ ONE IMPLEMENTATION OF WHAT A KEY DOES. This used to be a second copy
    // of the action handling, and it drifted the moment new actions were added
    // -- the layout button worked with a mouse and did nothing from the pad
    // (rhoquinn8217, 2026-09-02). The pad now presses the same way a click does.
    if (cross && k.kind == KK_ACTION && edge(deviceKey, 9, true)) {
        press_current(deviceKey);
        if (!visible()) return true;          // it may have closed itself
    } else if (!cross) {
        edge(deviceKey, 9, false);
    }

    // ⓘ An ordinary key rides with whatever modifiers are active, and the
    // shoulders are shortcuts to keys that are also ON the keyboard -- Steam
    // does the same, and a shortcut to something invisible is folklore.
    uint8_t usage = 0;
    // ⛔⛔ THE KEY IS CHOSEN WHEN CROSS GOES DOWN, AND NOT AGAIN (T-142).
    //
    // ⚠️ This used to read the key under the halo on EVERY report while Cross
    // was held. Hold Cross on `a`, move the halo to `b`, release -- and the
    // host saw `a` go up and `b` go down without the button ever having been
    // released, so BOTH letters typed.
    //
    // ⓘ The modifier and action paths immediately above both use edge(); this
    // one was the odd one out.
    //
    // ⭐ Measured before fixing: the sequence gives "a b (release)" the old way
    // and "a (release)" this way.
    // ⭐⭐ SQUARE IS BACKSPACE, TRIANGLE IS SPACE (2026-09-04).
    //
    // ⭐ Both Valve and Microsoft landed here independently: the Steam Deck uses
    // X for backspace and Y for space, and Windows 11's gamepad keyboard uses
    // the same two. Different companies, different operating systems, the same
    // answer -- which is the strongest evidence a convention exists.
    // ⓘ In DualSense terms X is Square and Y is Triangle.
    //
    // ⛔ These take priority over the key under the halo, because our device
    // sends ONE key at a time: Cross-and-Square together must not try to send
    // both. ⓘ They are also repeated by the same repeat timer as everything
    // else, so holding backspace deletes a run.
    // ⛔ R2 IS NOT ENTER, AND THAT IS DELIBERATE (2026-09-04). Both the Deck and
    // Windows put Enter on a trigger or the menu button -- but R2 is MouseLeft
    // in our presets, and with gyro-to-mouse someone clicks INTO a text field
    // while this keyboard is up. Taking R2 would remove the click they need.
    // ➡️ Revisit once T-149 moves clicks onto the touchpad. ⓘ Enter is on the
    // keyboard as a key meanwhile.
    uint8_t faceUsage = 0;
    if (faceArmed) {
        if (button_down(data, len, 2))      faceUsage = 0x2A;   // square: backspace
        else if (button_down(data, len, 3)) faceUsage = 0x2C;   // triangle: space
    }

    // ⓘ PER DEVICE, using the file's own edge helper (slot 6 was free) rather
    // than a bare static: two pads share this keyboard by design, and a static
    // would let one pad's press decide the other's key.
    const bool crossFresh = edge(deviceKey, 6, cross);
    uint8_t &heldUsage = heldUsageFor[deviceKey];

    if (cross && k.kind != KK_MOD && k.kind != KK_ACTION) {
        if (crossFresh) {
            heldUsage = k.usage;
            // ⓘ Esc is the backtick while L2 is held -- the developer console in
            // a great many PC games, and nothing else on the pad produces one.
            if (g_fnHeld.load() && heldUsage == 0x29) heldUsage = 0x35;
            if (fn_showing() && k.kind == KK_FN) {
                const int idx = fn_index(g_row, g_col);
                if (idx >= 0 && idx < 12) heldUsage = kFnUsages[idx];
            }
            // ⓘ And the PRESSED MARKER latches with it: it followed the halo
            // too, so the drawing disagreed with the key actually down.
            g_pressRow = g_row; g_pressCol = g_col;
            invalidate();
        }
        usage = heldUsage;
    } else if (!cross) {
        heldUsage = 0;
    }


    uint8_t mods = active_mods();
    if (g_shiftHeld.load()) mods |= KBD_SHIFT;
    // ⓘ A key can carry its own modifier -- " is shift+' -- so it types without
    // anything being latched first.
    if (usage != 0 && k.kind == KK_NORMAL) mods |= k.mod;

    // ⓘ A tap in flight outranks everything: it is a modifier with NO key, held
    // for a few reports so the host registers a press and a release.
    if (g_tapFrames > 0) {
        --g_tapFrames;
        uint8_t none[6] = { 0, 0, 0, 0, 0, 0 };
        ctm_keyboard_device::set_state_for(deviceKey, g_tapMod, none, 0);
        return true;
    }

    // ⛔ A face key WINS over the halo key, for the reason above.
    if (faceUsage != 0) usage = faceUsage;

    uint8_t keys[6] = { usage, 0, 0, 0, 0, 0 };
    ctm_keyboard_device::set_state_for(deviceKey, usage != 0 ? mods : 0,
                                       keys, usage != 0 ? 1 : 0);
    if (usage != 0) g_latchUsed = true;

    // ⛔ A LATCH RELEASES ON THE KEY IT MODIFIED, and a LOCK does not. That is
    // the whole difference between the two, and it happens on the RELEASE so
    // the modifier is still applied to the key that was just sent.
    static bool sentLast = false;
    if (usage != 0) {
        sentLast = true;
        // ⓘ The marker is latched on the press edge above and left alone here,
        // so it stays on the key that is down instead of following the halo.
    } else if (sentLast) {
        sentLast = false;
        if (g_pressRow >= 0) { g_pressRow = g_pressCol = -1; invalidate(); }
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
    // ⛔ The button that closed this is still held. Without swallowing it, the
    // next report goes down the ordinary path and a rebound Cross hands the app
    // behind an Enter nobody meant to press.
    ctm_rebind_swallow_held();
    // ⛔ Whatever was held goes up with the window. A key left down repeats
    // forever and looks like a stuck keyboard -- the same class of fault as a
    // stuck mouse button, and the same reason unbridging releases its keys.
    {
        std::lock_guard<std::mutex> lock(g_padMutex);
        for (const auto &entry : g_padPrev) {
            ctm_keyboard_device::forget_device(entry.first);
        }
        g_padPrev.clear();
        // ⓘ T-142's latched key goes with it, or a key held when the keyboard
        // closed would still be the "held" one when it next opens.
        heldUsageFor.clear();
    }
    // ⓘ Posting rather than destroying from here: the window belongs to the
    // thread that created it, and destroying it from another one is undefined.
    if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

} // namespace ctm_overlay
