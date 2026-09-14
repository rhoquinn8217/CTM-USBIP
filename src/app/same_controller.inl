// Is a new bridge session the same physical device as an older one?
//
// ⭐ THE ANSWER DECIDES WHETHER THE OLDER SESSION IS RETIRED. A re-bridge that
// arrives before the previous session has finished dying leaves two sessions
// on one pad, and the pad answers the old one (measured 2026-08-27), so
// agent.inl retires the older session when this says yes.
//
// ⛔ THE SERIAL ALONE IS NOT ENOUGH. One USB device can arrive as several
// bridges that share its serial: a GameSir's pad and its keyboard interface
// both carry 3286967D, and on 2026-09-14 bridging the pad retired the keyboard
// 1.5s after it attached. Two devices can share a serial too.
//
// ➡️ So the TV's own node decides as well. A re-bridge of the same device comes
// from the same node on the TV (/dev/hidraw2, /dev/input/event13), while the
// two halves of one device, or two devices, come from different ones. A TV
// that sends no node is matched on the serial alone, as before.

#pragma once

#include <string>

namespace same_controller {

inline bool matches(const std::string &serial, const std::string &tvPath,
                    const std::string &otherSerial, const std::string &otherTvPath)
{
    // ⓘ No serial is no identity: the placeholder a device without one gets
    // is shared by every such device.
    if (serial.empty() || serial != otherSerial) {
        return false;
    }
    if (tvPath.empty() || otherTvPath.empty()) {
        return true;
    }
    return tvPath == otherTvPath;
}

} // namespace same_controller
