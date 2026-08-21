namespace CtmBridgeProtocol {
    constexpr uint32_t kMagic = 0x54424d43u;
    constexpr uint16_t kVersion = 1;
    constexpr uint32_t kFlagOk = 0x00000001u;
    constexpr uint32_t kFlagPaced = 0x00000002u;
    constexpr size_t kMaxPayload = 65536;

    enum MessageType : uint16_t {
        MsgHello = 1,
        MsgHostConfig = 2,
        MsgInputReport = 3,
        MsgOutputReport = 4,
        MsgFeatureGet = 5,
        MsgFeatureReport = 6,
        MsgLog = 7,
        MsgError = 8,
        MsgFeatureSet = 9,
        MsgEnum = 10,           // forwarded composite USB enumeration (puck)
        MsgIsoAudio = 11,       // raw PCM audio: CTM-USBIP -> aurora-tv for wired ISO passthrough
        MsgMicAudio = 12,       // raw PCM audio: aurora-tv -> CTM-USBIP, the controller microphone
    };

#pragma pack(push, 1)
    struct Header {
        uint32_t magic;
        uint16_t version;
        uint16_t type;
        uint32_t flags;
        uint32_t sequence;
        uint64_t timestamp_us;
        uint32_t request_id;
        uint32_t payload_len;
    };

    struct DeviceCaps {
        uint16_t vendor_id;
        uint16_t product_id;
        uint16_t version;
        uint16_t bus;
        uint16_t input_report_len;
        uint16_t output_report_len;
        uint16_t feature_report_len;
        uint16_t flags;
        char path[64];
        char serial[64];
        char product[64];
        char manufacturer[64];
    };

    struct HidDescriptorInfo {
        uint32_t report_descriptor_len;
        uint8_t reserved[28];
    };

    struct HostConfig {
        uint32_t bt_pace_us;
        uint16_t input_report_len;
        uint16_t output_report_len;
        uint16_t feature_report_len;
        uint8_t paced_report_count;
        uint8_t paced_report_ids[16];
        // The DualSense's Bluetooth audio buffer. Higher is smoother, lower is
        // choppier -- measured on hardware. kLatencyUnset leaves the TV's own
        // value alone, so a client that never sets it behaves as before.
        //
        // 0 is a REAL VALUE and deliberately reachable: the point of host
        // control is to find where the audio stops being recoverable, and a
        // sentinel of 0 would put the interesting end of the range out of
        // reach. Taken from the reserved block, so the struct size is
        // unchanged and an older TV simply ignores it.
        uint16_t latency_ms;
        uint8_t reserved[29];
    };
    static constexpr uint16_t kLatencyUnset = 0xFFFFu;
    // Below this the controller has less than one 10ms Opus frame buffered and
    // plays nothing at all. Measured, not assumed. Not enforced -- see
    // configured_audio_latency().
    static constexpr int kLatencySilentBelow = 8;

    // CTMB_MSG_ENUM payload (puck composite): the device's own enumeration,
    // forwarded verbatim by the TV and replayed here. Layout:
    //   [EnumInfo][descriptors blob: descriptors_len][ iface_count x (EnumIface + report_desc) ]
    // full_speed=1 => present the virtual device as full-speed (legal 64B CDC bulk).
    struct EnumInfo {
        uint16_t descriptors_len;
        uint8_t  iface_count;
        uint8_t  full_speed;
        uint8_t  reserved[28];
    };

    struct EnumIface {
        uint8_t  interface_number;
        uint8_t  iface_class;
        uint16_t report_desc_len;
    };
#pragma pack(pop)
}

struct CtmBridgeMessage {
    CtmBridgeProtocol::Header header = {};
    std::vector<uint8_t> payload;
};

static uint64_t monotonic_us()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

class BridgeBackend final : public CtmBackend {
public:
    BridgeBackend(uint16_t port, double btPaceMs)
        : port_(port), btPaceMs_(btPaceMs)
    {
    }

    // Invoked (once, from the reader thread) when the client is gone and the
    // reconnect grace expired, or the idle rule fired; the owner should unplug
    // + reap this session. Must not block on joining this backend's threads —
    // the agent queues the reap and performs it on its own command loop.
    void set_closed_callback(std::function<void()> cb)
    {
        closedCallback_ = std::move(cb);
    }

    // Bounded accept windows, OPT-IN (agent path only): 0 = wait forever, the
    // CLI bridge mode's unchanged behavior.
    void set_session_timeouts(int initialAcceptMs, int reconnectGraceMs)
    {
        initialAcceptTimeoutMs_ = initialAcceptMs;
        reconnectGraceMs_ = reconnectGraceMs;
    }

    // Idle rule, OPT-IN: no input reports for idleMs -> session treated as
    // dead (closed callback fires). gamepadIdleMs is the tightened window the
    // reader applies instead when the HELLO descriptor's top-level collection
    // is a joystick/gamepad — pads chatter constantly, so silence means gone,
    // while a mouse/keyboard may idle legitimately for a long time.
    void set_idle_timeouts(int idleMs, int gamepadIdleMs)
    {
        idleTimeoutMs_ = idleMs;
        gamepadIdleTimeoutMs_ = gamepadIdleMs;
    }

