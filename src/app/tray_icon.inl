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
#pragma comment(lib, "shcore.lib")     // the scale of the display the menu opens on

#include <shellscalingapi.h>

namespace ctm_tray {

inline HWND g_hwnd = nullptr;
inline std::thread g_thread;
inline std::atomic_bool g_running{false};

inline const wchar_t *const kClassName = L"CtmTrayIcon";
inline const UINT WM_CTM_TRAY = WM_APP + 20;

// ⓘ Menu ids. Kept small and local; nothing else uses this window.
inline const UINT kIdToggle   = 1;
inline const UINT kIdSettings = 2;
inline const UINT kIdQuit     = 3;
// ⓘ The title's line. Choosing it closes the menu and does nothing else.
inline const UINT kIdHeader   = 10;
// ⓘ One id for each line of the Controllers list, in the order it was read as
// the menu opened, and one for each layout. More devices than this are not
// listed; nobody has bridged a tenth as many.
inline const UINT   kIdDeviceFirst = 100;
inline const size_t kMaxListed     = 64;
inline const UINT   kIdModeFirst   = 200;

// ⭐⭐ THE ICON GOES NOW (rhoquinn8217, 2026-10-01: *"I want to run it in the
// background and the tray icon to be the place where it is closed."*).
//
// ⛔ NOTHING REMOVED IT ON THE WAY OUT. The icon was deleted only by this
// thread's own teardown, which the agent never reaches: it sets the stop flag
// and the process ends. Windows then leaves the picture in the tray until the
// pointer next passes over it, and with no console window any more that
// picture is the ONLY sign the program is running. A sign that outlives the
// program is a lie someone clicks on.
//
// ⓘ From ANY thread: the icon is named by its window and its id, and the
// shell takes the request from whoever sends it. Asking twice is harmless,
// the second answer is simply "there is none".
inline void remove_icon()
{
    const HWND hwnd = g_hwnd;
    if (hwnd == nullptr) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;                 // ⚠️ the id thread_main adds it under
    if (Shell_NotifyIconW(NIM_DELETE, &nid)) {
        device_log::session_w() << L"tray: icon removed, the listener is stopping";
    }
}

inline void toggle_keyboard()
{
    // ⓘ Opened with no button, so the "the button that opened it also closes
    // it" rule has nothing to arm -- the close key, Circle and this icon are
    // the ways back out.
    if (ctm_overlay::visible()) ctm_overlay::hide();
    else                        ctm_overlay::show();
}

// ⓘ A nickname and the name the TV sent are UTF-8; a menu takes UTF-16.
inline std::wstring widen(const std::string &utf8)
{
    if (utf8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], n);
    return out;
}

// The scale of the display the menu is about to open on. ⓘ The tray's own
// window is never shown and sits on the first display, so asking IT would
// size the title for the wrong screen whenever the taskbar is on another.
inline UINT dpi_at(POINT pt)
{
    const HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) return 96;
    return dx;
}

