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

static std::wstring sweep_timestamp()
{
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t buffer[32] = {};
    swprintf(buffer, 32, L"%02u:%02u:%02u.%03u",
             static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute),
             static_cast<unsigned>(now.wSecond), static_cast<unsigned>(now.wMilliseconds));
    return std::wstring(buffer);
}

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

static std::wstring sweep_wide(const std::string &text)
{
    return widen_ascii(text.c_str(), text.size());
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
            std::wcout << L"agent sweep " << sweep_timestamp()
                       << L" session added busid=" << sweep_wide(entry.busIdAscii)
                       << L" kind=" << sweep_wide(entry.kind)
                       << L" port=" << entry.port
                       << L" ready=" << (entry.ready ? 1 : 0) << L"\n";
            continue;
        }
        entry.firstSeenTick = previous->firstSeenTick;
        entry.suspectPasses = previous->suspectPasses;
        if (entry.ready != previous->ready || entry.stopping != previous->stopping) {
            std::wcout << L"agent sweep " << sweep_timestamp()
                       << L" session state busid=" << sweep_wide(entry.busIdAscii)
                       << L" port=" << entry.port
                       << L" ready=" << (entry.ready ? 1 : 0)
                       << L" stopping=" << (entry.stopping ? 1 : 0)
                       << L" age_ms=" << (nowTick - entry.firstSeenTick) << L"\n";
        }
    }

    for (const SweepObservation &previous : g_sweep_previous) {
        if (sweep_find(current, previous.busIdAscii)) {
            continue;
        }
        std::wcout << L"agent sweep " << sweep_timestamp()
                   << L" session gone busid=" << sweep_wide(previous.busIdAscii)
                   << L" kind=" << sweep_wide(previous.kind)
                   << L" port=" << previous.port
                   << L" lived_ms=" << (nowTick - previous.firstSeenTick)
                   << L" ready=" << (previous.ready ? 1 : 0)
                   << L" stopping=" << (previous.stopping ? 1 : 0) << L"\n";
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
            std::wcout << L"agent sweep " << sweep_timestamp()
                       << L" port reuse port=" << previous.port
                       << L" gone_busid=" << sweep_wide(previous.busIdAscii)
                       << L" added_busid=" << sweep_wide(entry.busIdAscii) << L"\n";
        }
    }

    g_sweep_previous.swap(current);
}
