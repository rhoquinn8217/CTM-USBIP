// Microphone report tests: a DualSense's audio is kept from the map, and no
// other device's report is taken for it.

#include "harness.h"

#include <cstdint>

#include "input/mic_report.inl"

using namespace ctmtest;

int run_mic_report_tests()
{
    const uint16_t sony = 0x054c;
    const uint16_t dualsense = 0x0ce6;
    const uint16_t edge = 0x0df2;

    section("mic reports: a DualSense's audio is dropped");
    {
        const uint8_t audio[] = {0x31, 0x02, 0x00, 0x11, 0x22};
        CTM_CHECK(mic_report::is_audio_only(sony, dualsense, audio, sizeof audio));
        CTM_CHECK(mic_report::is_audio_only(sony, edge, audio, sizeof audio));
        // ⓘ The same test as before for a DualSense: bit 1 alone decides it.
        const uint8_t stateAndAudio[] = {0x31, 0x03, 0x00};
        CTM_CHECK(mic_report::is_audio_only(sony, dualsense, stateAndAudio, sizeof stateAndAudio));
    }

    section("mic reports: a DualSense's pad state is not");
    {
        const uint8_t state[] = {0x31, 0x01, 0x80, 0x80};
        CTM_CHECK(!mic_report::is_audio_only(sony, dualsense, state, sizeof state));
        const uint8_t usb[] = {0x01, 0x82, 0x80, 0x80};
        CTM_CHECK(!mic_report::is_audio_only(sony, dualsense, usb, sizeof usb));
    }

    section("mic reports: another device's report is never audio");
    {
        // ⛔ The fault this exists for (C1, 2026-09-16): a Switch Pro
        // Controller's reply to Steam's 80 02 was dropped, every time.
        const uint16_t nintendo = 0x057e;
        const uint16_t proController = 0x2009;
        const uint8_t handshakeReply[] = {0x81, 0x02, 0x00, 0x00};
        CTM_CHECK(!mic_report::is_audio_only(nintendo, proController, handshakeReply,
                                             sizeof handshakeReply));
        const uint8_t baudReply[] = {0x81, 0x03, 0x00, 0x00};
        CTM_CHECK(!mic_report::is_audio_only(nintendo, proController, baudReply, sizeof baudReply));
        // Its Bluetooth simple report, 0x3F, with a button in bit 1 of byte 1.
        const uint8_t simple[] = {0x3f, 0x02, 0x08, 0x00};
        CTM_CHECK(!mic_report::is_audio_only(nintendo, proController, simple, sizeof simple));
        // A DS4 is Sony too, and not a DualSense.
        const uint16_t ds4 = 0x09cc;
        const uint8_t sonyShape[] = {0x31, 0x02, 0x00};
        CTM_CHECK(!mic_report::is_audio_only(sony, ds4, sonyShape, sizeof sonyShape));
    }

    section("mic reports: nothing to read is not audio");
    {
        const uint8_t one[] = {0x31};
        CTM_CHECK(!mic_report::is_audio_only(sony, dualsense, one, sizeof one));
        CTM_CHECK(!mic_report::is_audio_only(sony, dualsense, nullptr, 8));
    }

    return 0;
}
