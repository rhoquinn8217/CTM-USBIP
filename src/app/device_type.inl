// What a bridged device is, read off its HID report descriptor: a controller, a
// keyboard or a mouse, or nothing said.
//
// ⭐⭐ WHY (rhoquinn8217, 2026-09-13): "I don't want the type to be labeled hid.
// I want it to be its actual name (Xbox Series Controller), or if that isn't
// available, its usb type (controller or keyboard) and hid is reserved when it
// doesn't have both." The settings page named the DualSense kinds and printed
// the raw kind for everything else, so a keyboard, a GameSir and a Pro
// Controller all showed as "hid". The name arrives from the TV at HELLO; this is
// the second answer, the type, for when there is no name.
//
// ⭐ THE FIRST TOP-LEVEL COLLECTION DECIDES, as the idle rule's
// descriptor_is_gamepad (bridge.inl) already reads it: a GameSir lists its pad
// before its media keys and keyboard, and is a controller.
//
// ⓘ No standard headers, as with every .inl (tests/units.h). Needs <cstdint>,
// <string> and <vector> first.

#pragma once

namespace device_type {

// "controller", "keyboard", "mouse", or "" when the descriptor says none of
// those (a consumer-control remote, a vendor device, a cut-off descriptor).
inline std::string from_descriptor(const std::vector<uint8_t> &d)
{
    uint32_t usagePage = 0;
    uint32_t usage = 0;
    size_t usageLen = 0;
    bool haveUsage = false;
    size_t i = 0;
    while (i < d.size()) {
        const uint8_t prefix = d[i];
        if (prefix == 0xFE) {   // a long item: its data size is in the next byte
            if (i + 1 >= d.size()) break;
            i += 3u + d[i + 1];
            continue;
        }
        const uint8_t sizeCode = prefix & 0x03;
        const size_t dataLen = sizeCode == 3 ? 4 : sizeCode;
        if (i + 1 + dataLen > d.size()) break;
        uint32_t value = 0;
        for (size_t b = 0; b < dataLen; ++b) {
            value |= static_cast<uint32_t>(d[i + 1 + b]) << (8 * b);
        }
        i += 1 + dataLen;

        const uint8_t tag = prefix & 0xFC;
        if (tag == 0x04) {              // Global: Usage Page
            usagePage = value;
        } else if (tag == 0x08) {       // Local: Usage
            usage = value;
            usageLen = dataLen;
            haveUsage = true;
        } else if (tag == 0xA0) {       // Main: Collection -- the first one decides
            if (!haveUsage) return "";
            // ⓘ A four-byte usage carries its own page in the upper half.
            const uint32_t page = (usageLen == 4 && usage > 0xFFFF) ? (usage >> 16) : usagePage;
            const uint32_t id = usage & 0xFFFF;
            if (page != 0x01) return "";
            if (id == 0x04 || id == 0x05 || id == 0x08) return "controller";
            if (id == 0x06 || id == 0x07) return "keyboard";
            if (id == 0x02 || id == 0x01) return "mouse";
            return "";
        }
    }
    return "";
}

}  // namespace device_type
