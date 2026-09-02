constexpr uint16_t kAgentDefaultPort = 48054;

struct AgentBridgeSession {
    std::string kind;
    std::wstring busId;
    std::string busIdAscii;
    uint16_t port = 0;
    std::shared_ptr<CtmUsbipDevice> device;
    std::unique_ptr<CtmBackend> backend;
    // Non-owning view of backend when it is the ENet transport, used to wire the
    // plug-out/plug-in callbacks. nullptr for the TCP BridgeBackend.
    EnetBridgeBackend *enetBackend = nullptr;
    std::thread worker;
    std::atomic_bool stopping{false};
    std::atomic_bool ready{false};
    std::mutex mutex;
    std::wstring lastError;
    // Per-controller config. ordinal is monotonic and never reused, so a stale
    // reference from an old device list fails rather than hitting the wrong
    // controller. linkedConfig empty means the shared [kind] section.
    std::string ordinal;
    // The word a person says out loud for this controller. Assigned with the
    // ordinal above and gone with the session, never used as a handle.
    std::string nickname;
    std::string physicalSerial;
    std::string linkedConfig;
};

static std::mutex g_agent_sessions_mutex;
static std::vector<std::unique_ptr<AgentBridgeSession>> g_agent_sessions;
static std::unique_ptr<CtmUsbipServer> g_agent_usbip_server;

// Reap requests from bridge reader threads (client lost / idle expired). The
// run_agent loop drains this on its own thread — the only thread that starts
// and stops sessions — so a reap can never race a concurrent start and never
// outlives shutdown (a detached reaper thread could UAF g_agent_sessions when
// the service stopped mid-reap).
static std::mutex g_agent_reap_mutex;
static std::vector<std::wstring> g_agent_reap_requests;

static void request_bridge_session_reap(const std::wstring &busId)
{
    std::lock_guard<std::mutex> lock(g_agent_reap_mutex);
    g_agent_reap_requests.push_back(busId);
}

static std::wstring find_relative_asset(const std::wstring &relative)
{
    // Live-edit override first: the agent/service resolves maps/profiles from
    // %ProgramData%\CTM Bridge\ exactly like the local CLI does.
    const std::wstring overridePath = programdata_override(relative);
    if (!overridePath.empty()) {
        return overridePath;
    }
    const std::wstring exeDir = module_directory();
    const std::vector<std::wstring> candidates = {
        relative,
        L"..\\" + relative,
        exeDir + L"\\" + relative,
        exeDir + L"\\..\\..\\..\\" + relative
    };
    for (const std::wstring &candidate : candidates) {
        if (file_exists(candidate)) return candidate;
    }
    return candidates[0];
}

static std::wstring bridge_profile_for_kind(const std::string &kind)
{
    if (kind == "ds4") {
        return find_relative_asset(L"profiles\\descriptors\\ds4_composite.profile");
    }
    if (kind == "ds5") {
        return find_ds5_descriptor_profile();
    }
    if (kind == "ds5_usb") {
        return find_relative_asset(L"profiles\\descriptors\\ds5_composite.profile");
    }
    if (kind == "ds5e_usb") {
        return find_relative_asset(L"profiles\\descriptors\\ds5e_composite.profile");
    }
    if (kind == "puck") {
        return find_relative_asset(L"profiles\\descriptors\\steam_puck.profile");
    }
    if (kind == "xbox") {
        return find_relative_asset(L"profiles\\descriptors\\xbox_gip_usb.profile");
    }
    return L"auto";
}

static std::wstring bridge_map_for_kind(const std::string &kind)
{
    if (kind == "ds4") {
        return find_relative_asset(L"maps\\ds4_usb_over_ds4_bt.map");
    }
    if (kind == "ds5") {
        return find_ds5_map_file();
    }
    if (kind == "ds5_usb") {
        return find_relative_asset(L"maps\\ds5_usb_over_ds5_usb.map");
    }
    if (kind == "ds5e_usb") {
        return find_relative_asset(L"maps\\ds5_usb_over_ds5_usb.map");
    }
    if (kind == "puck") {
        return find_relative_asset(L"maps\\steam_puck_identity.map");
    }
    if (kind == "xbox") {
        return find_relative_asset(L"maps\\xbox_gip_usb_over_xbox_bt.map");
    }
    return find_hid_identity_map_file();
}

// ⭐ Which controller a device belongs to. The input path holds a device
// pointer -- the chord fires there -- while the ordinal lives with the session.
//
// ⓘ Empty when it is not ours, and every caller treats that as "no particular
// controller" rather than guessing at one.
std::string ctm_ordinal_for_device(const void *deviceKey)
{
    if (!deviceKey) return std::string();
    std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
    for (const auto &s : g_agent_sessions) {
        if (s && s->device.get() == deviceKey) return s->ordinal;
    }
    return std::string();
}

static AgentBridgeSession *find_bridge_session_locked(const std::wstring &busId)
{
    for (auto &session : g_agent_sessions) {
        if (session->busId == busId) {
            return session.get();
        }
    }
    return nullptr;
}