// ⭐⭐ THE TITLE AND THE COUNT ARE A PICTURE, AND THAT IS NOT A SHORTCUT.
//
// A Windows menu draws every line in one font, so a title in a larger one has
// to be drawn by us (rhoquinn8217, 2026-10-02: *"Add a Title "DS5-USBIP" at
// the top in larger font. Directly underneath, regular font with "<x>
// controllers connected""*).
//
// ⛔ THE DOCUMENTED WAY COSTS THE WHOLE MENU ITS LOOKS. One owner-drawn line
// and Windows draws ALL of the menu in the old flat grey style with the blue
// bar, while its side menus stay as they are now. Seen side by side in a
// throwaway program before any of this was written.
// ⛔ AND hbmpItem PUTS THE PICTURE IN THE ICON COLUMN, so every other line's
// text then begins to the right of it.
// ➡️ A picture given as the line's own content (an old-style bitmap item)
// keeps the menu as Windows draws it today, and sits where a line's text sits.
//
// ⓘ 32 bits with its own transparency and the text in the menu's text colour,
// so it lies on whatever the menu's background turns out to be, the highlight
// under the pointer included.
//
// ⭐ BLACK AND BOLD, ON A LINE THAT CAN BE CLICKED (rhoquinn8217, on seeing the
// first one: *"Make the title clickable and when it is clicked, close it. The
// Text on the title also looks dark gray and a low res enough that it's
// noticible. Make it black bolded."*). Both complaints had one cause and one
// more beside it:
// - ⛔ a line that CANNOT be chosen is drawn by Windows at about two thirds
//   strength, pictures included, so black came out dark grey. A line that can
//   be chosen is drawn as given. Choosing it closes the menu and nothing else.
// - ⛔ grey-scale smoothing made the letters soft and the count heavier than
//   the menu's own lines beside it. Drawn with ClearType and the three
//   channels averaged into one coverage, the count matches the weight of the
//   line under it. Four renderings were photographed side by side first.
inline HBITMAP header_picture(const std::wstring &count, UINT dpi)
{
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        return nullptr;
    }
    // ⓘ ClearType, drawn white on black. Each channel is then how much of
    // one third of the pixel the letter covers, and their mean is how much of
    // the whole pixel: a transparency. The coloured edges are given up, the
    // sharper stems are kept.
    LOGFONTW countFace = metrics.lfMenuFont;
    countFace.lfQuality = CLEARTYPE_QUALITY;
    LOGFONTW titleFace = countFace;
    titleFace.lfHeight = MulDiv(titleFace.lfHeight, 140, 100);
    titleFace.lfWeight = FW_BOLD;
    const HFONT titleFont = CreateFontIndirectW(&titleFace);
    const HFONT countFont = CreateFontIndirectW(&countFace);
    if (titleFont == nullptr || countFont == nullptr) {
        if (titleFont != nullptr) DeleteObject(titleFont);
        if (countFont != nullptr) DeleteObject(countFont);
        return nullptr;
    }

    const HDC screen = GetDC(nullptr);
    const HDC dc = CreateCompatibleDC(screen);
    const std::wstring title = tray_menu::kTitle;
    SIZE titleSize = {}, countSize = {};
    HGDIOBJ oldFont = SelectObject(dc, titleFont);
    GetTextExtentPoint32W(dc, title.c_str(), static_cast<int>(title.size()), &titleSize);
    SelectObject(dc, countFont);
    GetTextExtentPoint32W(dc, count.c_str(), static_cast<int>(count.size()), &countSize);

    // ⛔⛔ NEVER NARROWER THAN THE MENU'S WIDEST LINE, OR IT IS A BLACK BOX
    // (rhoquinn8217, 2026-10-02: *"the tray menu title is now a black box"*).
    // A picture narrower than the menu's lines is drawn by Windows another
    // way, one that ignores the transparency, and with the ink black and the
    // rest clear that is solid black. "10 controllers connected" happened to
    // be wide enough; "4 devices connected" is not. Measured in the skill's
    // tools/menu-proto.cpp, this picture as it was against the same picture
    // this wide: black in every state, then clean in every state, the
    // pointer's own highlight showing through.
    // ⓘ The lines are the main menu's, both of the keyboard's wordings among
    // them, measured in the menu's font, which is the count's.
    LONG widest = titleSize.cx > countSize.cx ? titleSize.cx : countSize.cx;
    const std::wstring lines[] = {
        tray_menu::kDevices, tray_menu::kOpenConfig, tray_menu::keyboard_line(false),
        tray_menu::keyboard_line(true), tray_menu::kConfigMode, tray_menu::kQuit };
    for (const std::wstring &line : lines) {
        SIZE lineSize = {};
        GetTextExtentPoint32W(dc, line.c_str(), static_cast<int>(line.size()), &lineSize);
        if (lineSize.cx > widest) widest = lineSize.cx;
    }

    // ⓘ Nothing on the left: the menu already indents a line's content, and
    // the title should begin where the lines under it begin.
    const int above = MulDiv(2, dpi, 96);
    const int between = MulDiv(2, dpi, 96);
    const int below = MulDiv(4, dpi, 96);
    const int width = static_cast<int>(widest) + MulDiv(8, dpi, 96);
    const int height = above + titleSize.cy + between + countSize.cy + below;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;          // top row first
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    const HBITMAP picture = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (picture != nullptr && bits != nullptr) {
        const HGDIOBJ oldPicture = SelectObject(dc, picture);
        RECT all = { 0, 0, width, height };
        FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        SelectObject(dc, titleFont);
        TextOutW(dc, 0, above, title.c_str(), static_cast<int>(title.size()));
        SelectObject(dc, countFont);
        TextOutW(dc, 0, above + titleSize.cy + between, count.c_str(), static_cast<int>(count.size()));
        GdiFlush();

        // White on black is the coverage. Turn it into the menu's text colour
        // at that much opacity, with the colour already multiplied through,
        // which is the form Windows blends.
        const COLORREF ink = GetSysColor(COLOR_MENUTEXT);
        auto *px = static_cast<unsigned char *>(bits);
        for (int i = 0; i < width * height; ++i) {
            const unsigned cover = (px[i * 4 + 0] + px[i * 4 + 1] + px[i * 4 + 2]) / 3u;
            px[i * 4 + 0] = static_cast<unsigned char>(GetBValue(ink) * cover / 255);
            px[i * 4 + 1] = static_cast<unsigned char>(GetGValue(ink) * cover / 255);
            px[i * 4 + 2] = static_cast<unsigned char>(GetRValue(ink) * cover / 255);
            px[i * 4 + 3] = static_cast<unsigned char>(cover);
        }
        SelectObject(dc, oldPicture);
    }
    SelectObject(dc, oldFont);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    DeleteObject(titleFont);
    DeleteObject(countFont);
    return picture;
}

