// ---------------------------------------------------------------------------
// Haptic gain for the wired ISO audio path.
//
// WHY THIS IS NOT AN OUTPUT-REPORT OVERRIDE: rumble reaches the DualSense by
// two different routes, and they are easy to confuse because they share a
// name.
//
//   * CLASSIC RUMBLE -- a plain intensity level in the 0x02 output report at
//     bytes 3 and 4. Handled in ds5_output_overrides.inl.
//   * AUDIO-BASED HAPTICS -- the fine-grained waveform PS5-native games
//     actually use, streamed as channels 2 and 3 of the 4-channel USB audio
//     stream. Thousands of samples per second. Nothing in any output report
//     describes it, so no report-level override can touch it.
//
// See ds5-output-report-reference.md, "Rumble is split across both paths".
// Scaling one and not the other silently does nothing for most modern games.
//
// SCALES, DOES NOT REPLACE. Haptics are a signal, not a setting: the game
// varies the waveform continuously to express what is happening. A fixed
// value would flatten that into a constant buzz. The configured percentage is
// a multiplier -- 100 leaves the waveform untouched, 50 halves it, 0 silences
// haptics, 200 doubles it. Percentage-as-multiplier follows Ciprian's slider
// design; NO CODE WAS COPIED.
//
// PERFORMANCE: this runs on every audio chunk, ~100x/sec, inside the send
// path. The config lookup is therefore read ONCE and cached for the life of
// the process -- a per-chunk lookup here is exactly the kind of cost that
// resurfaces later as dropouts blamed on something else. The consequence is
// that CHANGING rumble_gain NEEDS A LISTENER RESTART, unlike the other
// settings, which a reseat picks up. Deliberate trade; revisit if it annoys.
//
// BACKWARD COMPATIBILITY: nothing on the wire. No message type, no enum, no
// map key, no default, no change to what is transmitted -- only the amplitude
// of samples already in flight. An unmodified ctm-bridge-webos client cannot
// observe this code existing. Inert unless rumble_gain is configured.
//
// INCLUDE ORDER: must come AFTER config/device_config.inl and BEFORE
// usbip/device.inl (its only caller).
// ---------------------------------------------------------------------------

namespace ctm_haptic_gain {

constexpr size_t kChannels      = 4;
constexpr size_t kBytesPerFrame = kChannels * sizeof(int16_t);
constexpr size_t kHapticChannelFirst = 2;   // 0,1 = speaker/headset; 2,3 = haptics
constexpr int    kGainMax       = 500;      // matches the wireless clamp of 5x

// Read once. -1 means "not configured", and every later call returns
// immediately on it.
inline int gain_percent()
{
    static const int cached = []() {
        const int configured = device_config_int("ds5", "rumble_gain", -1);
        if (configured < 0) {
            return -1;
        }
        const int clamped = configured > kGainMax ? kGainMax : configured;
        std::cout << "haptic gain: scaling audio-based haptics to " << clamped
                  << "%" << std::endl;
        return clamped;
    }();
    return cached;
}

// Cheap gate for the call site: lets the caller skip copying the buffer at all
// when no gain is configured.
inline bool configured()
{
    const int gain = gain_percent();
    return gain >= 0 && gain != 100;
}

// Scales channels 2 and 3 in place, leaving speaker and headset untouched.
inline void apply(std::vector<uint8_t> &data)
{
    const int gain = gain_percent();
    if (gain < 0 || gain == 100) {
        return;   // not configured, or configured to change nothing
    }
    const size_t frames = data.size() / kBytesPerFrame;
    if (frames == 0) {
        return;
    }

    for (size_t f = 0; f < frames; ++f) {
        uint8_t *frameStart = data.data() + f * kBytesPerFrame;
        for (size_t ch = kHapticChannelFirst; ch < kChannels; ++ch) {
            uint8_t *at = frameStart + ch * sizeof(int16_t);
            const int16_t sample = static_cast<int16_t>(
                static_cast<uint16_t>(at[0]) |
                (static_cast<uint16_t>(at[1]) << 8));

            // Widened before multiplying: a loud sample times a gain above 100
            // overflows int16_t, which would wrap a strong pulse into a strong
            // pulse of the opposite sign rather than clipping.
            int32_t scaled = (static_cast<int32_t>(sample) * gain) / 100;
            if (scaled > 32767) scaled = 32767;
            if (scaled < -32768) scaled = -32768;

            const uint16_t out = static_cast<uint16_t>(static_cast<int16_t>(scaled));
            at[0] = static_cast<uint8_t>(out & 0xff);
            at[1] = static_cast<uint8_t>((out >> 8) & 0xff);
        }
    }
}

}  // namespace ctm_haptic_gain
