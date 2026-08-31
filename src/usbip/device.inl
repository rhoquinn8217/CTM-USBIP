enum class InputReplyKind {
    None,
    Fresh,
    Stale,
    Zero
};

struct CtmSubmitInfo {
    uint8_t endpointAddress = 0;
    bool endpointIso = false;
    bool endpointInterrupt = false;
    InputReplyKind inputReply = InputReplyKind::None;
    uint32_t inputWaitUs = 0;
};

struct CtmAudioStats {
    uint64_t isoEvents = 0;
    uint64_t isoBytes = 0;
    uint64_t inputFrames = 0;
    uint64_t reservoirFrames = 0;
    uint64_t chunksBuilt = 0;
    uint64_t buildFails = 0;
    uint64_t sendFails = 0;
    uint64_t trailingBytes = 0;
    uint64_t reservoirDrops = 0;
    uint64_t reservoirWaits = 0;
    uint32_t reservoirFillMs = 0;
};

class CtmUsbipDevice {
public:
    ~CtmUsbipDevice()
    {
        stop_audio_stream();
    }

    bool load(
        const std::wstring &profilePath,
        const std::wstring &mapPath,
        uint8_t audioLatency,
        bool hasAudioBlockOverride,
        uint8_t audioBlockOverride,
        std::wstring *error)
    {
        CtmDescriptorProfile loadedProfile;
        if (!ctm_load_descriptor_profile(profilePath, &loadedProfile, error)) {
            return false;
        }
        if (!load_map(mapPath, audioLatency, hasAudioBlockOverride, audioBlockOverride, error)) {
            return false;
        }
        if (!set_profile(loadedProfile, error)) {
            return false;
        }
        return true;
    }

    bool load_map(
        const std::wstring &mapPath,
        uint8_t audioLatency,
        bool hasAudioBlockOverride,
        uint8_t audioBlockOverride,
        std::wstring *error)
    {
        if (!map_.load(mapPath, error)) {
            return false;
        }
        map_.set_audio_latency(audioLatency);
        if (hasAudioBlockOverride) {
            map_.set_audio_block_id(audioBlockOverride);
        }
        configure_audio_stream_from_map();
        return true;
    }

    bool set_profile(const CtmDescriptorProfile &profile, std::wstring *error)
    {
        if (profile.device_descriptor.empty() || profile.configuration_descriptor.empty()) {
            if (error) *error = L"descriptor profile is empty";
            return false;
        }
        profile_ = profile;
        info_ = parse_usb_info(profile_);
        return true;
    }

    double bt_audio_pace_ms() const
    {
        return map_.bt_audio_pace_ms();
    }

    uint32_t iso_out_completion_delay_us() const
    {
        const uint32_t delay = map_.iso_out_completion_delay_us();
        return delay == 0 ? 10000 : delay;
    }

    uint32_t iso_out_audio_duration_us(uint8_t endpointAddress, uint32_t byteCount) const
    {
        if (endpointAddress != audioIsoEndpoint_ || audioChannels_ == 0 || audioInputSampleRate_ == 0) {
            return 0;
        }
        const uint32_t bytesPerFrame = static_cast<uint32_t>(audioChannels_) * sizeof(int16_t);
        if (bytesPerFrame == 0) {
            return 0;
        }
        const uint32_t frames = byteCount / bytesPerFrame;
        if (frames == 0) {
            return 0;
        }
        const uint64_t durationUs =
            (static_cast<uint64_t>(frames) * 1000000ULL + audioInputSampleRate_ - 1) /
            audioInputSampleRate_;
        const double scale = map_.iso_out_completion_delay_scale();
        const uint64_t scaledDurationUs = static_cast<uint64_t>(
            static_cast<double>(durationUs) * scale + 0.5);
        return static_cast<uint32_t>((std::min<uint64_t>)(
            (std::max<uint64_t>)(scaledDurationUs, 1ULL),
            100000ULL));
    }

    uint32_t iso_out_audio_frames(uint8_t endpointAddress, uint32_t byteCount) const
    {
        if (endpointAddress != audioIsoEndpoint_ || audioChannels_ == 0) {
            return 0;
        }
        const uint32_t bytesPerFrame = static_cast<uint32_t>(audioChannels_) * sizeof(int16_t);
        return bytesPerFrame == 0 ? 0 : byteCount / bytesPerFrame;
    }

    uint32_t iso_out_expected_rate_hz(uint8_t endpointAddress) const
    {
        if (endpointAddress != audioIsoEndpoint_) {
            return 0;
        }
        return audioInputSampleRate_;
    }

    bool attach_backend(CtmBackend *backend, std::wstring *error)
    {
        backend_ = backend;
        const BackendCaps caps = backend_->caps();
        std::wstring virtualSerial;
        std::wstring requestedSerial = caps.serial;
        // Keep the PHYSICAL serial before any fallback is applied. For a
        // DualSense the TV reports its MAC here, unique per controller and
        // stable across sessions -- that is what per-controller config keys on.
        // ⚠️ Deliberately left EMPTY when the backend gave us nothing: the
        // constant below is shared by every device, so keying on it would apply
        // one controller's settings to another while looking correct.
        {
            std::lock_guard<std::mutex> lock(physicalSerialMutex_);
            physicalSerial_.clear();
            for (wchar_t c : caps.serial) {
                if (c < 128) physicalSerial_.push_back(static_cast<char>(c));
            }
        }
        if (requestedSerial.empty()) {
            requestedSerial = L"CTMUSBIP";
        }
        if (!apply_virtual_serial_to_profile(&profile_, requestedSerial, &virtualSerial, error)) {
            return false;
        }
        info_ = parse_usb_info(profile_);
        device_log::usb_w() << L"virtual USB serial: " << virtualSerial;
        // ⏱️ TIMED. A Bluetooth bridge takes seven seconds against a cable's
        // one, and the TV finishes its whole side in 1.2s -- so the rest is
        // here. Nine feature reports time out on Bluetooth and none on a cable,
        // but shortening the timeout from 2000ms to 500ms did not move the
        // number at all, which means the timeouts are not the bound. These two
        // lines say which part actually is.
        const auto attachT0 = std::chrono::steady_clock::now();
        if (!preload_features(error)) {
            return false;
        }
        const auto attachT1 = std::chrono::steady_clock::now();
        start_audio_stream();
        const auto attachT2 = std::chrono::steady_clock::now();
        device_log::usb_w() << L"attach timing: preload="
                   << std::chrono::duration_cast<std::chrono::milliseconds>(attachT1 - attachT0).count()
                   << L"ms audio_start="
                   << std::chrono::duration_cast<std::chrono::milliseconds>(attachT2 - attachT1).count()
                   << L"ms";
        return true;
    }

    // The physical controller's serial, or empty when the backend reported
    // none. Used only to resolve per-controller config.
    // ⭐ Read LIVE from the backend rather than from a value cached at attach
    // time. attach_backend() runs once per session, but a reconnect can bring a
    // DIFFERENT physical controller into the same session -- and a cached
    // serial would keep reporting the old one, handing the new controller
    // someone else's per-controller config.
    std::string physical_serial() const
    {
        if (backend_ != nullptr) {
            const std::wstring wide = backend_->caps().serial;
            std::string out;
            for (wchar_t c : wide) {
                if (c < 128) out.push_back(static_cast<char>(c));
            }
            if (!out.empty()) return out;
        }
        std::lock_guard<std::mutex> lock(physicalSerialMutex_);
        return physicalSerial_;
    }

    // The per-controller config this device reads, or empty for the shared
    // section. Set by the agent at bridge time and whenever a link changes.
    std::string linked_config() const
    {
        std::lock_guard<std::mutex> lock(physicalSerialMutex_);
        return linkedConfig_;
    }

    void set_linked_config(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(physicalSerialMutex_);
        linkedConfig_ = name;
    }

    void stop()
    {
        // Drop this controller's gyro motion state so a reconnect starts with
        // clean calibration and the per-device map does not grow forever.
        ctm_gyro_mouse::forget_device(this);
        ctm_touch_mouse_forget(this);
        ctm_stick_mouse_forget(this);
        stop_audio_stream();
    }

    CtmAudioStats audio_stats() const
    {
        CtmAudioStats stats;
        stats.isoEvents = audioIsoEvents_.load(std::memory_order_relaxed);
        stats.isoBytes = audioIsoBytes_.load(std::memory_order_relaxed);
        stats.inputFrames = audioInputFrames_.load(std::memory_order_relaxed);
        stats.reservoirFrames = audioReservoirFrames_.load(std::memory_order_relaxed);
        stats.chunksBuilt = audioChunksBuilt_.load(std::memory_order_relaxed);
        stats.buildFails = audioBuildFails_.load(std::memory_order_relaxed);
        stats.sendFails = audioSendFails_.load(std::memory_order_relaxed);
        stats.trailingBytes = audioTrailingBytes_.load(std::memory_order_relaxed);
        stats.reservoirDrops = audioReservoir_.dropOldestEvents.load(std::memory_order_relaxed);
        stats.reservoirWaits = audioReservoir_.consumerWaits.load(std::memory_order_relaxed);
        stats.reservoirFillMs = audioReservoir_.fill_ms();
        return stats;
    }

    // Cheap accessors for the USB/IP server's reservoir-aware ISO-ack pacing.
    uint32_t audio_reservoir_fill_ms() { return audioReservoir_.fill_ms(); }
    uint32_t iso_ack_min_fill_ms()
    {
        std::lock_guard<std::mutex> guard(mapMutex_);
        return map_.iso_ack_min_fill_ms();
    }

