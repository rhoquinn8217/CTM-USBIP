// Session-side half of the config routes.
//
// WHY THIS FILE EXISTS. These read and mutate g_agent_sessions, which agent.inl
// owns; agent.inl in turn calls into rest.inl. So neither can be included
// first. rest_config.inl DECLARES these, this file DEFINES them, and this file
// is included after agent.inl -- the same split, for the same reason, as
// collect_bridge_session_snapshots().
//
// ⭐ The alternative was writing them inside agent.inl. "The data lives there"
// is not a reason for our code to live in an upstream file; that is how a fork
// accumulates edits it can no longer separate.

#pragma once

static std::vector<RestDeviceView> rest_collect_devices()
{
    std::vector<RestDeviceView> out;
    std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
    out.reserve(g_agent_sessions.size());
    for (const auto &session : g_agent_sessions) {
        RestDeviceView view;
        view.ordinal = session->ordinal;
        view.kind = session->kind;
        // ⚠️ Reported rather than filtered. A session mid-startup has no serial
        // yet and a link against it may not survive -- but hiding it entirely
        // would make a controller that is plugged in simply not appear, which
        // is worse to explain than one marked not-ready.
        view.ready = session->ready.load();
        {
            std::lock_guard<std::mutex> sessionLock(session->mutex);
            view.serial = session->physicalSerial;
            view.linkedConfig = session->linkedConfig;
        }
        out.push_back(std::move(view));
    }
    return out;
}

static bool rest_find_device(const std::string &ordinal, RestDeviceView *out)
{
    for (const RestDeviceView &view : rest_collect_devices()) {
        if (view.ordinal == ordinal) { *out = view; return true; }
    }
    return false;
}

// Links a device to a config, or unlinks when configName is empty.
//
// ⚠️ A kind mismatch is refused rather than honoured quietly: putting a
// ds5_edge config on a ds5 is a user error worth naming, and silently applying
// settings written for a different product is how a config becomes untrustable.
static bool rest_link_device(const std::string &ordinal, const std::string &configName,
                             std::string *error)
{
    std::string kind;
    if (!configName.empty()) {
        config_store::ConfigFile cfg;
        if (!config_store::find_config(configName, &cfg)) {
            *error = "no config named " + configName;
            return false;
        }
        kind = cfg.kind;
    }

    std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
    for (const auto &session : g_agent_sessions) {
        if (session->ordinal != ordinal) continue;
        // ⚠️ Compare SETTINGS kinds, not session kinds. A "ds5_usb" device and a
        // "ds5" config are the same controller family -- comparing the raw
        // session kind refused every link a real DualSense ever attempted.
        const std::string deviceKind = config_store::settings_kind_for(session->kind);
        if (!configName.empty() && deviceKind != kind) {
            *error = "kind mismatch: " + ordinal + " is " +
                     (deviceKind.empty() ? session->kind : deviceKind) +
                     ", config is " + kind;
            return false;
        }
        std::lock_guard<std::mutex> sessionLock(session->mutex);
        session->linkedConfig = configName;
        // ⭐ Push it to the device as well. The session field is what the API
        // reports; the DEVICE field is what actually resolves settings on the
        // output path. Setting only the first would make every link a no-op
        // that still answered 200.
        if (session->device) {
            session->device->set_linked_config(configName);
        }
        device_log::config(device_log::msg()
            << ordinal << (configName.empty()
                ? std::string(" back on the shared section")
                : std::string(" now reads ") + configName));
        return true;
    }
    // ⭐ Ordinals are monotonic and never reused, so a stale reference from an
    // old device list can only ever FAIL here -- never silently act on a
    // different physical controller. That is what removes the need for a
    // session token in the list response.
    *error = ordinal + " is not connected";
    return false;
}
