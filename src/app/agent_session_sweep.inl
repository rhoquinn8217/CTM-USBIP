// Periodic observer over the agent's bridge-session list.
//
// LOGGING ONLY. It removes nothing, changes no session state, and alters no
// behaviour for any client. Called once per agent loop pass, immediately after
// the reap drain, on the agent loop thread -- the only thread that starts or
// stops sessions -- so it never inspects the list from a second thread.
//
// No code is copied from upstream; nothing here has an upstream counterpart.
//
// Why it exists: the log records that a session ended, but not when, not how
// long it had lived, and not what happened while nothing was being watched.
// A session that vanishes ~15 s after its last input hit the idle rule; one
// that vanishes ~30 s after hit the keepalive. The lived_ms figure on the
// "session gone" line is what separates them.
//
// Cadence: the agent loop's select() has a 1 s timeout but returns early on
// discovery traffic, so this runs at least once a second and often more. That
// is harmless -- it prints only when something changed -- and all timing comes
// from GetTickCount64(), never from counting passes.

struct SweepObservation {
    std::string busIdAscii;
    std::string kind;
    uint16_t port = 0;
    bool ready = false;
    bool stopping = false;
    unsigned long long firstSeenTick = 0;
    // Reserved for a later cleaning mode: consecutive passes in which this
    // session looked dead. Nothing reads it while the sweep is logging-only,
    // but the grace period it supports has to exist before anything acts.
    int suspectPasses = 0;
};

static std::vector<SweepObservation> g_sweep_previous;
static bool g_sweep_armed = false;

static const SweepObservation *sweep_find(const std::vector<SweepObservation> &list,
                                          const std::string &busIdAscii)
{
    for (const SweepObservation &entry : list) {
        if (entry.busIdAscii == busIdAscii) {
            return &entry;
        }
    }
    return nullptr;
}

static void sweep_bridge_sessions()
{
    const unsigned long long nowTick = GetTickCount64();

    std::vector<SweepObservation> current;
    {
        std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
        current.reserve(g_agent_sessions.size());
        for (const auto &session : g_agent_sessions) {
            SweepObservation entry;
            entry.busIdAscii = session->busIdAscii;
            entry.kind = session->kind;
            entry.port = session->port;
            entry.ready = session->ready.load();
            entry.stopping = session->stopping.load();
            entry.firstSeenTick = nowTick;
            current.push_back(entry);
        }
    }
    // session->lastError is deliberately NOT read here. It is guarded by the
    // per-session mutex, and taking that while holding the list mutex is the
    // only lock nesting this file would introduce. Not worth it for a v1 whose
    // whole point is being unable to disturb a working system.

    if (!g_sweep_armed) {
        g_sweep_armed = true;
        g_sweep_previous.swap(current);
        return;
    }

    for (SweepObservation &entry : current) {
        const SweepObservation *previous = sweep_find(g_sweep_previous, entry.busIdAscii);
        if (!previous) {
            device_log::session(device_log::msg()
                << "session added busid=" << entry.busIdAscii
                << " kind=" << entry.kind
                << " port=" << entry.port
                << " ready=" << (entry.ready ? 1 : 0));
            continue;
        }
        entry.firstSeenTick = previous->firstSeenTick;
        entry.suspectPasses = previous->suspectPasses;
        if (entry.ready != previous->ready || entry.stopping != previous->stopping) {
            device_log::session(device_log::msg()
                << "session state busid=" << entry.busIdAscii
                << " port=" << entry.port
                << " ready=" << (entry.ready ? 1 : 0)
                << " stopping=" << (entry.stopping ? 1 : 0)
                << " age_ms=" << (nowTick - entry.firstSeenTick));
        }
    }

    for (const SweepObservation &previous : g_sweep_previous) {
        if (sweep_find(current, previous.busIdAscii)) {
            continue;
        }
        device_log::session(device_log::msg()
            << "session gone busid=" << previous.busIdAscii
            << " kind=" << previous.kind
            << " port=" << previous.port
            << " lived_ms=" << (nowTick - previous.firstSeenTick)
            << " ready=" << (previous.ready ? 1 : 0)
            << " stopping=" << (previous.stopping ? 1 : 0));
        // Port takeover signature: a session leaves and another arrives on the
        // same port within one pass. A plug-in request stops the stale session
        // so the new one can take the port, both in the same operation -- while
        // a keepalive or idle teardown leaves the port with nothing behind it.
        for (const SweepObservation &entry : current) {
            if (entry.port != previous.port) {
                continue;
            }
            if (sweep_find(g_sweep_previous, entry.busIdAscii)) {
                continue;
            }
            device_log::session(device_log::msg()
                << "port reuse port=" << previous.port
                << " gone_busid=" << previous.busIdAscii
                << " added_busid=" << entry.busIdAscii);
        }
    }

    g_sweep_previous.swap(current);

    // !! DO NOT DELETE THIS FLUSH. Nothing in this file writes to wcout any
    // !! more -- the sweep's own lines go to device.log now -- so this looks
    // !! like dead code and is not. Console output is block-buffered when
    // !! redirected to a file, and this flush is what carries out UPSTREAM's
    // !! lines written elsewhere. Removing it reintroduces the bug where every
    // !! log before 2026-07-30 lost its teardown lines, and their absence read
    // !! as evidence of a silent teardown.
    std::wcout.flush();
}