    void record_unknown_report(const char *kind, uint8_t reportId)
    {
        std::ostringstream key;
        key << kind << ":0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(reportId);

        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(unknownLogMutex_);
        if (unknownLogLastFlush_.time_since_epoch().count() == 0) {
            unknownLogLastFlush_ = now;
        }
        unknownLogCounts_[key.str()]++;
        if (now - unknownLogLastFlush_ < std::chrono::seconds(5)) {
            return;
        }

        if (ctm_verbose_logs()) device_log::usb_s() << "unknown reports";
        for (const auto &entry : unknownLogCounts_) {
            device_log::usb_s() << " " << entry.first << "=" << entry.second;
        }
        device_log::usb_s() << std::endl;
        unknownLogCounts_.clear();
        unknownLogLastFlush_ = now;
    }

    void log_input31_research(const uint8_t *data, size_t length)
    {
        if (data == nullptr || length == 0 || data[0] != 0x31) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(input31LogMutex_);
        if (input31Baseline_.size() != length) {
            input31Baseline_.assign(data, data + length);
            input31Last_.assign(data, data + length);
            input31LastLog_ = now;
            input31FramesSinceLog_ = 0;
            input31ChangedFramesSinceLog_ = 0;
            input31DeltaCounts_.fill(0);
            if (ctm_verbose_logs()) device_log::usb_s() << "input31 baseline"
                      << " len=" << length
                      << " head=" << hex_span(data, (std::min<size_t>)(length, 32))
                      << std::endl;
            return;
        }

        ++input31FramesSinceLog_;
        bool changed = false;
        const size_t count = (std::min<size_t>)(length, input31DeltaCounts_.size());
        for (size_t i = 0; i < count; ++i) {
            if (i >= input31Last_.size() || input31Last_[i] != data[i]) {
                input31DeltaCounts_[i]++;
                changed = true;
            }
        }
        if (changed) {
            ++input31ChangedFramesSinceLog_;
        }
        input31Last_.assign(data, data + length);

        if (now - input31LastLog_ < std::chrono::seconds(2)) {
            return;
        }

        size_t baselineDiffCount = 0;
        std::ostringstream baselineDiff;
        for (size_t i = 0; i < count; ++i) {
            if (input31Baseline_[i] == data[i]) {
                continue;
            }
            ++baselineDiffCount;
            if (baselineDiffCount <= 24) {
                baselineDiff << " "
                             << std::hex << std::setw(2) << std::setfill('0') << i
                             << ":" << std::setw(2) << static_cast<unsigned int>(input31Baseline_[i])
                             << ">" << std::setw(2) << static_cast<unsigned int>(data[i])
                             << std::dec << std::setfill(' ');
            }
        }

        size_t changedOffsetCount = 0;
        std::ostringstream changedOffsets;
        for (size_t i = 0; i < count; ++i) {
            if (input31DeltaCounts_[i] == 0) {
                continue;
            }
            ++changedOffsetCount;
            if (changedOffsetCount <= 24) {
                changedOffsets << " "
                               << std::hex << std::setw(2) << std::setfill('0') << i
                               << "=" << std::dec << input31DeltaCounts_[i];
            }
        }

        if (ctm_verbose_logs()) device_log::usb_s() << "input31 research"
                  << " frames=" << input31FramesSinceLog_
                  << " changed_frames=" << input31ChangedFramesSinceLog_
                  << " baseline_diff=" << baselineDiffCount << baselineDiff.str()
                  << " changing_offsets=" << changedOffsetCount << changedOffsets.str()
                  << std::endl;

        input31FramesSinceLog_ = 0;
        input31ChangedFramesSinceLog_ = 0;
        input31DeltaCounts_.fill(0);
        input31LastLog_ = now;
    }

    void on_physical_input(const uint8_t *data, size_t length, uint8_t endpoint)
    {
        if (data == nullptr || length == 0) {
            return;
        }
        if (!compInLogged_[endpoint]) {
            compInLogged_[endpoint] = true;
            if (ctm_verbose_logs()) device_log::usb_s() << "composite input first report ep=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(endpoint) << std::dec << std::setfill(' ')
                      << " len=" << length
                      << " head=" << hex_span(data, (std::min<size_t>)(length, 12)) << std::endl;
        }
        log_input31_research(data, length);

        // ⛔ A MICROPHONE REPORT IS NOT PAD STATE. STOP HERE.
        //
        // A Bluetooth DualSense says what a report holds in the low nibble of
        // byte 1: bit 0 that controller state is present, bit 1 that audio is.
        // With the microphone streaming it sends audio-only reports -- bit 1
        // set, bit 0 CLEAR -- carrying encoded sound where the sticks and
        // buttons would be.
        //
        // Everything below hands the bytes to the map runtime, which reads
        // them as pad state. Measured 2026-08-13: letting these through drove
        // the mouse across the desktop continuously and made the machine
        // unusable until the controller was switched off. ⭐ That is upstream's
        // documented reason for deferring microphone support, reproduced.
        //
        // ⚠️ NOTHING IN THIS PROJECT TURNS THE MICROPHONE ON. This guard is
        // here because something else might -- a leftover state from another
        // program, or someone experimenting with the controller directly. It
        // costs nothing when nothing is streaming, and it is the difference
        // between a stray click and an unusable desktop when something is.
        //
        // ⓘ This is also the branch a decoder would live in, if the feature
        // is ever built: the frame starts at byte 3, runs to the end of the
        // report, and is stereo CELT at 10 ms. See bt-microphone-findings.md.
        if (length >= 2 && data[0] >= 0x31 && (data[1] & 0x02)) {
            static unsigned long micDropped = 0;
            ++micDropped;
            if (micDropped == 1 || (micDropped % 500) == 0) {
                device_log::usb_s() << "[mic] dropped " << micDropped
                          << " audio report(s) -- the controller's microphone is"
                          << " streaming and nothing here asked it to"
                          << std::endl;
            }
            return;
        }


        // Composite (puck): each interface's HID report is forwarded verbatim,
        // tagged with its physical IN endpoint (carried in the bridge INPUT
        // message's request_id). Deliver the raw bytes straight to that endpoint
        // with no map translation -- the virtual device IS the physical device,
        // so the map runtime (meant for protocol-translating bridges like
        // DS4-BT->USB) must not reshape it. Per-endpoint delivery
        // (handle_endpoint_in) routes each report to the matching composite child.
        if (endpoint != 0 && !profile_.interface_report_descriptors.empty()) {
            CTM_INPUT_REPORT raw = {};
            raw.length = static_cast<uint16_t>((std::min<size_t>)(length, sizeof(raw.data)));
            memcpy(raw.data, data, raw.length);
            raw.endpoint_address = endpoint;
            enqueue_input_report(raw);
            return;
        }

        CTM_INPUT_REPORT report = {};
        bool logMappedInput = false;
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            if (!map_.translate_controller_input(data, length, &report)) {
                record_unknown_report("input", data[0]);
                if (ctm_verbose_logs()) {
                    if (ctm_verbose_logs()) device_log::usb_s() << "input unmapped report=0x" << std::hex << std::setw(2) << std::setfill('0')
                              << static_cast<unsigned int>(data[0])
                              << std::dec << std::setfill(' ')
                              << " len=" << length
                              << " head=" << hex_span(data, (std::min<size_t>)(length, 24))
                              << std::endl;
                }
                return;
            }
            logMappedInput = map_.log_mapped_input();
        }
        if (logMappedInput) {
            log_mapped_input_debug(data, length, report);
        }
        // Gyro-to-mouse: read the DS5 motion out of the mapped report and feed
        // the synthetic mouse. Never modifies `report` -- the controller passes
        // through untouched; this only pushes a mouse delta into the mailbox.
        // No-op for non-DualSense devices and when no gate is configured.
        ctm_gyro_mouse::on_ds5_input(this, profile_.device_descriptor, linked_config(),
                                     report.data, report.length);

        // Touchpad-to-mouse: same shape as the gyro hook -- read-only, feeds
        // the same synthetic mouse. No-op unless a touchpad_* key is set.
        ctm_touch_mouse_apply(this, profile_.device_descriptor, linked_config(),
                              report.data, report.length);

        // Stick-to-mouse: same shape again -- read-only, same synthetic mouse.
        // No-op unless stick_to_mouse names a stick.
        ctm_stick_mouse_apply(this, profile_.device_descriptor, linked_config(),
                              report.data, report.length);

        // ⭐ Button rebinding. ⚠️ Unlike the gyro hook above, this MODIFIES the
        // report -- a rebound button is cleared before Windows sees it, so the
        // game never receives it. Placed here because `report` is a fresh local
        // that nobody else holds, and because the map has already run, so the
        // bytes are in the virtual device's layout.
        ctm_rebind_apply(this, profile_.device_descriptor, linked_config(),
                         report.data, report.length);