    bool start(RawInputCallback callback, std::wstring *error) override
    {
        callback_ = std::move(callback);
        WSADATA data = {};
        if (!wsaStarted_) {
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
                if (error) *error = wsa_error_message(L"WSAStartup failed");
                return false;
            }
            wsaStarted_ = true;
        }
        listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ == INVALID_SOCKET) {
            if (error) *error = wsa_error_message(L"socket failed");
            return false;
        }
        BOOL reuse = TRUE;
        setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port_);
        if (bind(listenSocket_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            if (error) *error = wsa_error_message(L"bridge bind failed");
            return false;
        }
        if (listen(listenSocket_, 1) == SOCKET_ERROR) {
            if (error) *error = wsa_error_message(L"bridge listen failed");
            return false;
        }
        if (!accept_client(true, error)) {
            return false;
        }

        running_.store(true);
        inputWorker_ = std::thread([this]() { input_worker_loop(); });
        reader_ = std::thread([this]() { reader_loop(); });
        return true;
    }

    void stop() override
    {
        running_.store(false);
        close_client_socket();
        if (listenSocket_ != INVALID_SOCKET) {
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }
        if (reader_.joinable()) {
            reader_.join();
        }
        inputCv_.notify_all();
        if (inputWorker_.joinable()) {
            inputWorker_.join();
        }
        if (wsaStarted_) {
            WSACleanup();
            wsaStarted_ = false;
        }
    }

    BackendCaps caps() const override
    {
        BackendCaps caps;
        caps.vendorId = capsRaw_.vendor_id ? capsRaw_.vendor_id : 0x054c;
        caps.productId = capsRaw_.product_id ? capsRaw_.product_id : 0x0ce6;
        caps.version = capsRaw_.version ? capsRaw_.version : 0x0100;
        caps.inputReportLength = capsRaw_.input_report_len ? capsRaw_.input_report_len : 64;
        caps.outputReportLength = capsRaw_.output_report_len ? capsRaw_.output_report_len : 64;
        caps.featureReportLength = capsRaw_.feature_report_len ? capsRaw_.feature_report_len : 64;
        // ⚠️ Guarded: unlike every other field here, the serial is now rewritten
        // on RECONNECT (a different controller can arrive in the same session),
        // so a reader on another thread can meet a write in progress. The rest
        // of capsRaw_ is still written once, before any reader exists.
        {
            std::lock_guard<std::mutex> lock(serialMutex_);
            caps.serial = widen_ascii(capsRaw_.serial, sizeof(capsRaw_.serial));
        }
        caps.product = widen_ascii(capsRaw_.product, sizeof(capsRaw_.product));
        caps.path = widen_ascii(capsRaw_.path, sizeof(capsRaw_.path));
        caps.hidReportDescriptor = hidReportDescriptor_;
        return caps;
    }

    const std::vector<uint8_t> &enum_payload() const override { return enumPayload_; }

    bool execute_feature_actions(
        const std::vector<CtmMapRuntime::PhysicalFeatureAction> &actions,
        std::vector<uint8_t> *scratch,
        const uint8_t **lastGetResponse,
        size_t *lastGetResponseLength,
        const char *reason,
        unsigned int timeoutMs) override
    {
        if (scratch == nullptr || lastGetResponse == nullptr || lastGetResponseLength == nullptr || actions.empty()) {
            return false;
        }
        *lastGetResponse = nullptr;
        *lastGetResponseLength = 0;
        for (const auto &action : actions) {
            if (action.length == 0 || 1 + action.payload.size() > action.length) {
                return false;
            }
            scratch->assign(action.length, 0);
            (*scratch)[0] = action.report;
            if (!action.payload.empty()) {
                memcpy(scratch->data() + 1, action.payload.data(), action.payload.size());
            }
            if (action.operation == CtmMapRuntime::PhysicalFeatureOperation::SetFeature) {
                if (!remote_feature(false, scratch->data(), scratch->size(), timeoutMs, scratch, reason)) {
                    if (action.bestEffort) continue;
                    return false;
                }
            } else {
                if (!remote_feature(true, scratch->data(), scratch->size(), timeoutMs, scratch, reason)) {
                    if (action.bestEffort) continue;
                    return false;
                }
                *lastGetResponse = scratch->data();
                *lastGetResponseLength = scratch->size();
            }
        }
        return *lastGetResponse != nullptr || !actions.empty();
    }

    // Read the configured audio buffer, or the sentinel when the file says
    // nothing. -1 from device_config_int is "absent", which is the case that
    // must leave the TV's own value alone -- NOT a value of zero, which is a
    // legitimate setting we deliberately allow.
    static uint16_t configured_audio_latency()
    {
        const int value = device_config_int("ds5", "audio_latency_ms", -1);
        if (value < 0 || value > 255) {
            return CtmBridgeProtocol::kLatencyUnset;
        }
        // Say so when the value is in the silent range, rather than clamping.
        //
        // Measured on hardware 2026-08-16, walking the whole range with game
        // audio playing: 0 to 7 is SILENT, 8 is white noise with the voice
        // barely there, and it improves steadily from there. 20 is choppy but
        // usable; 60 and 100 are smooth.
        //
        // It is a buffer in milliseconds and an Opus frame is 10ms, so below
        // about 8 there is less than one frame to play.
        //
        // A clamp was considered and rejected. The range was deliberately
        // opened -- the TV's own floor of 20 was removed -- so that the edge
        // could be found at all, and a clamp here would put it back out of
        // reach. Nothing is unrecoverable either: any value recovers by setting
        // a higher one. The failure is confusing, not dangerous, so the answer
        // is to explain it rather than prevent it.
        if (value < CtmBridgeProtocol::kLatencySilentBelow) {
            device_log::config(device_log::msg()
                << "audio_latency_ms=" << value << " is below "
                << CtmBridgeProtocol::kLatencySilentBelow
                << " -- the controller's speaker will be SILENT."
                << " Raise it to recover; 60 is the usual value");
        }
        return static_cast<uint16_t>(value);
    }

    // Re-send the whole host config with a new latency. Cheap -- it is 58 bytes
    // -- and re-sending the lot avoids a second message type that could drift
    // out of step with the first.
    bool send_audio_latency(uint16_t latencyMs, std::wstring *error) override
    {
        CtmBridgeProtocol::HostConfig hostConfig = lastHostConfig_;
        hostConfig.latency_ms = latencyMs;
        if (!send_message(
                CtmBridgeProtocol::MsgHostConfig,
                CtmBridgeProtocol::kFlagOk,
                0,
                reinterpret_cast<const uint8_t *>(&hostConfig),
                sizeof(hostConfig),
                error)) {
            return false;
        }
        lastHostConfig_ = hostConfig;
        return true;
    }

    bool send_output_report(const std::vector<uint8_t> &report, bool paced, std::wstring *error) override
    {
        if (clientSocket_.load() == INVALID_SOCKET) {
            return true;
        }
        if (send_message(
            CtmBridgeProtocol::MsgOutputReport,
            paced ? CtmBridgeProtocol::kFlagPaced : 0,
            0,
            report.data(),
            report.size(),
            error)) {
            return true;
        }
        return clientSocket_.load() == INVALID_SOCKET;
    }

    bool send_output_report_ep(const std::vector<uint8_t> &report, uint8_t endpoint, bool paced, std::wstring *error) override
    {
        if (clientSocket_.load() == INVALID_SOCKET) {
            return true;
        }
        if (send_message(
            CtmBridgeProtocol::MsgOutputReport,
            paced ? CtmBridgeProtocol::kFlagPaced : 0,
            endpoint,
            report.data(),
            report.size(),
            error)) {
            return true;
        }
        return clientSocket_.load() == INVALID_SOCKET;
    }
    bool send_iso_audio(const std::vector<uint8_t> &pcm, std::wstring *error) override
    {
        if (clientSocket_.load() == INVALID_SOCKET) { return true; }
        return send_message(
            CtmBridgeProtocol::MsgIsoAudio,
            0, 0,
            pcm.data(), pcm.size(),
            error);
    }

    // Composite: forward a SET/GET_REPORT verbatim to one interface's hidraw.
    // The interface is encoded in the high byte of request_id; the low 24 bits
    // are the reply correlation id. The TV routes by interface and echoes the id.
    bool remote_interface_feature(uint8_t interface, bool get, const std::vector<uint8_t> &request,
                                  std::vector<uint8_t> *reply, unsigned int timeoutMs) override
    {
        if (request.empty() || reply == nullptr) {
            return false;
        }
        const uint32_t reqId = (static_cast<uint32_t>(interface) << 24) |
                               (nextRequestId_.fetch_add(1) & 0x00ffffffu);
        std::wstring error;
        if (!send_message(
                get ? CtmBridgeProtocol::MsgFeatureGet : CtmBridgeProtocol::MsgFeatureSet,
                0, reqId, request.data(), request.size(), &error)) {
            return false;
        }
        std::unique_lock<std::mutex> lock(featureMutex_);
        const bool signaled = featureCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
            return featureReplies_.find(reqId) != featureReplies_.end() ||
                   g_stop.load() || clientSocket_.load() == INVALID_SOCKET;
        });
        if (!signaled) {
            featureReplies_.erase(reqId);
            return false;
        }
        auto it = featureReplies_.find(reqId);
        if (it == featureReplies_.end()) {
            return false;
        }
        const bool ok = it->second.first;
        if (ok && get) {
            *reply = std::move(it->second.second);
        }
        featureReplies_.erase(it);
        return ok;
    }

