// What a bridged device is called.
//
// ⭐ WHAT THESE PROTECT. The name a person reads for a device -- on the
// settings page's tabs and tables, and on the tray icon's menu -- comes from
// one table now. The kind strings read backwards ("ds5" is the BLUETOOTH
// DualSense), which nobody guesses correctly, so each one is pinned here to
// the words it was shown by before the table moved out of the page.
//
// WHAT THESE CANNOT DO. They do not show that the page asks for the name, or
// that the menu lists it. Those are looked at with the listener running.

#include "harness.h"

#include <string>

#include "app/device_names.inl"

using namespace ctmtest;

int run_device_names_tests()
{
    section("device names: each kind that names a model, and the link it arrived by");
    {
        using device_names::label;
        // ⛔ The backwards ones. "ds5" with no suffix is Bluetooth.
        CTM_CHECK_EQ(label("ds5", "", ""), std::string("DualSense (BT)"));
        CTM_CHECK_EQ(label("ds5_usb", "", ""), std::string("DualSense (USB)"));
        CTM_CHECK_EQ(label("ds5e_usb", "", ""), std::string("DualSense Edge (USB)"));
        CTM_CHECK_EQ(label("ds4", "", ""), std::string("DualShock 4 (BT)"));
        CTM_CHECK_EQ(label("ds4_usb", "", ""), std::string("DualShock 4 (USB)"));
        // ⓘ An Xbox pad's kind does not say how it reached the TV, so the
        // name does not either.
        CTM_CHECK_EQ(label("xbox", "", ""), std::string("Xbox Controller"));
        // A kind the agent never sends, which the page named all the same.
        CTM_CHECK_EQ(label("ds5_edge", "", ""), std::string("DualSense Edge"));
    }

    section("device names: a kind that names a model keeps it, whatever the TV called the device");
    {
        using device_names::label;
        // ⛔ The TV's own name for a DualSense is long and says "Wireless
        // Controller" for a pad on a cable. The model wins.
        CTM_CHECK_EQ(label("ds5_usb", "Sony Interactive Entertainment DualSense Wireless Controller", "controller"),
                     std::string("DualSense (USB)"));
        CTM_CHECK_EQ(label("xbox", "Generic X-Box pad", "controller"), std::string("Xbox Controller"));
    }

    section("device names: never the raw kind where anything better is known");
    {
        using device_names::label;
        // The model name the TV sent comes first...
        CTM_CHECK_EQ(label("hid", "Pro Controller", "controller"), std::string("Pro Controller"));
        // ...then what sort of device it is, with a capital...
        CTM_CHECK_EQ(label("hid", "", "keyboard"), std::string("Keyboard"));
        CTM_CHECK_EQ(label("hid", "", "mouse"), std::string("Mouse"));
        // ...and the kind only when there is neither.
        CTM_CHECK_EQ(label("hid", "", ""), std::string("hid"));
        CTM_CHECK_EQ(label("puck", "", ""), std::string("puck"));
        // Nothing at all still reads as something.
        CTM_CHECK_EQ(label("", "", ""), std::string("controller"));
        // ⓘ A type that already has its capital, or begins with a digit, is
        // left as it stands.
        CTM_CHECK_EQ(label("hid", "", "Keyboard"), std::string("Keyboard"));
        CTM_CHECK_EQ(label("hid", "", "8bitdo"), std::string("8bitdo"));
    }

    section("device names: the two halves, asked for separately");
    {
        using device_names::link_for_kind;
        using device_names::model_for_kind;
        CTM_CHECK_EQ(model_for_kind("ds5"), std::string("DualSense"));
        CTM_CHECK_EQ(model_for_kind("ds5_usb"), std::string("DualSense"));
        CTM_CHECK_EQ(model_for_kind("ds5e_usb"), std::string("DualSense Edge"));
        CTM_CHECK(model_for_kind("hid").empty());
        CTM_CHECK(model_for_kind("").empty());
        CTM_CHECK_EQ(link_for_kind("ds5"), std::string("BT"));
        CTM_CHECK_EQ(link_for_kind("ds4"), std::string("BT"));
        CTM_CHECK_EQ(link_for_kind("ds5_usb"), std::string("USB"));
        CTM_CHECK_EQ(link_for_kind("ds5e_usb"), std::string("USB"));
        CTM_CHECK_EQ(link_for_kind("ds4_usb"), std::string("USB"));
        // ⛔ Not known is not guessed.
        CTM_CHECK(link_for_kind("xbox").empty());
        CTM_CHECK(link_for_kind("hid").empty());
        CTM_CHECK(link_for_kind("ds5_edge").empty());
    }

    return 0;
}
