// iso_in_test_tone.inl -- FORK-ONLY (rhoquinn8217/CTM-USBIP).
//
// DIAGNOSTIC ONLY. OFF BY DEFAULT. NOT PART OF MICROPHONE SUPPORT.
//
// WHY THIS EXISTS
//   The inbound audio path is answered by handle_endpoint_in() with a
//   zero-filled buffer. Pacing that reply to real time made Windows treat it
//   as a working capture stream -- the recorder's clock runs and playback
//   works -- but the recording is silence, so a working path and a broken one
//   look identical.
//
//   This fills the same buffer with a generated tone. If a recording made on
//   Windows contains a clear steady tone, everything on the Windows side is
//   proven: the endpoint, the request rate, the pacing, the buffer and the
//   host's capture clock. Any silence after that is a TV-side or transport
//   problem, which is a much smaller place to look.
//
// This fabricates audio no microphone produced. It exists to bisect the path,
// and must be off in any session judging real audio.
//
// HOW TO ARM
//   Set CTM_MIC_TEST_TONE=1 BEFORE starting the listener (read once and
//   cached, same as CTM_USBIP_VERBOSE):
//     CTM_MIC_TEST_TONE=1 CTM_USBIP_VERBOSE=1 ./start-listener-fork.bat
//
// FORMAT
//   Matches the DualSense capture stream: S16_LE, 2 channels, 48000 Hz.
//   Measured from /proc/asound/card2/stream0 on an unrooted C1, and confirmed
//   by Windows reporting the bridged endpoint as "2 channel, 16 bit,
//   48000 Hz". Both 2026-08-02.
//
// LIMITATION: phase is held in one process-wide counter, so with two
// controllers bridged at once both share it and the tone is not continuous on
// either. Acceptable for a diagnostic; do not build on it.
//
// REQUIRED HEADERS (pulled in by main.cpp before this file):
//   <atomic> <cmath> <cstdint> <cstring> <vector>

// 440 Hz: a clear steady pitch, unmistakable against silence.
static constexpr double kIsoInTestToneHz = 440.0;

// About a third of full scale. Visible on a waveform without clipping, and
// quiet enough not to hurt if it reaches a headset.
static constexpr double kIsoInTestToneAmplitude = 10000.0;

static constexpr uint32_t kIsoInTestToneRateHz = 48000u;
static constexpr uint32_t kIsoInTestToneChannels = 2u;

static bool iso_in_test_tone_enabled()
{
    static const bool enabled = []() {
        wchar_t value[16] = {};
        const DWORD len = GetEnvironmentVariableW(L"CTM_MIC_TEST_TONE", value, 16);
        return len != 0 && value[0] != L'0';
    }();
    return enabled;
}

// Running sample position, so the tone is continuous across calls instead of
// restarting every buffer (a restarting tone clicks and reads as broken).
static std::atomic<uint64_t> g_isoInTestTonePhase{0};

// Fill out with byteCount bytes of tone. Returns false when disarmed, in
// which case the caller fills with silence exactly as before.
static bool iso_in_fill_test_tone(std::vector<uint8_t> *out, uint32_t byteCount)
{
    if (!iso_in_test_tone_enabled() || out == nullptr) {
        return false;
    }
    const uint32_t bytesPerFrame = kIsoInTestToneChannels * sizeof(int16_t);
    const uint32_t frames = byteCount / bytesPerFrame;
    out->assign(byteCount, 0);
    if (frames == 0) {
        // Too small for a whole frame. Silence, but still correctly sized.
        return true;
    }

    const uint64_t start = g_isoInTestTonePhase.fetch_add(frames);
    const double step = 2.0 * 3.14159265358979323846 * kIsoInTestToneHz /
                        static_cast<double>(kIsoInTestToneRateHz);

    uint8_t *cursor = out->data();
    for (uint32_t i = 0; i < frames; ++i) {
        const double angle = step * static_cast<double>(start + i);
        const int16_t sample =
            static_cast<int16_t>(kIsoInTestToneAmplitude * std::sin(angle));
        // Same sample in both channels: this is a path test, not a stereo
        // test, and identical channels make a wrong channel count obvious.
        for (uint32_t ch = 0; ch < kIsoInTestToneChannels; ++ch) {
            std::memcpy(cursor, &sample, sizeof(sample));
            cursor += sizeof(sample);
        }
    }
    return true;
}
