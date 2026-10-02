// Which folder the listener calls home: where its config, its configs, its
// log and its window state live, and where the settings page is read from.
//
// ⭐ FOUND FROM THE EXE, NOT FROM WHERE IT WAS STARTED. Those files were all
// opened by a relative path, so they landed in whatever the working directory
// happened to be. Start the listener from the wrong folder and a setting was
// saved in one place and read from another, which looks exactly like a setting
// that does nothing; the settings page was the copy built into the exe and not
// the one on disk; and the log everyone was reading was not the log being
// written. A launcher script hid that with `cd /d "%~dp0"`, which only helped
// whoever went through the script.
//
// ⭐ HOME IS THE NEAREST FOLDER, AT OR ABOVE THE EXE, THAT HOLDS THE PROFILES.
// In a release that is the exe's own folder. In a working checkout the exe is
// in out\x64\Debug and the profiles are three levels up, at the root, which is
// where the configs and the page have always been.
//
// ⭐ Pure on purpose -- no Windows call, no file -- so the test binary includes
// it as it is. The caller says what "holds the profiles" means.

#pragma once

#include <string>

namespace home_folder {

// ⓘ How far up to look. Three is out\x64\Debug in a working checkout. No
// further: a stray `profiles` folder higher on the disk is not this program's.
constexpr int kMaxUp = 3;

// How the folder in use was chosen, for the one log line that says where every
// relative path resolves. ⓘ Empty when nobody chose: a mode that does not ask
// still runs wherever it was started.
inline std::wstring g_chosen_how;

// A folder with any trailing separators taken off, so two spellings of one
// folder compare and print alike. ⓘ "C:\" stays "C:\": that slash is the root.
inline std::wstring trimmed(const std::wstring &folder)
{
    std::wstring out = folder;
    while (out.size() > 1 && (out.back() == L'\\' || out.back() == L'/')) {
        if (out.size() == 3 && out[1] == L':') break;   // "C:\"
        out.pop_back();
    }
    return out;
}

// The folder above this one, or empty when there is none.
inline std::wstring parent_of(const std::wstring &folder)
{
    const std::wstring here = trimmed(folder);
    const size_t slash = here.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    // ⓘ "C:\" has no parent, and "C:\foo" has "C:\" -- with its slash.
    if (slash == here.size() - 1) return std::wstring();
    if (slash == 2 && here[1] == L':') return here.substr(0, 3);
    if (slash == 0) return here.substr(0, 1);
    return here.substr(0, slash);
}

// The nearest folder at or above `exeFolder` that `holdsProfiles` accepts,
// looking no more than kMaxUp levels up. Empty when there is none.
template <typename HoldsProfiles>
inline std::wstring find(const std::wstring &exeFolder, HoldsProfiles holdsProfiles)
{
    std::wstring folder = trimmed(exeFolder);
    for (int up = 0; up <= kMaxUp && !folder.empty(); ++up) {
        if (holdsProfiles(folder)) return folder;
        folder = parent_of(folder);
    }
    return std::wstring();
}

}  // namespace home_folder