        enqueue_input_report(report);
    }

    void wake_input_waiters()
    {
        inputCv_.notify_all();
    }

    const UsbDeviceInfo &info() const { return info_; }
    bool is_composite() const { return info_.interfaces.size() > 1; }
    const CtmDescriptorProfile &profile() const { return profile_; }
    bool is_iso_endpoint(uint8_t address) const { return endpoint_is_iso(info_, address); }
    bool is_interrupt_endpoint(uint8_t address) const { return endpoint_is_interrupt(info_, address); }
    uint32_t endpoint_interval_us(uint8_t address, uint32_t fallbackUs) const
    {
        return ::endpoint_interval_us(info_, address, fallbackUs);
    }

    int handle_submit(
        uint32_t direction,
        uint32_t ep,
        const uint8_t setup[8],
        const std::vector<uint8_t> &outData,
        uint32_t transferLength,
        std::vector<uint8_t> *inData,
        CtmSubmitInfo *info)
    {
        if (inData == nullptr) {
            return kStatusStall;
        }
        inData->clear();
        const uint8_t endpointAddress = static_cast<uint8_t>((ep & 0x0f) | (direction == kUsbipDirIn ? 0x80 : 0x00));
        if (info != nullptr) {
            info->endpointAddress = endpointAddress;
            info->endpointIso = endpoint_is_iso(info_, endpointAddress);
            info->endpointInterrupt = endpoint_is_interrupt(info_, endpointAddress);
        }
        if ((ep & 0x0f) == 0) {
            return handle_control(setup, outData, transferLength, inData);
        }
        if (direction == kUsbipDirIn) {
            return handle_endpoint_in(endpointAddress, transferLength, inData, info);
        }
        return handle_endpoint_out(endpointAddress, outData);
    }

    int handle_interrupt_in(
        uint8_t endpointAddress,
        uint32_t transferLength,
        std::vector<uint8_t> *inData,
        CtmSubmitInfo *submitInfo,
        const std::atomic_bool *sessionActive)
    {
        if (inData == nullptr || !endpoint_is_interrupt(info_, endpointAddress)) {
            return kStatusStall;
        }
        if (!compPollLogged_[endpointAddress]) {
            compPollLogged_[endpointAddress] = true;
            if (ctm_verbose_logs()) device_log::usb_s() << "interrupt-IN first poll ep=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(endpointAddress) << std::dec << std::setfill(' ') << std::endl;
        }
        if (submitInfo != nullptr) {
            submitInfo->endpointAddress = endpointAddress;
            submitInfo->endpointInterrupt = true;
        }

        using clock = std::chrono::steady_clock;
        const auto waitStart = clock::now();
        std::unique_lock<std::mutex> lock(inputMutex_);
        InputEndpointState &state = inputEndpointStates_[endpointAddress];
        auto pendingForEndpoint = [&]() {
            return std::find_if(
                pendingInputReports_.begin(),
                pendingInputReports_.end(),
                [&](const QueuedInputReport &item) {
                    return item.report.endpoint_address == endpointAddress &&
                        item.sequence > state.deliveredSequence;
                });
        };
        inputCv_.wait(lock, [&]() {
            const bool hasPending = pendingForEndpoint() != pendingInputReports_.end();
            const bool hasLatest =
                hasInput_ &&
                latestInput_.endpoint_address == endpointAddress &&
                inputSequence_ > state.deliveredSequence;
            return g_stop.load() ||
                (sessionActive != nullptr && !sessionActive->load()) ||
                hasPending ||
                hasLatest;
        });
        CTM_INPUT_REPORT report = {};
        uint32_t deliveredSequence = 0;
        auto pendingIt = pendingForEndpoint();
        if (pendingIt != pendingInputReports_.end()) {
            report = pendingIt->report;
            deliveredSequence = pendingIt->sequence;
            pendingInputReports_.erase(pendingIt);
        } else if (hasInput_ &&
            latestInput_.endpoint_address == endpointAddress &&
            inputSequence_ > state.deliveredSequence) {
            report = latestInput_;
            deliveredSequence = inputSequence_;
        } else {
            return kStatusStall;
        }

        const size_t copy = (std::min<size_t>)(report.length, transferLength);
        inData->assign(report.data, report.data + copy);
        state.deliveredSequence = deliveredSequence;
        if (submitInfo != nullptr) {
            submitInfo->inputReply = InputReplyKind::Fresh;
            submitInfo->inputWaitUs = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - waitStart).count());
        }
        return kStatusOk;
    }

    // Public entry point for SYNTHETIC devices whose reports are generated on
    // this side rather than relayed from a bridge -- currently the gyro mouse,
    // whose pump thread produces movement from DualSense motion data. Ordinary
    // bridged devices never call this: their reports arrive through the bridge
    // input path and are enqueued internally.
    //
    // Deliberately a named wrapper rather than making enqueue_input_report()
    // public: it keeps "who may inject reports, and why" explicit at the call
    // site, and leaves the internal enqueue path private as before.
    void inject_synthetic_input(const CTM_INPUT_REPORT &report)
    {
        enqueue_input_report(report);
    }

