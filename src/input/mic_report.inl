// Which input reports are a DualSense's microphone audio rather than pad state.
//
// ⭐ A Bluetooth DualSense says what a report holds in the low nibble of byte 1:
// bit 0 that controller state is present, bit 1 that audio is. With the
// microphone streaming it sends audio-only reports, carrying encoded sound
// where the sticks and buttons would be, and those must never reach the map
// (device.inl, on_physical_input, has the measured reason).
//
// ⛔⛔ ONLY A DUALSENSE OR AN EDGE. The test used to be the report's shape alone,
// an id of 0x31 or more with bit 1 of byte 1 set, and that shape is not the
// DualSense's to own. A Switch Pro Controller answers its USB handshake with
// 81 02: id 0x81, byte 1 0x02. Every one of those was dropped here as
// microphone audio, so Steam's 80 02 was never answered and it gave up with
// "Couldn't setup USB mode". Measured on the C1 2026-09-16: the TV wrote every
// 80 02 to the pad and read every 81 02 back, and this side logged
// "[mic] dropped". Any other device with a report id from 0x31 up could be hit
// the same way.
//
// Pure: ids and bytes in, an answer out, so tests/mic_report_test.cpp checks it.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mic_report {

inline bool is_dualsense(uint16_t vid, uint16_t pid)
{
    return vid == 0x054c && (pid == 0x0ce6 || pid == 0x0df2);
}

// True when this input report, from a virtual device with these ids, is
// microphone audio and not pad state.
inline bool is_audio_only(uint16_t vid, uint16_t pid, const uint8_t *data, size_t length)
{
    return is_dualsense(vid, pid) && data != nullptr && length >= 2 && data[0] >= 0x31 &&
           (data[1] & 0x02) != 0;
}

}  // namespace mic_report
