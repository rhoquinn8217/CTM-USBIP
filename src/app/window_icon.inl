// The settings window's taskbar icon, kept sharp.
//
// ⭐⭐ WHY THE LISTENER REACHES INTO A BROWSER'S WINDOW (rhoquinn8217,
// 2026-10-01: *"use a higher resolution icon. the one in the task bar looks
// blurry. The one in tray looks ok."*).
//
// The settings window is Chrome's or Edge's (open_ui.inl), and a browser makes
// such a window's icons from the page's FAVICON. A favicon is a 16-unit
// picture: the browser keeps it at favicon sizes and STRETCHES that to whatever
// Windows asks for. Measured that day at 150% scaling: Windows wanted a 48 px
// icon for the taskbar, and the browser's 48 px icon was a small frame blown
// up to fit. Every edge in it was a smear.
//
// ⛔ NO FRAME IN OUR .ico CHANGES THAT, however large. The browser never looks
// past the favicon sizes, so "a higher resolution icon" cannot be delivered
// through the page at all.
//
// ➡️ So the listener gives the window its icons directly. WM_SETICON is an
// ordinary message, a window takes it from any program running at its own
// privilege level, and the picture is then the frame installer\make-icon-ds5.ps1
// drew for exactly that many pixels.
// ⓘ The TITLE BAR is not touched by this. The browser paints its own title bar
// from the favicon, at a size the favicon is good for.
//
// ⛔ ALSO TRIED, AND IT DOES NOTHING, so nobody tries it again: naming our icon
// in the window's System.AppUserModel.RelaunchIconResource property, with and
// without the relaunch command and name beside it. The shell accepts all three
// from another process, reads them back correctly, and the running taskbar
// button goes on showing the window's own icon.
//
// ⚠️ THREE THINGS HERE WERE MEASURED, NOT ASSUMED:
// - THE TASKBAR DRAWS THE BIG ICON, AND SETTING THE BIG ONE ALONE DOES NOT MAKE
//   IT LOOK. Set alone, the big one left the old picture on the button for as
//   long as it was watched. Set with the small one after it, the button showed
//   the new big icon within seconds. So: big first, small second, always both.
// - THE HANDLES ARE OURS AND DIE WITH US. An icon belongs to the process that
//   loaded it, and the window holds only its number. A probe that set one and
//   exited left the window holding a dead handle. So they are loaded once, kept
//   for the life of the listener, and taken back off the window in stop().
// - THE BROWSER SETS ITS OWN AFTER THE WINDOW EXISTS, when the favicon arrives,
//   and is free to do it again whenever it likes. So the icons are KEPT, not
//   set once: a few times a second this asks the window which icons it has and
//   puts ours back if they are not ours. The log says each time it had to.

#pragma once

namespace window_icon {

// ⓘ Four times a second. A window opening with the browser's icon wears it for
// a quarter of a second at most, and the cost is one walk of the top-level
// windows, which Windows' own shell does far more often than this.
inline const int kEveryMs = 250;

// ⛔ NEVER A PLAIN SendMessage. The window is another program's, and a browser
// that has stopped answering would stop this thread with it, for good.
inline const UINT kAskMs = 200;

inline std::atomic_bool g_running{false};
inline std::mutex g_mutex;          // a pass and stop() never overlap
inline HICON g_big = nullptr;
inline HICON g_small = nullptr;
inline int g_bigPx = 0;
inline int g_smallPx = 0;
inline HWND g_last = nullptr;       // where the window was found last pass
inline HWND g_wearing = nullptr;    // the window that has been given them
inline int g_putBack = 0;           // times the browser replaced them, on that window
inline bool g_saidNoIcon = false;

inline bool has_app_title(HWND hwnd)
{
    wchar_t title[512] = {};
    if (GetWindowTextW(hwnd, title, 511) <= 0) return false;
    return window_icon_rule::is_app_window_title(title, ctm_open_ui::kTitleMarker);
}

inline BOOL CALLBACK find_proc(HWND hwnd, LPARAM param)
{
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (!has_app_title(hwnd)) return TRUE;
    *reinterpret_cast<HWND *>(param) = hwnd;
    return FALSE;                                   // stop at the first match
}

// ⓘ The window from last time is asked first, so a pass with the window up
// costs one title read rather than a walk of every window.
// ⚠️ Its title is read again every time: Windows hands a closed window's
// number to the next one made, and that one is not ours.
inline HWND find_app_window()
{
    if (g_last != nullptr && IsWindow(g_last) && IsWindowVisible(g_last) && has_app_title(g_last)) {
        return g_last;
    }
    g_last = nullptr;
    EnumWindows(find_proc, reinterpret_cast<LPARAM>(&g_last));
    return g_last;
}

inline bool ask(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, DWORD_PTR *answer = nullptr)
{
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(hwnd, msg, wp, lp, SMTO_ABORTIFHUNG, kAskMs, &result) == 0) return false;
    if (answer != nullptr) *answer = result;
    return true;
}

// ⓘ LoadImage at the exact size, as the tray does and for the same reason: it
// answers with the frame drawn for that size when the icon carries one.
// Resource 1 is the exe's own icon (app\ctm-usbip.rc).
inline HICON load(int px)
{
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                         IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
}

