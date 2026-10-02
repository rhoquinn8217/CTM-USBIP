// The words on the tray icon's menu, and the order they come in.
//
// ⭐ WHAT THE MENU IS (rhoquinn8217, 2026-10-02, in their order):
//
//     DS5-USBIP                      a title, larger
//     2 controllers connected        and a count under it
//     ----------------------------
//     Controllers                >   one line each; choosing one opens the
//     ----------------------------   config window on that controller
//     Open Controller Config
//     Open Virtual Keyboard
//     ----------------------------
//     Config Mode                >   Advanced, Simple, Quick, with the
//     ----------------------------   padlock on the one the window is in
//     Quit
//
// ⓘ Quit was not in the order they gave. It is the only way the listener is
// closed, so it stays, last, under a break of its own.
//
// ⛔ "CONFIG MODE" HERE IS THE WINDOW'S LAYOUT. Inside the listener the same
// two words already name something else -- the state in which a pad drives
// the settings page instead of the game -- so in code and in the log the
// layout is always called the VIEW or the mode of the window, never that.
//
// ⭐ Pure on purpose -- no Windows call -- so the test binary includes it as it
// is. tray_icon.inl builds the menu from these and does the drawing.

#pragma once

#include <cstddef>
#include <string>

namespace tray_menu {

inline const wchar_t *const kTitle = L"DS5-USBIP";
inline const wchar_t *const kControllers = L"Controllers";
inline const wchar_t *const kOpenConfig = L"Open Controller Config";
inline const wchar_t *const kConfigMode = L"Config Mode";
inline const wchar_t *const kQuit = L"Quit";

// ⓘ By code point, so no editor and no compiler setting can turn it into
// something else on the way to the screen. It is the mark the settings page's
// own Mode picker puts on the mode the window is in.
inline const wchar_t *const kPadlock = L"\U0001F512";

// The line under the title. ⓘ It counts the lines the Controllers list will
// show, so the two cannot disagree.
inline std::wstring count_line(size_t devices)
{
    if (devices == 0) return L"No controllers connected";
    if (devices == 1) return L"1 controller connected";
    return std::to_wstring(devices) + L" controllers connected";
}

// The keyboard's line says what choosing it will DO, so it changes with the
// keyboard: there is one line, not two.
inline const wchar_t *keyboard_line(bool keyboardIsOpen)
{
    return keyboardIsOpen ? L"Close Virtual Keyboard" : L"Open Virtual Keyboard";
}

// What a pad last said about its charge, or "" when it has said nothing.
// ⛔ A percent below zero is "the pad did not say", which is not an empty
// battery: the line then leaves the battery out altogether.
inline std::string battery_words(int percent, const std::string &state)
{
    if (percent < 0) return std::string();
    if (percent > 100) percent = 100;
    std::string words = std::to_string(percent) + "%";
    if (state == "charging") words += ", charging";
    return words;
}

// One controller's line: its nickname, what it is and how it is connected,
// and its battery when it has said. "Kestrel - DualSense (USB) - 85%".
// ⓘ `label` is device_names::label(), which already carries USB or BT.
inline std::string device_line(const std::string &nickname, const std::string &label,
                               int batteryPercent, const std::string &batteryState)
{
    std::string line = nickname;
    if (!label.empty()) line += (line.empty() ? "" : " - ") + label;
    const std::string battery = battery_words(batteryPercent, batteryState);
    if (!battery.empty()) line += (line.empty() ? "" : " - ") + battery;
    return line;
}

// The three layouts of the config window, in the order the page lists them.
struct Mode {
    const wchar_t *name;
    bool compact;
    bool quick;
};
inline constexpr Mode kModes[3] = {
    { L"Advanced", false, false },
    { L"Simple",   true,  false },
    { L"Quick",    true,  true  },
};
inline constexpr int kModeCount = 3;

// Which of the three the window is in, or -1 when the listener has not been
// told: it has just started for the first time and no window has opened yet.
inline int mode_index(bool known, bool compact, bool quick)
{
    if (!known) return -1;
    if (!compact) return 0;
    return quick ? 2 : 1;
}

// A mode's line. The padlock goes on the one the window is in, as it does in
// the page's own picker, and on no other.
inline std::wstring mode_line(int index, int current)
{
    if (index < 0 || index >= kModeCount) return std::wstring();
    std::wstring line = kModes[index].name;
    if (index == current) { line += L" "; line += kPadlock; }
    return line;
}

} // namespace tray_menu