private:
    using FeatureCacheKey = std::pair<uint8_t, std::vector<uint8_t>>;

    struct QueuedInputReport {
        uint32_t sequence = 0;
        CTM_INPUT_REPORT report = {};
    };

    struct InputEndpointState {
        uint32_t deliveredSequence = 0;
    };

    void configure_audio_stream_from_map()
    {
        audioIsoEndpoint_ = map_.usb_iso_out_endpoint();
        audioInputSampleRate_ = map_.iso_out_sample_rate();
        if (audioInputSampleRate_ == 0) {
            audioInputSampleRate_ = 48000;
        }
        audioReservoirSampleRate_ = map_.reservoir_sample_rate();
        audioChannels_ = map_.iso_channels();
        audioFrameSamples_ = map_.iso_frame_samples();
        audioExpectedLength_ = map_.iso_expected_length();
        audioReservoir_.configure(
            audioReservoirSampleRate_,
            audioChannels_,
            map_.intermediate_buffer_max_ms(),
            map_.intermediate_buffer_warmup_ms());
        if (ctm_verbose_logs()) device_log::usb_s() << "audio pipeline"
                  << " usb_rate=" << audioInputSampleRate_
                  << " reservoir_rate=" << audioReservoirSampleRate_
                  << " channels=" << static_cast<unsigned int>(audioChannels_)
                  << " chunk_frames=" << audioFrameSamples_
                  << " chunk_bytes=" << audioExpectedLength_
                  << " bt_pace_ms=" << std::fixed << std::setprecision(3) << map_.bt_audio_pace_ms()
                  << " builder_pace_ms=" << std::fixed << std::setprecision(3) << map_.audio_builder_pace_ms()
                  << std::defaultfloat
                  << std::endl;
    }

    void enqueue_input_report(const CTM_INPUT_REPORT &report)
    {
        if (report.length == 0 || report.length > sizeof(report.data)) {
            return;
        }
        std::lock_guard<std::mutex> lock(inputMutex_);
        latestInput_ = report;
        hasInput_ = true;
        ++inputSequence_;
        pendingInputReports_.push_back(QueuedInputReport{inputSequence_, report});
        while (pendingInputReports_.size() > 64) {
            pendingInputReports_.pop_front();
        }
        inputCv_.notify_all();
    }

    void queue_virtual_input_reports(const CTM_USB_EVENT &event)
    {
        std::vector<CTM_INPUT_REPORT> reports;
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            if (!map_.build_virtual_input_reports(event, &reports)) {
                return;
            }
        }
        for (const CTM_INPUT_REPORT &report : reports) {
            enqueue_input_report(report);
        }
        if (!reports.empty()) {
            if (ctm_verbose_logs()) device_log::usb_s() << "virtual input queued"
                      << " trigger=" << event.event_type
                      << " count=" << reports.size()
                      << " ep=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(reports.front().endpoint_address)
                      << std::dec << std::setfill(' ')
                      << std::endl;
        }
    }

    void log_mapped_input_debug(
        const uint8_t *source,
        size_t sourceLength,
        const CTM_INPUT_REPORT &report)
    {
        using clock = std::chrono::steady_clock;
        const auto now = clock::now();
        std::lock_guard<std::mutex> lock(mappedInputLogMutex_);
        ++mappedInputFramesSinceLog_;
        if (mappedInputLastLog_.time_since_epoch().count() != 0 &&
            now - mappedInputLastLog_ < std::chrono::milliseconds(500)) {
            return;
        }
        mappedInputLastLog_ = now;
        const uint64_t frames = mappedInputFramesSinceLog_;
        mappedInputFramesSinceLog_ = 0;
        if (ctm_verbose_logs()) device_log::usb_s() << "mapped input"
                  << " frames=" << frames
                  << " src_len=" << sourceLength
                  << " src=" << hex_span(source, (std::min<size_t>)(sourceLength, 24))
                  << " dst_ep=0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned int>(report.endpoint_address)
                  << std::dec << std::setfill(' ')
                  << " dst_len=" << report.length
                  << " dst=" << hex_span(report.data, (std::min<size_t>)(report.length, 48))
                  << std::endl;
    }

    void start_audio_stream()
    {
        std::lock_guard<std::mutex> lock(audioThreadMutex_);
        if (audioThreadRunning_.load(std::memory_order_relaxed) || backend_ == nullptr) {
            return;
        }
        audioThreadRunning_.store(true, std::memory_order_relaxed);
        audioBuilderThread_ = std::thread([this]() { audio_builder_loop(); });
    }

    void stop_audio_stream()
    {
        {
            std::lock_guard<std::mutex> lock(audioThreadMutex_);
            if (!audioThreadRunning_.load(std::memory_order_relaxed)) {
                return;
            }
            audioThreadRunning_.store(false, std::memory_order_relaxed);
            audioReservoir_.request_stop();
        }
        if (audioBuilderThread_.joinable()) {
            audioBuilderThread_.join();
        }
    }

    void audio_builder_loop()
    {
        const size_t chunkSamples =
            static_cast<size_t>(audioFrameSamples_) * static_cast<size_t>(audioChannels_);
        const size_t chunkBytes = chunkSamples * sizeof(int16_t);
        const size_t eventCapacity = sizeof(((CTM_USB_EVENT *)0)->data);
        if (chunkBytes == 0 || chunkBytes > eventCapacity) {
            device_log::usb_s() << "audio issue reason=invalid-chunk"
                      << " chunk_bytes=" << chunkBytes
                      << " event_capacity=" << eventCapacity
                      << std::endl;
            return;
        }

        std::vector<int16_t> chunk(chunkSamples, 0);
        using clock = std::chrono::steady_clock;
        const double builderPaceMs = map_.audio_builder_pace_ms();
        const bool paceBuilder = builderPaceMs > 0.0;
        const auto builderPace = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double, std::milli>(builderPaceMs));
        auto nextBuild = clock::time_point{};
        const bool underrunSilence = map_.underrun_silence();
        const uint32_t chunkMs = (std::max<uint32_t>)(1, static_cast<uint32_t>(
            static_cast<uint64_t>(audioFrameSamples_) * 1000ULL /
            (audioReservoir_.sampleRate == 0 ? 32000 : audioReservoir_.sampleRate)));
        bool streamStarted = false;
        while (audioThreadRunning_.load(std::memory_order_relaxed) && !g_stop.load()) {
            int pulled;
            // A CONTROLLER THAT HAS JUST BEEN BRIDGED HAS NEVER PLAYED
            // ANYTHING, so streamStarted is false and the pull below blocks
            // until real audio arrives -- which means NO stream report is
            // emitted at all, not merely one without an audio block.
            //
            // That is why a confirmation tone from the TV was inaudible on a
            // freshly bridged controller and audible the moment a browser was
            // routed to it: the TV had nothing to write into.
            //
            // During the hold, take the timed path instead. It already fills
            // with silence on timeout and keeps emitting, which is exactly
            // what is wanted -- the keepalive existed, it was just gated
            // behind having started.
            // NEVER PARK IN A BLOCKING PULL.
            //
            // pull() waits on a condition variable until real audio arrives.
            // A thread sitting in there notices nothing else -- not a message,
            // not a shutdown, not a device going away -- because it never
            // reaches the top of the loop again.
            //
            // That cost an evening: a request from the TV to keep the stream
            // alive was arriving while this thread was asleep waiting for
            // audio, and audio was not what it was being told about. The
            // request is gone now, but a thread that sleeps until one specific
            // event is exactly the shape of that bug, so the timed wait stays.
            //
            // It wakes every chunk, and goes straight back round below when
            // there is nothing to send -- so an idle controller still costs no
            // reports.
            const bool stayAwake = true;
            if ((underrunSilence && streamStarted) || stayAwake) {
                pulled = audioReservoir_.pull_timed(audioFrameSamples_, chunk.data(), chunkMs);
            } else {
                pulled = audioReservoir_.pull(audioFrameSamples_, chunk.data()) ? 1 : -1;
            }
            if (pulled < 0) {
                break;
            }
            if (pulled == 0) {
                // Awake only to watch for a hold, with nothing to send and
                // nothing held: go back round rather than emit a report
                // nobody asked for.
                if (stayAwake && !(underrunSilence && streamStarted)) {
                    continue;
                }
                // Keep-alive: hold the pad-side stream continuous through host
                // bursts (per-sound WASAPI open/close) — silence, not a stall.
                std::fill(chunk.begin(), chunk.end(), static_cast<int16_t>(0));
            } else {
                streamStarted = true;
            }

            CTM_USB_EVENT event = {};
            event.event_type = CTM_USB_EVENT_ISO_OUT;
            event.endpoint_address = audioIsoEndpoint_;
            event.length = static_cast<uint16_t>(chunkBytes);
            memcpy(event.data, chunk.data(), chunkBytes);

            std::vector<uint8_t> report;
            bool ok = false;
            {
                std::lock_guard<std::mutex> guard(mapMutex_);
                ok = map_.build_stream_output_report(event, &report);
            }
            if (!ok) {
                audioBuildFails_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            audioChunksBuilt_.fetch_add(1, std::memory_order_relaxed);

            std::wstring error;
            if (!backend_->send_output_report(report, true, &error)) {
                audioSendFails_.fetch_add(1, std::memory_order_relaxed);
                std::wcerr << L"backend ISO output failed: " << error << L"\n";
            }
            std::vector<uint8_t> controllerOutput;
            {
                std::lock_guard<std::mutex> guard(lastControllerOutputMutex_);
                controllerOutput = lastControllerOutput_;
            }
            if (!controllerOutput.empty() && controllerOutput[0] == 0x31) {
                std::wstring outputError;
                if (!backend_->send_output_report(controllerOutput, false, &outputError)) {
                    audioSendFails_.fetch_add(1, std::memory_order_relaxed);
                    std::wcerr << L"backend repeated HID output failed: " << outputError << L"\n";
                }
            }
            if (paceBuilder) {
                const auto now = clock::now();
                if (nextBuild == clock::time_point{} ||
                    now > nextBuild + std::chrono::milliseconds(50)) {
                    nextBuild = now;
                }
                nextBuild += builderPace;
                if (nextBuild > now) {
                    std::this_thread::sleep_until(nextBuild);
                }
            }
        }
    }

    // ⛔⛔ HOW LONG TO WAIT FOR A FEATURE REPORT THE DEVICE MAY NEVER ANSWER.
    //
    // Was 2000ms. Measured on the host 2026-08-18: a Bluetooth DualSense bridge
    // took ~8 seconds against ~2 on a cable, and the log named it twice --
    //
    //   bridge feature issue reason=feature-preload op=get-timeout report=0x0b
    //   bridge feature issue reason=feature-preload op=get-timeout report=0x20
    //
    // -- between "agent bridge starting" and "agent bridge ready". The TV
    // finishes its whole side in 1.2 seconds; the rest was this, waiting twice
    // for answers that were not coming.
    //
    // ⭐ A device that answers, answers fast: this is a HID feature read, and
    // the only thing between us and it is a LAN or an internet round trip.
    // 500ms is comfortably above either and four times shorter than before.
    //
    // ⚠️ Timing out is not a failure. The preload is a CACHE -- it exists so
    // Windows can be served without a round trip later. A miss costs one live
    // fetch on first use; a two-second wait costs everyone eight seconds of
    // staring at nothing.
    static constexpr unsigned int kFeatureProbeTimeoutMs = 500;

    // ⓘ UNUSED WHILE kSkipPreloadProbes IS TRUE. Kept because the number is
    // the thing to revisit if the cache is ever wanted back -- three seconds
    // was measured as too short.
    // ⛔⛔ HOW LONG THE PRELOAD WOULD WAIT BEFORE PROBING.
    //
    // The probes were moved to their own thread so a bridge would not wait five
    // seconds for nine feature reports a Bluetooth DualSense never answers.
    // That fixed the bridge -- eight seconds to two -- and broke something
    // else: they now ran IN PARALLEL with it, nine timeouts over the same
    // Bluetooth link at exactly the moment the TV plays its confirmation tone.
    //
    // ⭐ It showed up in the TV's log as a difference between two events that
    // run the same code:
    //
    //   pattern=1 (unbridge)  600ms for 600ms of audio   -- perfect
    //   pattern=0 (bridge)    15531ms, 5454ms, 5401ms    -- same audio
    //
    // Skipping the probes entirely made every bridge 600ms for 600ms, on
    // repeated attempts with power cycles in between. That confirmed it.
    //
    // ⭐⭐ So they are delayed rather than skipped: the cache is still worth
    // having, and nothing needs it in the first seconds of a session. Three
    // seconds clears the bridge, the tone, and the host's own enumeration.
    //
    // ⚠️ A request arriving before the cache is warm is served live, which is
    // exactly what a cold cache does anyway.
    static constexpr unsigned int kPreloadDelayMs = 3000;
    static constexpr bool kSkipPreloadProbes = true;

    bool preload_features(std::wstring *error)
    {
        if (backend_ == nullptr) {
            if (error) *error = L"backend not attached";
            return false;
        }
        const size_t physicalFeatureLength = (std::max<size_t>)(
            backend_->caps().featureReportLength == 0 ? 64 : backend_->caps().featureReportLength,
            CTM_SHARED_FEATURE_REPORT_BYTES);
        physicalFeatureScratch_.assign(physicalFeatureLength, 0);

        std::vector<std::vector<uint8_t>> connectRequests;
        if (!map_.build_connect_feature_requests(physicalFeatureLength, &connectRequests)) {
            if (error) *error = L"map connect feature request build failed";
            return false;
        }
        connectFeatureRequests_ = connectRequests;

        std::vector<CtmMapRuntime::FeaturePreloadRequest> preloads;
        if (!map_.build_preload_feature_requests(physicalFeatureLength, &preloads)) {
            if (error) *error = L"map preload feature request build failed";
            return false;
        }

        // ⛔⛔ RUN THE PROBES ON A THREAD. THIS IS THE BLUETOOTH BRIDGE DELAY.
        //
        // Measured 2026-08-18: preload=5415ms on a Bluetooth DualSense against
        // ~0 on a cable, and it is nearly all of the seven seconds between
        // pressing bridge and the controller appearing in Windows. The TV
        // finishes its whole side in 1.2 seconds.
        //
        // Nine feature reports time out, every time, on every Bluetooth bridge:
        // 0x20 0x22 0x82 0x83 0xf0 0xf1 0xf2 0xf4 and a set on 0x80. The device
        // does not answer them and will not start.
        //
        // ⭐ THE PRELOAD IS A CACHE, NOT A REQUIREMENT. It exists so Windows can
        // be served a feature report without a round trip. A cold cache costs
        // one live fetch on first use. Waiting for it costs five seconds on
        // EVERY bridge, to learn the same thing every time.
        //
    // ⛔⛔ SKIPPED OUTRIGHT, 2026-08-19. The switch below is off and stays off.
    //
    // Running them on a thread was not enough: they then timed out over the
    // Bluetooth link at the moment the TV was playing its confirmation tone,
    // which broke the tone and took the controller offline.
    //
    // ⚠️ DELAYING THEM WAS TRIED AND FAILED. Three seconds put them in the
    // TAIL of the tone -- a bridge completes around two seconds and the tone
    // runs about 1.2 s after that -- so it was close to the worst number
    // available. A longer head start would probably work, and is not worth
    // finding: a cold cache costs one live fetch on first use, which is what a
    // cache miss costs anyway.
    //
    // ⓘ The early return is INSIDE the thread, so the thread is still created
    // and returns at once. That is deliberate and it is measured: thread
    // creation is not the cost, the probes are.
        //
        // ⓘ Safe on this thread: every write below is under mapMutex_, and
        // featureCache_ is read under the same lock on the serving path. A
        // request that arrives mid-probe simply misses the cache and is served
        // live, which is exactly what a cold cache does anyway.
        std::thread([this, preloads, physicalFeatureLength]() mutable {
            // Nothing below this runs today -- see kSkipPreloadProbes above.
            if (kSkipPreloadProbes) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(kPreloadDelayMs));
            std::vector<uint8_t> scratch(physicalFeatureLength, 0);
            log_feature_probe_requests(connectFeatureRequests_, "connect-feature", kFeatureProbeTimeoutMs);
            preload_probe_loop(preloads, &scratch);
        }).detach();
        return true;
    }

    void preload_probe_loop(std::vector<CtmMapRuntime::FeaturePreloadRequest> &preloads,
                            std::vector<uint8_t> *scratch)
    {
        for (CtmMapRuntime::FeaturePreloadRequest &preload : preloads) {
            const uint8_t *physicalResponse = nullptr;
            size_t physicalResponseLength = 0;
            if (!backend_->execute_feature_actions(
                    preload.actions,
                    scratch,
                    &physicalResponse,
                    &physicalResponseLength,
                    "feature-preload",
                    kFeatureProbeTimeoutMs)) {
                continue;
            }
            CTM_USB_EVENT fakeEvent = {};
            fakeEvent.event_type = CTM_USB_EVENT_FEATURE_GET;
            fakeEvent.request_id = 1;
            fakeEvent.report_id = preload.usbReport;
            CTM_USB_RESPONSE cachedResponse = {};
            std::lock_guard<std::mutex> guard(mapMutex_);
            if (map_.build_feature_response_from_physical(
                    fakeEvent,
                    preload.lastFeatureSet,
                    physicalResponse,
                    physicalResponseLength,
                    &cachedResponse)) {
                cachedResponse.request_id = 0;
                featureCache_[FeatureCacheKey(preload.usbReport, preload.cacheSelector)] = cachedResponse;
            }
        }
    }

    void log_feature_probe_requests(
        const std::vector<std::vector<uint8_t>> &requests,
        const char *reason,
        unsigned int timeoutMs)
    {
        if (backend_ == nullptr) {
            return;
        }
        std::vector<uint8_t> scratch;
        for (const std::vector<uint8_t> &request : requests) {
            if (request.empty()) continue;
            CtmMapRuntime::PhysicalFeatureAction action;
            action.operation = CtmMapRuntime::PhysicalFeatureOperation::GetFeature;
            action.report = request[0];
            action.length = static_cast<uint16_t>(request.size());
            action.payload.assign(request.begin() + 1, request.end());
            std::vector<CtmMapRuntime::PhysicalFeatureAction> actions = { action };
            const uint8_t *response = nullptr;
            size_t responseLength = 0;
            const bool ok = backend_->execute_feature_actions(
                actions,
                &scratch,
                &response,
                &responseLength,
                reason,
                timeoutMs);
            device_log::usb_s() << reason << " get report=0x"
                      << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(action.report)
                      << std::dec << std::setfill(' ')
                      << " ok=" << (ok ? 1 : 0)
                      << " len=" << responseLength;
            if (ok && response != nullptr && responseLength > 0) {
                if (ctm_verbose_logs()) device_log::usb_s() << " head=" << hex_span(response, (std::min<size_t>)(responseLength, 32));
            }
            device_log::usb_s() << std::endl;
        }
    }

    int handle_control(
        const uint8_t setup[8],
        const std::vector<uint8_t> &outData,
        uint32_t transferLength,
        std::vector<uint8_t> *inData)
    {
        (void)transferLength;
        const uint8_t bmRequestType = setup[0];
        const uint8_t request = setup[1];
        const uint16_t value = read_le16(setup + 2);
        const uint16_t index = read_le16(setup + 4);
        const uint16_t requestedLength = read_le16(setup + 6);
        const bool dirIn = (bmRequestType & 0x80) != 0;
        const uint8_t requestType = static_cast<uint8_t>((bmRequestType >> 5) & 0x03);
        const uint8_t recipient = static_cast<uint8_t>(bmRequestType & 0x1f);

        if (requestType == 0 && request == 0x06 && dirIn) {
            const uint8_t descriptorType = static_cast<uint8_t>(value >> 8);
            const uint8_t descriptorIndex = static_cast<uint8_t>(value & 0xff);
            std::vector<uint8_t> descriptor;
            if (descriptorType == 0x01) {
                descriptor.assign(profile_.device_descriptor.begin(), profile_.device_descriptor.end());
            } else if (descriptorType == 0x02) {
                descriptor.assign(profile_.configuration_descriptor.begin(), profile_.configuration_descriptor.end());
            } else if (descriptorType == 0x03) {
                if (descriptorIndex == 0xee && !profile_.microsoft_os_string_descriptor.empty()) {
                    descriptor.assign(
                        profile_.microsoft_os_string_descriptor.begin(),
                        profile_.microsoft_os_string_descriptor.end());
                } else {
                    descriptor = string_descriptor_by_index(profile_, descriptorIndex);
                }
            } else if (descriptorType == 0x21) {
                size_t reportLen = profile_.hid_report_descriptor.size();
                if (!profile_.interface_report_descriptors.empty()) {
                    auto it = profile_.interface_report_descriptors.find(static_cast<uint8_t>(index & 0xff));
                    if (it != profile_.interface_report_descriptors.end()) reportLen = it->second.size();
                }
                descriptor = make_hid_descriptor(reportLen);
            } else if (descriptorType == 0x22) {
                const std::vector<unsigned char> *rd = &profile_.hid_report_descriptor;
                if (!profile_.interface_report_descriptors.empty()) {
                    auto it = profile_.interface_report_descriptors.find(static_cast<uint8_t>(index & 0xff));
                    if (it != profile_.interface_report_descriptors.end()) rd = &it->second;
                }
                descriptor.assign(rd->begin(), rd->end());
            } else if (descriptorType == 0x0f) {
                // BOS: the puck's bcdUSB 0x0201 makes USB 2.01+ hosts request it
                // (wValue 0x0f00). Mirror a 12-byte BOS (USB 2.0 Extension cap) so
                // Windows stops retry-stalling a BOS request during composite
                // enumeration. Harmless for the interrupt-only single-HID profiles.
                descriptor = {0x05, 0x0f, 0x0c, 0x00, 0x01,
                              0x07, 0x10, 0x02, 0x00, 0x00, 0x00, 0x00};
            }
            if (descriptor.empty()) {
                return kStatusStall;
            }
            const size_t copy = (std::min<size_t>)(descriptor.size(), requestedLength);
            inData->assign(descriptor.begin(), descriptor.begin() + static_cast<std::ptrdiff_t>(copy));
            return kStatusOk;
        }

        const uint8_t interfaceNumber = static_cast<uint8_t>(index & 0xff);
        const bool isHidInterfaceRequest =
            requestType == 1 &&
            recipient == 1 &&
            interface_is_class(info_, interfaceNumber, 0x03);

        if (isHidInterfaceRequest) {
            switch (request) {
            case 0x0a: // HID SET_IDLE
                if (dirIn) return kStatusStall;
                hidIdle_ = static_cast<uint8_t>(value >> 8);
                queue_control_state(setup, outData);
                return kStatusOk;
            case 0x0b: // HID SET_PROTOCOL
                if (dirIn) return kStatusStall;
                hidProtocol_ = static_cast<uint8_t>(value & 0xff);
                queue_control_state(setup, outData);
                return kStatusOk;
            case 0x02: // HID GET_IDLE
                if (!dirIn) return kStatusStall;
                inData->push_back(hidIdle_);
                return kStatusOk;
            case 0x03: // HID GET_PROTOCOL
                if (!dirIn) return kStatusStall;
                inData->push_back(hidProtocol_);
                return kStatusOk;
            case 0x09: // HID SET_REPORT
                if (dirIn) return kStatusStall;
                return handle_hid_set_report(value, outData, interfaceNumber);
            case 0x01: // HID GET_REPORT
                if (!dirIn) return kStatusStall;
                return handle_hid_get_report(value, setup, requestedLength, inData, interfaceNumber);
            default:
                break;
            }
        }

        // Composite: ACK class requests to NON-HID interfaces (the CDC COM port's
        // SET/GET_LINE_CODING, SET_CONTROL_LINE_STATE). We don't model the serial
        // function; acking stops usbser from retry-storming. HID interfaces are
        // handled above.
        if (requestType == 1 && recipient == 1 && is_composite() &&
            !interface_is_class(info_, interfaceNumber, 0x03)) {
            if (dirIn) {
                inData->assign((std::min<size_t>)(static_cast<size_t>(requestedLength), (size_t)64), 0);
            }
            return kStatusOk;
        }

        if (requestType == 0) {
            switch (request) {
            case 0x00: // GET_STATUS
                inData->assign(2, 0);
                if (recipient == 0) {
                    (*inData)[0] = 1;
                }
                return kStatusOk;
            case 0x08: // GET_CONFIGURATION
                inData->push_back(configuration_);
                return kStatusOk;
            case 0x09: // SET_CONFIGURATION
                configuration_ = static_cast<uint8_t>(value & 0xff);
                if (ctm_verbose_logs()) device_log::usb_s() << "usb control set_configuration"
                          << " value=" << static_cast<unsigned int>(configuration_)
                          << " setup=" << hex_span(setup, 8)
                          << std::endl;
                queue_control_state(setup, outData);
                return kStatusOk;
            case 0x0a: // GET_INTERFACE
                if (recipient == 1 && (index & 0xff) < interfaceAlternate_.size()) {
                    inData->push_back(interfaceAlternate_[index & 0xff]);
                    return kStatusOk;
                }
                return kStatusStall;
            case 0x0b: // SET_INTERFACE
                if (recipient == 1 && (index & 0xff) < interfaceAlternate_.size()) {
                    interfaceAlternate_[index & 0xff] = static_cast<uint8_t>(value & 0xff);
                    if (ctm_verbose_logs()) device_log::usb_s() << "usb control set_interface"
                              << " interface=" << static_cast<unsigned int>(index & 0xff)
                              << " alt=" << static_cast<unsigned int>(value & 0xff)
                              << " setup=" << hex_span(setup, 8)
                              << std::endl;
                    queue_control_state(setup, outData);
                    return kStatusOk;
                }
                return kStatusStall;
            case 0x01: // CLEAR_FEATURE
            case 0x03: // SET_FEATURE
                return kStatusOk;
            default:
                break;
            }
        }

        if (requestType == 2 &&
            dirIn &&
            profile_.microsoft_os_string_descriptor.size() >= 17 &&
            !profile_.microsoft_os_compatible_id_descriptor.empty()) {
            const uint8_t vendorCode = profile_.microsoft_os_string_descriptor[16];
            if (request == vendorCode && index == 0x0004) {
                const std::vector<unsigned char> &descriptor =
                    profile_.microsoft_os_compatible_id_descriptor;
                const size_t copy = (std::min<size_t>)(descriptor.size(), requestedLength);
                inData->assign(
                    descriptor.begin(),
                    descriptor.begin() + static_cast<std::ptrdiff_t>(copy));
                return kStatusOk;
            }
        }

        CTM_USB_EVENT event = {};
        event.event_type = CTM_USB_EVENT_CONTROL;
        event.request_id = nextRequestId_++;
        event.endpoint_address = 0;
        event.length = static_cast<uint16_t>((std::min<size_t>)(sizeof(event.data), 8 + outData.size()));
        memcpy(event.data, setup, 8);
        if (!outData.empty() && event.length > 8) {
            memcpy(event.data + 8, outData.data(), event.length - 8);
        }
        if (!dirIn || !outData.empty()) {
            if (ctm_verbose_logs()) device_log::usb_s() << "usb control write"
                      << " setup=" << hex_span(setup, 8)
                      << " payload=" << hex_span(outData.data(), (std::min<size_t>)(outData.size(), 32))
                      << std::endl;
        }
        CTM_USB_RESPONSE response = {};
        bool handled = false;
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            handled = map_.build_control_response(event, &response);
        }
        if (!handled || response.status != CTM_USB_RESPONSE_SUCCESS) {
            if (ctm_verbose_logs()) device_log::usb_s() << "usb control unmapped"
                      << " setup=" << hex_span(setup, 8)
                      << " payload=" << hex_span(outData.data(), (std::min<size_t>)(outData.size(), 16))
                      << std::endl;
            return kStatusStall;
        }
        const size_t copy = (std::min<size_t>)(response.length, requestedLength);
        inData->assign(response.data, response.data + copy);
        return kStatusOk;
    }

    void queue_control_state(const uint8_t setup[8], const std::vector<uint8_t> &outData)
    {
        CTM_USB_EVENT event = {};
        event.event_type = CTM_USB_EVENT_CONTROL;
        event.length = static_cast<uint16_t>((std::min<size_t>)(sizeof(event.data), 8 + outData.size()));
        memcpy(event.data, setup, 8);
        if (!outData.empty() && event.length > 8) {
            memcpy(event.data + 8, outData.data(), event.length - 8);
        }
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            (void)map_.apply_usb_control_state(event);
        }
        queue_virtual_input_reports(event);
    }

    int handle_hid_set_report(uint16_t value, const std::vector<uint8_t> &payload, uint8_t interfaceNumber)
    {
        const uint8_t reportType = static_cast<uint8_t>(value >> 8);
        const uint8_t reportId = static_cast<uint8_t>(value & 0xff);
        if (payload.empty()) {
            return kStatusOk;
        }
        if (is_composite() && reportType == 0x03 && !map_has_feature_set_rule(reportId)) {
            // Composite identity fallback: forward the feature SET verbatim to
            // the addressed interface's hidraw (no map). Best-effort ack so a
            // physical reject doesn't stall Windows init / Steam's config.
            // ONLY when the map has no set rule/selector for this report —
            // else the map path must run (e.g. record the DS4 0xa0 page
            // selector so the paired 0xa4 GET can match).
            std::vector<uint8_t> raw;
            if (reportId != 0 && payload[0] != reportId) raw.push_back(reportId);
            raw.insert(raw.end(), payload.begin(), payload.end());
            std::vector<uint8_t> reply;
            (void)backend_->remote_interface_feature(interfaceNumber, false, raw, &reply, 250);
            return kStatusOk;
        }
        CTM_USB_EVENT event = {};
        event.event_type = reportType == 0x02 ? CTM_USB_EVENT_HID_OUTPUT :
            (reportType == 0x03 ? CTM_USB_EVENT_FEATURE_SET : CTM_USB_EVENT_CONTROL);
        event.report_id = reportId;
        event.endpoint_address = 0;
        bool prependReportId = reportId != 0 && payload[0] != reportId;
        event.length = static_cast<uint16_t>((std::min<size_t>)(
            sizeof(event.data),
            payload.size() + (prependReportId ? 1 : 0)));
        size_t offset = 0;
        if (prependReportId && event.length != 0) {
            event.data[0] = reportId;
            offset = 1;
        }
        if (event.length > offset) {
            memcpy(event.data + offset, payload.data(), event.length - offset);
        }
        if (event.event_type == CTM_USB_EVENT_HID_OUTPUT) {
            if (ctm_verbose_logs()) device_log::usb_s() << "usb hid set-output"
                      << " report=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(reportId)
                      << std::dec << std::setfill(' ')
                      << " len=" << event.length
                      << " head=" << hex_span(event.data, (std::min<size_t>)(event.length, 32))
                      << std::endl;
        }
        if (event.event_type == CTM_USB_EVENT_FEATURE_SET) {
            lastFeatureSet_.assign(event.data, event.data + event.length);
            if (ctm_verbose_logs()) device_log::usb_s() << "usb feature set"
                      << " report=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(reportId)
                      << std::dec << std::setfill(' ')
                      << " len=" << event.length
                      << " head=" << hex_span(event.data, (std::min<size_t>)(event.length, 32))
                      << std::endl;
            bool handled = false;
            std::vector<CtmMapRuntime::PhysicalFeatureAction> actions;
            const size_t physicalFeatureLength = (std::max<size_t>)(
                backend_->caps().featureReportLength == 0 ? 64 : backend_->caps().featureReportLength,
                CTM_SHARED_FEATURE_REPORT_BYTES);
            {
                std::lock_guard<std::mutex> guard(mapMutex_);
                handled = map_.handle_feature_set(event);
                if (map_.build_physical_feature_actions(event, lastFeatureSet_, physicalFeatureLength, &actions)) {
                    handled = true;
                }
            }
            if (!actions.empty()) {
                const uint8_t *ignoredResponse = nullptr;
                size_t ignoredResponseLength = 0;
                if (!backend_->execute_feature_actions(
                        actions,
                        &physicalFeatureScratch_,
                        &ignoredResponse,
                        &ignoredResponseLength,
                        "feature-set",
                        250)) {
                    return kStatusStall;
                }
            }
            if (!handled) {
                record_unknown_report("feature-set", reportId);
                if (ctm_verbose_logs()) {
                    if (ctm_verbose_logs()) device_log::usb_s() << "feature set unmapped report=0x" << std::hex << std::setw(2) << std::setfill('0')
                              << static_cast<unsigned int>(reportId)
                              << std::dec << std::setfill(' ')
                              << " head=" << hex_span(event.data, (std::min<size_t>)(event.length, 24))
                              << std::endl;
                }
            }
            return kStatusOk;
        }
        if (event.event_type == CTM_USB_EVENT_HID_OUTPUT) {
            return process_hid_output_event(event);
        }
        return kStatusOk;
    }

    bool map_has_feature_get_rule(uint8_t reportId)
    {
        std::lock_guard<std::mutex> guard(mapMutex_);
        return map_.has_feature_get_rule(reportId);
    }

    bool map_has_feature_set_rule(uint8_t reportId)
    {
        std::lock_guard<std::mutex> guard(mapMutex_);
        return map_.has_feature_set_rule(reportId);
    }

    bool map_iso_passthrough_enabled()
    {
        std::lock_guard<std::mutex> guard(mapMutex_);
        return map_.iso_passthrough_enabled();
    }

    int handle_hid_get_report(
        uint16_t value,
        const uint8_t setup[8],
        uint16_t requestedLength,
        std::vector<uint8_t> *inData,
        uint8_t interfaceNumber)
    {
        const uint8_t reportType = static_cast<uint8_t>(value >> 8);
        const uint8_t reportId = static_cast<uint8_t>(value & 0xff);
        if (reportType != 0x03) {
            return kStatusStall;
        }
        if (is_composite() && !map_has_feature_get_rule(reportId)) {
            // Composite identity fallback: serve GET_REPORT from the addressed
            // interface's hidraw (HIDIOCGFEATURE) at the full requested length.
            // The request buffer is the report (id in byte 0) sized to wLength
            // so the TV ioctl reads the whole report, not just the id byte.
            // ONLY when the map has no rule for this report: identity forwarding
            // is wrong whenever USB and physical report ids differ (DS4: USB
            // 0x02/0x12 don't exist on the BT pad) — the map always wins.
            const size_t want = requestedLength ? static_cast<size_t>(requestedLength) : 64;
            std::vector<uint8_t> req(want, 0);
            req[0] = reportId;
            std::vector<uint8_t> reply;
            if (backend_->remote_interface_feature(interfaceNumber, true, req, &reply, 250) && !reply.empty()) {
                const size_t copy = (std::min<size_t>)(reply.size(), want);
                inData->assign(reply.begin(), reply.begin() + static_cast<std::ptrdiff_t>(copy));
                return kStatusOk;
            }
            return kStatusStall;
        }
        CTM_USB_EVENT event = {};
        event.event_type = CTM_USB_EVENT_FEATURE_GET;
        event.request_id = nextRequestId_++;
        event.endpoint_address = 0;
        event.report_id = reportId;
        event.length = 8;
        memcpy(event.data, setup, 8);

        FeatureCacheKey cacheKey;
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            cacheKey = FeatureCacheKey(reportId, map_.cache_selector_for(event, lastFeatureSet_));
        }
        auto cached = featureCache_.find(cacheKey);
        if (cached != featureCache_.end()) {
            const CTM_USB_RESPONSE &response = cached->second;
            const size_t copy = (std::min<size_t>)(response.length, requestedLength);
            inData->assign(response.data, response.data + copy);
            return kStatusOk;
        }

        CTM_USB_RESPONSE staticResponse = {};
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            if (map_.build_static_feature_response(event, lastFeatureSet_, &staticResponse)) {
                if (map_.should_cache_usb_control_response(event)) {
                    CTM_USB_RESPONSE cachedResponse = staticResponse;
                    cachedResponse.request_id = 0;
                    featureCache_[cacheKey] = cachedResponse;
                }
                const size_t copy = (std::min<size_t>)(staticResponse.length, requestedLength);
                inData->assign(staticResponse.data, staticResponse.data + copy);
                return kStatusOk;
            }
        }

        const size_t physicalFeatureLength = (std::max<size_t>)(
            backend_->caps().featureReportLength == 0 ? 64 : backend_->caps().featureReportLength,
            CTM_SHARED_FEATURE_REPORT_BYTES);
        std::vector<CtmMapRuntime::PhysicalFeatureAction> actions;
        bool handled = false;
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            handled = map_.build_physical_feature_actions(event, lastFeatureSet_, physicalFeatureLength, &actions);
        }
        if (!handled) {
            record_unknown_report("feature-get", reportId);
            if (ctm_verbose_logs()) {
                device_log::usb_s() << "feature get unmapped report=0x" << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned int>(reportId)
                          << std::dec << std::setfill(' ') << std::endl;
            }
            CtmMapRuntime::FeatureGetMissDiagnostic diagnostic;
            {
                std::lock_guard<std::mutex> guard(mapMutex_);
                if (map_.feature_get_miss_diagnostic(
                        event,
                        lastFeatureSet_,
                        physicalFeatureLength,
                        &diagnostic)) {
                    if (ctm_verbose_logs()) {
                        log_feature_get_unmapped_detail(event, lastFeatureSet_, diagnostic);
                    }
                }
            }
            return kStatusStall;
        }
        const uint8_t *physicalResponse = nullptr;
        size_t physicalResponseLength = 0;
        if (!backend_->execute_feature_actions(
                actions,
                &physicalFeatureScratch_,
                &physicalResponse,
                &physicalResponseLength,
                "feature-on-demand",
                250)) {
            return kStatusStall;
        }
        CTM_USB_RESPONSE response = {};
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            handled = map_.build_feature_response_from_physical(
                event,
                lastFeatureSet_,
                physicalResponse,
                physicalResponseLength,
                &response);
            if (handled && map_.should_cache_usb_control_response(event)) {
                CTM_USB_RESPONSE cachedResponse = response;
                cachedResponse.request_id = 0;
                featureCache_[cacheKey] = cachedResponse;
            }
        }
        if (!handled || response.status != CTM_USB_RESPONSE_SUCCESS) {
            CtmMapRuntime::FeatureResponseExpectation expectation;
            bool hasExpectation = false;
            {
                std::lock_guard<std::mutex> guard(mapMutex_);
                hasExpectation = map_.feature_response_expectation(event, lastFeatureSet_, &expectation);
            }
            if (hasExpectation) {
                log_feature_get_response_rejected(
                    event,
                    lastFeatureSet_,
                    expectation,
                    physicalResponse,
                    physicalResponseLength);
            }
            return kStatusStall;
        }
        const size_t copy = (std::min<size_t>)(response.length, requestedLength);
        inData->assign(response.data, response.data + copy);
        return kStatusOk;
    }

    int handle_endpoint_out(uint8_t endpointAddress, const std::vector<uint8_t> &data)
    {
        CTM_USB_EVENT event = {};
        event.event_type = endpoint_is_iso(info_, endpointAddress) ? CTM_USB_EVENT_ISO_OUT : CTM_USB_EVENT_HID_OUTPUT;
        event.endpoint_address = endpointAddress;
        if (event.event_type == CTM_USB_EVENT_ISO_OUT) {
            return map_iso_passthrough_enabled()
                ? process_iso_output_wired(endpointAddress, data)
                : process_iso_output(endpointAddress, data);
        }
        event.length = static_cast<uint16_t>((std::min<size_t>)(sizeof(event.data), data.size()));
        if (event.length != 0) {
            memcpy(event.data, data.data(), event.length);
        }
        ds5_apply_output_overrides(event.data, event.length, profile_.device_descriptor,
                                   linked_config());
        // ⛔ EVERY OUTPUT REPORT, at roughly 250 a second. A game drives the
        // triggers and rumble continuously, so this buried the handful of lines
        // a person actually needs -- and scrolled the console fast enough that
        // "controller bridged" was gone before it could be read.
        if (ctm_verbose_logs()) device_log::usb_s() << "usb endpoint out"
                  << " ep=0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned int>(endpointAddress)
                  << std::dec << std::setfill(' ')
                  << " len=" << event.length
                  << " head=" << hex_span(event.data, (std::min<size_t>)(event.length, 32))
                  << std::endl;
        return process_hid_output_event(event);
    }

    int process_iso_output(uint8_t endpointAddress, const std::vector<uint8_t> &data)
    {
        if (endpointAddress != audioIsoEndpoint_) {
            return kStatusOk;
        }
        if (audioChannels_ == 0 || audioInputSampleRate_ == 0 || audioReservoirSampleRate_ == 0) {
            return kStatusOk;
        }
        const size_t bytesPerFrame = static_cast<size_t>(audioChannels_) * sizeof(int16_t);
        if (bytesPerFrame == 0) {
            return kStatusOk;
        }
        const size_t frames = data.size() / bytesPerFrame;
        const size_t trailing = data.size() - frames * bytesPerFrame;
        if (trailing != 0) {
            audioTrailingBytes_.fetch_add(trailing, std::memory_order_relaxed);
        }
        if (frames == 0) {
            return kStatusOk;
        }

        std::vector<int16_t> source(frames * audioChannels_);
        for (size_t i = 0; i < source.size(); ++i) {
            source[i] = read_s16_le(data.data() + i * sizeof(int16_t));
        }

        const uint64_t outFrameCap =
            (static_cast<uint64_t>(frames) * audioReservoirSampleRate_ + audioInputSampleRate_ - 1) /
            audioInputSampleRate_ + 1;
        std::vector<int16_t> resampled(static_cast<size_t>(outFrameCap) * audioChannels_, 0);
        const size_t outFrames = ctm_resample_interleaved_linear(
            source.data(),
            frames,
            audioInputSampleRate_,
            audioReservoirSampleRate_,
            audioChannels_,
            resampled.data());
        if (outFrames == 0) {
            return kStatusOk;
        }

        audioReservoir_.push(resampled.data(), outFrames * audioChannels_);
        audioIsoEvents_.fetch_add(1, std::memory_order_relaxed);
        audioIsoBytes_.fetch_add(data.size(), std::memory_order_relaxed);
        audioInputFrames_.fetch_add(frames, std::memory_order_relaxed);
        audioReservoirFrames_.fetch_add(outFrames, std::memory_order_relaxed);
        return kStatusOk;
    }

    // Wired ISO audio: send raw PCM straight to the TV client, bypassing the
    // Opus/reservoir pipeline. The physical controller is a USB audio device on
    // the TV, so the client writes these samples to it directly.
    //
    // Deliberately does NOT touch audioReservoir_ or the audio counters -- those
    // belong to the Bluetooth path, which uses a different audio mechanism at
    // the hardware level.
    //
    // DEPENDS ON UPSTREAM DEFAULTS (verified against 08df624):
    //   ack_min_fill_ms  = 0     -> the reservoir-fill ack gate in server.inl
    //                              can never fire (unsigned < 0 is never true)
    //   underrun_silence = false -> the keep-alive silence lane stays inactive
    // Both come from the map; ds5_usb_over_ds5_usb.map sets neither. If either
    // default changes, or either is added to the wired map, this path changes
    // behaviour with no error and no build failure. Re-check on upstream merges.
    int process_iso_output_wired(uint8_t endpointAddress, const std::vector<uint8_t> &data)
    {
        // Mirrored from process_iso_output() @ 08df624 -- endpoint filter only.
        // Nothing else runs above the divergence point there: the counters and
        // reservoir push all happen after resampling.
        if (endpointAddress != audioIsoEndpoint_) {
            return kStatusOk;
        }
        if (data.empty() || !backend_) {
            return kStatusOk;
        }
        std::wstring err;
        // data is const and owned by the caller, so scaling needs a copy --
        // made only when a gain is configured, so an unconfigured build pays
        // nothing on this ~100/sec path.
        if (ctm_audio_gain::configured()) {
            std::vector<uint8_t> scaled(data);
            ctm_audio_gain::apply(scaled);
            ctm_pcm_amp::maybe_log(scaled);
            backend_->send_iso_audio(scaled, &err);
            return kStatusOk;
        }
        ctm_pcm_amp::maybe_log(data);
        backend_->send_iso_audio(data, &err);
        return kStatusOk;
    }

    int process_hid_output_event(const CTM_USB_EVENT &event)
    {
        // Composite (puck): forward the host's OUT bytes verbatim to the sibling
        // interface that owns this OUT endpoint (tagged via request_id on the
        // wire). No map translation -- the virtual device IS the physical device.
        if (event.endpoint_address != 0 && !profile_.interface_report_descriptors.empty()) {
            std::vector<uint8_t> raw(event.data, event.data + event.length);
            std::wstring epError;
            if (!backend_->send_output_report_ep(raw, event.endpoint_address, false, &epError) &&
                ctm_verbose_logs()) {
                device_log::usb_s() << "composite output forward failed ep=0x" << std::hex
                          << static_cast<unsigned int>(event.endpoint_address)
                          << std::dec << " len=" << event.length << std::endl;
            }
            return kStatusOk;
        }

        queue_virtual_input_reports(event);

        std::vector<uint8_t> output;
        bool ok = false;
        {
            std::lock_guard<std::mutex> guard(mapMutex_);
            ok = map_.translate_controller_output(event, &outputSeq_, &output);
        }
        if (!ok) {
            record_unknown_report("hid-output", event.report_id);
            if (ctm_verbose_logs()) {
                device_log::usb_s() << "hid output unmapped"
                          << " endpoint=0x" << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned int>(event.endpoint_address)
                          << std::dec << std::setfill(' ')
                          << " len=" << event.length << std::endl;
            }
            return kStatusOk;
        }
        if (!output.empty() && output[0] == 0x31) {
            std::lock_guard<std::mutex> guard(lastControllerOutputMutex_);
            lastControllerOutput_ = output;
        }
        std::wstring error;
        if (!backend_->send_output_report(output, false, &error)) {
            // Best-effort delivery: the host's OUT write is valid and must be
            // ACKed even when we cannot forward it to the physical controller.
            // Returning a STALL here makes the Windows host driver (e.g.
            // xboxgip) treat the OUT endpoint as broken and abandon device
            // init, which kills the input pipe entirely. Rumble/feedback is
            // non-essential; never break the virtual device over it.
            if (ctm_verbose_logs()) {
                std::wcerr << L"backend HID output failed (acked to host): " << error << L"\n";
            }
            return kStatusOk;
        }
        return kStatusOk;
    }

    int handle_endpoint_in(
        uint8_t endpointAddress,
        uint32_t transferLength,
        std::vector<uint8_t> *inData,
        CtmSubmitInfo *submitInfo)
    {
        if (endpoint_is_iso(info_, endpointAddress)) {
            if (submitInfo != nullptr) {
                submitInfo->endpointIso = true;
            }
            // Microphone audio. The tone, when armed, overrides everything --
            // it exists to prove the path without a real signal. Otherwise the
            // ring supplies what has arrived from the TV and pads the rest with
            // silence. Neither call ever waits: this is the URB read loop.
            if (!iso_in_fill_test_tone(inData, transferLength)) {
                mic_ring_pop_fill(backend_, inData, transferLength);
            }
            return kStatusOk;
        }

        const bool isInterrupt = endpoint_is_interrupt(info_, endpointAddress);
        if (submitInfo != nullptr) {
            submitInfo->endpointInterrupt = isInterrupt;
        }
        if (isInterrupt) {
            return handle_interrupt_in(endpointAddress, transferLength, inData, submitInfo, nullptr);
        }

        inData->assign((std::min<uint32_t>)(transferLength, 64), 0);
        if (submitInfo != nullptr) {
            submitInfo->inputReply = InputReplyKind::Zero;
            submitInfo->inputWaitUs = 0;
        }
        return kStatusOk;
    }

    CtmDescriptorProfile profile_;
    CtmMapRuntime map_;
    UsbDeviceInfo info_;
    CtmBackend *backend_ = nullptr;
    std::mutex mapMutex_;
    std::mutex inputMutex_;
    std::condition_variable inputCv_;
    bool hasInput_ = false;
    uint32_t inputSequence_ = 0;
    CTM_INPUT_REPORT latestInput_ = {};
    std::deque<QueuedInputReport> pendingInputReports_;
    std::map<uint8_t, InputEndpointState> inputEndpointStates_;
    std::array<bool, 256> compInLogged_ = {};    // diag: first input report seen per endpoint
    std::array<bool, 256> compPollLogged_ = {};  // diag: first interrupt-IN poll seen per endpoint
    uint32_t nextRequestId_ = 1;
    uint8_t hidIdle_ = 0;
    uint8_t hidProtocol_ = 1;
    uint8_t configuration_ = 1;
    std::array<uint8_t, 32> interfaceAlternate_ = {};
    uint8_t outputSeq_ = 0;
    std::mutex lastControllerOutputMutex_;
    std::vector<uint8_t> lastControllerOutput_;
    std::vector<uint8_t> lastFeatureSet_;
    std::vector<uint8_t> physicalFeatureScratch_;
    std::map<FeatureCacheKey, CTM_USB_RESPONSE> featureCache_;
    mutable std::mutex physicalSerialMutex_;
    std::string physicalSerial_;
    std::string linkedConfig_;
    std::mutex unknownLogMutex_;
    std::map<std::string, uint64_t> unknownLogCounts_;
    std::chrono::steady_clock::time_point unknownLogLastFlush_;
    std::mutex input31LogMutex_;
    std::vector<uint8_t> input31Baseline_;
    std::vector<uint8_t> input31Last_;
    std::array<uint64_t, 512> input31DeltaCounts_ = {};
    std::chrono::steady_clock::time_point input31LastLog_;
    uint64_t input31FramesSinceLog_ = 0;
    uint64_t input31ChangedFramesSinceLog_ = 0;
    std::mutex mappedInputLogMutex_;
    std::chrono::steady_clock::time_point mappedInputLastLog_;
    uint64_t mappedInputFramesSinceLog_ = 0;
    uint8_t audioIsoEndpoint_ = 0x01;
    uint32_t audioInputSampleRate_ = 48000;
    uint32_t audioReservoirSampleRate_ = 48000;
    uint8_t audioChannels_ = 4;
    uint16_t audioFrameSamples_ = 480;
    uint16_t audioExpectedLength_ = 3840;
    CtmPcmReservoir audioReservoir_;
    std::mutex audioThreadMutex_;
    std::thread audioBuilderThread_;
    std::atomic_bool audioThreadRunning_{false};
    std::vector<std::vector<uint8_t>> connectFeatureRequests_;
    std::atomic<uint64_t> audioIsoEvents_{0};
    std::atomic<uint64_t> audioIsoBytes_{0};
    std::atomic<uint64_t> audioInputFrames_{0};
    std::atomic<uint64_t> audioReservoirFrames_{0};
    std::atomic<uint64_t> audioChunksBuilt_{0};
    std::atomic<uint64_t> audioBuildFails_{0};
    std::atomic<uint64_t> audioSendFails_{0};
    std::atomic<uint64_t> audioTrailingBytes_{0};
};