static void set_bridge_session_error(AgentBridgeSession *session, const std::wstring &error)
{
    if (!session) {
        return;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    session->lastError = error;
}

static bool stop_bridge_session(const std::wstring &busId);
static void drain_bridge_session_reaps();

static void bridge_session_worker(AgentBridgeSession *session)
{
    std::wstring error;
    const std::wstring profile = bridge_profile_for_kind(session->kind);
    const std::wstring map = bridge_map_for_kind(session->kind);
    const bool dynamicProfile = profile == L"auto";
    const uint8_t audioLatency = 0x60;
    const bool hasAudioBlockOverride = false;
    const uint8_t audioBlockOverride = 0;

    if (dynamicProfile) {
        if (!session->device->load_map(map, audioLatency, hasAudioBlockOverride, audioBlockOverride, &error)) {
            set_bridge_session_error(session, error);
            std::wcerr << L"agent bridge load failed busid=" << session->busId << L": " << error << L"\n";
            return;
        }
    } else if (!session->device->load(profile, map, audioLatency, hasAudioBlockOverride, audioBlockOverride, &error)) {
        set_bridge_session_error(session, error);
        std::wcerr << L"agent bridge load failed busid=" << session->busId << L": " << error << L"\n";
        return;
    }

    // Transport selection: ENet/UDP when the agent was started with --enet,
    // otherwise the unchanged TCP BridgeBackend. Both bind the same port number.
    CtmBackend *backendPtr = nullptr;
    EnetBridgeBackend *enetBackendPtr = nullptr;
    if (g_use_enet.load()) {
        auto backend = std::make_unique<EnetBridgeBackend>(session->port, session->device->bt_audio_pace_ms());
        enetBackendPtr = backend.get();
        backendPtr = backend.get();
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->stopping.load()) {
            return;
        }
        session->enetBackend = enetBackendPtr;
        session->backend = std::move(backend);
    } else {
        auto backend = std::make_unique<BridgeBackend>(session->port, session->device->bt_audio_pace_ms());
        backendPtr = backend.get();
        // Agent-only session policy (the CLI bridge mode keeps wait-forever
        // defaults): bounded initial accept + reconnect grace, TCP keepalive
        // probing, and the idle rule — gamepads chatter constantly so silence
        // means gone (15 s); mice/keyboards may idle legitimately (15 min).
        // Generic "hid" gets the long window unless its HELLO descriptor is a
        // gamepad, which the backend detects and tightens itself.
        backend->set_session_timeouts(30000, 15000);
        const bool gamepadKind = session->kind == "ds4" || session->kind == "ds5" ||
                                 session->kind == "xbox" || session->kind == "puck";
        backend->set_idle_timeouts(gamepadKind ? 15000 : 15 * 60 * 1000, 15000);
        // TCP path: when the TV client vanishes and the reconnect grace runs
        // out (or the idle rule fires), unplug the virtual device and reap the
        // session (mirrors the ENet link-down behavior; a reaped port also
        // can't hijack the next plug's handshake). The reap is queued to the
        // agent loop: stop() joins the reader thread that fires this callback,
        // so reaping inline here would self-join.
        {
            const std::wstring busId = session->busId;
            backend->set_closed_callback([busId]() {
                device_log::session_w() << L"agent bridge client lost busid=" << busId
                           << L" -> virtual device UNPLUGGED";
                request_bridge_session_reap(busId);
            });
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->stopping.load()) {
            return;
        }
        session->backend = std::move(backend);
    }

    const std::shared_ptr<CtmUsbipDevice> device = session->device;
    if (!backendPtr->start([device](const uint8_t *data, size_t length, uint8_t endpoint) {
            device->on_physical_input(data, length, endpoint);
        }, &error)) {
        set_bridge_session_error(session, error);
        if (!session->stopping.load()) {
            std::wcerr << L"agent bridge backend failed busid=" << session->busId << L": " << error << L"\n";
        }
        return;
    }

    if (dynamicProfile) {
        CtmDescriptorProfile dynamicDescriptor;
        if (!make_dynamic_hid_profile(backendPtr->caps(), &dynamicDescriptor, &error) ||
            !session->device->set_profile(dynamicDescriptor, &error)) {
            set_bridge_session_error(session, error);
            backendPtr->stop();
            std::wcerr << L"agent bridge dynamic profile failed busid=" << session->busId << L": " << error << L"\n";
            return;
        }
    }

    // Composite puck: replace the static fallback profile with one built from
    // the forwarded enumeration (device + configuration descriptors + each HID
    // interface's report descriptor), presented full-speed. The enum arrives
    // during the handshake (CTMB_MSG_ENUM, before HELLO) and is stored in the
    // backend. If it did not arrive, the static profile loaded above stands.
    if (session->kind == "puck" && !backendPtr->enum_payload().empty()) {
        CtmDescriptorProfile compositeDescriptor;
        std::wstring compositeError;
        if (make_composite_profile_from_enum(backendPtr->enum_payload(), &compositeDescriptor, &compositeError) &&
            session->device->set_profile(compositeDescriptor, &compositeError)) {
            std::wcerr << L"agent bridge composite profile built busid=" << session->busId
                       << L" hid_ifaces=" << compositeDescriptor.interface_report_descriptors.size()
                       << L" full_speed=" << (compositeDescriptor.full_speed ? 1 : 0) << L"\n";
        } else {
            std::wcerr << L"agent bridge composite build failed busid=" << session->busId
                       << L" (keeping static profile): " << compositeError << L"\n";
        }
    }

    if (!session->device->attach_backend(backendPtr, &error)) {
        set_bridge_session_error(session, error);
        backendPtr->stop();
        std::wcerr << L"agent bridge attach failed busid=" << session->busId << L": " << error << L"\n";
        return;
    }

    if (!g_agent_usbip_server ||
        !g_agent_usbip_server->add_device(session->device, session->busIdAscii, &error)) {
        set_bridge_session_error(session, error);
        backendPtr->stop();
        std::wcerr << L"agent USB/IP export failed busid=" << session->busId << L": " << error << L"\n";
        return;
    }

    // For the ENet transport, surface an explicit plug-out on link loss and a
    // plug-in on reconnect. Detach mirrors BRIDGE_STOP: remove the export from
    // the shared USB/IP server (Windows sees an unplug). Reconnect re-exports
    // and re-attaches the same device. The TCP path sets no callbacks and keeps
    // its reconnect-in-place behavior unchanged.
    if (enetBackendPtr != nullptr) {
        const std::wstring busId = session->busId;
        const std::string busIdAscii = session->busIdAscii;
        const std::shared_ptr<CtmUsbipDevice> sessionDevice = session->device;
        enetBackendPtr->set_disconnect_callback([busId, busIdAscii]() {
            device_log::session_w() << L"agent bridge link down busid=" << busId
                       << L" -> virtual device UNPLUGGED";
            if (g_agent_usbip_server) {
                g_agent_usbip_server->remove_device(busIdAscii);
            }
        });
        enetBackendPtr->set_reconnect_callback([busId, busIdAscii, sessionDevice]() {
            device_log::session_w() << L"agent bridge link up busid=" << busId
                       << L" -> virtual device PLUGGED IN";
            std::wstring reAddError;
            if (g_agent_usbip_server &&
                g_agent_usbip_server->add_device(sessionDevice, busIdAscii, &reAddError)) {
                if (!run_usbip_attach(busId, kDefaultUsbipPort)) {
                    std::wcerr << L"agent re-attach failed busid=" << busId << L"\n";
                }
            } else {
                std::wcerr << L"agent re-export failed busid=" << busId << L": " << reAddError << L"\n";
            }
        });
    }

    // ⛔ RETIRE AN OLDER SESSION FOR THE SAME PHYSICAL CONTROLLER.
    //
    // Measured 2026-08-27: a re-bridge arriving before the previous session has
    // finished dying leaves TWO sessions attached to one pad. The controller
    // answers the OLD session's endpoint, so the new one's feature request times
    // out -- three attempts, 370ms apart, all of them -- and the gyro falls back
    // to a scale 62x too slow.
    //
    // ⚠️ The stale check upstream matches on PORT, and the TV picks a new port
    // each bridge: the log shows busid-9 on 48056 and busid-10 on 48057, same
    // serial, coexisting for five seconds. The SERIAL is what identifies the
    // physical device.
    //
    // ⛔ OUTSIDE the session->mutex block below, deliberately. Taking
    // g_agent_sessions_mutex while holding a session mutex inverts the lock
    // order the rest of this file uses, and stop_bridge_session takes both.
    {
        const std::string mySerial = session->device ? session->device->physical_serial()
                                                     : std::string();
        if (!mySerial.empty()) {
            std::vector<std::wstring> older;
            {
                std::lock_guard<std::mutex> guard(g_agent_sessions_mutex);
                for (const auto &other : g_agent_sessions) {
                    if (other->busId == session->busId) continue;
                    // ⓘ A session that has not reached ready yet has no serial,
                    // so it cannot be matched -- and does not need to be: it is
                    // not holding the pad's control endpoint either.
                    std::lock_guard<std::mutex> otherLock(other->mutex);
                    if (other->physicalSerial == mySerial) {
                        older.push_back(other->busId);
                    }
                }
            }
            for (const std::wstring &staleId : older) {
                device_log::session_w()
                    << L"retiring an older session for the same controller busid="
                    << staleId << L" -- it was still holding the pad";
                (void)stop_bridge_session(staleId);
            }
        }
    }

    // Per-controller config: pick up the physical serial, then auto-link if a
    // config claims it. A manual link made later overrides this for the life of
    // the session -- auto_link decides the starting point, not the whole story.
    {
        const std::string serial = session->device ? session->device->physical_serial()
                                                   : std::string();
        std::lock_guard<std::mutex> lock(session->mutex);
        session->physicalSerial = serial;
        if (session->linkedConfig.empty()) {
            session->linkedConfig = config_store::auto_link_for(serial, session->kind);
            if (!session->linkedConfig.empty()) {
                device_log::config(device_log::msg()
                    << session->ordinal << " auto-linked to " << session->linkedConfig);
            }
        }
        // The device resolves its own settings section on the output path, so
        // it needs the link too -- the session field alone would be a link
        // nothing acts on.
        if (session->device) {
            session->device->set_linked_config(session->linkedConfig);
        }

        // ⭐ Read the controller's own gyro calibration, once the session is up.
        //
        // Here rather than at attach because it is a round trip to the TV: the
        // feature request has to reach the physical pad and come back. Best
        // effort -- a controller whose calibration cannot be read still works,
        // on the old fixed scale, and says so in the log.
        if (session->kind == "ds5" || session->kind == "ds5_usb" ||
            session->kind == "ds5e_usb") {
            ctm_gyro_calib::fetch(session->device.get(), backendPtr, session->ordinal);
        }
    }

    session->ready.store(true);
    device_log::session_w() << L"agent bridge ready kind=" << widen_ascii(session->kind.c_str(), session->kind.size())
               << L" port=" << session->port << L" busid=" << session->busId;

    // ⭐⭐ THE WINDOW COMES UP ON THE CONTROLLER THAT JUST BRIDGED
    // (rhoquinn8217, 2026-09-01). Bridging is always a deliberate act -- there
    // is no mechanism that re-establishes a dropped connection on its own, so
    // this cannot fire in a loop.
    //
    // ⛔ EXCEPT WHEN THE WINDOW IS ALREADY IN FRONT. Then the person is looking
    // at it and using it, and destroying what they are doing to show them a tab
    // they can reach in one press is the wrong trade. Same rule the chord
    // already follows.
    //
    // ⓘ Unfocused but open counts as closed here: it is rebuilt on the new
    // controller, which is what makes a pad picked up from the sofa land
    // somewhere useful.
    if (ctm_open_ui::g_open_ui && !ctm_open_ui::window_has_foreground()) {
        device_log::session_w() << L"bridge: opening the settings window on "
                   << widen_ascii(session->ordinal.c_str(), session->ordinal.size());
        // ⛔⛔ ON ITS OWN THREAD, NEVER THIS ONE (rhoquinn8217, 2026-09-01:
        // "when two controllers are bridged, all buttons are rapid firing").
        //
        // ⚠️ THIS IS THE REPORT RELAY THREAD. ctm_chord_show_ui closes any open
        // window, POLLS UP TO A SECOND for it to actually go, then launches a
        // browser -- so the controller's reports queued behind all of that and
        // flushed in a burst afterwards, which reads as every button firing
        // over and over. Two controllers, two stalled relays.
        //
        // ⓘ Detached because nothing here waits on the result: the window opens
        // when it opens, and the session must not care.
        // ⓘ The target is set even when another open is already running: the
        // one in flight reads it as it builds its URL, so bridging two pads in
        // quick succession lands on the second -- the one just picked up.
        // ⭐ BACK ON, 2026-09-01, once the cause was understood. Bridging two
        // pads rapid-fired every button, and switching this off was how that
        // was traced -- but the fault was never here. It was the synthetic
        // KEYBOARD: one shared last-writer-wins state that every gated
        // controller wrote on every report, so an idle pad cancelled a held one
        // hundreds of times a second. Opening the window was merely what put
        // two gated pads on the config page at once.
        //
        // ⓘ ctm_chord_show_ui takes the one-at-a-time claim itself and leaves
        // if it cannot get it -- and it sets the target first either way, so
        // bridging two pads in quick succession lands on the second, which is
        // the one just picked up.
        const std::string target = session->ordinal;
        std::thread([target]() { ctm_chord_show_ui(target); }).detach();
    }
    // Bring up the synthetic gyro mouse the first time a DualSense session is
    // ready. Idempotent -- later sessions are no-ops. Always-present by design:
    // the gyro gate decides whether it MOVES, not whether it exists, so
    // enabling gyro mid-session through live config works without a reseat.
    if (session->kind == "ds5" || session->kind == "ds5_usb" ||
        session->kind == "ds5e_usb") {
        ctm_gyro_mouse_ensure_mouse_started();   // defined in mouse_device.inl
    }

    // ⭐ The keyboard comes up for ANY controller that can carry a config, not
    // just a DualSense -- buttons are the universal capability, and an Xbox pad
    // can be rebound too.
    //
    // ⓘ Same always-present reasoning as the mouse: a rebind configured
    // mid-session should work without a reseat. It sends nothing until
    // something is bound.
    if (config_store::kind_supports_config(session->kind)) {
        ctm_rebind_ensure_keyboard_started();
    }
    {
        std::string linked;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            linked = session->linkedConfig;
        }
        ds5_apply_initial_settings(backendPtr, linked);

        // ⭐ Push the audio buffer too, now that the link is known.
        //
        // ⛔ The handshake read in bridge.inl can only see the shared section:
        // it runs before the session exists, so no config is linked yet. Without
        // this, a controller linked to a config holding audio_latency_ms would
        // be sent the SHARED value at bridge and only get its own on the next
        // config change. Found 2026-08-22, when a stale shared 7 muted a
        // controller whose own config said 100.
        const std::string latencyKind = config_store::settings_kind_for(session->kind);
        if (!latencyKind.empty()) {
            const std::string section = device_settings_section(latencyKind.c_str(), linked);
            const int latency = device_config_int(section.c_str(), "audio_latency_ms", -1);
            if (latency >= 0 && latency <= 255) {
                std::wstring latencyError;
                if (backendPtr->send_audio_latency(static_cast<uint16_t>(latency), &latencyError)) {
                    device_log::report(device_log::msg()
                        << section << ": audio latency set to " << latency << " ms at bridge");
                } else {
                    // ⛔ The failure used to be silent -- the error was captured
                    // and discarded, which made an unreachable config and an
                    // unsupported feature look identical. That cost hours.
                    device_log::report(device_log::msg()
                        << section << ": audio latency FAILED -- "
                        << narrow_ascii(latencyError));
                }
            }

            // ⭐⭐ AND THE AUDIO SETTINGS, T-130, 2026-08-25.
            //
            // ⛔ THE FAULT: ds5_output_overrides.inl patches speaker volume,
            // headset volume, routing and rumble gain into the host's outbound
            // report, and every one of those overrides begins
            // `if (data[0] != 0x02) return;`. 0x02 is the WIRED report id. Over
            // Bluetooth the host sends 0x36, so all of them silently did
            // nothing. Heard rather than inferred: speaker_volume = 0 left the
            // controller at full volume.
            //
            // They travel instead of being patched here because a Bluetooth
            // output report is SIGNED, and the TV re-signs only when it patched
            // something itself. A report edited on this side alone would arrive
            // with a stale signature and be dropped by the controller.
            //
            // ⓘ Same place as the latency send, and for the same reason the
            // comment above gives: the handshake in bridge.inl runs before the
            // session exists, so no config is linked yet and only the shared
            // section is visible.
            //
            // ⚠️ kAudioUnset for anything absent, never zero -- zero is a legal
            // percentage and means silent.
            {
                const int spk  = device_config_int(section.c_str(), "speaker_volume", -1);
                const int hset = device_config_int(section.c_str(), "headset_volume", -1);
                const std::string outStr = device_config_str(section.c_str(), "audio_output");

                uint8_t mode = CtmBridgeProtocol::kAudioUnset;
                // ⓘ The TV's enum: AUTO 0, OFF 1, SPEAKER 2, HEADSET 3, BOTH 4.
                // headset_mono has NO TV equivalent -- it is a Windows-side
                // downmix -- so it maps to HEADSET and the mono part stays on
                // this side.
                if      (outStr == "auto")         mode = 0;
                else if (outStr == "off")          mode = 1;
                else if (outStr == "speaker")      mode = 2;
                else if (outStr == "headset")      mode = 3;
                else if (outStr == "headset_mono") mode = 3;
                else if (outStr == "both")         mode = 4;

                const uint8_t spkByte  = (spk  >= 0 && spk  <= 100)
                                       ? static_cast<uint8_t>(spk)
                                       : CtmBridgeProtocol::kAudioUnset;
                const uint8_t hsetByte = (hset >= 0 && hset <= 100)
                                       ? static_cast<uint8_t>(hset)
                                       : CtmBridgeProtocol::kAudioUnset;

                if (spkByte  != CtmBridgeProtocol::kAudioUnset ||
                    hsetByte != CtmBridgeProtocol::kAudioUnset ||
                    mode     != CtmBridgeProtocol::kAudioUnset) {
                    std::wstring audioError;
                    if (backendPtr->send_audio_settings(spkByte, hsetByte, mode, &audioError)) {
                        device_log::report(device_log::msg()
                            << section << ": audio settings sent at bridge -- speaker="
                            << static_cast<int>(spkByte) << " headset="
                            << static_cast<int>(hsetByte) << " mode="
                            << static_cast<int>(mode)
                            << " (255 = leave the TV's value)");
                    } else {
                        device_log::config(device_log::msg()
                            << section << ": audio settings FAILED -- "
                            << narrow_ascii(audioError));
                    }
                }
            }
        }
    }
    if (!run_usbip_attach(session->busId, kDefaultUsbipPort)) {
        std::wcerr << L"agent local attach failed busid=" << session->busId << L"\n";
    }
}