// ⭐ THE CONFIG WINDOW'S LAYOUT, CHANGED FROM THE MENU.
//
// The listener holds which layout that window is in, and a window asks for it
// as it opens. So a change made here is: write it down, and if a window is
// open, replace it with one that will ask.
//
// ⛔⛔ THE OLD WINDOW GOES FIRST, AND IS GONE BEFORE THE LAYOUT IS WRITTEN.
// On its way out a window has its place and size read, and they are filed
// under whichever layout is current at that moment. Written the other way
// round, Advanced's size would be filed as a size someone had dragged Quick
// to, and Quick would then open as large as Advanced.
//
// ⓘ With no window open, none is opened. The padlock marks the layout the
// window opens in, and that is what has been changed.
inline void change_layout(int index)
{
    if (index < 0 || index >= tray_menu::kModeCount) return;
    bool compact = false, quick = false;
    std::string ordinal;
    const bool known = ui_view_get(&compact, &quick, &ordinal);
    if (tray_menu::mode_index(known, compact, quick) == index) return;   // it is in that one

    const bool wasOpen = ui_close_window();
    if (wasOpen) {
        int waited = 0;
        for (; waited < 80; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            if (!ctm_open_ui::window_exists()) break;
        }
        device_log::input_w() << L"tray: waited " << (waited * 25)
                              << L"ms for the config window to go before changing its layout";
    }
    const tray_menu::Mode &mode = tray_menu::kModes[index];
    ui_view_set(mode.compact, mode.quick, false);
    device_log::session_w() << L"tray: the config window's layout is " << mode.name << L" now"
                            << (wasOpen ? L", and the window is being opened again in it"
                                        : L"; no window was open, so the next one opens in it");
    if (wasOpen) ctm_chord_show_ui(ordinal);
}

// The part a device's line speaks for: the first that takes a config, else the
// first -- the part the settings page leads that device's tab with, so the
// line names what the tab names.
inline size_t lead_part(const std::vector<RestDeviceView> &devices,
                        const std::vector<size_t> &parts)
{
    for (size_t i : parts) {
        if (config_store::kind_supports_config(devices[i].kind)) return i;
    }
    return parts.front();
}

// The part whose battery a device's line shows: the lead's when it reports
// one, else the first part that does. A receiver's parts report none.
inline size_t battery_part(const std::vector<RestDeviceView> &devices,
                           const std::vector<size_t> &parts)
{
    const size_t lead = lead_part(devices, parts);
    if (devices[lead].batteryPercent >= 0) return lead;
    for (size_t i : parts) {
        if (devices[i].batteryPercent >= 0) return i;
    }
    return lead;
}