private:
    void close_client_socket()
    {
        SOCKET client = clientSocket_.exchange(INVALID_SOCKET);
        if (client != INVALID_SOCKET) {
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
        featureCv_.notify_all();
    }

    bool accept_client(bool initial, std::wstring *error)
    {
        // Bounded wait (opt-in via set_session_timeouts; 0 = wait forever, the
        // CLI behavior): the initial connect must arrive shortly after the TV
        // requested this bridge; a reconnect gets a grace window and then the
        // session declares itself dead (closed callback -> device unplug)
        // instead of parking on the port forever. A parked listener is what
        // produced zombie devices and stole the next plug's handshake.
        const int timeoutMs = initial ? initialAcceptTimeoutMs_ : reconnectGraceMs_;
        const uint64_t deadlineUs = timeoutMs > 0
            ? monotonic_us() + static_cast<uint64_t>(timeoutMs) * 1000u
            : 0;
        for (;;) {
            if (!initial && (!running_.load() || g_stop.load())) {
                return false;
            }
            std::wcout << L"bridge waiting on TCP port " << port_ << L"\n";
            SOCKET pendingOn = listenSocket_;
            if (pendingOn == INVALID_SOCKET) {
                return false;
            }
            // Slice the wait so stop()/g_stop stay responsive and the deadline
            // is enforced even with no incoming connections.
            bool pending = false;
            for (;;) {
                if (!initial && (!running_.load() || g_stop.load())) {
                    return false;
                }
                if (deadlineUs != 0 && monotonic_us() >= deadlineUs) {
                    if (initial) {
                        if (error) *error = L"bridge initial accept timed out";
                    } else {
                        std::wcerr << L"bridge reconnect grace expired on port " << port_ << L"\n";
                    }
                    return false;
                }
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(pendingOn, &fds);
                timeval tv = {};
                tv.tv_usec = 500 * 1000;
                const int sel = select(0, &fds, nullptr, nullptr, &tv);
                if (sel == SOCKET_ERROR) {
                    break;   // fall through to accept for a proper error path
                }
                if (sel > 0) {
                    pending = true;
                    break;
                }
            }
            (void)pending;
            SOCKET client = accept(listenSocket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (!initial && (!running_.load() || g_stop.load())) {
                    return false;
                }
                std::wstring acceptError = wsa_error_message(L"bridge accept failed");
                if (initial) {
                    if (error) *error = acceptError;
                    return false;
                }
                std::wcerr << L"bridge accept failed, retrying: " << acceptError << L"\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            if (!initial && (!running_.load() || g_stop.load())) {
                closesocket(client);
                return false;
            }
            int noDelay = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));
            // Kernel-level "still connected?" probe: a vanished peer (TV power
            // cut leaves the link half-open) fails recv within ~30 s instead
            // of never. Transparent for live idle links — the peer's kernel
            // ACKs without the app's involvement.
            {
                tcp_keepalive keepAlive = {};
                keepAlive.onoff = 1;
                keepAlive.keepalivetime = 10000;
                keepAlive.keepaliveinterval = 2000;
                DWORD keepAliveBytes = 0;
                // Upstream discarded this result with (void). A silent
                // failure leaves the session with NO heartbeat and no indication
                // of it -- which would make "ghosts persist despite the four
                // nets" a false premise. Log only; behaviour is unchanged.
                if (WSAIoctl(client, SIO_KEEPALIVE_VALS, &keepAlive, sizeof(keepAlive),
                             nullptr, 0, &keepAliveBytes, nullptr, nullptr) == SOCKET_ERROR) {
                    std::wcerr << wsa_error_message(L"bridge keepalive enable FAILED") << L"\n";
                } else {
                    std::wcout << L"bridge keepalive enabled time_ms=" << keepAlive.keepalivetime
                               << L" interval_ms=" << keepAlive.keepaliveinterval << L"\n";
                }
            }
            clientSocket_.store(client);

            CtmBridgeMessage hello;
            std::wstring helloError;
            // The TV may send CTMB_MSG_ENUM (composite enumeration) BEFORE HELLO;
            // accept and skip a leading ENUM so the handshake still completes. The
            // composite builder consumes the enumeration in a later stage.
            bool gotHello = false;
            for (int skip = 0; skip < 4; ++skip) {
                if (!recv_message(&hello, &helloError)) break;
                if (hello.header.type == CtmBridgeProtocol::MsgEnum) {
                    std::wcout << L"bridge received composite enumeration ("
                               << hello.payload.size() << L" bytes)\n";
                    if (initial) enumPayload_ = hello.payload;   // stored for the composite builder
                    continue;
                }
                gotHello = (hello.header.type == CtmBridgeProtocol::MsgHello &&
                            hello.payload.size() >= sizeof(CtmBridgeProtocol::DeviceCaps));
                break;
            }
            if (!gotHello) {
                close_client_socket();
                if (initial) {
                    if (error) *error = helloError.empty() ? L"bridge hello failed" : helloError;
                    return false;
                }
                std::wcerr << L"bridge hello failed: "
                           << (helloError.empty() ? L"bad hello" : helloError) << L"\n";
                continue;
            }

            CtmBridgeProtocol::DeviceCaps peerCaps = {};
            memcpy(&peerCaps, hello.payload.data(), sizeof(peerCaps));
            // ⚠️ The SERIAL is refreshed on every connect, not just the first.
            // Everything else stays initial-only, as before -- descriptors and
            // capabilities describe the device type and do not change. But the
            // serial identifies WHICH physical controller is on the other end,
            // and within the reconnect grace a different one can arrive: unplug
            // A, plug in B, and B lands in A's session. Before this, B kept A's
            // serial and therefore A's per-controller config.
            {
                std::lock_guard<std::mutex> lock(serialMutex_);
                memcpy(capsRaw_.serial, peerCaps.serial, sizeof(capsRaw_.serial));
            }
            if (initial) {
                capsRaw_ = peerCaps;
                hidReportDescriptor_.clear();
                if (hello.payload.size() >= sizeof(CtmBridgeProtocol::DeviceCaps) + sizeof(CtmBridgeProtocol::HidDescriptorInfo)) {
                    CtmBridgeProtocol::HidDescriptorInfo descInfo = {};
                    memcpy(
                        &descInfo,
                        hello.payload.data() + sizeof(CtmBridgeProtocol::DeviceCaps),
                        sizeof(descInfo));
                    const size_t descriptorOffset =
                        sizeof(CtmBridgeProtocol::DeviceCaps) + sizeof(CtmBridgeProtocol::HidDescriptorInfo);
                    if (descInfo.report_descriptor_len != 0 &&
                        descInfo.report_descriptor_len <= CtmBridgeProtocol::kMaxPayload &&
                        descriptorOffset + descInfo.report_descriptor_len <= hello.payload.size()) {
                        hidReportDescriptor_.assign(
                            hello.payload.begin() + static_cast<std::ptrdiff_t>(descriptorOffset),
                            hello.payload.begin() + static_cast<std::ptrdiff_t>(descriptorOffset + descInfo.report_descriptor_len));
                    }
                }
            }

            CtmBridgeProtocol::HostConfig hostConfig = {};
            hostConfig.bt_pace_us = static_cast<uint32_t>(btPaceMs_ * 1000.0 + 0.5);
            hostConfig.input_report_len = capsRaw_.input_report_len;
            hostConfig.output_report_len = capsRaw_.output_report_len;
            hostConfig.feature_report_len = capsRaw_.feature_report_len;
            hostConfig.paced_report_count = 2;
            hostConfig.paced_report_ids[0] = 0x36;
            hostConfig.paced_report_ids[1] = 0x15;
            hostConfig.latency_ms = configured_audio_latency();
            std::wstring sendError;
            if (!send_message(
                    CtmBridgeProtocol::MsgHostConfig,
                    CtmBridgeProtocol::kFlagOk,
                    0,
                    reinterpret_cast<const uint8_t *>(&hostConfig),
                    sizeof(hostConfig),
                    &sendError)) {
                close_client_socket();
                if (initial) {
                    if (error) *error = sendError;
                    return false;
                }
                std::wcerr << L"bridge host config failed: " << sendError << L"\n";
                continue;
            }
            lastHostConfig_ = hostConfig;

            std::wcout << L"bridge backend"
                       << (initial ? L"" : L" reconnected")
                       << L" product=" << widen_ascii(peerCaps.product, sizeof(peerCaps.product))
                       << L" serial=" << widen_ascii(peerCaps.serial, sizeof(peerCaps.serial)) << L"\n";
            return true;
        }
    }

    bool send_message(
        uint16_t type,
        uint32_t flags,
        uint32_t requestId,
        const uint8_t *payload,
        size_t payloadLength,
        std::wstring *error)
    {
        if (payloadLength > CtmBridgeProtocol::kMaxPayload) {
            if (error) *error = L"bridge payload too large";
            return false;
        }
        CtmBridgeProtocol::Header header = {};
        header.magic = CtmBridgeProtocol::kMagic;
        header.version = CtmBridgeProtocol::kVersion;
        header.type = type;
        header.flags = flags;
        header.sequence = sendSequence_.fetch_add(1) + 1;
        header.timestamp_us = monotonic_us();
        header.request_id = requestId;
        header.payload_len = static_cast<uint32_t>(payloadLength);
        std::lock_guard<std::mutex> guard(sendMutex_);
        SOCKET s = clientSocket_.load();
        if (s == INVALID_SOCKET) {
            if (error) *error = L"bridge socket closed";
            return false;
        }
        if (!send_all(s, reinterpret_cast<const uint8_t *>(&header), sizeof(header))) {
            if (error) *error = wsa_error_message(L"bridge send header failed");
            close_client_socket();
            return false;
        }
        if (payloadLength != 0 && !send_all(s, payload, payloadLength)) {
            if (error) *error = wsa_error_message(L"bridge send payload failed");
            close_client_socket();
            return false;
        }
        return true;
    }

    bool recv_message(CtmBridgeMessage *message, std::wstring *error)
    {
        if (message == nullptr) return false;
        message->payload.clear();
        SOCKET s = clientSocket_.load();
        if (s == INVALID_SOCKET) {
            if (error) *error = L"bridge socket closed";
            return false;
        }
        if (!recv_all(s, reinterpret_cast<uint8_t *>(&message->header), sizeof(message->header))) {
            if (error) *error = wsa_error_message(L"bridge recv header failed");
            return false;
        }
        if (message->header.magic != CtmBridgeProtocol::kMagic ||
            message->header.version != CtmBridgeProtocol::kVersion ||
            message->header.payload_len > CtmBridgeProtocol::kMaxPayload) {
            if (error) *error = L"bridge protocol header rejected";
            return false;
        }
        if (message->header.payload_len != 0) {
            message->payload.resize(message->header.payload_len);
            if (!recv_all(s, message->payload.data(), message->payload.size())) {
                if (error) *error = wsa_error_message(L"bridge recv payload failed");
                return false;
            }
        }
        return true;
    }

    // Fire the closed callback exactly once (self-exit paths: grace expired,
    // idle rule). External stop() never routes through here.
    void fire_closed_callback()
    {
        if (running_.load() && !g_stop.load() && closedCallback_) {
            std::function<void()> cb = std::move(closedCallback_);
            closedCallback_ = nullptr;
            cb();
        }
    }

    // True when the HELLO report descriptor's first top-level collection is a
    // joystick/gamepad/multi-axis controller (Generic Desktop 0x04/0x05/0x08).
    // Used to tighten the idle window for generic "hid" sessions that are
    // really pads. Only touched from the reader thread (HELLO parse included).
    bool descriptor_is_gamepad() const
    {
        uint32_t usagePage = 0;
        uint32_t lastUsage = 0;
        int depth = 0;
        const std::vector<uint8_t> &d = hidReportDescriptor_;
        for (size_t i = 0; i < d.size();) {
            const uint8_t prefix = d[i];
            if (prefix == 0xFE) {   // long item: skip
                if (i + 2 >= d.size()) break;
                i += 3u + d[i + 1];
                continue;
            }
            const uint8_t sizeCode = prefix & 0x03;
            const size_t dataLen = sizeCode == 3 ? 4 : sizeCode;
            if (i + 1 + dataLen > d.size()) break;
            uint32_t value = 0;
            for (size_t b = 0; b < dataLen; ++b) {
                value |= static_cast<uint32_t>(d[i + 1 + b]) << (8 * b);
            }
            const uint8_t tagType = prefix & 0xFC;
            if (tagType == 0x04) {                       // Global: Usage Page
                usagePage = value;
            } else if (tagType == 0x08 && depth == 0) {  // Local: Usage (top level)
                lastUsage = value;
            } else if (tagType == 0xA0) {                // Main: Collection
                if (depth == 0 && usagePage == 0x01 &&
                    (lastUsage == 0x04 || lastUsage == 0x05 || lastUsage == 0x08)) {
                    return true;
                }
                ++depth;
            } else if (tagType == 0xC0 && depth > 0) {   // Main: End Collection
                --depth;
            }
            i += 1 + dataLen;
        }
        return false;
    }

    int effective_idle_timeout_ms() const
    {
        if (idleTimeoutMs_ <= 0) {
            return 0;
        }
        if (gamepadIdleTimeoutMs_ > 0 && gamepadIdleTimeoutMs_ < idleTimeoutMs_ &&
            descriptor_is_gamepad()) {
            return gamepadIdleTimeoutMs_;
        }
        return idleTimeoutMs_;
    }

    void reader_loop()
    {
        uint64_t lastInputReceiveUs = 0;
        uint64_t idleBasisUs = monotonic_us();
        while (running_.load() && !g_stop.load()) {
            // Idle rule (opt-in): slice the wait so silence is noticed — a
            // plain blocking recv would never return for a live-but-mute peer.
            const int idleMs = effective_idle_timeout_ms();
            if (idleMs > 0) {
                bool readable = false;
                while (running_.load() && !g_stop.load()) {
                    SOCKET s = clientSocket_.load();
                    if (s == INVALID_SOCKET) {
                        break;   // recv_message reports the closed socket
                    }
                    fd_set fds;
                    FD_ZERO(&fds);
                    FD_SET(s, &fds);
                    timeval tv = {};
                    tv.tv_sec = 1;
                    const int sel = select(0, &fds, nullptr, nullptr, &tv);
                    if (sel != 0) {
                        readable = true;   // data or socket error: recv_message decides
                        break;
                    }
                    const uint64_t basisUs = lastInputReceiveUs != 0 ? lastInputReceiveUs : idleBasisUs;
                    if (monotonic_us() - basisUs >= static_cast<uint64_t>(idleMs) * 1000u) {
                        std::wcerr << L"bridge idle timeout (" << idleMs
                                   << L" ms without input) on port " << port_ << L"\n";
                        close_client_socket();
                        fire_closed_callback();
                        return;
                    }
                }
                if (!readable && (!running_.load() || g_stop.load())) {
                    break;
                }
            }
            CtmBridgeMessage message;
            std::wstring error;
            if (!recv_message(&message, &error)) {
                if (running_.load() && !g_stop.load()) {
                    std::wcerr << L"bridge client disconnected: " << error << L"\n";
                }
                close_client_socket();
                // The TV is gone, so whatever microphone audio it sent is from a
                // session that no longer exists. Clearing here stops the next
                // session from opening with the tail of the old one -- seen as
                // ~109 MB dropped in one second at start-up while the ring
                // spilled stale audio nothing was reading.
                mic_ring_reset(this);
                lastInputReceiveUs = 0;
                if (!accept_client(false, nullptr)) {
                    // Self-exit (grace expired), not an external stop(): tell the
                    // agent so the virtual device gets unplugged and the session
                    // reaped instead of lingering half-dead on the port.
                    fire_closed_callback();
                    break;
                }
                idleBasisUs = monotonic_us();
                continue;
            }
            if (message.header.type == CtmBridgeProtocol::MsgInputReport) {
                if (!message.payload.empty()) {
                    const uint64_t receivedUs = monotonic_us();
                    const uint64_t deltaUs = lastInputReceiveUs == 0 ? 0 : receivedUs - lastInputReceiveUs;
                    lastInputReceiveUs = receivedUs;
                    enqueue_input_report(std::move(message.payload),
                                         static_cast<uint8_t>(message.header.request_id),
                                         receivedUs, deltaUs);
                }
            } else if (message.header.type == CtmBridgeProtocol::MsgFeatureReport) {
                const bool ok = (message.header.flags & CtmBridgeProtocol::kFlagOk) != 0;
                {
                    std::lock_guard<std::mutex> lock(featureMutex_);
                    featureReplies_[message.header.request_id] = std::make_pair(ok, std::move(message.payload));
                }
                featureCv_.notify_all();
            } else if (message.header.type == CtmBridgeProtocol::MsgMicAudio) {
                // Microphone audio from the controller. Straight into the ring;
                // the URB loop takes it from there when Windows asks.
                if (!message.payload.empty()) {
                    mic_ring_push(this, message.payload.data(), message.payload.size());
                }
            } else if (message.header.type == CtmBridgeProtocol::MsgLog ||
                       message.header.type == CtmBridgeProtocol::MsgError) {
                std::string text(message.payload.begin(), message.payload.end());
                std::cout << "bridge peer "
                          << (message.header.type == CtmBridgeProtocol::MsgError ? "error " : "log ")
                          << text << std::endl;
            }
        }
    }

    struct QueuedInputReport {
        std::vector<uint8_t> payload;
        uint8_t endpoint = 0;   // physical IN endpoint (composite puck); 0 = default
        uint64_t receivedUs = 0;
        uint64_t tcpDeltaUs = 0;
    };

    static void atomic_max_u64(std::atomic<uint64_t> *target, uint64_t value)
    {
        if (target == nullptr) {
            return;
        }
        uint64_t current = target->load(std::memory_order_relaxed);
        while (current < value &&
               !target->compare_exchange_weak(current, value, std::memory_order_relaxed)) {
        }
    }

    void enqueue_input_report(std::vector<uint8_t> &&payload, uint8_t endpoint, uint64_t receivedUs, uint64_t tcpDeltaUs)
    {
        if (payload.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(inputMutex_);
            if (inputQueue_.size() >= kInputQueueCapacity) {
                inputQueue_.pop_front();
                inputDropped_.fetch_add(1, std::memory_order_relaxed);
            }
            QueuedInputReport item;
            item.payload = std::move(payload);
            item.endpoint = endpoint;
            item.receivedUs = receivedUs;
            item.tcpDeltaUs = tcpDeltaUs;
            inputQueue_.push_back(std::move(item));
        }
        inputQueued_.fetch_add(1, std::memory_order_relaxed);
        inputTcpDeltaUsTotal_.fetch_add(tcpDeltaUs, std::memory_order_relaxed);
        atomic_max_u64(&inputTcpDeltaUsMax_, tcpDeltaUs);
        inputCv_.notify_one();
    }

    void input_worker_loop()
    {
        using clock = std::chrono::steady_clock;
        auto lastStatus = clock::now();
        uint64_t lastQueued = 0;
        uint64_t lastProcessed = 0;
        uint64_t lastDropped = 0;
        uint64_t lastTcpDeltaUsTotal = 0;
        uint64_t lastQueueDelayUsTotal = 0;
        uint64_t lastCallbackUsTotal = 0;

        while (running_.load() && !g_stop.load()) {
            QueuedInputReport item;
            {
                std::unique_lock<std::mutex> lock(inputMutex_);
                inputCv_.wait(lock, [&]() {
                    return !inputQueue_.empty() || !running_.load() || g_stop.load();
                });
                if (inputQueue_.empty()) {
                    break;
                }
                item = std::move(inputQueue_.front());
                inputQueue_.pop_front();
            }

            const uint64_t callbackStartUs = monotonic_us();
            const uint64_t queueDelayUs = callbackStartUs > item.receivedUs ? callbackStartUs - item.receivedUs : 0;
            inputQueueDelayUsTotal_.fetch_add(queueDelayUs, std::memory_order_relaxed);
            atomic_max_u64(&inputQueueDelayUsMax_, queueDelayUs);
            if (callback_) {
                callback_(item.payload.data(), item.payload.size(), item.endpoint);
            }
            const uint64_t callbackUs = monotonic_us() - callbackStartUs;
            inputCallbackUsTotal_.fetch_add(callbackUs, std::memory_order_relaxed);
            atomic_max_u64(&inputCallbackUsMax_, callbackUs);
            inputProcessed_.fetch_add(1, std::memory_order_relaxed);

            const auto now = clock::now();
            if (now - lastStatus >= std::chrono::seconds(2)) {
                const double seconds = std::chrono::duration<double>(now - lastStatus).count();
                const uint64_t queued = inputQueued_.load(std::memory_order_relaxed);
                const uint64_t processed = inputProcessed_.load(std::memory_order_relaxed);
                const uint64_t dropped = inputDropped_.load(std::memory_order_relaxed);
                const uint64_t tcpDeltaUsTotal = inputTcpDeltaUsTotal_.load(std::memory_order_relaxed);
                const uint64_t queueDelayUsTotal = inputQueueDelayUsTotal_.load(std::memory_order_relaxed);
                const uint64_t callbackUsTotal = inputCallbackUsTotal_.load(std::memory_order_relaxed);
                const uint64_t queuedDelta = queued - lastQueued;
                const uint64_t processedDelta = processed - lastProcessed;
                const uint64_t droppedDelta = dropped - lastDropped;
                const uint64_t tcpDeltaUsDelta = tcpDeltaUsTotal - lastTcpDeltaUsTotal;
                const uint64_t queueDelayUsDelta = queueDelayUsTotal - lastQueueDelayUsTotal;
                const uint64_t callbackUsDelta = callbackUsTotal - lastCallbackUsTotal;
                size_t depth = 0;
                {
                    std::lock_guard<std::mutex> lock(inputMutex_);
                    depth = inputQueue_.size();
                }
                const uint64_t tcpMaxUs = inputTcpDeltaUsMax_.exchange(0, std::memory_order_relaxed);
                const uint64_t queueMaxUs = inputQueueDelayUsMax_.exchange(0, std::memory_order_relaxed);
                const uint64_t callbackMaxUs = inputCallbackUsMax_.exchange(0, std::memory_order_relaxed);

                std::cout << "bridge input"
                          << " rx_hz=" << std::fixed << std::setprecision(1)
                          << static_cast<double>(queuedDelta) / seconds
                          << " callback_hz=" << static_cast<double>(processedDelta) / seconds
                          << " drops=" << droppedDelta
                          << " depth=" << depth
                          << " tcp_gap_avg_ms=" << std::fixed << std::setprecision(3)
                          << (queuedDelta == 0 ? 0.0 : static_cast<double>(tcpDeltaUsDelta) / 1000.0 / queuedDelta)
                          << " tcp_gap_max_ms=" << static_cast<double>(tcpMaxUs) / 1000.0
                          << " queue_wait_avg_ms="
                          << (processedDelta == 0 ? 0.0 : static_cast<double>(queueDelayUsDelta) / 1000.0 / processedDelta)
                          << " queue_wait_max_ms=" << static_cast<double>(queueMaxUs) / 1000.0
                          << " callback_avg_us="
                          << (processedDelta == 0 ? 0.0 : static_cast<double>(callbackUsDelta) / processedDelta)
                          << " callback_max_us=" << static_cast<double>(callbackMaxUs)
                          << std::defaultfloat
                          << std::endl;

                lastQueued = queued;
                lastProcessed = processed;
                lastDropped = dropped;
                lastTcpDeltaUsTotal = tcpDeltaUsTotal;
                lastQueueDelayUsTotal = queueDelayUsTotal;
                lastCallbackUsTotal = callbackUsTotal;
                lastStatus = now;
            }
        }
    }

    bool remote_feature(
        bool get,
        const uint8_t *request,
        size_t requestLength,
        unsigned int timeoutMs,
        std::vector<uint8_t> *response,
        const char *reason)
    {
        if (request == nullptr || requestLength == 0 || response == nullptr) {
            return false;
        }
        const uint32_t requestId = nextRequestId_.fetch_add(1);
        std::wstring error;
        if (!send_message(
                get ? CtmBridgeProtocol::MsgFeatureGet : CtmBridgeProtocol::MsgFeatureSet,
                0,
                requestId,
                request,
                requestLength,
                &error)) {
            std::wcerr << L"bridge feature send failed: " << error << L"\n";
            return false;
        }
        std::unique_lock<std::mutex> lock(featureMutex_);
        const bool signaled = featureCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
            return featureReplies_.find(requestId) != featureReplies_.end() ||
                   g_stop.load() ||
                   clientSocket_.load() == INVALID_SOCKET;
        });
        if (!signaled || g_stop.load() || clientSocket_.load() == INVALID_SOCKET) {
            std::cout << "bridge feature issue reason=" << reason
                      << " op=" << (get ? "get-timeout" : "set-timeout")
                      << " report=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(request[0])
                      << std::dec << std::setfill(' ') << std::endl;
            featureReplies_.erase(requestId);
            return false;
        }
        auto it = featureReplies_.find(requestId);
        const bool ok = it->second.first;
        if (ok) {
            *response = std::move(it->second.second);
        }
        featureReplies_.erase(it);
        return ok;
    }

    uint16_t port_ = 0;
    double btPaceMs_ = 10.0;
    // The last host config sent, kept so a live latency change can re-send the
    // whole thing rather than reconstruct it from scratch and risk drifting
    // from the handshake's version.
    CtmBridgeProtocol::HostConfig lastHostConfig_ = {};
    bool wsaStarted_ = false;
    // Bounded accept windows + idle rule (see accept_client / reader_loop).
    // All 0 = disabled: the CLI bridge mode keeps wait-forever semantics; the
    // agent opts in via set_session_timeouts / set_idle_timeouts.
    int initialAcceptTimeoutMs_ = 0;
    int reconnectGraceMs_ = 0;
    int idleTimeoutMs_ = 0;
    int gamepadIdleTimeoutMs_ = 0;
    std::function<void()> closedCallback_;
    SOCKET listenSocket_ = INVALID_SOCKET;
    std::atomic<SOCKET> clientSocket_{INVALID_SOCKET};
    CtmBridgeProtocol::DeviceCaps capsRaw_ = {};
    // Guards capsRaw_.serial only -- see caps(). Mutable so caps() can stay const.
    mutable std::mutex serialMutex_;
    std::vector<uint8_t> hidReportDescriptor_;
    std::vector<uint8_t> enumPayload_;   // forwarded composite enumeration (CTMB_MSG_ENUM)
    RawInputCallback callback_;
    std::atomic_bool running_{false};
    std::thread reader_;
    std::thread inputWorker_;
    static constexpr size_t kInputQueueCapacity = 16;
    std::mutex inputMutex_;
    std::condition_variable inputCv_;
    std::deque<QueuedInputReport> inputQueue_;
    std::atomic<uint64_t> inputQueued_{0};
    std::atomic<uint64_t> inputProcessed_{0};
    std::atomic<uint64_t> inputDropped_{0};
    std::atomic<uint64_t> inputTcpDeltaUsTotal_{0};
    std::atomic<uint64_t> inputTcpDeltaUsMax_{0};
    std::atomic<uint64_t> inputQueueDelayUsTotal_{0};
    std::atomic<uint64_t> inputQueueDelayUsMax_{0};
    std::atomic<uint64_t> inputCallbackUsTotal_{0};
    std::atomic<uint64_t> inputCallbackUsMax_{0};
    std::mutex sendMutex_;
    std::atomic<uint32_t> sendSequence_{0};
    std::atomic<uint32_t> nextRequestId_{1};
    std::mutex featureMutex_;
    std::condition_variable featureCv_;
    std::map<uint32_t, std::pair<bool, std::vector<uint8_t>>> featureReplies_;
};
