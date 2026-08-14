#include <opus/opus.h>

// --- microphone decode ------------------------------------------------------
//
// ⛔⛔ EXPERIMENTAL BRANCH ONLY -- mic-capture-experimental.
//
// Turns a Bluetooth DualSense's microphone reports into PCM and feeds the ring
// the virtual USB microphone reads from.
//
// ⚠️ NOTHING ON THE STABLE BRANCH ARMS A MICROPHONE, so this would never run
// there -- but it is kept out of it anyway, because the feature it completes
// is not one we are willing to ship yet. See bt-microphone-findings.md.
//
// THE FORMAT, measured rather than assumed (2026-08-13):
//
//   report id     0x31, 78 bytes -- the SAME id and length as pad state
//   byte 1        low nibble: bit 0 = pad state present, bit 1 = audio present
//   byte 2        a counter, increments per frame
//   byte 3..77    the Opus frame, 75 bytes
//   TOC 0xd4      CELT, 10 ms, STEREO
//
// ⛔ The earlier plan said mono, 71 bytes, starting at byte 4. All three were
// wrong. libopus reports two channels from the packet itself.

#define MICDEC_FRAME_AT      3
#define MICDEC_RATE          48000
#define MICDEC_CHANNELS      2
#define MICDEC_MAX_SAMPLES   (48 * 20 * MICDEC_CHANNELS)   // 20 ms of headroom

// One decoder per backend. Opus is a STREAM codec -- frames depend on the ones
// before them -- so a fresh decoder per report would produce noise, and
// sharing one across controllers would interleave two people's voices.
struct MicDecoder {
    const CtmBackend *owner = nullptr;
    OpusDecoder *dec = nullptr;
    unsigned long frames = 0;
    unsigned long failed = 0;
};

static std::mutex g_micDecMutex;
static std::vector<MicDecoder> g_micDecoders;

static OpusDecoder *mic_decoder_for(const CtmBackend *owner)
{
    for (auto &d : g_micDecoders) {
        if (d.owner == owner) return d.dec;
    }
    int err = 0;
    OpusDecoder *dec = opus_decoder_create(MICDEC_RATE, MICDEC_CHANNELS, &err);
    if (dec == nullptr || err != OPUS_OK) {
        std::cout << "[mic-decode] could not create a decoder (err=" << err << ")"
                  << std::endl;
        return nullptr;
    }
    g_micDecoders.push_back(MicDecoder{owner, dec, 0, 0});
    std::cout << "[mic-decode] decoder ready for this controller" << std::endl;
    return dec;
}

// Returns true if the report was microphone audio and has been dealt with.
// ⚠️ The caller must NOT pass it on when this returns true -- everything
// downstream reads reports as sticks and buttons.
static bool mic_decode_report(const CtmBackend *owner,
                              const uint8_t *data, size_t length)
{
    if (owner == nullptr || data == nullptr || length < 4) return false;
    if (data[0] != 0x31) return false;
    if (!(data[1] & 0x02)) return false;          // no audio in this one

    std::lock_guard<std::mutex> lock(g_micDecMutex);
    OpusDecoder *dec = mic_decoder_for(owner);
    if (dec == nullptr) return true;               // still not pad state

    int16_t pcm[MICDEC_MAX_SAMPLES];
    const int n = opus_decode(dec,
                              data + MICDEC_FRAME_AT,
                              (opus_int32)(length - MICDEC_FRAME_AT),
                              pcm,
                              MICDEC_MAX_SAMPLES / MICDEC_CHANNELS,
                              0);

    for (auto &d : g_micDecoders) {
        if (d.owner != owner) continue;
        if (n > 0) ++d.frames; else ++d.failed;
        if (d.frames == 1 || (d.frames % 500) == 0) {
            std::cout << "[mic-decode] " << d.frames << " frame(s) decoded, "
                      << d.failed << " failed; latest " << n << " samples"
                      << std::endl;
        }
        break;
    }

    if (n > 0) {
        const size_t bytes = (size_t)n * MICDEC_CHANNELS * sizeof(int16_t);

        // ⚠️ WHICH POINTER, ONCE. The ring is keyed by the exact backend
        // pointer; a mismatch fills one slot while the reader drains another,
        // and the symptom is frames decoding perfectly into in_bytes=0. That
        // has now happened twice, so print it rather than reason about it.
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::cout << "[mic-decode] pushing " << bytes << " bytes to ring for owner="
                      << (const void *)owner << std::endl;
        }

        mic_ring_push(owner, reinterpret_cast<const uint8_t *>(pcm), bytes);
    }
    return true;
}
