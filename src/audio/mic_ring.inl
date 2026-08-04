// mic_ring.inl -- FORK-ONLY (rhoquinn8217/CTM-USBIP).
//
// Holds microphone audio arriving from the TV until the Windows audio driver
// asks for it. ONE RING PER SESSION, keyed by the backend that owns it.
//
// WHY PER SESSION
//   There used to be a single ring for the whole process. With two
//   controllers bridged at once, both TVs pushed into it and both Windows
//   microphone entries drew from it, so each entry received a mixture of both
//   voices. Measured 2026-08-03: in_bytes 384,000/sec (both controllers
//   sending correctly), out_bytes matching, nothing dropped or short -- the
//   plumbing was right and only the ROUTING was wrong.
//
//   The owner is the backend pointer. The bridge reader knows its own `this`
//   when audio arrives; the virtual device knows its backend_ when Windows
//   asks. Same object, so the two ends agree without any new plumbing.
//
// WHY A BUFFER IS NEEDED AT ALL
//   Two clocks that nobody keeps in step. The TV reads the controller's
//   microphone in periods and sends each as it lands; Windows asks on its own
//   schedule. Without something in between, a chunk arriving a millisecond
//   late means the host gets nothing.
//
// THE RULES, MIRRORED FROM THE OUTBOUND DESIGN
//   From iso-audio-jitter-buffer.md, which settled these for audio going the
//   other way. They hold inbound with the roles swapped:
//     - shallow buffer, so added delay stays in the low milliseconds
//     - SHORT REPLY ON UNDERFLOW: hand over what exists and no more. An
//       asynchronous capture endpoint signals its true rate by varying how
//       much it hands over; padding with silence claims a rate we do not
//       have. NEVER make the host wait.
//     - DROP OLDEST ON OVERFLOW: never make the receiver wait.
//
//   The no-blocking rule matters more than it looks. The pop happens on the
//   URB read loop, which also carries button input at 250 Hz. Anything that
//   waits there stalls the controller itself.
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

// A fixed table rather than a map, so this file needs no extra headers and
// cannot allocate on the URB read loop. Four bridged controllers at once is
// already well past anything tested.
static constexpr size_t kMicRingMaxSessions = 4;

struct MicRingSlot {
    const CtmBackend *owner = nullptr;
    std::vector<uint8_t> data;
    uint64_t pushed = 0;        // bytes arrived from the TV
    uint64_t popped = 0;        // bytes handed to Windows
    uint64_t dropped = 0;       // bytes discarded on overflow
    uint64_t shortfall = 0;     // bytes asked for and not supplied
    uint64_t requests = 0;
    uint64_t lastLogUs = 0;
};

static std::mutex g_micRingMutex;
static MicRingSlot g_micRings[kMicRingMaxSessions];

// Caller must hold g_micRingMutex. Returns nullptr only if every slot is
// taken by a different owner.
static MicRingSlot *mic_ring_slot(const CtmBackend *owner, bool create)
{
    for (size_t i = 0; i < kMicRingMaxSessions; ++i) {
        if (g_micRings[i].owner == owner) {
            return &g_micRings[i];
        }
    }
    if (!create) {
        return nullptr;
    }
    for (size_t i = 0; i < kMicRingMaxSessions; ++i) {
        if (g_micRings[i].owner == nullptr) {
            g_micRings[i].owner = owner;
            return &g_micRings[i];
        }
    }
    return nullptr;
}

// Called from the bridge reader thread when a microphone message arrives.
static void mic_ring_push(const CtmBackend *owner, const uint8_t *data, size_t len)
{
    if (owner == nullptr || data == nullptr || len == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_micRingMutex);
    MicRingSlot *slot = mic_ring_slot(owner, true);
    if (slot == nullptr) {
        return;             // more sessions than slots; drop rather than mix
    }
    slot->data.insert(slot->data.end(), data, data + len);
    slot->pushed += len;
    if (slot->data.size() > kMicRingCapacity) {
        // Drop from the FRONT: the newest audio is what someone is waiting to
        // hear. Keeping stale samples would grow the delay permanently, which
        // is worse than a brief gap.
        const size_t excess = slot->data.size() - kMicRingCapacity;
        slot->data.erase(slot->data.begin(), slot->data.begin() + excess);
        slot->dropped += excess;
    }
}

// Called from the URB read loop. Hands over as much as this session's ring
// holds, up to `want`, and NO MORE. NEVER WAITS.
static void mic_ring_pop_fill(const CtmBackend *owner, std::vector<uint8_t> *out, uint32_t want)
{
    if (out == nullptr) {
        return;
    }
    out->clear();
    if (want == 0 || owner == nullptr) {
        return;
    }

    bool report = false;
    uint64_t pushed = 0, popped = 0, dropped = 0, shortfall = 0, requests = 0;
    size_t remaining = 0;
    size_t slotIndex = 0;
    {
        std::lock_guard<std::mutex> lock(g_micRingMutex);
        MicRingSlot *slot = mic_ring_slot(owner, false);
        if (slot == nullptr) {
            return;         // nothing has ever arrived for this session
        }
        slotIndex = static_cast<size_t>(slot - &g_micRings[0]);
        const size_t taken = slot->data.size() < want ? slot->data.size() : want;
        if (taken != 0) {
            out->assign(slot->data.begin(), slot->data.begin() + taken);
            slot->data.erase(slot->data.begin(), slot->data.begin() + taken);
        }
        slot->popped += taken;
        slot->shortfall += (want - taken);
        ++slot->requests;
        remaining = slot->data.size();

        const uint64_t nowUs = monotonic_us();
        if (slot->lastLogUs == 0) {
            slot->lastLogUs = nowUs;
        } else if (nowUs - slot->lastLogUs >= 1000000ULL) {
            slot->lastLogUs = nowUs;
            report = true;
            pushed = slot->pushed;
            popped = slot->popped;
            dropped = slot->dropped;
            shortfall = slot->shortfall;
            requests = slot->requests;
            slot->pushed = 0;
            slot->popped = 0;
            slot->dropped = 0;
            slot->shortfall = 0;
            slot->requests = 0;
        }
    }

    // One line a second PER SESSION, so a two-controller session can be read
    // without arming anything. ⭐ short_bytes near zero with a steady
    // fill_bytes is a healthy stream.
    if (report) {
        std::cout << "mic ring"
                  << " session=" << slotIndex
                  << " in_bytes=" << pushed
                  << " out_bytes=" << popped
                  << " short_bytes=" << shortfall
                  << " dropped_bytes=" << dropped
                  << " requests=" << requests
                  << " fill_bytes=" << remaining
                  << std::endl;
    }
}

// Clear and release this session's slot on teardown, so a new session does
// not start by playing the tail of the old one and the slot can be reused.
static void mic_ring_reset(const CtmBackend *owner)
{
    if (owner == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_micRingMutex);
    MicRingSlot *slot = mic_ring_slot(owner, false);
    if (slot == nullptr) {
        return;
    }
    slot->data.clear();
    slot->data.shrink_to_fit();
    slot->owner = nullptr;
    slot->pushed = 0;
    slot->popped = 0;
    slot->dropped = 0;
    slot->shortfall = 0;
    slot->requests = 0;
    slot->lastLogUs = 0;
}
