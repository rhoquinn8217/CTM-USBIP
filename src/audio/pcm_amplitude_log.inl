// ---------------------------------------------------------------------------
// PCM amplitude logging -- T-028, ported under T-056.
//
// PORTED FROM: ctm-ds5-bridge (our pre-fork working copy of this relay; a
// filesystem copy with no shared git history), commits 7c8091b and 19c1965,
// where this lived inline inside CtmUsbipDevice::process_iso_output() in
// src/usbip/device.inl. Kept out of device.inl here on purpose: that file is
// upstream's, and 60 lines of ours in it is 60 lines of future merge conflict.
//
// WHAT IT IS: an instrument, not a diagnosis. Report-settable gain was
// disproven as the lever for wired speaker attenuation, so sample amplitude
// does not explain audible loudness -- do not re-run that experiment. What
// this still provides is proof that audio chunks were flowing and the exact
// timestamp they stopped: the only delivery-continuity instrument we have on
// the Windows side.
//
// BACKWARD COMPATIBILITY: inert unless explicitly armed, and nothing here
// touches the wire -- no message type, no enum value, no map key, no default,
// no change to what send_iso_audio() transmits. An unmodified ctm-bridge-webos
// client cannot observe this code existing.
//
// INCLUDE ORDER: must come AFTER backend/bridge.inl (monotonic_us) and BEFORE
// usbip/device.inl (its only caller).
// ---------------------------------------------------------------------------

namespace ctm_pcm_amp {

// Folder holding the running executable. The pre-fork version wrote to a
// relative path, so the log landed wherever the listener happened to be
// started from; this pins it to one predictable place.
inline std::filesystem::path runtime_dir()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        std::error_code ec;
        return std::filesystem::current_path(ec);
    }
    return std::filesystem::path(buf).parent_path();
}

inline bool armed()
{
    // Environment variable: read once, cannot change mid-process.
    static const bool viaEnv = []() {
        // GetEnvironmentVariableW, not std::getenv: getenv raises C4996 under
        // MSVC and this file should not be the only warning in a clean build.
        // Returns chars copied (or required size), so >0 means set non-empty.
        wchar_t probe[2];
        return GetEnvironmentVariableW(L"CTM_PCM_AMPLITUDE_LOG", probe, 2) > 0;
    }();
    if (viaEnv) {
        return true;
    }

    // Sentinel file: re-checked live so logging can be armed mid-session
    // (deliberate, from 19c1965), but throttled to ~4x/sec. The caller runs
    // ~100x/sec inside the audio send path, and a filesystem probe per chunk
    // there is exactly the kind of cost that resurfaces later as dropouts
    // blamed on something else.
    static std::atomic<uint64_t> nextProbeUs{0};
    static std::atomic<bool> lastResult{false};

    const uint64_t nowUs = monotonic_us();
    if (nowUs >= nextProbeUs.load(std::memory_order_relaxed)) {
        nextProbeUs.store(nowUs + 250000u, std::memory_order_relaxed);
        std::error_code ec;
        const bool present =
            std::filesystem::exists(runtime_dir() / L"pcm_amplitude_log_on", ec);
        lastResult.store(present && !ec, std::memory_order_relaxed);
    }
    return lastResult.load(std::memory_order_relaxed);
}

inline void log_chunk(const std::vector<uint8_t> &data)
{
    static std::ofstream logFile;
    static std::once_flag initFlag;
    std::call_once(initFlag, []() {
        logFile.open(runtime_dir() / L"ctm_pcm_amplitude.log", std::ios::app);
    });
    if (!logFile.is_open()) {
        return;
    }

    constexpr size_t kChannels = 4;
    constexpr size_t kBytesPerFrame = kChannels * sizeof(int16_t);
    const size_t frames = data.size() / kBytesPerFrame;
    if (frames == 0) {
        return;
    }

    // peak widened to int32_t vs. the pre-fork int16_t: negating the most
    // negative int16_t value overflows, so the loudest possible sample used to
    // report a large negative peak.
    int32_t peak = 0;
    int64_t sumSquares = 0;
    size_t sampleCount = 0;

    for (size_t f = 0; f < frames; ++f) {
        const uint8_t *frameStart = data.data() + f * kBytesPerFrame;
        for (size_t ch = 0; ch < 2; ++ch) {
            const int16_t sample = static_cast<int16_t>(
                static_cast<uint16_t>(frameStart[ch * 2]) |
                (static_cast<uint16_t>(frameStart[ch * 2 + 1]) << 8));
            const int32_t absSample = sample < 0
                ? -static_cast<int32_t>(sample)
                : static_cast<int32_t>(sample);
            if (absSample > peak) {
                peak = absSample;
            }
            sumSquares += static_cast<int64_t>(sample) * sample;
            ++sampleCount;
        }
    }

    const double rms = sampleCount > 0
        ? std::sqrt(static_cast<double>(sumSquares) / static_cast<double>(sampleCount))
        : 0.0;

    static std::atomic<uint64_t> writeCounter{0};
    const uint64_t n = writeCounter.fetch_add(1, std::memory_order_relaxed) + 1;

    logFile << "[pcm-amp] n=" << n
            << " ts_us=" << monotonic_us()
            << " frames=" << frames
            << " peak=" << peak
            << " rms=" << std::fixed << std::setprecision(1) << rms
            << std::endl;
}

// Single entry point, so the upstream call site is exactly one line.
inline void maybe_log(const std::vector<uint8_t> &data)
{
    if (armed()) {
        log_chunk(data);
    }
}

}  // namespace ctm_pcm_amp
