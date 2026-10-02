// Which folder the listener calls home, found from where its exe is.
//
// ⭐ WHAT THESE PROTECT. The config, the configs, the log and the settings
// page all resolve in one folder, and until this rule that folder was wherever
// the listener happened to be started from. Twice that cost a day: a listener
// started from the wrong folder read no configs and served a stale page, and
// said nothing. The rule is small, and these pin both layouts it has to find:
// a release, where the exe sits beside its profiles, and a working checkout,
// where it sits three folders below them.
//
// WHAT THESE CANNOT DO. They do not touch a disk. "Holds the profiles" is a
// list of folders handed in, so that the listener then runs in the folder this
// names is main's to do and is checked by starting it.

#include "harness.h"

#include <set>
#include <string>

#include "app/home_folder.inl"

using namespace ctmtest;

namespace {

// A disk made of the folders that hold a profiles\descriptors.
struct FakeDisk {
    std::set<std::wstring> withProfiles;
    int asked = 0;

    bool operator()(const std::wstring &folder)
    {
        ++asked;
        return withProfiles.count(folder) != 0;
    }
};

std::string narrow(const std::wstring &w)
{
    return std::string(w.begin(), w.end());
}

} // namespace

int run_home_folder_tests()
{
    using home_folder::find;
    using home_folder::parent_of;
    using home_folder::trimmed;

    section("home: in a release the exe's own folder is home");
    {
        FakeDisk disk;
        disk.withProfiles.insert(L"C:\\Users\\someone\\Desktop\\DS5-USBIP-0.1.0");
        const std::wstring home = find(L"C:\\Users\\someone\\Desktop\\DS5-USBIP-0.1.0", std::ref(disk));
        CTM_CHECK_EQ(narrow(home), std::string("C:\\Users\\someone\\Desktop\\DS5-USBIP-0.1.0"));
        // ⓘ Found at once: nothing above it is looked at.
        CTM_CHECK_EQ(disk.asked, 1);
    }

    section("home: in a working checkout it is the root, three folders above the exe");
    {
        FakeDisk disk;
        disk.withProfiles.insert(L"C:\\repos\\listener");
        const std::wstring home = find(L"C:\\repos\\listener\\out\\x64\\Debug", std::ref(disk));
        CTM_CHECK_EQ(narrow(home), std::string("C:\\repos\\listener"));
    }

    section("home: the NEAREST folder wins, so a release staged inside a checkout is its own home");
    {
        // ⛔ The release folder is built inside the checkout, at
        // out\release\<name>, three folders below a root that has profiles of
        // its own. It must not reach up to them: it would then read the
        // checkout's configs and write the checkout's log.
        FakeDisk disk;
        disk.withProfiles.insert(L"C:\\repos\\listener");
        disk.withProfiles.insert(L"C:\\repos\\listener\\out\\release\\DS5-USBIP-0.1.0");
        const std::wstring home = find(L"C:\\repos\\listener\\out\\release\\DS5-USBIP-0.1.0", std::ref(disk));
        CTM_CHECK_EQ(narrow(home), std::string("C:\\repos\\listener\\out\\release\\DS5-USBIP-0.1.0"));
    }

    section("home: profiles further up than a checkout puts them are somebody else's");
    {
        FakeDisk disk;
        disk.withProfiles.insert(L"C:\\a");
        // Four folders above the exe: one more than a checkout's three.
        const std::wstring home = find(L"C:\\a\\b\\c\\d\\e", std::ref(disk));
        CTM_CHECK(home.empty());
        // ⓘ The exe's folder and three above it, and no more.
        CTM_CHECK_EQ(disk.asked, 4);
    }

    section("home: no profiles anywhere is no home, and the caller says so");
    {
        FakeDisk disk;
        CTM_CHECK(find(L"C:\\Users\\someone\\Downloads", std::ref(disk)).empty());
        CTM_CHECK(find(L"", std::ref(disk)).empty());
    }

    section("home: an exe at the top of a drive looks there and stops");
    {
        FakeDisk none;
        CTM_CHECK(find(L"C:\\", std::ref(none)).empty());
        CTM_CHECK_EQ(none.asked, 1);            // there is nothing above a root
        FakeDisk root;
        root.withProfiles.insert(L"C:\\");
        CTM_CHECK_EQ(narrow(find(L"C:\\", std::ref(root))), std::string("C:\\"));
        // One folder down, the root is its parent -- with its slash.
        CTM_CHECK_EQ(narrow(find(L"C:\\tools", std::ref(root))), std::string("C:\\"));
    }

    section("home: a trailing slash, or a forward one, is the same folder");
    {
        FakeDisk disk;
        disk.withProfiles.insert(L"C:\\rel");
        CTM_CHECK_EQ(narrow(find(L"C:\\rel\\", std::ref(disk))), std::string("C:\\rel"));
        CTM_CHECK_EQ(narrow(trimmed(L"C:\\rel\\\\")), std::string("C:\\rel"));
        CTM_CHECK_EQ(narrow(trimmed(L"C:\\")), std::string("C:\\"));
        CTM_CHECK_EQ(narrow(trimmed(L"C:/rel/")), std::string("C:/rel"));
        CTM_CHECK_EQ(narrow(parent_of(L"C:/repos/listener/out")), std::string("C:/repos/listener"));
    }

    section("home: a build's output folder says where its home is, and the exe goes there");
    {
        using home_folder::pointed_at;
        const std::wstring out = L"C:\\repos\\listener\\out\\x64\\Debug";
        // ⭐ What a build writes: three folders up, from the exe's own folder.
        // ⓘ Joined, not tidied. Windows works the `..` out when it is asked for
        // the folder, and the caller asks.
        CTM_CHECK_EQ(narrow(pointed_at(out, L"..\\..\\..")),
                     std::string("C:\\repos\\listener\\out\\x64\\Debug\\..\\..\\.."));
        // As the file really arrives: with its line end, and only its first line.
        CTM_CHECK_EQ(narrow(pointed_at(out, L"..\\..\\..\r\n")),
                     std::string("C:\\repos\\listener\\out\\x64\\Debug\\..\\..\\.."));
        CTM_CHECK_EQ(narrow(pointed_at(out, L"..\\..\\..\nsomething else")),
                     std::string("C:\\repos\\listener\\out\\x64\\Debug\\..\\..\\.."));
        // A whole path is taken as it stands, wherever the exe is.
        CTM_CHECK_EQ(narrow(pointed_at(out, L"D:\\my configs")), std::string("D:\\my configs"));
        CTM_CHECK_EQ(narrow(pointed_at(out, L"\\\\server\\share\\configs")),
                     std::string("\\\\server\\share\\configs"));
        // ⓘ Quotes and stray spaces around it are a person editing the file.
        CTM_CHECK_EQ(narrow(pointed_at(out, L"  \"D:\\my configs\"  ")), std::string("D:\\my configs"));
        // ⛔ A file that says nothing names nothing: the exe then looks for its
        // home the ordinary way, and does not take "" for the folder it is in.
        CTM_CHECK(pointed_at(out, L"").empty());
        CTM_CHECK(pointed_at(out, L"   \r\n").empty());
        CTM_CHECK(pointed_at(out, L"\r\n..\\..\\..").empty());
        // ⓘ A trailing slash on the exe's folder does not double up.
        CTM_CHECK_EQ(narrow(pointed_at(out + L"\\", L"..")), narrow(out) + "\\..");
    }

    section("home: the folder above");
    {
        CTM_CHECK_EQ(narrow(parent_of(L"C:\\a\\b")), std::string("C:\\a"));
        CTM_CHECK_EQ(narrow(parent_of(L"C:\\a\\b\\")), std::string("C:\\a"));
        CTM_CHECK_EQ(narrow(parent_of(L"C:\\a")), std::string("C:\\"));
        CTM_CHECK(parent_of(L"C:\\").empty());
        CTM_CHECK(parent_of(L"a").empty());
        CTM_CHECK(parent_of(L"").empty());
    }

    return 0;
}
