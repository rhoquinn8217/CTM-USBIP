// ---------------------------------------------------------------------------
// Config file watcher.
//
// Watches the device config file and applies changes to a RUNNING session, so
// tuning a setting no longer needs a virtual reseat. rhoquinn8217, 2026-08-01: the
// reseat "is like tuning an instrument but every time you make an adjustment,
// you have to put the instrument away then take it out again."
//
// WHAT IT DOES, AND WHAT IT DELIBERATELY DOES NOT
//
// Applies immediately:
//   * rumble_gain      -- scaled on every audio chunk, so a save lands within
//                         milliseconds
//   * any setting a game writes -- the overrides read their values live from
//                         the in-memory table, so the next report a game sends
//                         gets the new value
//
// Does NOT apply until something writes the field:
//   * speaker_volume while a game is running. Games write the audio fields at
//     launch and exit and not in between, so there is nothing to correct in
//     the meantime. Pushing a fresh settings report would fix this, but that
//     means reaching into a LIVE session to send -- see the note below. Left
//     out on purpose.
//
// WHY THERE IS NO SEND HERE, AND WHERE THE SEND ACTUALLY HAPPENS.
// This file only raises a flag. The agent loop notices it on its next pass and
// does the sending -- see apply_pending_config_to_sessions() in
// agent_session_sweep.inl. Reason: Sending would mean holding the global session
// lock while doing network I/O. The client socket has no send timeout, so a
// wedged TV would stall that send for minutes, and every other user of that
// lock -- sessions starting, sessions stopping, the sweep -- would queue up
// behind it. That is the same shape as the TV-side plug-out hang: the danger
// is waiting on work that has no time limit. Nothing in this file waits on
// anything but the filesystem.
//
// THREAD: one detached thread, started once, never stopped. It only ever
// touches the config store, which is mutex-guarded. The process exit takes it.
// Deliberate: a shutdown handshake here would buy nothing and could deadlock.
//
// BACKWARD COMPATIBILITY: nothing on the wire. No message type, no enum, no
// map key, no default. A missing config file behaves exactly as before.
//
// INCLUDE ORDER: after config/device_config.inl and audio/audio_gain.inl
// (both are called on change), before app/agent.inl (its only caller).
// ---------------------------------------------------------------------------

namespace ctm_config_watcher {

// Editors write a file as several operations, so one save fires the watcher
// two or three times. Wait this long after the last event before acting.
//
// Kept short on purpose: the control panel writes as a slider is dragged, and
// this delay is felt directly. Long enough to coalesce one save's worth of
// events and to guarantee the file is never read half-written.
constexpr DWORD kDebounceMs = 120;

// Raised when the config file changes; cleared by the agent loop once it has
// pushed the new settings to every live session. Cross-thread on purpose: the
// watcher must not touch a session itself.
inline std::atomic<bool> &change_pending()
{
    static std::atomic<bool> value{false};
    return value;
}

inline std::filesystem::file_time_type &last_seen_write()
{
    static std::filesystem::file_time_type value{};
    return value;
}

// True when the file's timestamp differs from the last one acted on. Guards
// against acting on notifications for other files in the same directory.
inline bool config_changed()
{
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(kDeviceConfigFileName, ec);
    if (ec) {
        return false;   // missing or unreadable -- nothing to apply
    }
    if (stamp == last_seen_write()) {
        return false;
    }
    last_seen_write() = stamp;
    return true;
}

inline void apply_change()
{
    device_config_invalidate();      // drop and reload the file
    // Per-controller config files are reloaded on the same beat. This watcher
    // covers the shared file only, so a hand-edited config file is otherwise
    // picked up on the next bridge -- touching the shared file is a way to
    // force it without reseating a controller.
    config_store::reload_all();
    ctm_audio_gain::refresh();       // pick up changed speaker/rumble gains
    change_pending().store(true, std::memory_order_relaxed);
    device_log::config(device_log::msg()
        << "applied a change to " << kDeviceConfigFileName);
}

inline void watch_loop()
{
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::current_path(ec);
    if (ec) {
        device_log::config("cannot resolve the working directory,"
                           " live config changes are off");
        return;
    }

    // Seed the timestamp so the first real edit is what triggers, not the file
    // simply existing at startup.
    config_changed();

    const HANDLE notify = FindFirstChangeNotificationW(
        dir.wstring().c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (notify == INVALID_HANDLE_VALUE || notify == nullptr) {
        device_log::config("could not watch the folder,"
                           " live config changes are off");
        return;
    }

    device_log::config(device_log::msg()
        << "watching " << kDeviceConfigFileName << " for changes");

    for (;;) {
        const DWORD waited = WaitForSingleObject(notify, INFINITE);
        if (waited != WAIT_OBJECT_0) {
            break;   // handle went bad; stop rather than spin
        }
        // Let the burst finish, then swallow whatever else it queued, so one
        // save produces one apply.
        std::this_thread::sleep_for(std::chrono::milliseconds(kDebounceMs));
        while (FindNextChangeNotification(notify) &&
               WaitForSingleObject(notify, 0) == WAIT_OBJECT_0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kDebounceMs));
        }
        if (config_changed()) {
            apply_change();
        }
        if (!FindNextChangeNotification(notify)) {
            break;
        }
    }
    FindCloseChangeNotification(notify);
}

// Start once, on the first bridge session. Cheap to call repeatedly.
// Wires config_store's change hook to the same flag a shared-file save raises,
// so a per-controller config write reaches a live controller by the path that
// already exists rather than a second one.
inline void adopt_config_store()
{
    config_store::g_on_change = []() {
        change_pending().store(true, std::memory_order_relaxed);
        device_log::config(device_log::msg()
            << "a controller config changed, pushing to live sessions");
    };
}

inline void ensure_started()
{
    static std::once_flag started;
    std::call_once(started, []() {
        std::thread(watch_loop).detach();
    });
}

}  // namespace ctm_config_watcher
