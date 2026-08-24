// Opening the settings page.
//
// WHY THE EXE DOES THIS. The launcher script used to open the page every time
// it started, and the listener stops on every build -- so a few rebuilds left a
// pile of identical browser windows lying around. Doing it here means the check
// for "one is already open" happens in the same place as the launch, which a
// batch file cannot do reliably.
//
// ⭐ REUSE IS BY WINDOW TITLE. Chrome offers no way to say "focus my app window
// if it exists"; --user-data-dir shares a profile but still opens a second
// window. So this walks the top-level windows looking for the page's own title
// and brings that one forward instead.
//
// ⚠️ That makes the page's <title> load-bearing. If it changes, this stops
// matching and the old behaviour returns -- a new window each time. The marker
// below is deliberately a fragment rather than the whole title so a version
// suffix or a trailing app name does not break it.

#pragma once

// ShellExecuteW lives here, not in windows.h. main.cpp does not include it
// because nothing else in the project launches another program.
#include <shellapi.h>

namespace ctm_open_ui {

inline bool g_open_ui = false;              // set by --ui
inline bool g_ui_already_focused = false;   // main focused one before starting

// A distinctive fragment of the page's <title>. See the warning above.
inline const wchar_t *kTitleMarker = L"Controller config";

inline const wchar_t *kRelativePage = L"tools\\controller-config-test-client.html";

struct FindState {
    HWND found = nullptr;
};

inline BOOL CALLBACK find_window_proc(HWND hwnd, LPARAM param)
{
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[512] = {};
    if (GetWindowTextW(hwnd, title, 511) <= 0) return TRUE;
    if (wcsstr(title, kTitleMarker) == nullptr) return TRUE;
    reinterpret_cast<FindState *>(param)->found = hwnd;
    return FALSE;                                   // stop at the first match
}

// Brings an already-open page forward. Returns false when there is none.
//
// ⓘ The Alt-key dance is not superstition: Windows refuses SetForegroundWindow
// from a process that does not own the foreground, and faking a keypress is the
// documented way around it. Without it the window is raised but stays behind.
inline bool focus_existing()
{
    FindState state;
    EnumWindows(find_window_proc, reinterpret_cast<LPARAM>(&state));
    if (state.found == nullptr) return false;

    if (IsIconic(state.found)) {
        ShowWindow(state.found, SW_RESTORE);
    }
    INPUT alt = {};
    alt.type = INPUT_KEYBOARD;
    alt.ki.wVk = VK_MENU;
    SendInput(1, &alt, sizeof(alt));
    SetForegroundWindow(state.found);
    alt.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &alt, sizeof(alt));
    return true;
}

// file:/// URL for the page, from the working directory.
//
// ⚠️ The agent's working directory is the REPO ROOT, not the exe's folder --
// which is also where it reads its config and writes its log. Resolving from
// the exe path instead would look in out/x64/Debug and find nothing.
inline std::wstring page_url(std::wstring *diskPath)
{
    wchar_t cwd[MAX_PATH] = {};
    if (GetCurrentDirectoryW(MAX_PATH, cwd) == 0) return std::wstring();
    std::wstring path = std::wstring(cwd) + L"\\" + kRelativePage;
    *diskPath = path;

    std::wstring url = L"file:///";
    for (wchar_t c : path) {
        if (c == L'\\') url += L'/';
        else if (c == L' ') url += L"%20";
        else url += c;
    }
    return url;
}

inline std::wstring find_browser()
{
    const wchar_t *candidates[] = {
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
        L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
    };
    for (const wchar_t *c : candidates) {
        if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) return c;
    }
    return std::wstring();
}

// Opens the settings page, or focuses the one already open.
//
// !! Never fatal. A missing page or browser is logged and the agent carries on
// !! -- the listener is the point, and the page is a convenience on top of it.
// Brings an existing page forward. Returns true when it did.
//
// ⭐ Called EARLY, before any socket is bound: the commonest failure is another
// listener already holding the port, and in that case surfacing the page you
// already have is exactly right.
inline bool focus_only()
{
    if (!focus_existing()) return false;
    std::wcout << L"settings page already open, brought to the front\n";
    return true;
}

// Opens a new page.
//
// ⛔ Called LATE, only once the agent is actually up. Opening one on a failed
// start would leave a browser window reporting an unreachable agent -- a
// confusing thing to hand someone whose real problem is that the exe did not
// start.
inline void open_new()
{
    std::wstring diskPath;
    const std::wstring url = page_url(&diskPath);
    if (url.empty() || GetFileAttributesW(diskPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"settings page not found at " << diskPath
                   << L" -- skipping (the listener is unaffected)\n";
        return;
    }

    const std::wstring browser = find_browser();
    if (browser.empty()) {
        // No Chrome or Edge: hand it to whatever is registered. Loses the app
        // window, which is cosmetic -- and losing the page entirely is not.
        std::wcout << L"no Chrome or Edge found, opening in the default browser\n";
        ShellExecuteW(nullptr, L"open", diskPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    // --app strips the address bar, tabs and bookmarks: this is a control
    // surface, not a page being browsed, and on a TV that furniture is a row of
    // things to hit by accident.
    const std::wstring args = L"--app=" + url + L" --window-size=1500,950";
    ShellExecuteW(nullptr, L"open", browser.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
    std::wcout << L"settings page opened\n";
}

// Is a listener already running on the control port?
//
// ⭐ Asked BEFORE starting anything, so one exe with one set of flags covers
// every combination of "listener up or not" and "page open or not". Without
// this the second launch dies on a bind error and never reaches the page --
// the case where you most wanted it.
//
// ⛔ CONNECTS rather than binds. A bind probe was tried first and reported the
// port FREE while the agent held it: the agent binds INADDR_ANY, the probe
// bound loopback, and Windows permits that combination. Connecting asks the
// question directly -- is something accepting on this port -- and has no such
// ambiguity.
//
// ⚠️ Loopback only. A listener on another machine is not this one, and probing
// beyond the local host would be both wrong and slow.
inline bool agent_already_running(uint16_t port)
{
    WSADATA data = {};
    const bool startedWsa = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe == INVALID_SOCKET) {
        if (startedWsa) WSACleanup();
        return false;
    }

    // Non-blocking, so a firewall that black-holes the connection cannot stall
    // startup: nothing answering within the timeout counts as nothing there.
    u_long nonBlocking = 1;
    ioctlsocket(probe, FIONBIO, &nonBlocking);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    bool connected = connect(probe, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
    if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(probe, &writable);
        timeval timeout = {};
        timeout.tv_usec = 300000;                  // 300 ms is generous on loopback
        connected = select(0, nullptr, &writable, nullptr, &timeout) > 0;
    }

    closesocket(probe);
    if (startedWsa) WSACleanup();
    return connected;
}

} // namespace ctm_open_ui
