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

// ⭐ An http:// URL now, not a file path.
//
// The agent serves the page, so there is nothing on disk to point at -- which
// is the whole reason a released exe works with no files beside it.
inline std::wstring page_url(uint16_t restPort)
{
    return L"http://127.0.0.1:" + std::to_wstring(restPort) + L"/";
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

// ⭐ Closes the settings page if one is open. Returns true when it did.
//
// Same lookup as focus_existing -- by window title -- with a close message
// instead of a focus one.
//
// ⓘ WM_CLOSE rather than terminating anything: the browser owns the window, and
// asking it to close lets it tear the page down properly, which is what fires
// the page's own "I am going away" beacon.
//
// ⚠️ Best effort by design. A window that will not close is untidy; a gate that
// will not release is what strands someone. So callers release the gate FIRST
// and treat this as cleanup.
// Is a settings window up right now? Same lookup, no side effects.
inline bool window_exists()
{
    FindState state;
    EnumWindows(find_window_proc, reinterpret_cast<LPARAM>(&state));
    return state.found != nullptr;
}

inline bool close_existing()
{
    FindState state;
    EnumWindows(find_window_proc, reinterpret_cast<LPARAM>(&state));
    if (state.found == nullptr) return false;
    PostMessageW(state.found, WM_CLOSE, 0, 0);
    return true;
}

// ⭐ Show the settings window and take the controllers. One implementation for
// the REST endpoint and the chord, so they cannot drift apart.
//
// ⛔ Kill and recreate rather than focusing an existing window: every call then
// lands in a known state, with nothing carried over from one left mid-edit.
//
// ⚠️ Declared here and defined in main.cpp, because this needs the gate --
// which lives in rebind.inl, included long after this file.
void ctm_show_settings_window();

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
    device_log::session_w() << L"settings page already open, brought to the front";
    return true;
}

// Opens a new page.
//
// ⛔ Called LATE, only once the agent is actually up. Opening one on a failed
// start would leave a browser window reporting an unreachable agent -- a
// confusing thing to hand someone whose real problem is that the exe did not
// start.
inline void open_new(uint16_t restPort)
{
    const std::wstring url = page_url(restPort);

    const std::wstring browser = find_browser();
    if (browser.empty()) {
        // No Chrome or Edge: hand it to whatever is registered. Loses the app
        // window, which is cosmetic -- and losing the page entirely is not.
        device_log::session_w() << L"no Chrome or Edge found, opening in the default browser";
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    // --app strips the address bar, tabs and bookmarks: this is a control
    // surface, not a page being browsed, and on a TV that furniture is a row of
    // things to hit by accident.
    const std::wstring args = L"--app=" + url + L" --window-size=1150,820";
    ShellExecuteW(nullptr, L"open", browser.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
    device_log::session_w() << L"settings page opened";
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
