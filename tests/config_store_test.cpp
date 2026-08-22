// Tests for per-controller config files.
//
// WHAT THESE PROTECT. The property that matters most here is PRESERVATION: a
// write must not disturb comments, ordering, blank lines, or any key it did not
// name. Someone who hand-edited a config must not lose that work because a UI
// touched the file. Most of what follows exists for that one property.
//
// The rest guard the rules that are easy to state and easy to break silently:
// a config only claims settings under its own kind, a serial is normalised the
// same way everywhere, and nothing reaches the file that could forge a line.
//
// WHAT THEY CANNOT DO. They say nothing about whether a linked config actually
// reaches a controller -- that is threading through the output path and needs
// hardware. Protecting logic, not behaviour.

#include "harness.h"
#include "units.h"

using namespace ctmtest;

namespace {

namespace cs = units::config_store;

// The runner executes from the build output directory, so configs/ here is
// scratch space and never the repo's.
void wipe_configs()
{
    std::error_code ignored;
    std::filesystem::remove_all("configs", ignored);
}

void write_config(const std::string &name, const std::string &body)
{
    std::filesystem::create_directories("configs");
    std::ofstream out("configs\\" + name + ".txt", std::ios::binary | std::ios::trunc);
    out << body;
    out.close();
    cs::reload_all();
}

std::string read_config(const std::string &name)
{
    std::ifstream in("configs\\" + name + ".txt", std::ios::binary);
    std::ostringstream all;
    all << in.rdbuf();
    return all.str();
}

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

// How many times `needle` appears -- catches a key being duplicated rather
// than rewritten in place.
int count_of(const std::string &haystack, const std::string &needle)
{
    int count = 0;
    size_t at = 0;
    while ((at = haystack.find(needle, at)) != std::string::npos) { ++count; ++at; }
    return count;
}

} // namespace