// One pass. ⚠️ Called with g_mutex held.
inline void keep()
{
    const HWND hwnd = find_app_window();
    if (hwnd == nullptr) {
        g_wearing = nullptr;
        return;
    }

    // ⭐ THE WINDOW'S OWN SCALE, asked of the window. It is the display the
    // window is ON that decides the pixels, and dragging it to another one
    // changes the answer.
    const unsigned dpi = GetDpiForWindow(hwnd);
    const int bigPx = window_icon_rule::big_px(dpi);
    const int smallPx = window_icon_rule::small_px(dpi);

    HICON staleBig = nullptr, staleSmall = nullptr;
    const bool resized = (bigPx != g_bigPx || smallPx != g_smallPx);
    if (resized || g_big == nullptr || g_small == nullptr) {
        // ⚠️ Not `small`: the Windows headers define that word as a type.
        const HICON newBig = load(bigPx);
        const HICON newSmall = load(smallPx);
        if (newBig == nullptr || newSmall == nullptr) {
            // ⓘ A build with no icon resource. The browser's own stays, which
            // is what the window had before any of this existed.
            if (newBig != nullptr) DestroyIcon(newBig);
            if (newSmall != nullptr) DestroyIcon(newSmall);
            if (!g_saidNoIcon) {
                g_saidNoIcon = true;
                device_log::session_w() << L"settings window: this build carries no icon, "
                                           L"so the browser's own is left on it";
            }
            return;
        }
        // ⛔ The old pair is destroyed only AFTER the window has the new one,
        // or it would hold a dead handle in between.
        staleBig = g_big;
        staleSmall = g_small;
        g_big = newBig;
        g_small = newSmall;
        g_bigPx = bigPx;
        g_smallPx = smallPx;
    }

    DWORD_PTR hasBig = 0, hasSmall = 0;
    const bool answered = ask(hwnd, WM_GETICON, ICON_BIG, 0, &hasBig) &&
                          ask(hwnd, WM_GETICON, ICON_SMALL, 0, &hasSmall);
    const bool ours = reinterpret_cast<HICON>(hasBig) == g_big &&
                      reinterpret_cast<HICON>(hasSmall) == g_small;
    if (answered && !ours) {
        // ⛔ BIG FIRST, SMALL SECOND. The taskbar draws the big one and did not
        // look again until the small one was set too, so the other order
        // risks a redraw that still shows the old big one.
        const bool set = ask(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_big)) &&
                         ask(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_small));
        if (set && (hwnd != g_wearing || resized)) {
            g_wearing = hwnd;
            g_putBack = 0;
            device_log::session_w() << L"settings window: icon set, " << bigPx << L" and "
                                    << smallPx << L" px for a display at "
                                    << ((dpi != 0 ? dpi : window_icon_rule::kDpiAt100) * 100 /
                                        window_icon_rule::kDpiAt100)
                                    << L"%";
        } else if (set) {
            // ⓘ The first few and then every hundredth: enough to see that the
            // browser does this and how often, without a line four times a
            // second if one ever does it on every repaint.
            ++g_putBack;
            if (g_putBack <= 3 || g_putBack % 100 == 0) {
                device_log::session_w() << L"settings window: icon put back, the browser had "
                                           L"replaced it (" << g_putBack << L" on this window)";
            }
        }
    }

    if (staleBig != nullptr) DestroyIcon(staleBig);
    if (staleSmall != nullptr) DestroyIcon(staleSmall);
}

inline void start()
{
    if (g_running.exchange(true)) return;
    std::thread([]() {
        while (g_running.load()) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_running.load()) keep();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kEveryMs));
        }
    }).detach();
}

// ⭐ TAKES OUR ICONS BACK OFF THE WINDOW before the handles die with us.
//
// The settings window can outlive the listener -- it is the browser's -- and a
// window left holding a dead handle shows whatever Windows makes of one. With
// none of its own it falls back to the browser's icon, which is the honest
// picture for a page whose listener has gone.
// ⓘ Only where they are still ours: if the browser has put its own back in the
// last quarter second, that one is left alone.
inline void stop()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_running.exchange(false)) return;

    const HWND hwnd = find_app_window();
    if (hwnd != nullptr) {
        DWORD_PTR has = 0;
        if (g_big != nullptr && ask(hwnd, WM_GETICON, ICON_BIG, 0, &has) &&
            reinterpret_cast<HICON>(has) == g_big) {
            ask(hwnd, WM_SETICON, ICON_BIG, 0);
        }
        if (g_small != nullptr && ask(hwnd, WM_GETICON, ICON_SMALL, 0, &has) &&
            reinterpret_cast<HICON>(has) == g_small) {
            ask(hwnd, WM_SETICON, ICON_SMALL, 0);
        }
    }
    if (g_big != nullptr) DestroyIcon(g_big);
    if (g_small != nullptr) DestroyIcon(g_small);
    g_big = nullptr;
    g_small = nullptr;
    g_bigPx = 0;
    g_smallPx = 0;
    g_wearing = nullptr;
}

} // namespace window_icon
