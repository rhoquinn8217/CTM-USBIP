// Are two bridge sessions two PARTS OF ONE PHYSICAL DEVICE?
//
// ⭐ THE ANSWER DECIDES WHETHER THEY SHARE A NICKNAME. A keyboard-and-mouse
// dongle arrives as two or three separate bridges, and each used to be named
// on its own: one KMA2 dongle came up as "Kraken" and "Rift", one RONGYUAN
// dongle as "Bean" and "Luna" (both measured 2026-09-19). A word whose whole
// job is to be said out loud across a room cannot name half a device.
//
// ⛔⛔ THIS IS NOT same_controller.inl, AND MUST NEVER BE WIRED INTO IT. That
// one decides which older session to RETIRE, and it works hard to keep the
// parts of one device APART -- a GameSir's pad retired its own keyboard on
// 2026-09-14 because the two share a serial. This one groups precisely what
// that one separates, so handing this answer to the retire path would kill a
// device's siblings every time one of its parts bridged.
//
// ⭐ WHAT ACTUALLY REACHES THE LISTENER, measured 2026-09-19 across three
// multi-part devices: every part of one device arrives with an IDENTICAL
// product string, because the TV sends the USB device's iProduct rather than
// the part's own name. /dev/hidraw1 is "RONGYUAN 2.4G Wireless Device
// Keyboard" in the television's own list and reaches here as "RONGYUAN 2.4G
// Wireless Device" -- the same string its mouse half sends.
//
// ⓘ So the prefix match below is a FALLBACK, not the rule. It was specified
// against the names the TV's list shows ("iFEKER (mouse), iFEKER consumer
// control (keyboard)"), which is where the parts of a device really do get a
// suffix. It costs three lines and covers the day a part's own name crosses.
//
// ⚠️ THE VENDOR AND PRODUCT IDS ARE THE GUARD. Every interface of one device
// carries the same pair, and they are what stops a prefix match joining two
// different models: a "Razer Orochi V2" and a "Razer Orochi V2 Pro" are two
// devices with two ids, however close the names read.
//
// ⓘ ACCEPTED when this was specified (rhoquinn8217): two IDENTICAL devices
// with no serial, bridged at once, will share a nickname. "If the case happens
// where a 2 controller have no serial we can live with those controller being
// the same name." The ordinal stays the handle for every command and log line,
// so nothing that has to be unambiguous becomes ambiguous.

#pragma once

#include <cstdint>
#include <string>

namespace same_device {

// One name is the other, or one is the other plus a suffix naming a part
// ("<device> Consumer Control").
inline bool name_matches(const std::string &a, const std::string &b)
{
    // ⓘ No name is no identity, exactly as no serial is no identity in
    // same_controller.inl: every pre-HELLO session has an empty one, so
    // matching on it would join all of them.
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    const std::string &shorter = a.size() < b.size() ? a : b;
    const std::string &longer = a.size() < b.size() ? b : a;
    if (longer.compare(0, shorter.size(), shorter) != 0) return false;
    // ⚠️ The space is the point: without it "Orochi" matches "Orochimaru".
    return longer[shorter.size()] == ' ';
}

inline bool matches(uint16_t vendorId, uint16_t productId,
                    const std::string &serial, const std::string &product,
                    uint16_t otherVendorId, uint16_t otherProductId,
                    const std::string &otherSerial, const std::string &otherProduct)
{
    // ⛔ Different hardware, whatever the names say.
    if (vendorId != otherVendorId || productId != otherProductId) return false;
    // One device has ONE serial across all its interfaces (T-182: a GameSir's
    // pad and its keyboard both carry 3286967D), so two different serials are
    // two devices. ⓘ A serial against no serial is not a claim of sameness
    // either -- and failing here leaves the old behaviour, two nicknames,
    // which is visible and harmless. Joining two devices wrongly is neither.
    if (serial.empty() != otherSerial.empty()) return false;
    if (!serial.empty() && serial != otherSerial) return false;
    return name_matches(product, otherProduct);
}

} // namespace same_device
