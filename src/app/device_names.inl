// What a bridged device is called, wherever a person reads its name.
//
// ⭐ ONE TABLE, AND IT IS THE LISTENER'S. The settings page worked this out for
// itself -- which model a kind names, and whether it arrived over USB or
// Bluetooth -- and that was fine while the page was the only place a device
// was named. The tray icon's menu lists the devices too, and the menu belongs
// to the listener. Two tables naming one thing drift apart, so the page asks
// for the name now (`label`, in each device of /api/v1/devices) and keeps no
// table of its own.
//
// ⛔ THE KIND STRINGS READ BACKWARDS, and that is not fixable here: "ds5" is a
// DualSense on BLUETOOTH, and the cabled one carries the _usb suffix. The TV
// sends them, so renaming would be a change to two repos' protocol rather
// than a rename. This file is where they are turned into words.
//
// ⓘ USB or Bluetooth is how the device reaches the TELEVISION, and it is in
// the kind for a DualSense, an Edge and a DualShock 4 only. For anything else
// it is not known here, and the name says nothing rather than guess.
//
// ⭐ Pure on purpose -- no Windows call, no session -- so the test binary
// includes it as it is.

#pragma once

#include <string>

namespace device_names {

// The model a session's kind names, or "" for a kind that names none: a Pro
// Controller, a keyboard dongle, anything that arrives as a plain device.
inline std::string model_for_kind(const std::string &kind)
{
    // ⓘ "ds5_edge" is not a kind the agent has ever used, and the page named
    // it all the same. Kept, so nothing that read as an Edge stops doing so.
    if (kind == "ds5e_usb" || kind == "ds5_edge") return "DualSense Edge";
    if (kind == "ds5" || kind == "ds5_usb") return "DualSense";
    if (kind == "ds4" || kind == "ds4_usb") return "DualShock 4";
    if (kind == "xbox") return "Xbox Controller";
    return std::string();
}

// How that kind reaches the TV, in the short form a list has room for: "BT",
// "USB", or "" when the kind does not say.
inline std::string link_for_kind(const std::string &kind)
{
    if (kind == "ds5" || kind == "ds4") return "BT";
    if (kind == "ds5_usb" || kind == "ds5e_usb" || kind == "ds4_usb") return "USB";
    return std::string();
}

// ⭐ The name a device is listed by: "DualSense (USB)", "DualShock 4 (BT)",
// "Xbox Controller".
//
// ⛔ NEVER "hid" WHERE ANYTHING BETTER IS KNOWN (rhoquinn8217, 2026-09-13):
// for a kind this file does not name, the model name the TV sent comes first,
// then what sort of device its descriptor says it is, and the raw kind only
// after both.
inline std::string label(const std::string &kind, const std::string &product,
                         const std::string &deviceType)
{
    const std::string model = model_for_kind(kind);
    if (!model.empty()) {
        const std::string link = link_for_kind(kind);
        return link.empty() ? model : model + " (" + link + ")";
    }
    if (!product.empty()) return product;
    if (!deviceType.empty()) {
        std::string type = deviceType;
        if (type[0] >= 'a' && type[0] <= 'z') type[0] = static_cast<char>(type[0] - 'a' + 'A');
        return type;
    }
    if (!kind.empty()) return kind;
    return "controller";
}

} // namespace device_names
