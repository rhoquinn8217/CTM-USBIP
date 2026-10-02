// A program with no window of its own that still answers in the terminal it
// was typed into.
//
// ⭐ WHY THE EXE IS A WINDOWS PROGRAM NOW, AND NOT A CONSOLE ONE. A console
// program brings a terminal window with it whenever it is double-clicked, and
// the listener is something that lives in the tray: rhoquinn8217, 2026-10-01,
// *"I don't want the terminal to be shown either. I want to run it in the
// background and the tray icon to be the place where it is closed."* A
// launcher got rid of the window by starting the exe through Windows' console
// host with its window turned off, which meant a script, or a shortcut with a
// long command in it, standing between a person and the program.
//
// ⛔ BUT IT IS STILL A COMMAND, and a Windows program is not given a console.
// Typed into a terminal -- `ctm-usbip version`, `ctm-usbip list-bt`, the agent
// run by hand to watch its log -- it would print nothing at all. So the first
// thing it does is ask for its PARENT's console, and write there.
//
// ⓘ What that is like to use. The shell does not wait for a Windows program,
// so the prompt comes back at once and the output arrives after it. A script
// that reads the output is unaffected: it hands the program a pipe or a file,
// which this leaves exactly as it was given.

#pragma once

namespace console_attach {

// ⭐ Whether this process has a console to write on. Read by anything that
// starts a console program of its own: with no console here, that child would
// be given a new one, and a window with it.
inline bool g_has_console = false;

// True when a standard handle already leads somewhere a script chose: a file
// or a pipe. ⓘ Not a console, and not nothing, which are the two a Windows
// program starts with.
inline bool leads_somewhere(DWORD which)
{
    const HANDLE handle = GetStdHandle(which);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;
    const DWORD type = GetFileType(handle);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

struct Result {
    bool attached = false;      // the parent had a console, and it is ours too now
    bool redirected = false;    // the output was already going to a file or a pipe

    // Somebody will read what this prints: a person at a terminal, or a script.
    // ⓘ False is a double-click, a shortcut, or anything else with no terminal.
    bool someone_reads() const { return attached || redirected; }
};

inline Result attach_to_parent()
{
    Result result;
    const bool outGoes = leads_somewhere(STD_OUTPUT_HANDLE);
    const bool errGoes = leads_somewhere(STD_ERROR_HANDLE);
    const bool inGoes = leads_somewhere(STD_INPUT_HANDLE);
    result.redirected = outGoes || errGoes;

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        result.attached = true;
        g_has_console = true;
        // ⛔ ONLY THE STREAMS THAT LEAD NOWHERE. A stream a script redirected
        // must keep going where it was sent: `ctm-usbip list-bt > devices.json`
        // typed in a terminal has a console AND a file, and the file wins.
        FILE *ignored = nullptr;
        if (!outGoes) freopen_s(&ignored, "CONOUT$", "w", stdout);
        if (!errGoes) freopen_s(&ignored, "CONOUT$", "w", stderr);
        if (!inGoes) freopen_s(&ignored, "CONIN$", "r", stdin);
        // ⓘ The C++ streams were built before there was anything to write to,
        // and remember that they failed.
        std::cout.clear();
        std::wcout.clear();
        std::cerr.clear();
        std::wcerr.clear();
        std::cin.clear();
        std::wcin.clear();
    }
    return result;
}

}  // namespace console_attach
