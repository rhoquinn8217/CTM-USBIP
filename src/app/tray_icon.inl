// The notification area icon.
//
// ⭐ WHY IT EXISTS. The on-screen keyboard is opened by a controller binding,
// so a person with only a POINTER -- an LG Magic Remote is the case that
// prompted this -- could type on it perfectly well but had no way to summon it
// at all. rhoquinn8217, 2026-09-02.
//
// ⓘ A tray icon is also how Windows' own touch keyboard is reached, so it is
// the place someone will already look.
//
// ⛔ ITS OWN HIDDEN WINDOW, ON ITS OWN THREAD. A tray icon needs a window to
// send its clicks to, and that window has to outlive the keyboard -- which
// comes and goes. Sharing the overlay's window would mean the icon stopped
// working the moment the keyboard was closed, which is exactly when it is
// needed.

#pragma once

#pragma comment(lib, "shell32.lib")

namespace ctm_tray {

inline HWND g_hwnd = nullptr;
inline std::thread g_thread;
inline std::atomic_bool g_running{false};

inline const wchar_t *const kClassName = L"CtmTrayIcon";
inline const UINT WM_CTM_TRAY = WM_APP + 20;

// ⓘ Menu ids. Kept small and local; nothing else uses this window.
inline const UINT kIdToggle   = 1;
inline const UINT kIdSettings = 2;
inline const UINT kIdExit     = 3;

inline void toggle_keyboard()
{
    // ⓘ Opened with no button, so the "the button that opened it also closes
    // it" rule has nothing to arm -- the close key, Circle and this icon are
    // the ways back out.
    if (ctm_overlay::visible()) ctm_overlay::hide();
    else                        ctm_overlay::show();
}

inline void show_menu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;

    // ⭐ Settings FIRST (T-163). With Circle closing that window rather than
    // hiding it, this menu is the way back to it, and it is what a click is
    // most often for now that a click opens a menu at all.
    AppendMenuW(menu, MF_STRING, kIdSettings, L"Open settings");
    AppendMenuW(menu, MF_STRING, kIdToggle,
                ctm_overlay::visible() ? L"Hide keyboard" : L"Show keyboard");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdExit, L"Exit");

    POINT pt;
    GetCursorPos(&pt);

    // ⛔ THE FOREGROUND DANCE. A popup menu will not close when you click away
    // unless its owner is the foreground window, and it will not become the
    // foreground window on its own from a tray click. The posted null message
    // afterwards is what lets the menu tidy itself up. This is the documented
    // workaround, not a hack of ours.
    SetForegroundWindow(hwnd);
    const int chosen = (int)TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (chosen) {
    case kIdToggle:
        toggle_keyboard();
        break;
    case kIdSettings:
        // ⓘ The same path the chord takes, with no controller in hand.
        ctm_chord_show_ui(std::string());
        break;
    case kIdExit:
        // ⛔ THE SAME FLAG CTRL+C SETS, not an exit. Bridged controllers get
        // torn down properly; killing the process would leave them attached
        // with nothing driving them.
        g_stop.store(true);
        break;
    default:
        break;
    }
}

inline LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CTM_TRAY) {
        // ⭐⭐ A LEFT CLICK OPENS THE MENU (T-163). It used to open the keyboard
        // outright, which was the icon's whole meaning while the keyboard was
        // all it offered.
        //
        // ⛔ That stopped being true at T-150: Circle now CLOSES the config
        // window rather than hiding it, so this icon is one of only two ways
        // back to that window, and the only one that needs no controller. A
        // click that assumes the keyboard hides the thing most people are
        // reaching for.
        if (LOWORD(lp) == WM_LBUTTONUP) {
            show_menu(hwnd);
            return 0;
        }
        if (LOWORD(lp) == WM_RBUTTONUP) {
            show_menu(hwnd);
            return 0;
        }
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

inline void thread_main()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // ⓘ Never shown. It exists only to receive the icon's messages and to own
    // the popup menu.
    g_hwnd = CreateWindowExW(0, kClassName, L"CTM tray", WS_OVERLAPPED,
                             0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    if (g_hwnd == nullptr) {
        device_log::session_w() << L"tray: could not create its window";
        g_running.store(false);
        return;
    }

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_CTM_TRAY;
    // ⭐⭐ THE ICON IS A CONTROLLER, BORROWED FROM joy.cpl (T-163,
    // rhoquinn8217: the listener is a controller tool, and the keyboard is one
    // of the things it offers).
    //
    // ⛔ There is no stock icon to ask for by name: shell32's indices are
    // undocumented and shift between releases, and Windows 11 moved the shell
    // icons into imageres.dll.mun entirely. Picking an index would be a guess
    // that silently becomes the wrong picture on some machine.
    //
    // ⭐ joy.cpl is Windows' OWN Game Controllers panel. It is present on every
    // Windows, it holds exactly one icon, and that icon is a gamepad -- checked
    // by extracting it and looking at it, 2026-09-17, rather than assumed.
    // ⓘ osk.exe stays as the fallback: it was the icon until today, its icon is
    // a keyboard, and it is better than a blank application square.
    // ⓘ Full paths rather than bare names, so nothing earlier on the PATH can
    // answer instead.
    auto extract_system_icon = [&wc](const wchar_t *leaf) -> HICON {
        wchar_t path[MAX_PATH] = {};
        const UINT len = GetSystemDirectoryW(path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH - 16) return nullptr;
        wcscat_s(path, L"\\");
        wcscat_s(path, leaf);
        HICON icon = ExtractIconW(wc.hInstance, path, 0);
        // ⓘ ExtractIcon answers 1 for "not an icon source" as well as null for
        // none, so both count as failure.
        return (icon != nullptr && icon != (HICON)1) ? icon : nullptr;
    };

    const wchar_t *iconSource = L"joy.cpl (a controller)";
    nid.hIcon = extract_system_icon(L"joy.cpl");
    if (nid.hIcon == nullptr) {
        nid.hIcon = extract_system_icon(L"osk.exe");
        iconSource = L"osk.exe (a keyboard, the fallback)";
    }
    bool extracted = (nid.hIcon != nullptr);
    if (!extracted) {
        nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        iconSource = L"the stock application icon";
    }
    // ⓘ The tip names what a click DOES now that a click opens the menu.
    wcscpy_s(nid.szTip, L"DS5-USBIP — click for settings or the keyboard");
    Shell_NotifyIconW(NIM_ADD, &nid);
    device_log::session_w() << L"tray: icon added, from " << iconSource;

    MSG m;
    while (g_running.load() && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    Shell_NotifyIconW(NIM_DELETE, &nid);
    // ⓘ ExtractIcon hands back a handle we own. LoadIcon's shared one must NOT
    // be destroyed, so only the extracted one is.
    if (extracted && nid.hIcon != nullptr) DestroyIcon(nid.hIcon);
    if (g_hwnd != nullptr) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    UnregisterClassW(kClassName, wc.hInstance);
    g_running.store(false);
    device_log::session_w() << L"tray: icon removed";
}

inline void start()
{
    if (g_running.exchange(true)) return;
    g_thread = std::thread(thread_main);
    g_thread.detach();
}

inline void stop()
{
    if (!g_running.exchange(false)) return;
    // ⓘ Posted, because the window belongs to the thread that made it.
    if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

} // namespace ctm_tray