static bool start_bridge_session(const std::string &kind, uint16_t port, const std::wstring &busId, std::wstring *error)
{
    const std::string busIdAscii = narrow_ascii(busId);
    if (busIdAscii.empty() || busIdAscii.size() > 31) {
        if (error) *error = L"invalid bridge busid";
        return false;
    }

    // Pending reaps first (same thread as the drain loop): a TV re-plug can
    // reuse a busid whose old session just died — deduping against that dying
    // session would leave the TV talking to nothing.
    drain_bridge_session_reaps();

    // A re-plug arrives with a fresh busid but the same port. Any older session
    // still holding that port (dead client, wedged handshake) must go first --
    // two listeners on one port (SO_REUSEADDR) steal each other's connections,
    // which is how "bridge protocol header rejected" flaps happened.
    std::wstring staleBusId;
    {
        std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
        if (find_bridge_session_locked(busId)) {
            return true;
        }
        for (const auto &existing : g_agent_sessions) {
            if (existing->port == port) {
                staleBusId = existing->busId;
                break;
            }
        }
    }
    if (!staleBusId.empty()) {
        device_log::session_w() << L"agent bridge replacing stale session busid=" << staleBusId
                   << L" (port " << port << L" requested by busid=" << busId << L")";
        (void)stop_bridge_session(staleBusId);
    }

    std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
    if (find_bridge_session_locked(busId)) {
        return true;
    }

    device_config_invalidate();   // a reseat re-reads the settings file
    ctm_audio_gain::refresh();   // and picks up a changed rumble gain
    ctm_config_watcher::ensure_started();   // live config changes, no reseat
    ctm_config_watcher::adopt_config_store();   // ...including per-controller ones
    config_store::reload_all();             // per-controller config files
    auto session = std::make_unique<AgentBridgeSession>();
    // Assigned at CREATION, not when listed -- two consecutive device lists
    // must not disagree. Monotonic per kind and never reused: ds5_1 that
    // unbridges comes back as ds5_3, so a command naming a stale ordinal fails
    // instead of hitting a different controller.
    {
        static std::map<std::string, unsigned> nextOrdinal;
        session->ordinal = kind + "_" + std::to_string(++nextOrdinal[kind]);
    }
    // ⭐ The nickname is assigned HERE, beside the ordinal, so the two share a
    // lifetime exactly: both are born with the session and both are gone when
    // it ends. ⓘ That is what fixes the re-rolling -- the settings window is
    // killed and recreated by every chord, but the agent is not.
    //
    // ⛔ Not an identifier. The ordinal remains the handle for commands and the
    // log; this is the word you say out loud.
    {
        // ⓘ g_agent_sessions_mutex is already held here -- taking it again
        // would deadlock.
        std::vector<std::string> taken;
        for (const auto &s : g_agent_sessions) {
            if (s && !s->nickname.empty()) taken.push_back(s->nickname);
        }
        session->nickname = ctm_nickname::pick(taken);
    }
    session->kind = kind;
    session->busId = busId;
    session->busIdAscii = busIdAscii;
    session->port = port;
    session->device = std::make_shared<CtmUsbipDevice>();
    AgentBridgeSession *sessionPtr = session.get();
    const std::string ordinalForLog = session->ordinal;
    const std::string nicknameForLog = session->nickname;
    session->worker = std::thread([sessionPtr]() { bridge_session_worker(sessionPtr); });
    g_agent_sessions.push_back(std::move(session));
    // ⭐ BOTH NAMES ON THE LINE, so it greps either way: the ordinal is what a
    // command or a bug report will quote, the nickname is what the person
    // watching actually called it.
    device_log::session_w() << L"agent bridge starting "
               << widen_ascii(ordinalForLog.c_str(), ordinalForLog.size())
               << L" (" << widen_ascii(nicknameForLog.c_str(), nicknameForLog.size()) << L")"
               << L" kind=" << widen_ascii(kind.c_str(), kind.size())
               << L" port=" << port << L" busid=" << busId;
    return true;
}

