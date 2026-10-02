// What a binding's value names: the one reader a button's remap and a
// touchpad gesture both ask.
//
// ⭐ WHAT THESE PROTECT. The settings page offers a list of names, the config
// stores one, the config reader lowercases it, and the listener has to
// recognise what comes back. Each of those steps has already lost a binding
// once: a mixed-case name compared literally never matched, and a keyboard
// binding did nothing at all for the same reason. The last section reads the
// list the page is actually sent and asks the reader about every name on it.
//
// WHAT THESE CANNOT DO. They do not press anything. Whether a key reaches
// Windows, or a button a game, is the device's and the rebinder's.
//
// ⛔ THE LAST SECTION READS THE SOURCE, so this suite runs BEFORE the
// scratch-directory switch in tests_main, like the schema suite beside it.

#include "harness.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "input/binding_names.inl"

using namespace ctmtest;

namespace {

// The names in the "key_names" list of the settings reply, read out of the
// source the way the schema suite reads the settings.
std::vector<std::string> offered_names(bool *found)
{
    *found = false;
    std::vector<std::string> names;
    const char *candidates[] = {
        "src/app/rest_config.inl",
        "../src/app/rest_config.inl",
        "../../src/app/rest_config.inl",
        "../../../src/app/rest_config.inl",
        "../../../../src/app/rest_config.inl",
    };
    std::string text;
    for (const char *where : candidates) {
        std::ifstream in(where, std::ios::binary);
        if (!in) continue;
        std::stringstream buffer;
        buffer << in.rdbuf();
        text = buffer.str();
        break;
    }
    if (text.empty()) return names;

    const std::string opener = "\"key_names\":[";
    const size_t start = text.find(opener);
    if (start == std::string::npos) return names;
    const size_t end = text.find(']', start);
    if (end == std::string::npos) return names;
    *found = true;

    size_t at = start + opener.size();
    while (at < end) {
        const size_t open = text.find('"', at);
        if (open == std::string::npos || open >= end) break;
        const size_t close = text.find('"', open + 1);
        if (close == std::string::npos || close > end) break;
        names.push_back(text.substr(open + 1, close - open - 1));
        at = close + 1;
    }
    return names;
}

} // namespace

