// Pins the pacing arithmetic for inbound audio (microphone) completions.
// See IsoInPacer in src/audio/iso_in_pacing.inl.
//
// WHAT THIS PROTECTS
// Windows free-ran the microphone endpoint at ~28,000 requests/second because
// completions were answered instantly. They are now held for the duration of
// the audio they carry. If duration_us_for_bytes() ever returns 0, the busy
// loop comes straight back with no error and no build failure -- the only
// symptom is a number in a log line nobody is watching.
//
// WHAT THIS CANNOT TEST
// The live rate (ep82_in_hz) needs Windows, a bridged controller and a real
// stream. Only the arithmetic is testable here. Measured live on C1
// 2026-08-02: 27733 -> ~98 requests/second, queue depth 10-11, none refused.
//
// ANCHORED ON MEASUREMENT, NOT PREDICTION
// The original prediction was ~1000 requests/second, i.e. 1 ms of audio per
// request, taken from the endpoint's 1 ms interval. That was WRONG by 10x:
// the host batches about 10 ms into each request. The code survived because
// it derives the hold time from the BYTE COUNT rather than an assumed packet
// rate. Test 1 below pins the measured 10 ms case for exactly that reason.

#include "harness.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/iso_in_pacing.inl"

using namespace ctmtest;

int run_iso_in_pacing_tests(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    // --- Test 1: the measured case ----------------------------------------
    // 1920 bytes = 480 frames x 2 channels x 2 bytes = 10 ms at 48 kHz.
    // This is the size the Windows USB audio driver actually asks for.
    section("a real request holds for the audio it carries");
    {
        CTM_CHECK_EQ(IsoInPacer::duration_us_for_bytes(1920u), 10000u);
    }

    // --- Test 2: the size the prediction assumed --------------------------
    // 192 bytes = 1 ms. Kept so the 1 ms case stays correct even though it is
    // not the one the host uses -- a different host, or a format change, may
    // produce it.
    section("a one-millisecond request holds one millisecond");
    {
        CTM_CHECK_EQ(IsoInPacer::duration_us_for_bytes(192u), 1000u);
    }

    // --- Test 3: nothing asked for, nothing held --------------------------
    // A zero-length completion must not be delayed; delaying it would stall
    // the queue behind a reply that carries no audio at all.
    section("an empty request is not held");
    {
        CTM_CHECK_EQ(IsoInPacer::duration_us_for_bytes(0u), 0u);
    }

    // --- Test 4: THE REGRESSION GUARD -------------------------------------
    // Any non-empty request must hold for at least 1 us. If integer division
    // ever truncates a small request to 0, every completion is answered
    // instantly again and the ~28k/second busy loop returns silently.
    section("a tiny request still holds a nonzero time");
    {
        // One byte is ~5.2 us at 192000 B/s; the ceiling makes it 6, not 1.
        // The property that matters is "never 0" -- the exact value is pinned
        // so a change to the rounding is visible rather than silent.
        CTM_CHECK_EQ(IsoInPacer::duration_us_for_bytes(1u), 6u);
        CTM_CHECK(IsoInPacer::duration_us_for_bytes(2u) > 0u);
        CTM_CHECK(IsoInPacer::duration_us_for_bytes(95u) > 0u);
    }

    // --- Test 5: the clamp ------------------------------------------------
    // An implausibly large request must not park the pacing thread for
    // minutes. 19200 bytes is exactly 100 ms and sits on the ceiling;
    // anything larger is clamped to it rather than scaling on.
    section("an oversized request is clamped, not obeyed");
    {
        CTM_CHECK_EQ(IsoInPacer::duration_us_for_bytes(19200u), 100000u);
        CTM_CHECK_EQ(IsoInPacer::duration_us_for_bytes(1920000u), 100000u);
        CTM_CHECK_EQ(IsoInPacer::duration_us_for_bytes(0xFFFFFFFFu), 100000u);
    }

    // --- Test 6: control. Proves the tests above can fail. -----------------
    // Without this, every check would still pass if the function returned a
    // single constant. Two different inputs must give two different answers,
    // and the larger input must give the larger answer.
    section("control: the hold time actually tracks the byte count");
    {
        const uint32_t small = IsoInPacer::duration_us_for_bytes(192u);
        const uint32_t large = IsoInPacer::duration_us_for_bytes(1920u);
        CTM_CHECK(large > small);
        CTM_CHECK_EQ(large, small * 10u);
    }

    // --- Test 7: the rate constant is the measured one --------------------
    // 2 channels x 2 bytes x 48000 Hz. Read from the device itself:
    // /proc/asound/card2/stream0 on an unrooted C1, and confirmed by Windows
    // reporting the bridged endpoint as "2 channel, 16 bit, 48000 Hz", both
    // 2026-08-02. If the capture format ever becomes configurable, this
    // constant moves to the device config file and this check moves with it.
    section("capture rate constant matches the measured stream");
    {
        CTM_CHECK_EQ(kIsoInBytesPerSecond, 192000u);
    }

    return 0;   // main() in tests_main.cpp reports the combined summary
}