inline void show_menu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;

    POINT pt;
    GetCursorPos(&pt);

    // ⭐ READ AS THE MENU OPENS. The menu is built afresh on every click, so
    // the count and the list are what is true at this moment and need no
    // keeping up to date.
    const std::vector<RestDeviceView> devices = rest_collect_devices();
    // ⭐ ONE LINE PER DEVICE (rhoquinn8217, 2026-10-02): the parts under one
    // nickname are one device here, as on the settings page's tabs.
    std::vector<std::string> names;
    for (const RestDeviceView &d : devices) names.push_back(d.nickname);
    const std::vector<std::vector<size_t>> groups = tray_menu::group_by_name(names);
    const size_t listed = groups.size() < kMaxListed ? groups.size() : kMaxListed;

    // ---- The title, and how many are connected ------------------------------
    // ⓘ The count is of the lines the list below will show, so the two cannot
    // disagree.
    const std::wstring count = tray_menu::count_line(listed);
    const HBITMAP header = header_picture(count, dpi_at(pt));
    // ⓘ Not switched off: see header_picture() for what that did to it.
    if (header != nullptr) {
        AppendMenuW(menu, MF_BITMAP, kIdHeader, reinterpret_cast<LPCWSTR>(header));
    } else {
        // ⓘ No picture to be had: the same two lines in the menu's own font.
        // Smaller than asked for, and still there.
        AppendMenuW(menu, MF_STRING, kIdHeader, tray_menu::kTitle);
        AppendMenuW(menu, MF_STRING, kIdHeader, count.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // ---- Devices: a line each, and choosing one opens the window on it ------
    // ⓘ Every bridged device is here, a keyboard or a mouse included: it is
    // the list the settings page's tabs show, one line per device.
    const HMENU list = CreatePopupMenu();
    for (size_t i = 0; i < listed; ++i) {
        const RestDeviceView &d = devices[lead_part(devices, groups[i])];
        const RestDeviceView &b = devices[battery_part(devices, groups[i])];
        const std::string line = tray_menu::device_line(
            d.nickname, device_names::label(d.kind, d.product, d.deviceType),
            b.batteryPercent, b.batteryState);
        AppendMenuW(list, MF_STRING, kIdDeviceFirst + static_cast<UINT>(i), widen(line).c_str());
    }
    // ⓘ Greyed with nothing bridged, and then it opens no side menu.
    AppendMenuW(menu, MF_POPUP | (listed == 0 ? MF_GRAYED : 0u),
                reinterpret_cast<UINT_PTR>(list), tray_menu::kDevices);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // ---- The config window and the keyboard ---------------------------------
    // ⭐ With Circle closing that window rather than hiding it, this menu is the
    // way back to it.
    AppendMenuW(menu, MF_STRING, kIdSettings, tray_menu::kOpenConfig);
    AppendMenuW(menu, MF_STRING, kIdToggle, tray_menu::keyboard_line(ctm_overlay::visible()));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // ---- Config Mode: the window's layout, the padlock on the one it is in ---
    bool compact = false, quick = false;
    const bool known = ui_view_get(&compact, &quick, nullptr);
    const int current = tray_menu::mode_index(known, compact, quick);
    const HMENU modes = CreatePopupMenu();
    for (int i = 0; i < tray_menu::kModeCount; ++i) {
        AppendMenuW(modes, MF_STRING, kIdModeFirst + static_cast<UINT>(i),
                    tray_menu::mode_line(i, current).c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(modes), tray_menu::kConfigMode);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // ⓘ "Quit", not "Exit" (rhoquinn8217, 2026-10-01). It ends the whole
    // program, settings window included, and that is the word for it.
    AppendMenuW(menu, MF_STRING, kIdQuit, tray_menu::kQuit);

    // ⛔ THE FOREGROUND DANCE. A popup menu will not close when you click away
    // unless its owner is the foreground window, and it will not become the
    // foreground window on its own from a tray click. The posted null message
    // afterwards is what lets the menu tidy itself up. This is the documented
    // workaround, not a hack of ours.
    SetForegroundWindow(hwnd);
    const UINT chosen = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd, nullptr));
    PostMessageW(hwnd, WM_NULL, 0, 0);
    // ⓘ Takes the two side menus with it. The picture is ours to let go of.
    DestroyMenu(menu);
    if (header != nullptr) DeleteObject(header);

    if (chosen >= kIdDeviceFirst && chosen < kIdDeviceFirst + listed) {
        // ⭐ The same call the pad's own chord makes: the window opens on that
        // device's tab. ⓘ The page finds a device's tab from any of its parts.
        const RestDeviceView &d = devices[lead_part(devices, groups[chosen - kIdDeviceFirst])];
        device_log::session_w() << L"tray: opening the config window on " << widen(d.ordinal);
        ctm_chord_show_ui(d.ordinal);
        return;
    }
    if (chosen >= kIdModeFirst && chosen < kIdModeFirst + static_cast<UINT>(tray_menu::kModeCount)) {
        change_layout(static_cast<int>(chosen - kIdModeFirst));
        return;
    }

    switch (chosen) {
    case kIdHeader:
        // ⭐ The title: the menu has closed, and that is all it does.
        break;
    case kIdToggle:
        toggle_keyboard();
        break;
    case kIdSettings:
        // ⓘ The same path the chord takes, with no controller in hand.
        ctm_chord_show_ui(std::string());
        break;
    case kIdQuit:
        // ⭐⭐ QUIT TAKES THE SETTINGS WINDOW WITH IT (rhoquinn8217, 2026-10-01:
        // *"I want to change exit to Quit and I want that also to close the
        // DS5-USBIP config window."*). It read Exit and left that window
        // open, showing a page with no listener behind it.
        //
        // ⓘ Closed HERE as well as where the listener stops (agent.inl),
        // because here is where someone is looking: the window goes the
        // moment they choose Quit, not a second later when the agent's loop
        // next comes round.
        ui_close_window();
        // ⛔ THE SAME FLAG CTRL+C SETS, not an exit. Bridged controllers get
        // torn down properly; killing the process would leave them attached
        // with nothing driving them.
        g_stop.store(true);
        // ⭐ And the icon with it, at once. See remove_icon().
        remove_icon();
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
    // ⭐ ddores.dll is Windows' DEVICE icon library, and #108 is its gamepad:
    // a white silhouette that stays crisp all the way down to 16px, which is
    // the size the tray actually draws. Every candidate was extracted and looked
    // at side by side at 80, 48, 32, 24 and 16 before this was chosen
    // (rhoquinn8217, 2026-09-18) -- joy.cpl's pad, which this replaces, is
    // prettier at 64px and collapses into a blob at 16.
    //
    // ⚠️ IT IS A WHITE SILHOUETTE, so on a LIGHT taskbar its body disappears
    // and only the dark detail is left. Chosen with a dark taskbar in front of
    // us; joy.cpl's coloured pad is the theme-safe one and is the first fallback
    // below, so switching back is one line.
    // ⓘ osk.exe stays as the last resort: it was the icon until 2026-09-17, its
    // icon is a keyboard, and it beats a blank application square.
    // ⛔ Index 108 is a POSITION in the file, which is what ExtractIcon takes.
    // Windows' own Change Icon dialog orders by resource and can disagree, so
    // check a number from there by extracting it, never by trusting it.
    // ⓘ Full paths rather than bare names, so nothing earlier on the PATH can
    // answer instead.
    auto extract_system_icon = [&wc](const wchar_t *leaf, int index) -> HICON {
        wchar_t path[MAX_PATH] = {};
        const UINT len = GetSystemDirectoryW(path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH - 16) return nullptr;
        wcscat_s(path, L"\\");
        wcscat_s(path, leaf);
        HICON icon = ExtractIconW(wc.hInstance, path, index);
        // ⓘ ExtractIcon answers 1 for "not an icon source" as well as null for
        // none, so both count as failure.
        return (icon != nullptr && icon != (HICON)1) ? icon : nullptr;
    };

    // ⭐⭐ THE LISTENER'S OWN ICON FIRST (rhoquinn8217, 2026-10-01: *"update the
    // window icon, the taskbar icon and the exe icon to the black bridge"*).
    //
    // ⓘ Everything above about ddores.dll is still true, and it is now the
    // FALLBACK: a build with no icon resource gets the system gamepad, as before.
    // ⚠️ The warning about 16 px applies to this one too. The .ico carries a
    // frame drawn for that size (installer/make-icon-ds5.ps1 sharpens every
    // frame below 64 px), which is the reason it holds up; a plain scale-down
    // did not.
    //
    // ⓘ LoadImage at the small-icon size, not LoadIcon: LoadIcon answers with
    // the 32 px frame and lets the shell shrink it, which throws that drawn
    // 16 px frame away. Not LR_SHARED, so the handle is ours and is destroyed
    // with the rest below.
    const wchar_t *iconSource = L"the listener's own icon (the black bridge)";
    nid.hIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (nid.hIcon == nullptr) {
        nid.hIcon = extract_system_icon(L"ddores.dll", 108);
        iconSource = L"ddores.dll #108 (a controller, the fallback)";
    }
    if (nid.hIcon == nullptr) {
        nid.hIcon = extract_system_icon(L"joy.cpl", 0);
        iconSource = L"joy.cpl (a controller, the fallback)";
    }
    if (nid.hIcon == nullptr) {
        nid.hIcon = extract_system_icon(L"osk.exe", 0);
        iconSource = L"osk.exe (a keyboard, the last resort)";
    }
    bool extracted = (nid.hIcon != nullptr);
    if (!extracted) {
        nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        iconSource = L"the stock application icon";
    }
    // ⓘ The tip names what a click DOES now that a click opens the menu: the
    // things on it, in the order they come.
    // ⛔ PLAIN ASCII. It had a long dash in it, and hovering over the icon
    // showed three odd characters where the dash was meant to be. This file
    // has no byte-order mark and the build did not say its sources were
    // UTF-8, so the compiler took the dash's three bytes for three
    // characters. Text a person reads does not need a dash that depends on
    // how the file happened to be saved.
    wcscpy_s(nid.szTip, L"DS5-USBIP: click for controllers, config, the keyboard or Quit");
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
