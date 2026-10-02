// How a listener that could not start says so.
//
// ⛔ A FAILED START USED TO BE SILENT. The reasons were written to the console,
// and a listener started from a shortcut has none: no window came up, no tray
// icon appeared, and the only trace was a line in a log nobody had been told
// to read. With no launcher script in front of the exe there is nobody else
// left to say it either, so the exe says it itself: in the terminal when it
// was typed into one, and in a message box when it was not.

#pragma once

namespace start_report {

// ⓘ Set once main knows: is anybody reading what this program prints?
inline bool g_someone_reads = true;
// The reason the start failed, kept for the box. Empty when it did not.
inline std::wstring g_reason;

// Record why the listener is not going to start. ⓘ The first reason is the
// one that matters; anything after it is usually a consequence.
inline void failed(const std::wstring &why)
{
    std::wcerr << why << L"\n";
    if (g_reason.empty()) g_reason = why;
}

// A message box, when nobody would have seen the reason any other way.
// ⓘ Blocks until it is dismissed, which is fine: there is no listener running
// behind it to hold up.
inline void show_if_unseen(const wchar_t *title)
{
    if (g_someone_reads || g_reason.empty()) return;
    MessageBoxW(nullptr, g_reason.c_str(), title,
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
}

}  // namespace start_report
