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

// ⭐⭐ WHICH SECTIONS THIS DEVICE SHOULD BE OFFERED (T-227).
//
// A setting a pad has no hardware for is worse than a missing one: it reads as
// broken when it does nothing. ⓘ Nothing is DETECTED here -- the layout
// already carries the answer, and this only reads it.
//
// ⚠️ WHEN T-203 LANDS, AUDIO STOPS BEING DUALSENSE-ONLY. A DS4 has a speaker
// and a headset jack too; its audio simply is not carried yet. ➡️ The audio
// line below is the ONE place to change, and it is written as a kind test
// rather than folded into the layout so it is easy to find.
static void rest_fill_capabilities(RestDeviceView *view)
{
    if (view == nullptr) return;
    const std::string k = config_store::settings_kind_for(view->kind);
    const bool isDs5 = (k == "ds5" || k == "ds5_edge");

    // Audio and the three rumble gains are patched by ds5_output_overrides.inl,
    // and every override there begins `if (data[0] != 0x02) return;` -- the
    // DualSense's wired report id. On any other pad they do nothing at all.
    view->hasAudio = isDs5;
    view->hasRumbleGains = isDs5;

    // ⚠️ A WIRED XBOX PAD ARRIVES AS "xpad", WHICH settings_kind_for() DOES NOT
    // MAP -- it lists only the kinds the TV sends for a config, and xpad is not
    // among them. So fall back to the RAW kind for the layout, or a cabled Xbox
    // pad would be offered gyro and a touchpad it has never had.
    // ⓘ Harmless if xpad never reaches here: layout_for() simply answers the
    // same nullptr it would have anyway.
    const ctm_rebind::Layout *lay = ctm_rebind::layout_for(k.c_str());
    if (lay == nullptr && (view->kind == "xpad" || view->kind == "xbox")) {
        lay = ctm_rebind::layout_for("xbox");
    }
    if (lay == nullptr) {
        // ⚠️ An unknown pad keeps everything. See the note on the defaults.
        view->hasTouchpad = true;
        view->hasGyro = true;
        return;
    }
    view->hasTouchpad = lay->touch.present;

    // ⛔⛔ THE XBOX LAYOUT NEVER GETS GYRO, WHOEVER MADE THE PAD.
    //
    // ⚠️ An earlier version trusted the layout's "no" only for vendor 0x045e
    // and kept the section for anyone else, reasoning that a third-party pad
    // emulating an Xbox pad might really have a gyro. ⛔ It might -- and we
    // still could not carry it.
    //
    // ⭐ CHECKED AGAINST THE WIDER WORLD 2026-09-19, at rhoquinn8217's asking:
    // **XInput has no slots for gyroscope data at all**, so a pad in Xbox mode
    // strips it before anything downstream sees it. Where gyro does work on
    // such pads, software reads the pad's RAW HID stream instead -- SDL carries
    // per-device code for the GameSir G7 Pro 8K to do exactly that -- or the
    // pad is switched out of Xbox mode entirely, into DirectInput over
    // Bluetooth or a Switch Pro mode.
    //
    // ➡️ So on the reports WE relay there is no gyro to offer, and showing the
    // section would promise a setting that cannot work -- the exact fault this
    // file exists to remove. ⓘ The route for someone who wants that pad's gyro
    // is its OTHER mode, where it arrives as a `hid` device with its own
    // descriptor and then needs a button table: 🔗 T-196's Blocker C.
    view->hasGyro = lay->motion.present;
}

static std::vector<RestDeviceView> rest_collect_devices()
{
    std::vector<RestDeviceView> out;
    std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
    out.reserve(g_agent_sessions.size());
    for (const auto &session : g_agent_sessions) {
        RestDeviceView view;
        view.ordinal = session->ordinal;
        view.nickname = session->nickname;
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
        // ⓘ Empty until the TV's HELLO has arrived, like the serial.
        if (session->device) {
            view.product = session->device->product_name();
            view.deviceType = session->device->device_kind_by_descriptor();
            // ⭐ What the pad last said about its own charge (T-195). Left at
            // -1 for a pad with no battery byte, so the JSON omits it.
            const ctm_rebind::BatteryReading battery =
                ctm_battery::reading_for(session->device.get());
            if (battery.known) {
                view.batteryPercent = battery.percent;
                view.batteryState = ctm_battery::state_word(battery.state);
            }
            rest_fill_capabilities(&view);
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
// ⭐⭐ ANY CONFIG, ON ANY DEVICE THAT TAKES ONE (rhoquinn8217, 2026-09-12).
// This used to refuse a "kind mismatch" -- a ds5_edge config on a ds5 -- on the
// grounds that settings written for one product would misbehave on another.
// ⛔ That premise did not hold: a config stores button positions, percents and
// milliseconds, and each feature checks the pad before it acts, so a setting
// a controller cannot use is unused on it rather than misapplied.
//
// ⚠️ What IS still refused is a device that takes no config at all, a keyboard
// or a mouse. Linking one would report success and do nothing, which is the
// worst shape a setting can take.
static bool rest_link_device(const std::string &ordinal, const std::string &configName,
                             std::string *error)
{
    if (!configName.empty()) {
        config_store::ConfigFile cfg;
        if (!config_store::find_config(configName, &cfg)) {
            *error = "no config named " + configName;
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
    for (const auto &session : g_agent_sessions) {
        if (session->ordinal != ordinal) continue;
        // ⓘ Unlinking is always allowed: it only returns a device to the
        // shared settings.
        if (!configName.empty() && !config_store::kind_supports_config(session->kind)) {
            RestDeviceView view;
            view.ordinal = ordinal;
            view.kind = session->kind;
            *error = rest_no_config_error(view);
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
        // ⭐⭐ AND ASK FOR THE SETTINGS REPORT TO BE RE-SENT.
        //
        // ⛔ WITHOUT THIS, LINKING A CONFIG ONLY HALF APPLIED IT. The two lines
        // above change what the per-report overrides RESOLVE, which covers
        // every setting that rides on a report the game is already sending.
        // They do nothing for the settings we SET once -- audio routing,
        // volume, the microphone, and the trigger effects. Those only reached
        // the controller when the config FILE changed or the pad was
        // re-bridged, so linking a config and feeling nothing happen was the
        // documented behaviour rather than a fault (found 2026-09-10, when an
        // L2 wall from another config outlived a link that had already moved).
        //
        // ⓘ Raised as a flag rather than sent from here on purpose. The sweep
        // that acts on it runs on the AGENT loop, which is the only thread
        // where a session cannot be torn down underneath a backend pointer --
        // see the warning above apply_pending_config_to_sessions(). This
        // borrows that guarantee instead of needing its own.
        ctm_config_watcher::change_pending().store(true, std::memory_order_relaxed);

        device_log::config(device_log::msg()
            << ordinal << (configName.empty()
                ? std::string(" back on the shared section")
                : std::string(" now reads ") + configName)
            << " -- settings queued for the next sweep");
        return true;
    }
    // ⭐ Ordinals are monotonic and never reused, so a stale reference from an
    // old device list can only ever FAIL here -- never silently act on a
    // different physical controller. That is what removes the need for a
    // session token in the list response.
    *error = ordinal + " is not connected";
    return false;
}
