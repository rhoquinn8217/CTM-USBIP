// The on-screen keyboard, on a button.
//
// ⭐ MEASURED BEFORE BUILT (rhoquinn8217, 2026-08-31). Windows has two of its
// own and Steam has a third, and they are not interchangeable:
//
//   steam   steam://open/keyboard and steam://close/keyboard both work, and
//           the keyboard is navigable with the controller because Steam reads
//           the pad directly. ⭐ The default, and the only one of the three
//           with a clean programmatic open AND close.
//   osk     osk.exe, the classic accessibility keyboard. Opens and closes
//           cleanly, but its keys are CLICK TARGETS -- the d-pad cannot walk
//           them, so it needs the cursor. Kept as the fallback for a machine
//           with no Steam.
//   touch   ⛔ NOT OFFERED. TabTip on Windows 11 is only a launcher for the
//           modern input stack; the shell decides whether the keyboard shows.
//           Measured: it would not reopen after being closed once. A keyboard
//           that works once per boot is worse than one that is not listed.
//
// ⚠️ Same button opens and closes, which is the Deck's own convention: Steam+X
// raises the keyboard and the same combination dismisses it.
//
// ⚠️ AND ONE THING WORTH KNOWING: Steam's keyboard also closes on Circle -- but
// only if Circle still reaches Steam. A rebound button is stripped from the
// report before anything downstream sees it, so binding Circle to Escape takes
// that away. This button is then the way to close it.

#pragma once

namespace ctm_osk {

// ⭐ Ours joins the two that existed (rhoquinn8217, 2026-09-01). Steam's needs
// Steam running and cannot be driven by a REBOUND pad; Windows' own ignores a
// bridged DS5 entirely. Ours is drawn by the listener and driven by the pad
// through the same report path everything else uses.
enum class Program { Steam, Osk, Overlay };

// ⛔ parse_program is GONE (2026-09-03). It read the osk_program setting, and
// with the keyboard named by the binding there is nothing left to parse. ⓘ
// Removed rather than left: an unused parser is the kind of thing someone later
// wires back up, reintroducing the coupling on purpose.

enum class Action { Open, Close };

// ⭐ THE DECISION, kept separate from the Win32 calls so it can be tested.
//
// `known` is what the system can actually tell us: 1 open, 0 closed, -1 no way
// to know. `remembered` is what we last did.
//
// ⚠️ The -1 case is Steam's: there is no way to ask whether its keyboard is up,
// so the flag is all we have, and it DRIFTS -- close the keyboard with Circle
// or its own Move button and our next press sends a close that does nothing
// visible. Pressing again then opens it. That is a real limitation and it is
// better to say so than to pretend a query exists.
inline Action next_action(int known, bool remembered)
{
    if (known == 1) return Action::Close;
    if (known == 0) return Action::Open;
    return remembered ? Action::Close : Action::Open;
}

// ---- Windows wiring --------------------------------------------------------

// The classic keyboard's window class, which is how we ask whether it is up and
// how we close it without killing a process.
inline const wchar_t *const kOskClass = L"OSKMainClass";

inline int osk_known_state()
{
    return FindWindowW(kOskClass, nullptr) != nullptr ? 1 : 0;
}

inline void run_shell(const wchar_t *what)
{
    // ⓘ ShellExecute rather than CreateProcess: steam:// is a protocol handler,
    // not an executable, and the shell is what resolves both.
    ShellExecuteW(nullptr, L"open", what, nullptr, nullptr, SW_SHOWNORMAL);
}

inline void do_open(Program program, int openedByButton)
{
    // ⓘ No process to launch: it is a window this same program owns.
    // ⭐ The button is passed through so the overlay can be closed by the same
    // one -- it swallows every press while it is up, so the binding cannot fire
    // a second time to close it.
    if (program == Program::Overlay) {
        // ⛔ 0 MEANS "THE CHOSEN SIZE", not a default to fill in. This passed
        // 900x300 -- a leftover from before the three sizes existed -- so the
        // keyboard opened tiny on a 4K desktop and only reached a sensible size
        // once Create had been pressed once. Found on hardware 2026-09-02.
        ctm_overlay::show(0, 0, openedByButton);
        return;
    }
    if (program == Program::Osk) {
        run_shell(L"osk.exe");
        return;
    }
    run_shell(L"steam://open/keyboard");
}

inline void do_close(Program program)
{
    if (program == Program::Overlay) {
        ctm_overlay::hide();
        return;
    }
    if (program == Program::Osk) {
        HWND w = FindWindowW(kOskClass, nullptr);
        // ⓘ WM_CLOSE, not a kill: it is a normal window and asking it to close
        // is what a person clicking its X would do.
        if (w != nullptr) PostMessageW(w, WM_CLOSE, 0, 0);
        return;
    }
    run_shell(L"steam://close/keyboard");
}

inline std::atomic<bool> g_remembered{false};

// ⭐⭐ THE BINDING NAMES THE KEYBOARD (rhoquinn8217, 2026-09-03).
//
// ⛔ There used to be one OSKeyboard action and an osk_program setting saying
// WHICH keyboard it meant -- a switch in one section changing what a binding in
// another section did. Same hidden coupling the exclusivity settings had: you
// could bind the keyboard, press it, and get something you did not choose,
// with the reason living somewhere you had no cause to look.
//
// ⓘ Three bindings now, and the one you picked is the one that opens.
inline void toggle(const std::string &section, int button, Program program)
{
    (void)section;
    // ⭐ Ours is the only one we can ASK. Steam's window cannot be found
    // reliably and osk.exe has to be probed; the overlay is our own window, so
    // "is it up" is a fact rather than a guess -- and the remembered flag,
    // which exists to cover not knowing, is not consulted for it.
    const int known = (program == Program::Overlay) ? (ctm_overlay::visible() ? 1 : 0)
                    : (program == Program::Osk)     ? osk_known_state()
                                                    : -1;
    const Action action = next_action(known, g_remembered.load());
    if (action == Action::Open) {
        do_open(program, button);
        g_remembered.store(true);
    } else {
        do_close(program);
        g_remembered.store(false);
    }
}

} // namespace ctm_osk

// ⓘ `button` is the standard index of the button that fired this, so the
// overlay can be dismissed by the same one. The other two keyboards ignore it.
// ⓘ The overlay's Steam key. Defined here because this file already knows how
// to hand a steam:// URL to the shell, and declared in overlay_window.inl,
// which is included before it.
void ctm_overlay_open_steam()
{
    ctm_osk::run_shell(L"steam://open/bigpicture");
}

void ctm_osk_toggle(const std::string &section, int button, int program)
{
    ctm_osk::toggle(section, button,
                    static_cast<ctm_osk::Program>(program));
}
