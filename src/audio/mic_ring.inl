// mic_ring.inl -- FORK-ONLY (rhoquinn8217/CTM-USBIP).
//
// Holds microphone audio arriving from the TV until the Windows audio driver
// asks for it.
//
// WHY A BUFFER IS NEEDED AT ALL
//   Two clocks that nobody keeps in step. The TV reads the controller's
//   microphone in 10 ms chunks and sends each one as it lands; Windows asks
//   for audio on its own schedule. Without something in between, a chunk
//   arriving a millisecond late means the host gets nothing, and a chunk
//   arriving early has nowhere to go.
//
// THE RULES, MIRRORED FROM THE OUTBOUND DESIGN
//   The outbound jitter design (iso-audio-jitter-buffer.md) settled these for
//   audio going the other way. They hold inbound with the roles swapped:
//     - shallow buffer, so added delay stays in the low milliseconds
//     - SILENCE-FILL ON UNDERFLOW: never make the host wait
//     - DROP-OLDEST ON OVERFLOW: never make the receiver wait
//     - neither side ever blocks on the other
//
//   The no-blocking rule matters more here than it looks. The pop happens on
//   the URB read loop, which also carries button input at 250 Hz. Anything
//   that waits there stalls the controller itself -- the same reason the
//   inbound pacing had to be moved off that thread.
//
// LIMITATION: one ring for the whole process. With two controllers bridged at
// once their microphones would share it and both would be wrong. Acceptable
// while the feature is being brought up; NOT acceptable for release. Fixing it
// means keying the ring by session, which is a bigger change than this file.
//
// REQUIRED HEADERS (pulled in by main.cpp before this file):
//   <atomic> <chrono> <cstdint> <cstring> <mutex> <vector>

// Capture stream: 2 channels x 2 bytes x 48000 Hz = 192000 bytes/second.
// Measured from the device on the TV and confirmed by what Windows reports
// for the bridged endpoint, both 2026-08-02.
static constexpr size_t kMicRingBytesPerSecond = 192000u;

// About 200 ms. Deep enough to ride out a network hiccup, shallow enough that
// the delay between speaking and being heard stays unnoticeable.
static constexpr size_t kMicRingCapacity = kMicRingBytesPerSecond / 5u;

static std::mutex g_micRingMutex;
static std::vector<uint8_t> g_micRing;      // FIFO of raw PCM bytes
static uint64_t g_micRingPushed = 0;        // bytes arrived from the TV
static uint64_t g_micRingPopped = 0;        // bytes handed to Windows
static uint64_t g_micRingDropped = 0;       // bytes discarded on overflow
static uint64_t g_micRingUnderfilled = 0;   // bytes of silence used to make up a short read
static uint64_t g_micRingRequests = 0;
static uint64_t g_micRingLastLogUs = 0;

// Called from the bridge reader thread when a microphone message arrives.
static void mic_ring_push(const uint8_t *data, size_t len)
{
    if (data == nullptr || len == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_micRingMutex);
    g_micRing.insert(g_micRing.end(), data, data + len);
    g_micRingPushed += len;
    if (g_micRing.size() > kMicRingCapacity) {
        // Drop from the FRONT: the newest audio is the audio someone is
        // waiting to hear. Keeping stale samples would grow the delay
        // permanently, which is worse than a brief gap.
        const size_t excess = g_micRing.size() - kMicRingCapacity;
        g_micRing.erase(g_micRing.begin(), g_micRing.begin() + excess);
        g_micRingDropped += excess;
    }
}

// Called from the URB read loop. Fills `out` with exactly `want` bytes, using
// silence for whatever the ring cannot supply. NEVER WAITS.
static void mic_ring_pop_fill(std::vector<uint8_t> *out, uint32_t want)
{
    if (out == nullptr) {
        return;
    }
    out->assign(want, 0);
    if (want == 0) {
        return;
    }
    size_t taken = 0;
    size_t remaining = 0;
    {
        std::lock_guard<std::mutex> lock(g_micRingMutex);
        taken = g_micRing.size() < want ? g_micRing.size() : want;
        if (taken != 0) {
            std::memcpy(out->data(), g_micRing.data(), taken);
            g_micRing.erase(g_micRing.begin(), g_micRing.begin() + taken);
        }
        g_micRingPopped += taken;
        g_micRingUnderfilled += (want - taken);
        ++g_micRingRequests;
        remaining = g_micRing.size();
    }

    // One line a second, so a session can be read without arming anything.
    // Reported: bytes in and out per second, how much silence had to be
    // invented, how much was thrown away, and how full the ring is sitting.
    const uint64_t nowUs = monotonic_us();
    bool report = false;
    uint64_t pushed = 0, popped = 0, dropped = 0, underfilled = 0, requests = 0;
    {
        std::lock_guard<std::mutex> lock(g_micRingMutex);
        if (g_micRingLastLogUs == 0) {
            g_micRingLastLogUs = nowUs;
        } else if (nowUs - g_micRingLastLogUs >= 1000000ULL) {
            g_micRingLastLogUs = nowUs;
            report = true;
            pushed = g_micRingPushed;
            popped = g_micRingPopped;
            dropped = g_micRingDropped;
            underfilled = g_micRingUnderfilled;
            requests = g_micRingRequests;
            g_micRingPushed = 0;
            g_micRingPopped = 0;
            g_micRingDropped = 0;
            g_micRingUnderfilled = 0;
            g_micRingRequests = 0;
        }
    }
    if (report) {
        std::cout << "mic ring"
                  << " in_bytes=" << pushed
                  << " out_bytes=" << popped
                  << " silence_bytes=" << underfilled
                  << " dropped_bytes=" << dropped
                  << " requests=" << requests
                  << " fill_bytes=" << remaining
                  << std::endl;
    }
}

// Clear on session teardown so a new session does not start by playing the
// tail of the old one.
static void mic_ring_reset()
{
    std::lock_guard<std::mutex> lock(g_micRingMutex);
    g_micRing.clear();
    g_micRingPushed = 0;
    g_micRingPopped = 0;
    g_micRingDropped = 0;
    g_micRingUnderfilled = 0;
    g_micRingRequests = 0;
    g_micRingLastLogUs = 0;
}
