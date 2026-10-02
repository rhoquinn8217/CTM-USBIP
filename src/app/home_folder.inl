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
// In a release that is the exe's own folder.
//
// ⛔⛔ EXCEPT IN A BUILD'S OUTPUT FOLDER, WHICH SAYS WHERE ITS HOME IS. A
// working checkout keeps its configs, its log and the settings page at its
// ROOT, and the exe three folders below, in out\x64\Debug. The first version
// of this rule expected to find no profiles down there and to walk up to the
// root. It found them: the build copies the profiles beside the exe, so the
// output folder passed as a home, and a double-clicked listener read seven
// stale configs there and wrote its log there while the person's own configs
// sat at the root. Nothing said so; it was seen in where the log had gone.
// ➡️ So the build leaves one line beside the exe, in home-folder.txt, naming
// the root, and the exe goes where that points. A release has no such file.
// ⓘ Not guessed from the folder's name or from what else is in it: a release
// staged inside a checkout sits three folders below the same root, beside
// profiles of its own, and has to stay its own home.
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

// ⭐ The file a build leaves beside the exe, and what its one line names.
// A path that is not absolute is taken from the exe's folder, so the file a
// build writes says `..\..\..` and still holds when the checkout is moved.
// ⓘ Returned as written, joined but not tidied: the caller asks Windows for
// the folder it means, and whether it exists. Empty when the line says nothing.
constexpr const wchar_t *kPointerFile = L"home-folder.txt";

inline std::wstring pointed_at(const std::wstring &exeFolder, const std::wstring &line)
{
    // The first line only, without the spaces, quotes or line end around it.
    std::wstring path = line.substr(0, line.find_first_of(L"\r\n"));
    const auto blank = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'"'; };
    while (!path.empty() && blank(path.back())) path.pop_back();
    size_t first = 0;
    while (first < path.size() && blank(path[first])) ++first;
    path = path.substr(first);
    if (path.empty()) return std::wstring();

    const bool hasDrive = path.size() >= 2 && path[1] == L':';
    const bool fromRoot = path[0] == L'\\' || path[0] == L'/';
    if (hasDrive || fromRoot) return path;
    return trimmed(exeFolder) + L"\\" + path;
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