int run_config_store_tests()
{
    wipe_configs();      // start from nothing, whatever a previous run left

    section("config store: names");
    CTM_CHECK(cs::valid_name("couch"));
    CTM_CHECK(cs::valid_name("ds5_custom-01"));
    CTM_CHECK(!cs::valid_name(""));
    CTM_CHECK(!cs::valid_name("has space"));
    CTM_CHECK(!cs::valid_name("dots.bad"));
    CTM_CHECK(!cs::valid_name("../escape"));          // must not reach outside configs/
    CTM_CHECK(!cs::valid_name("archive"));            // reserved: the archive folder
    CTM_CHECK(!cs::valid_name("shared"));             // reserved: the shared section
    CTM_CHECK(!cs::valid_name("SHARED"));
    CTM_CHECK(!cs::valid_name(std::string(49, 'a')));

    section("config store: a key or value cannot forge a line");
    CTM_CHECK(cs::valid_setting_key("gyro_mouse_px_per_360"));
    CTM_CHECK(!cs::valid_setting_key(""));
    CTM_CHECK(!cs::valid_setting_key("has=equals"));
    CTM_CHECK(!cs::valid_setting_key("has space"));
    CTM_CHECK(cs::valid_setting_value("64000"));
    CTM_CHECK(cs::valid_setting_value("!touchpad"));  // a real gate value
    CTM_CHECK(!cs::valid_setting_value("a\r\n[ds5_edge]"));   // would write a section
    CTM_CHECK(!cs::valid_setting_value("40 # comment"));      // would comment the line
    CTM_CHECK(!cs::valid_setting_value("has=equals"));

    section("config store: serials normalise the same way however punctuated");
    CTM_CHECK_EQ(cs::normalise_serial("AA:BB:CC:DD:EE:FF"), std::string("aabbccddeeff"));
    CTM_CHECK_EQ(cs::normalise_serial("aa-bb-cc-dd-ee-ff"), std::string("aabbccddeeff"));
    CTM_CHECK_EQ(cs::normalise_serial("aabbccddeeff"), std::string("aabbccddeeff"));
    CTM_CHECK_EQ(cs::normalise_serial(""), std::string(""));

    section("config store: the section a device reads");
    CTM_CHECK_EQ(cs::section_for("", "ds5"), std::string("ds5"));
    CTM_CHECK_EQ(cs::section_for("Couch", "ds5"), std::string("cfg:couch/ds5"));
    CTM_CHECK_EQ(cs::section_for("couch", "ds5_edge"), std::string("cfg:couch/ds5_edge"));

    section("config store: only the DS5 family may carry a config");
    // ⚠️ These are the SESSION kinds the agent actually uses. An earlier version
    // listed "ds5_edge", which the agent has never used, so a real DualSense
    // arriving as "ds5_usb" was refused a config entirely.
    CTM_CHECK(cs::kind_supports_config("ds5"));
    CTM_CHECK(cs::kind_supports_config("ds5_usb"));
    CTM_CHECK(cs::kind_supports_config("ds5e_usb"));
    CTM_CHECK(!cs::kind_supports_config("ds4"));
    CTM_CHECK(!cs::kind_supports_config("puck"));

    section("config store: ⭐ a session kind maps to the settings section name");
    // The section a setting is READ from comes from the USB product id, not the
    // session kind. A config stored under the session kind would load into a
    // section nothing ever reads -- a link that reports success and does
    // nothing.
    CTM_CHECK_EQ(cs::settings_kind_for("ds5"), std::string("ds5"));
    CTM_CHECK_EQ(cs::settings_kind_for("ds5_usb"), std::string("ds5"));
    CTM_CHECK_EQ(cs::settings_kind_for("ds5e_usb"), std::string("ds5_edge"));
    CTM_CHECK_EQ(cs::settings_kind_for("ds4"), std::string(""));

    section("config store: a config created for a ds5_usb device stores ds5");
    {
        std::string error;
        CTM_CHECK(cs::create_config("usbcfg", "ds5_usb", &error));
        cs::ConfigFile made;
        CTM_CHECK(cs::find_config("usbcfg", &made));
        CTM_CHECK_EQ(made.kind, std::string("ds5"));      // not "ds5_usb"
        // and its settings therefore land where a reader will look for them
        CTM_CHECK(cs::set_setting("usbcfg", "speaker_volume", "42", &error));
        CTM_CHECK_EQ(units::device_config_int("cfg:usbcfg/ds5", "speaker_volume", -1), 42);
    }

    wipe_configs();

    section("config store: settings load into the namespaced section");
    {
        write_config("couch",
            "[config]\r\nkind = ds5\r\nauto_link =\r\n\r\n"
            "[ds5]\r\nspeaker_volume = 65\r\n");
        CTM_CHECK_EQ(units::device_config_int("cfg:couch/ds5", "speaker_volume", -1), 65);
        // ...and does not leak into the shared section
        CTM_CHECK_EQ(units::device_config_int("ds5", "speaker_volume", -1), -1);
    }

    section("config store: a block for another kind is ignored, not honoured");
    {
        // ⚠️ A [ds5] block inside a ds5_edge config is a mistake. Silently
        // applying it would make the file behave differently from how it reads.
        write_config("edgecfg",
            "[config]\r\nkind = ds5_edge\r\nauto_link =\r\n\r\n"
            "[ds5]\r\nspeaker_volume = 11\r\n"
            "[ds5_edge]\r\nspeaker_volume = 22\r\n");
        CTM_CHECK_EQ(units::device_config_int("cfg:edgecfg/ds5_edge", "speaker_volume", -1), 22);
        CTM_CHECK_EQ(units::device_config_int("cfg:edgecfg/ds5", "speaker_volume", -1), -1);
    }

    section("config store: a file with no kind is not a config");
    {
        write_config("nokind", "[ds5]\r\nspeaker_volume = 50\r\n");
        cs::ConfigFile found;
        CTM_CHECK(!cs::find_config("nokind", &found));
    }

    section("config store: ⭐ a write preserves comments, layout and other keys");
    {
        write_config("keep",
            "# a note someone wrote\r\n"
            "[config]\r\nkind = ds5\r\nauto_link =\r\n\r\n"
            "[ds5]\r\n"
            "speaker_volume = 40   # why it is 40\r\n"
            "#gyro_mouse_invert = 3\r\n"
            "\r\n");
        std::string error;
        CTM_CHECK(cs::set_setting("keep", "gyro_to_mouse_gate", "L2", &error));

        const std::string body = read_config("keep");
        CTM_CHECK(contains(body, "# a note someone wrote"));    // leading comment
        CTM_CHECK(contains(body, "why it is 40"));              // inline comment
        CTM_CHECK(contains(body, "speaker_volume = 40"));       // untouched key
        CTM_CHECK(contains(body, "gyro_to_mouse_gate = L2"));   // the new key
        CTM_CHECK(contains(body, "[config]"));                  // headers intact
    }

    section("config store: ⭐ a commented-out key is uncommented, not duplicated");
    {
        std::string error;
        CTM_CHECK(cs::set_setting("keep", "gyro_mouse_invert", "2", &error));
        const std::string body = read_config("keep");
        // Exactly one mention -- the commented line was rewritten in place.
        CTM_CHECK_EQ(count_of(body, "gyro_mouse_invert"), 1);
        CTM_CHECK(contains(body, "gyro_mouse_invert = 2"));
        CTM_CHECK(!contains(body, "#gyro_mouse_invert"));
    }

    section("config store: rewriting a key does not duplicate it either");
    {
        std::string error;
        CTM_CHECK(cs::set_setting("keep", "gyro_to_mouse_gate", "R2", &error));
        const std::string body = read_config("keep");
        CTM_CHECK_EQ(count_of(body, "gyro_to_mouse_gate"), 1);
        CTM_CHECK(contains(body, "gyro_to_mouse_gate = R2"));
    }

    section("config store: a bad key or value is refused BEFORE the write");
    {
        const std::string before = read_config("keep");
        std::string error;
        CTM_CHECK(!cs::set_setting("keep", "bad=key", "1", &error));
        CTM_CHECK(!error.empty());
        CTM_CHECK(!cs::set_setting("keep", "ok_key", "a\r\n[ds5]", &error));
        // ⭐ The file is byte-for-byte unchanged -- a refusal must not half-write.
        CTM_CHECK_EQ(read_config("keep"), before);
    }

    section("config store: create writes an empty settings block");
    {
        std::string error;
        CTM_CHECK(cs::create_config("fresh", "ds5", &error));
        const std::string body = read_config("fresh");
        CTM_CHECK(contains(body, "kind = ds5"));
        CTM_CHECK(contains(body, "[ds5]"));
        // ⭐ Deliberately empty: "all settings at defaults" and "nothing
        // overridden" are the same thing when an absent key is left alone.
        CTM_CHECK(!contains(body, "speaker_volume"));
        // and a second create with the same name is refused
        CTM_CHECK(!cs::create_config("fresh", "ds5", &error));
        // as is a kind that cannot carry a config
        CTM_CHECK(!cs::create_config("nope", "ds4", &error));
        CTM_CHECK(contains(error, "unsupported"));
    }

    section("config store: auto_link claims and refusals");
    {
        std::string error;
        CTM_CHECK(cs::create_config("first", "ds5", &error));
        CTM_CHECK(cs::create_config("second", "ds5", &error));
        CTM_CHECK(cs::add_auto_link("first", "AA:BB:CC:DD:EE:FF", &error));
        CTM_CHECK_EQ(cs::auto_link_for("aabbccddeeff", "ds5"), std::string("first"));

        // ⭐ THE REFUSAL IS THE POINT OF THE VERB: two configs claiming one
        // serial is resolvable but ambiguous, so it is kept unreachable.
        CTM_CHECK(!cs::add_auto_link("second", "aabbccddeeff", &error));
        CTM_CHECK(contains(error, "already claimed"));

        // a kind that does not match does not match
        CTM_CHECK_EQ(cs::auto_link_for("aabbccddeeff", "ds5e_usb"), std::string(""));
        // an empty serial never auto-links
        CTM_CHECK_EQ(cs::auto_link_for("", "ds5"), std::string(""));

        CTM_CHECK(cs::remove_auto_link("first", "aabbccddeeff", &error));
        CTM_CHECK_EQ(cs::auto_link_for("aabbccddeeff", "ds5"), std::string(""));
    }

    section("config store: rename carries the file, its claim and its settings");
    {
        std::string error;
        write_config("oldname",
            "# oldname\r\n[config]\r\nkind = ds5\r\nauto_link = aabbccddeeff\r\n\r\n"
            "[ds5]\r\nspeaker_volume = 55\r\n");
        CTM_CHECK_EQ(units::device_config_int("cfg:oldname/ds5", "speaker_volume", -1), 55);

        CTM_CHECK(cs::rename_config("oldname", "newname", &error));

        cs::ConfigFile gone, moved;
        CTM_CHECK(!cs::find_config("oldname", &gone));
        CTM_CHECK(cs::find_config("newname", &moved));
        // ⭐ Settings follow, under the new namespaced section.
        CTM_CHECK_EQ(units::device_config_int("cfg:newname/ds5", "speaker_volume", -1), 55);
        CTM_CHECK_EQ(units::device_config_int("cfg:oldname/ds5", "speaker_volume", -1), -1);
        // auto_link lives inside the file, so the claim moves with it.
        CTM_CHECK_EQ(cs::auto_link_for("aabbccddeeff", "ds5"), std::string("newname"));
        // and the header comment is corrected rather than left stale
        CTM_CHECK(contains(read_config("newname"), "# newname"));
    }

    section("config store: rename refuses to overwrite or take a reserved name");
    {
        std::string error;
        CTM_CHECK(cs::create_config("other", "ds5", &error));
        // ⛔ Renaming onto a name in use would silently destroy the other one.
        CTM_CHECK(!cs::rename_config("newname", "other", &error));
        CTM_CHECK(contains(error, "already exists"));
        CTM_CHECK(!cs::rename_config("newname", "shared", &error));
        CTM_CHECK(!cs::rename_config("newname", "bad name", &error));
        // the original survived every refusal
        cs::ConfigFile still;
        CTM_CHECK(cs::find_config("newname", &still));
    }

    section("config store: archive moves rather than deletes, and never overwrites");
    {
        std::string error, movedTo;
        CTM_CHECK(cs::archive_config("fresh", &error, &movedTo));
        CTM_CHECK(contains(movedTo, "archive"));
        CTM_CHECK(std::filesystem::exists(movedTo));       // nothing destroyed
        cs::ConfigFile gone;
        CTM_CHECK(!cs::find_config("fresh", &gone));       // out of the listing

        // ⭐ Recreate and archive again: the first archived copy must survive.
        CTM_CHECK(cs::create_config("fresh", "ds5", &error));
        std::string secondMove;
        CTM_CHECK(cs::archive_config("fresh", &error, &secondMove));
        CTM_CHECK(secondMove != movedTo);
        CTM_CHECK(std::filesystem::exists(movedTo));
        CTM_CHECK(std::filesystem::exists(secondMove));
    }

    section("config store: archived configs are not reloaded");
    {
        cs::reload_all();
        cs::ConfigFile gone;
        CTM_CHECK(!cs::find_config("fresh", &gone));
    }

    section("config store: a reload drops values from a deleted file");
    {
        write_config("temp",
            "[config]\r\nkind = ds5\r\nauto_link =\r\n\r\n[ds5]\r\nspeaker_volume = 77\r\n");
        CTM_CHECK_EQ(units::device_config_int("cfg:temp/ds5", "speaker_volume", -1), 77);
        std::error_code ignored;
        std::filesystem::remove("configs\\temp.txt", ignored);
        cs::reload_all();
        // ⚠️ Stale values surviving a delete would be worse than the delete
        // failing -- the file would say one thing and the controller do another.
        CTM_CHECK_EQ(units::device_config_int("cfg:temp/ds5", "speaker_volume", -1), -1);
    }

    section("config store: the shared section survives a config reload");
    {
        // ⛔ device_config_invalidate() once cleared the WHOLE map, wiping every
        // config section on each bridge and each save of the shared file. This
        // guards the other direction too: a config reload must leave the shared
        // section alone.
        std::ofstream shared("ctm-device-config.txt", std::ios::binary | std::ios::trunc);
        shared << "[ds5]\r\nspeaker_volume = 33\r\n";
        shared.close();
        units::device_config_invalidate();
        CTM_CHECK_EQ(units::device_config_int("ds5", "speaker_volume", -1), 33);

        write_config("alive",
            "[config]\r\nkind = ds5\r\nauto_link =\r\n\r\n[ds5]\r\nspeaker_volume = 99\r\n");
        CTM_CHECK_EQ(units::device_config_int("cfg:alive/ds5", "speaker_volume", -1), 99);
        CTM_CHECK_EQ(units::device_config_int("ds5", "speaker_volume", -1), 33);

        // ...and the config section survives an invalidate of the shared file
        units::device_config_invalidate();
        CTM_CHECK_EQ(units::device_config_int("cfg:alive/ds5", "speaker_volume", -1), 99);
    }

    section("config store: ⭐ a deleted key really disappears on reload");
    {
        // ⛔ device_config_load_locked() only INSERTS -- it never clears -- so a
        // reload alone keeps a key that was deleted from the file. That is how
        // an emptied ctm-device-config.txt kept reporting speaker_volume=33
        // while the file on disk was bare. Anything that rereads must erase
        // first; this guards the config-file side of that rule.
        write_config("vanish",
            "[config]\r\nkind = ds5\r\nauto_link =\r\n\r\n"
            "[ds5]\r\nspeaker_volume = 55\r\nheadset_volume = 44\r\n");
        CTM_CHECK_EQ(units::device_config_int("cfg:vanish/ds5", "speaker_volume", -1), 55);
        CTM_CHECK_EQ(units::device_config_int("cfg:vanish/ds5", "headset_volume", -1), 44);

        // rewrite with one key removed
        write_config("vanish",
            "[config]\r\nkind = ds5\r\nauto_link =\r\n\r\n"
            "[ds5]\r\nspeaker_volume = 55\r\n");
        CTM_CHECK_EQ(units::device_config_int("cfg:vanish/ds5", "speaker_volume", -1), 55);
        CTM_CHECK_EQ(units::device_config_int("cfg:vanish/ds5", "headset_volume", -1), -1);
    }

    wipe_configs();
    return 0;
}