// ---------------------------------------------------------------------------
// Push changed settings to every live session.
//
// Called from the agent loop, next to the sweep, so a config edit reaches a
// RUNNING controller without a reseat. The gains already apply on save by
// themselves -- they are read live off the audio path. This is for settings
// that are SENT to the controller, like speaker_volume, which otherwise sit
// unchanged until something happens to write that field.
//
// !! WHY THE POINTERS ARE COPIED AND THE LOCK RELEASED BEFORE SENDING.
// !! Sending is network I/O on a socket with no send timeout, so a wedged TV
// !! can stall it. Holding the session list lock across that would block a
// !! starting session's worker, which takes the same lock. Copying and
// !! releasing avoids it.
// !!
// !! THAT IS ONLY SAFE BECAUSE SESSION TEARDOWN HAPPENS ON THIS THREAD.
// !! stop_bridge_session() and the reap drain both run on the agent loop, so
// !! no backend can be freed while this function is running. If teardown ever
// !! moves to another thread, this becomes a use-after-free and must be
// !! revisited -- copying a raw pointer out from under a lock is only correct
// !! under that guarantee.
//
// A stall here delays the agent loop -- no new sessions, no reaps -- until the
// send returns. That is the least-bad place for it: one thread waiting rather
// than every thread queued behind a lock.
static void apply_pending_config_to_sessions()
{
    if (!ctm_config_watcher::change_pending().exchange(false, std::memory_order_relaxed)) {
        return;
    }

    struct Target {
        CtmBackend *backend;
        // ⚠️ The linked config travels with the target. Without it this sweep
        // pushed SHARED-section values over a linked device's settings, quietly
        // undoing its config every time the shared file changed.
        std::string linkedConfig;
        std::string kind;
        std::string busIdAscii;
    };
    std::vector<Target> targets;
    {
        std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
        for (const auto &session : g_agent_sessions) {
            if (!session->backend || !session->ready.load() || session->stopping.load()) {
                continue;
            }
            std::string linked;
            {
                std::lock_guard<std::mutex> sessionLock(session->mutex);
                linked = session->linkedConfig;
            }
            targets.push_back(Target{session->backend.get(), linked, session->kind,
                                     session->busIdAscii});
        }
    }

    for (const Target &target : targets) {
        device_log::config(device_log::msg()
            << "pushing settings to live session busid=" << target.busIdAscii);
        ds5_apply_initial_settings(target.backend, target.linkedConfig);

        // The audio buffer is not part of the settings report -- it lives in
        // the host config, which the TV accepts at any point in a session. Sent
        // separately for that reason, and only when the file names it: absent
        // must leave the TV's own value alone.
        // ⛔ Was hardcoded to the shared "ds5" section, which ignored the
        // linked config entirely: a controller linked to a config holding
        // audio_latency_ms=100 was sent whatever the SHARED section said, and
        // a value set in its own config did nothing. Found 2026-08-22 when a
        // stale shared 7 muted a controller whose config said 100.
        //
        // ⚠️ Uses the SESSION kind, not a literal, so an Edge resolves its own
        // section rather than borrowing the DualSense one.
        const std::string latencyKind = config_store::settings_kind_for(target.kind);
        const std::string latencySection =
            device_settings_section(latencyKind.empty() ? "ds5" : latencyKind.c_str(),
                                    target.linkedConfig);
        const int latency = device_config_int(latencySection.c_str(), "audio_latency_ms", -1);
        // Warn on the live path as well as at handshake. This is the one a
        // person actually meets: they edit the file mid-session, the audio goes
        // quiet, and without this there is nothing to explain why.
        // ⚠️ Uses the shared constant rather than a literal. This said 8 until
        // 2026-08-22, which was the WIRED figure -- so a Bluetooth user hitting
        // the 11-14 dead band got silence and no warning at all.
        if (latency >= 0 && latency < CtmBridgeProtocol::kLatencySilentBelow) {
            device_log::config(device_log::msg()
                << "audio_latency_ms=" << latency << " is below "
                << CtmBridgeProtocol::kLatencySilentBelow
                << " -- the controller's speaker may be SILENT."
                << " Raise it to recover; 60 is the usual value");
        }
        if (latency >= 0 && latency <= 255) {
            std::wstring latencyError;
            if (target.backend->send_audio_latency(static_cast<uint16_t>(latency), &latencyError)) {
                device_log::config(device_log::msg()
                    << latencySection << ": audio latency set to " << latency
                    << " ms on busid=" << target.busIdAscii);
            } else {
                // ⛔ This used to log NOTHING. The error was captured into
                // latencyError and discarded, so a failed send was
                // indistinguishable from the code never running -- which led
                // to "audio_latency_ms is wired-only", exactly backwards.
                device_log::config(device_log::msg()
                    << latencySection << ": audio latency FAILED on busid="
                    << target.busIdAscii << " -- " << narrow_ascii(latencyError));
            }
        }
    }
}