static bool stop_bridge_session(const std::wstring &busId)
{
    std::unique_ptr<AgentBridgeSession> session;
    {
        std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
        for (size_t i = 0; i < g_agent_sessions.size(); ++i) {
            if (g_agent_sessions[i]->busId != busId) {
                continue;
            }
            session = std::move(g_agent_sessions[i]);
            g_agent_sessions.erase(g_agent_sessions.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (!session) {
        return false;
    }
    session->stopping.store(true);
    if (g_agent_usbip_server) {
        g_agent_usbip_server->remove_device(session->busIdAscii);
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->backend) {
            session->backend->stop();
        }
    }
    if (session->device) {
        session->device->wake_input_waiters();
        session->device->stop();
    }
    if (session->worker.joinable()) {
        session->worker.join();
    }
    device_log::session_w() << L"agent bridge stopped busid=" << busId;
    return true;
}

static void stop_all_bridge_sessions()
{
    std::vector<std::wstring> busIds;
    {
        std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
        for (const auto &session : g_agent_sessions) {
            busIds.push_back(session->busId);
        }
    }
    for (const std::wstring &busId : busIds) {
        (void)stop_bridge_session(busId);
    }
}

// Drained once per run_agent loop tick (~1 s cadence). Runs on the same thread
// that serves BRIDGE_START/STOP, so reaps serialize with every other session
// lifecycle change.
static void drain_bridge_session_reaps()
{
    std::vector<std::wstring> requests;
    {
        std::lock_guard<std::mutex> lock(g_agent_reap_mutex);
        requests.swap(g_agent_reap_requests);
    }
    for (const std::wstring &busId : requests) {
        (void)stop_bridge_session(busId);
    }
}


#include "agent_session_sweep.inl"

static void send_text(SOCKET sock, const std::string &text)
{
    send(sock, text.c_str(), static_cast<int>(text.size()), 0);
}

static void handle_agent_client(SOCKET client, const sockaddr_in &peer)
{
    char line[512] = {};
    int n = recv(client, line, sizeof(line) - 1, 0);
    if (n <= 0) {
        return;
    }
    line[n] = '\0';
    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = '\0';

    std::istringstream input(line);
    std::string command;
    input >> command;
    if (command == "STATUS") {
        send_text(client, "OK CTM_AGENT_V1\n");
        return;
    }

    if (command == "USBIP_ATTACH") {
        std::string busIdAscii;
        input >> busIdAscii;
        if (busIdAscii.empty() || busIdAscii.size() > 31) {
            send_text(client, "ERR bad busid\n");
            return;
        }
        char peerIp[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, peerIp, sizeof(peerIp));
        if (run_usbip_attach_to(widen_ascii(peerIp, strlen(peerIp)), widen_ascii(busIdAscii.c_str(), busIdAscii.size()), kDefaultUsbipPort)) {
            send_text(client, "OK usbip attached\n");
        } else {
            send_text(client, "ERR usbip attach failed\n");
        }
        return;
    }

    if (command == "BRIDGE_START") {
        std::string kind;
        unsigned long port = 0;
        std::string busIdAscii;
        input >> kind >> port >> busIdAscii;
        if ((kind != "ds4" && kind != "ds5" && kind != "ds5_usb" && kind != "ds5e_usb" &&
             kind != "hid" && kind != "puck" && kind != "xbox") ||
            port < 1024 || port > 65535 ||
            busIdAscii.empty() || busIdAscii.size() > 31) {
            send_text(client, "ERR bad bridge args\n");
            return;
        }
        std::wstring error;
        if (start_bridge_session(kind, static_cast<uint16_t>(port), widen_ascii(busIdAscii.c_str(), busIdAscii.size()), &error)) {
            send_text(client, "OK bridge starting\n");
        } else {
            std::string err = narrow_ascii(error);
            send_text(client, "ERR " + (err.empty() ? std::string("bridge start failed") : err) + "\n");
        }
        return;
    }

    if (command == "BRIDGE_STOP") {
        std::string busIdAscii;
        input >> busIdAscii;
        if (busIdAscii.empty()) {
            send_text(client, "ERR bad busid\n");
            return;
        }
        // Actually tear the session down: removes the device from the shared
        // USB/IP server (Windows sees an unplug), stops the cmub_ backend,
        // joins the worker. This is what makes "controller off" cleanly
        // remove the virtual device instead of leaving a stale export.
        const std::wstring busId = widen_ascii(busIdAscii.c_str(), busIdAscii.size());
        if (stop_bridge_session(busId)) {
            send_text(client, "OK bridge stopped\n");
        } else {
            send_text(client, "OK no such bridge\n");
        }
        return;
    }

    if (command == "RESTART") {
        std::string arg;
        input >> arg;
        if (arg == "hard") {
            // Full restart of the process via the SCM (service mode only). The
            // session/USB-IP state is rebuilt from scratch when the TV
            // re-issues BRIDGE_START after the service comes back.
            if (request_service_restart()) {
                send_text(client, "OK service restarting\n");
            } else {
                send_text(client, "ERR hard restart requires service mode\n");
            }
            return;
        }
        // Soft reset: tear down every bridge session and its USB/IP export
        // (Windows sees each virtual device unplug); the agent keeps listening
        // and rebuilds on the next BRIDGE_START.
        stop_all_bridge_sessions();
        send_text(client, "OK bridges reset\n");
        return;
    }

    send_text(client, "ERR unknown command\n");
}

static int run_agent(uint16_t port)
{
    device_log::session_w() << L"ctm-usbip " << widen_ascii(CTM_VERSION_DISPLAY, strlen(CTM_VERSION_DISPLAY))
               << L" agent starting";
    WSADATA data = {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::wcerr << wsa_error_message(L"WSAStartup failed") << L"\n";
        return 4;
    }

    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (udp == INVALID_SOCKET || tcp == INVALID_SOCKET) {
        std::wcerr << wsa_error_message(L"agent socket failed") << L"\n";
        WSACleanup();
        return 4;
    }

    BOOL reuse = TRUE;
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(udp, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        bind(tcp, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(tcp, 8) == SOCKET_ERROR) {
        std::wcerr << wsa_error_message(L"agent bind/listen failed") << L"\n";
        closesocket(udp);
        closesocket(tcp);
        WSACleanup();
        return 4;
    }

    g_agent_usbip_server = std::make_unique<CtmUsbipServer>();
    std::wstring usbipError;
    if (!g_agent_usbip_server->start(kDefaultUsbipPort, &usbipError)) {
        std::wcerr << L"agent USB/IP server failed: " << usbipError << L"\n";
        closesocket(udp);
        closesocket(tcp);
        WSACleanup();
        return 4;
    }

    g_rest_agent_start = std::chrono::steady_clock::now();
    SOCKET rest = INVALID_SOCKET;
    if (g_rest_port != 0) {
        std::wstring restError;
        rest = rest_open_listener(&restError);
        if (rest == INVALID_SOCKET) {
            // --rest was asked for explicitly; a silently missing API would be
            // worse than failing startup, and every other bind above is fatal.
            std::wcerr << L"agent REST listener failed: " << restError << L"\n";
            g_agent_usbip_server->stop();
            g_agent_usbip_server.reset();
            closesocket(udp);
            closesocket(tcp);
            WSACleanup();
            return 4;
        }
        device_log::session_w() << L"ctm agent REST API on " << (g_rest_bind_lan ? L"0.0.0.0" : L"127.0.0.1")
                   << L":" << g_rest_port
                   << (g_rest_token.empty() ? L"" : L" (bearer token required)");
    }

    device_log::session_w() << L"ctm agent listening udp/tcp port " << port;
    {
        // ⛔ EVERY RELATIVE PATH RESOLVES HERE -- configs/, device.log, the
        // shared settings file. Added 2026-08-31 after configs created in one
        // session were unfindable in the next: the answer to "where did that
        // file go" must be a grep of this line, never a theory.
        wchar_t cwd[MAX_PATH] = L"?";
        GetCurrentDirectoryW(MAX_PATH, cwd);
        device_log::session_w() << L"working directory: " << cwd
            << L" (configs/, logs and settings resolve here)";
    }

    // ⭐ Open the page HERE, once the agent is actually listening.
    //
    // ⛔ main.cpp only FOCUSES an existing window, and only opens a new one when
    // it finds the port already taken. So a first run -- nothing running, no
    // window -- opened nothing at all, and the page appeared only on the second
    // launch. This is the missing half.
    //
    // ⓘ Late rather than early on purpose: a page opened before the agent binds
    // would load, fail its first poll and show "cannot reach the listener".
    if (ctm_open_ui::g_open_ui && !ctm_open_ui::g_ui_already_focused) {
        ctm_open_ui::open_new(g_rest_port);
    }
    while (!g_stop.load()) {
        drain_bridge_session_reaps();
        sweep_bridge_sessions();
        apply_pending_config_to_sessions();   // push a config edit to live sessions
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(udp, &readfds);
        FD_SET(tcp, &readfds);
        if (rest != INVALID_SOCKET) {
            FD_SET(rest, &readfds);
        }
        timeval timeout {};
        timeout.tv_sec = 1;
        int rc = select(0, &readfds, nullptr, nullptr, &timeout);
        if (rc <= 0) {
            continue;
        }
        if (FD_ISSET(udp, &readfds)) {
            char buf[128] = {};
            sockaddr_in from = {};
            int fromLen = sizeof(from);
            int n = recvfrom(udp, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen);
            if (n > 0) {
                buf[n] = '\0';
                if (strncmp(buf, "CTM_DISCOVER_V1", 15) == 0) {
                    std::string reply = "CTM_AGENT_V1 port=" + std::to_string(port) + "\n";
                    sendto(udp, reply.c_str(), static_cast<int>(reply.size()), 0,
                           reinterpret_cast<sockaddr *>(&from), fromLen);
                }
            }
        }
        if (FD_ISSET(tcp, &readfds)) {
            sockaddr_in peer = {};
            int peerLen = sizeof(peer);
            SOCKET client = accept(tcp, reinterpret_cast<sockaddr *>(&peer), &peerLen);
            if (client != INVALID_SOCKET) {
                handle_agent_client(client, peer);
                closesocket(client);
            }
        }
        if (rest != INVALID_SOCKET && FD_ISSET(rest, &readfds)) {
            sockaddr_in peer = {};
            int peerLen = sizeof(peer);
            SOCKET client = accept(rest, reinterpret_cast<sockaddr *>(&peer), &peerLen);
            if (client != INVALID_SOCKET) {
                // Inline on the loop thread, like handle_agent_client — this is
                // what lets rest_route call start/stop_bridge_session directly.
                rest_handle_client(client, port);
                closesocket(client);
            }
        }
    }

    if (rest != INVALID_SOCKET) {
        closesocket(rest);
    }
    stop_all_bridge_sessions();
    if (g_agent_usbip_server) {
        g_agent_usbip_server->stop();
        g_agent_usbip_server.reset();
    }
    closesocket(udp);
    closesocket(tcp);
    WSACleanup();
    return 0;
}