int run_binding_names_tests()
{
    using binding::parse;

    section("binding: nothing set names nothing");
    {
        CTM_CHECK_EQ(static_cast<int>(parse("").kind), static_cast<int>(binding::kNone));
        CTM_CHECK_EQ(static_cast<int>(parse("banana").kind), static_cast<int>(binding::kNone));
        // ⓘ A bare letter is a gate's name for a button, not a binding's for a key.
        CTM_CHECK_EQ(static_cast<int>(parse("x").kind), static_cast<int>(binding::kNone));
    }

    section("binding: a key is its usage, and a modifier is its bit");
    {
        const binding::Target a = parse("KeyA");
        CTM_CHECK_EQ(static_cast<int>(a.kind), static_cast<int>(binding::kKey));
        CTM_CHECK_EQ(static_cast<int>(a.usage), 0x04);
        CTM_CHECK_EQ(static_cast<int>(a.modifier), 0);

        const binding::Target enter = parse("Enter");
        CTM_CHECK_EQ(static_cast<int>(enter.kind), static_cast<int>(binding::kKey));
        CTM_CHECK_EQ(static_cast<int>(enter.usage), 0x28);

        // ⓘ A modifier is a bit in the report's first byte, with no key slot.
        const binding::Target shift = parse("ShiftLeft");
        CTM_CHECK_EQ(static_cast<int>(shift.kind), static_cast<int>(binding::kKey));
        CTM_CHECK_EQ(static_cast<int>(shift.usage), 0);
        CTM_CHECK_EQ(static_cast<int>(shift.modifier), 0x02);
    }

    section("binding: the config reader's lowercase names the same thing");
    {
        // ⛔ device_config_str LOWERCASES what it returns. Every kind has to
        // survive that, or the binding silently does nothing.
        CTM_CHECK_EQ(static_cast<int>(parse("keya").usage), 0x04);
        CTM_CHECK_EQ(static_cast<int>(parse("arrowup").usage), 0x52);
        CTM_CHECK_EQ(static_cast<int>(parse("shiftleft").modifier), 0x02);
        CTM_CHECK_EQ(static_cast<int>(parse("mouseleft").mouseMask), 0x01);
        CTM_CHECK_EQ(parse("mousewheeldown").wheel, -1);
        CTM_CHECK_EQ(parse("keyboardds5_usbip").osk, static_cast<int>(binding::kOskOurs));
        CTM_CHECK_EQ(parse("BUTTON_CIRCLE").button, static_cast<int>(ctm_rebind::kBtnFaceRight));
    }

    section("binding: the mouse's buttons are masks and its wheel is a direction");
    {
        CTM_CHECK_EQ(static_cast<int>(parse("MouseLeft").kind), static_cast<int>(binding::kMouseButton));
        CTM_CHECK_EQ(static_cast<int>(parse("MouseLeft").mouseMask), 0x01);
        CTM_CHECK_EQ(static_cast<int>(parse("MouseRight").mouseMask), 0x02);
        CTM_CHECK_EQ(static_cast<int>(parse("MouseMiddle").mouseMask), 0x04);
        // ⛔ A wheel is not a button: its mask is 0, so nothing that holds a
        // button can hold it by mistake.
        const binding::Target up = parse("MouseWheelUp");
        CTM_CHECK_EQ(static_cast<int>(up.kind), static_cast<int>(binding::kMouseWheel));
        CTM_CHECK_EQ(up.wheel, 1);
        CTM_CHECK_EQ(static_cast<int>(up.mouseMask), 0);
        CTM_CHECK_EQ(parse("MouseWheelDown").wheel, -1);
    }

    section("binding: each on-screen keyboard is its own, and the old name is ours");
    {
        CTM_CHECK_EQ(static_cast<int>(parse("KeyboardSteam").kind), static_cast<int>(binding::kOsk));
        CTM_CHECK_EQ(parse("KeyboardSteam").osk, static_cast<int>(binding::kOskSteam));
        CTM_CHECK_EQ(parse("KeyboardWindows").osk, static_cast<int>(binding::kOskWindows));
        CTM_CHECK_EQ(parse("KeyboardDS5_USBIP").osk, static_cast<int>(binding::kOskOurs));
        // ⓘ Configs written before the three were told apart say OSKeyboard.
        CTM_CHECK_EQ(parse("OSKeyboard").osk, static_cast<int>(binding::kOskOurs));
        CTM_CHECK_EQ(binding::osk_program_for("KeyA"), -1);
    }

    section("binding: a pad button is named with its prefix, and only with it");
    {
        const binding::Target cross = parse("button_cross");
        CTM_CHECK_EQ(static_cast<int>(cross.kind), static_cast<int>(binding::kPadButton));
        CTM_CHECK_EQ(cross.button, static_cast<int>(ctm_rebind::kBtnFaceDown));
        CTM_CHECK_EQ(parse("button_r2").button, static_cast<int>(ctm_rebind::kBtnR2));
        CTM_CHECK_EQ(parse("button_dpad_up").button, static_cast<int>(ctm_rebind::kBtnDpadUp));
        // ⓘ The bare name is a gate's, and is nothing here.
        CTM_CHECK_EQ(static_cast<int>(parse("cross").kind), static_cast<int>(binding::kNone));
    }

    section("binding: a pad button that does not exist is nothing, and is not read as a key");
    {
        // ⛔ TWO DIFFERENT "NO". `button_nosuch` claims to be a pad button and
        // names none: it must stop there. Falling through would hand the same
        // text to the key reader, and a future key name could match it.
        CTM_CHECK_EQ(binding::pad_button_for("button_nosuch"), static_cast<int>(binding::kNoSuchPadButton));
        CTM_CHECK_EQ(static_cast<int>(parse("button_nosuch").kind), static_cast<int>(binding::kNone));
        // ⓘ Not a pad button at all: something else may still read it.
        CTM_CHECK_EQ(binding::pad_button_for("KeyA"), static_cast<int>(binding::kNotAPadButton));
        CTM_CHECK_EQ(binding::pad_button_for("button_"), static_cast<int>(binding::kNotAPadButton));
        CTM_CHECK_EQ(static_cast<int>(parse("button_").kind), static_cast<int>(binding::kNone));
    }

    section("binding: every name the settings page is sent reads as something");
    {
        // ⭐ The list the page builds its picker from, out of the source. A name
        // offered there that the reader does not know is a choice that saves
        // and does nothing.
        bool found = false;
        const std::vector<std::string> names = offered_names(&found);
        CTM_CHECK(found);
        // ⓘ Well over a hundred today. A short list means the read went wrong.
        CTM_CHECK(names.size() > 100);
        int unknown = 0;
        int keys = 0, mouse = 0, keyboards = 0, buttons = 0;
        for (const std::string &name : names) {
            const binding::Target t = parse(name);
            if (t.kind == binding::kNone) {
                ++unknown;
                std::printf("    not recognised: %s\n", name.c_str());
            }
            if (t.kind == binding::kKey) ++keys;
            if (t.kind == binding::kMouseButton || t.kind == binding::kMouseWheel) ++mouse;
            if (t.kind == binding::kOsk) ++keyboards;
            if (t.kind == binding::kPadButton) ++buttons;
        }
        CTM_CHECK_EQ(unknown, 0);
        CTM_CHECK_EQ(mouse, 5);
        CTM_CHECK_EQ(keyboards, 3);
        CTM_CHECK_EQ(buttons, static_cast<int>(ctm_rebind::kButtonCount));
        CTM_CHECK(keys > 90);
    }

    return 0;
}
