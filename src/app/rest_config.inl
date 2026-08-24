// REST routes for per-controller config.
//
// ⭐ THESE ARE ROUTES, NOT A SECOND SERVER. Everything about transport --
// listening, HTTP parsing, bearer auth, method-not-allowed, JSON escaping,
// error shape -- comes from rest.inl and is reused verbatim. A parallel HTTP
// server would have duplicated all of it and let the two drift apart.
//
// WHY THESE ENDPOINTS. The existing REST surface is status, sessions and
// restart -- session control, which the TV's line protocol already does. It
// touches ctm-device-config.txt not at all. These are the endpoints with a
// user: a web app that manages controller configs, which is the thing the API
// was wanted for.
//
// ⭐ A DEVICE MAY BE NAMED WHEREVER A SERIAL OR KIND IS NEEDED, so a UI can
// work entirely in terms of the controllers it just listed and never show a
// person a MAC address.

#pragma once

// Provided by rest_config_sessions.inl, which is included after agent.inl --
// same split, and for the same reason, as collect_bridge_session_snapshots().
struct RestDeviceView {
    std::string ordinal;        // "ds5_3" -- monotonic, never reused
    std::string kind;
    std::string serial;
    std::string linkedConfig;   // "" = shared [kind] section
    bool ready = false;         // false = still starting or tearing down
};
static std::vector<RestDeviceView> rest_collect_devices();
static bool rest_link_device(const std::string &ordinal, const std::string &configName,
                             std::string *error);
static bool rest_find_device(const std::string &ordinal, RestDeviceView *out);

// The name the shared section answers to in the API. Reserved, so a real config
// cannot take it -- see config_store::valid_name().
static const char *kSharedName = "shared";

static std::string rest_device_json(const RestDeviceView &d)
{
    std::string out = "{\"ordinal\":\"" + rest_json_escape(d.ordinal) + "\"";
    out += ",\"kind\":\"" + rest_json_escape(d.kind) + "\"";
    out += ",\"serial\":\"" + rest_json_escape(d.serial) + "\"";
    out += ",\"linked_config\":\"" + rest_json_escape(d.linkedConfig) + "\"";
    // ⚠️ linked_config is "" for an unlinked device, which reads as "nothing
    // applies" and is wrong: it is reading the shared section. `reads` says so.
    out += ",\"reads\":\"" +
           rest_json_escape(d.linkedConfig.empty() ? std::string(kSharedName) : d.linkedConfig) +
           "\"";
    out += ",\"ready\":" + std::string(d.ready ? "true" : "false");
    out += ",\"supports_config\":" +
           std::string(config_store::kind_supports_config(d.kind) ? "true" : "false") + "}";
    return out;
}

static std::string rest_devices_json()
{
    std::string out = "{\"devices\":[";
    bool first = true;
    for (const RestDeviceView &d : rest_collect_devices()) {
        if (!first) out += ",";
        first = false;
        out += rest_device_json(d);
    }
    return out + "]}";
}

static std::string rest_config_json(const config_store::ConfigFile &cfg,
                                    const std::vector<RestDeviceView> &devices)
{
    std::string out = "{\"name\":\"" + rest_json_escape(cfg.name) + "\"";
    out += ",\"kind\":\"" + rest_json_escape(cfg.kind) + "\"";
    out += ",\"auto_link\":[";
    for (size_t i = 0; i < cfg.autoLink.size(); ++i) {
        if (i) out += ",";
        out += "\"" + rest_json_escape(cfg.autoLink[i]) + "\"";
    }
    // Which live devices currently read this config. Makes the consequence of
    // archiving visible before anyone does it.
    out += "],\"linked_by\":[";
    bool first = true;
    for (const RestDeviceView &d : devices) {
        if (config_store::lower(d.linkedConfig) != config_store::lower(cfg.name)) continue;
        if (!first) out += ",";
        first = false;
        out += "\"" + rest_json_escape(d.ordinal) + "\"";
    }
    return out + "]}";
}

// ⭐ THE SHARED SECTION, EXPOSED AS A CONFIG BUT READ-ONLY.
//
// Every device with no link reads ctm-device-config.txt's [ds5] block. That is
// long-standing behaviour and stays -- for a single-controller setup it is the
// whole feature, and create-and-link is a lot of ceremony to change one volume.
//
// ⚠️ But it applies to EVERYTHING, silently. A stale value there attenuated
// every controller in this project for an evening while the API cheerfully
// reported "linked_config": "" -- which reads as "nothing applies" and actually
// means "reading whatever is in that file".
//
// So it is listed like any other config and its settings are readable, but the
// API refuses to WRITE it: a change that affects every device should be a
// deliberate hand edit, not something a UI can do by accident.
static std::string rest_shared_json(const std::vector<RestDeviceView> &devices)
{
    std::string out = "{\"name\":\"shared\",\"kind\":\"ds5\",\"read_only\":true";
    out += ",\"note\":\"Applies to every device with no config linked. "
           "Edit ctm-device-config.txt by hand.\"";
    out += ",\"auto_link\":[],\"linked_by\":[";
    bool first = true;
    for (const RestDeviceView &d : devices) {
        if (!d.linkedConfig.empty()) continue;          // linked devices do not read it
        if (!first) out += ",";
        first = false;
        out += "\"" + rest_json_escape(d.ordinal) + "\"";
    }
    out += "],\"settings\":{";
    {
        // ⭐ RELOAD, do not trust the cache.
        //
        // Everything else reads settings through the cached table, which the
        // file watcher refreshes on save -- correct, and fast enough for a path
        // that runs 250 times a second.
        //
        // ⛔ This endpoint must not. Its entire job is answering "what is
        // actually in effect right now", and it was caught on 2026-08-21
        // reporting speaker_volume=33 from a file that had already been
        // emptied: the watcher's debounce had not yet fired. A diagnostic that
        // can itself be stale is worse than no diagnostic, because it is
        // believed. This is a human-paced call -- rereading a small file costs
        // nothing next to being wrong.
        std::lock_guard<std::mutex> lock(g_device_config_mutex);

        // ⚠️ ERASE FIRST. device_config_load_locked() only INSERTS -- it never
        // clears -- so reloading alone would keep a key that had been DELETED
        // from the file. That is precisely the case this fix exists for: an
        // emptied file still reporting speaker_volume=33.
        //
        // Only the sections this file owns are dropped. The "cfg:" sections
        // belong to config_store and are not reloaded here; clearing them
        // would blank every linked controller's settings until something
        // reloaded them.
        for (auto it = g_device_config.begin(); it != g_device_config.end(); ) {
            if (it->first.rfind("cfg:", 0) == 0) ++it;
            else it = g_device_config.erase(it);
        }
        g_device_config_loaded = false;
        device_config_load_locked();

        auto it = g_device_config.find("ds5");
        bool firstKey = true;
        if (it != g_device_config.end()) {
            for (const auto &entry : it->second) {
                if (!firstKey) out += ",";
                firstKey = false;
                out += "\"" + rest_json_escape(entry.first) + "\":\"" +
                       rest_json_escape(entry.second) + "\"";
            }
        }
    }
    return out + "}}";
}

static std::string rest_configs_json()
{
    const std::vector<RestDeviceView> devices = rest_collect_devices();
    std::string out = "{\"configs\":[";
    bool first = false;
    out += rest_shared_json(devices);              // always first, always present
    for (const config_store::ConfigFile &cfg : config_store::list_configs()) {
        if (!first) out += ",";
        first = false;
        out += rest_config_json(cfg, devices);
    }
    return out + "]}";
}

// One config with its settings as currently loaded.
static std::string rest_config_detail_json(const config_store::ConfigFile &cfg)
{
    const std::string section = config_store::section_for(cfg.name, cfg.kind);
    std::string out = rest_config_json(cfg, rest_collect_devices());
    out.pop_back();                                   // drop the closing brace
    out += ",\"settings\":{";
    // ⭐ Reload from disk first, for the same reason as the shared endpoint:
    // this answers "what is in this config right now", and a hand edit that
    // the watcher has not yet noticed would otherwise be invisible. Cheap --
    // these files are tiny and this is a human-paced call.
    config_store::reload_all();
    {
        std::lock_guard<std::mutex> lock(g_device_config_mutex);
        if (!g_device_config_loaded) {
            device_config_load_locked();
        }
        auto it = g_device_config.find(section);
        bool first = true;
        if (it != g_device_config.end()) {
            for (const auto &entry : it->second) {
                if (!first) out += ",";
                first = false;
                out += "\"" + rest_json_escape(entry.first) + "\":\"" +
                       rest_json_escape(entry.second) + "\"";
            }
        }
    }
    return out + "}}";
}

// ⭐ Describes every setting a UI can offer: type, range, choices, and a line
// of help. The agent does NOT enforce this list -- a hand-written key still
// works -- so it is descriptive, not a schema. It exists so a web app can
// build its own form without hard-coding what this fork happens to support.
static std::string rest_keys_json()
{
    return R"({"keys":[
{"key":"gyro_to_mouse_gate","type":"choice","choices":["","always","L2","R2","L1","R1","touchpad","!touchpad","touchpad_click","PS"],"default":"","help":"What must be held for gyro to move the mouse. Blank is off."},
{"key":"gyro_mouse_px_per_360","type":"int","min":1000,"max":200000,"default":1920,"help":"Pixels the cursor travels for one full turn of the controller. Higher is faster."},
{"key":"gyro_mouse_min_sens","type":"int","min":0,"max":60,"default":8,"help":"Sensitivity for slow, precise movement."},
{"key":"gyro_mouse_sens","type":"int","min":1,"max":200,"default":50,"help":"Legacy single sensitivity. Scales both tiers; 50 leaves them as shipped."},
{"key":"gyro_mouse_debug_gate","type":"bool","default":false,"help":"Logs the raw trigger and touchpad bytes twice a second, for diagnosing a gate that never opens."},
{"key":"gyro_mouse_debug_scale","type":"bool","default":false,"help":"Logs measured degrees turned, pixels emitted, report interval and rate twice a second. For finding why a usable speed needs a px_per_360 far above the calibrated figure."},
{"key":"gyro_mouse_max_sens","type":"int","min":0,"max":60,"default":16,"help":"Sensitivity for fast turns. Equal to min for no acceleration."},
{"key":"gyro_mouse_min_threshold","type":"int","min":0,"max":200,"default":5,"help":"Degrees per second below which min_sens applies."},
{"key":"gyro_mouse_max_threshold","type":"int","min":0,"max":400,"default":75,"help":"Degrees per second above which max_sens applies."},
{"key":"gyro_mouse_speed_h","type":"int","min":10,"max":300,"default":100,"help":"Horizontal speed as a percentage. 100 is unchanged, not maximum."},
{"key":"gyro_mouse_speed_v","type":"int","min":10,"max":300,"default":100,"help":"Vertical speed as a percentage. Lower than horizontal suits fine aiming, since a screen is wider than it is tall."},
{"key":"gyro_mouse_invert","type":"int","min":0,"max":3,"default":0,"help":"0 neither, 1 horizontal, 2 vertical, 3 both."},
{"key":"gyro_mouse_player_space","type":"bool","default":true,"help":"Account for how the controller is being held."},
{"key":"gyro_mouse_recenter_button","type":"choice","choices":["","touchpad_click","PS","L1","R1"],"default":"","help":"Warps the cursor to screen centre. Desktop only -- games hide the cursor and read movement instead."},
{"key":"audio_output","type":"choice","choices":["auto","headset","headset_mono","speaker","both","off"],"default":"auto","help":"Where controller audio goes. The mode also decides which volume keys apply."},
{"key":"speaker_volume","type":"int","min":0,"max":100,"default":100,"help":"Controller speaker level. 100 is maximum."},
{"key":"headset_volume","type":"int","min":0,"max":100,"default":100,"help":"Controller headset jack level. 100 is maximum."},
{"key":"audio_gain","type":"int","min":0,"max":500,"default":100,"help":"Scales audio passing through. 100 is unchanged, not maximum."},
{"key":"audio_latency_ms","type":"int","min":0,"max":255,"default":60,"help":"Controller speaker jitter buffer, Bluetooth only. Below 20 is unusable; 20-60 trades responsiveness for reliability. Absent leaves the TV to decide."},
{"key":"force_echo_cancel","type":"bool","default":false,"help":"Off makes the controller mute its own speaker as feedback protection."},
{"key":"master_rumble_gain","type":"int","min":0,"max":500,"default":100,"help":"Scales all rumble. 100 is unchanged."},
{"key":"rumble_gain_heavy","type":"int","min":0,"max":500,"default":100,"help":"Heavy weight only. Multiplies with master."},
{"key":"rumble_gain_soft","type":"int","min":0,"max":500,"default":100,"help":"Soft weight only. Multiplies with master."}
]})";
}

// Resolves a body that may name either a device or a raw value.
static bool rest_kind_from_body(const RestJson &json, std::string *kind, std::string *error)
{
    auto kindIt = json.strings.find("kind");
    if (kindIt != json.strings.end() && !kindIt->second.empty()) {
        *kind = kindIt->second;
        return true;
    }
    auto deviceIt = json.strings.find("device");
    if (deviceIt != json.strings.end()) {
        RestDeviceView view;
        if (!rest_find_device(deviceIt->second, &view)) {
            *error = deviceIt->second + " is not connected";
            return false;
        }
        *kind = view.kind;
        return true;
    }
    *error = "kind or device is required";
    return false;
}

// Returns true when this request was one of ours and `out` holds the response.
static bool rest_route_config(const RestRequest &req, std::string *out)
{
    // ⭐ GET / -- the settings page itself.
    //
    // Handled here rather than in rest.inl so the whole settings surface, page
    // and endpoints, sits in one file. It is also the only route that answers
    // with something other than JSON.
    if (req.path == "/" || req.path == "/index.html") {
        if (req.method != "GET") {
            *out = rest_error_response(405, "method not allowed", "Allow: GET, OPTIONS\r\n");
            return true;
        }
        *out = ctm_ui_page::http_response();
        return true;
    }

    // GET /api/v1/devices
    if (req.path == "/api/v1/devices") {
        if (req.method != "GET") {
            *out = rest_error_response(405, "method not allowed", "Allow: GET, OPTIONS\r\n");
            return true;
        }
        *out = rest_http_response(200, rest_devices_json());
        return true;
    }

    // POST /api/v1/devices/{ordinal}/link   {"config":"name"} or {} to unlink
    static const char kDevPrefix[] = "/api/v1/devices/";
    if (req.path.compare(0, sizeof(kDevPrefix) - 1, kDevPrefix) == 0) {
        const std::string rest = req.path.substr(sizeof(kDevPrefix) - 1);
        const size_t slash = rest.find('/');
        if (slash == std::string::npos || rest.substr(slash) != "/link") {
            *out = rest_error_response(404, "unknown path");
            return true;
        }
        const std::string ordinal = rest.substr(0, slash);
        if (req.method != "POST") {
            *out = rest_error_response(405, "method not allowed", "Allow: POST, OPTIONS\r\n");
            return true;
        }
        RestJson json;
        std::string parseError;
        if (!req.body.empty() && !rest_parse_flat_json(req.body, &json, &parseError)) {
            *out = rest_error_response(400, parseError);
            return true;
        }
        auto it = json.strings.find("config");
        const std::string configName = it == json.strings.end() ? std::string() : it->second;
        std::string error;
        if (!rest_link_device(ordinal, configName, &error)) {
            *out = rest_error_response(400, error);
            return true;
        }
        *out = rest_http_response(200, rest_devices_json());
        return true;
    }

    // GET /api/v1/keys
    if (req.path == "/api/v1/keys") {
        if (req.method != "GET") {
            *out = rest_error_response(405, "method not allowed", "Allow: GET, OPTIONS\r\n");
            return true;
        }
        *out = rest_http_response(200, rest_keys_json());
        return true;
    }

    // GET, POST /api/v1/configs
    if (req.path == "/api/v1/configs") {
        if (req.method == "GET") {
            *out = rest_http_response(200, rest_configs_json());
            return true;
        }
        if (req.method == "POST") {
            RestJson json;
            std::string parseError;
            if (!rest_parse_flat_json(req.body, &json, &parseError)) {
                *out = rest_error_response(400, parseError);
                return true;
            }
            auto nameIt = json.strings.find("name");
            if (nameIt == json.strings.end()) {
                *out = rest_error_response(400, "name is required");
                return true;
            }
            std::string kind, error;
            if (!rest_kind_from_body(json, &kind, &error)) {
                *out = rest_error_response(400, error);
                return true;
            }
            if (!config_store::create_config(nameIt->second, kind, &error)) {
                *out = rest_error_response(409, error);
                return true;
            }
            // Link the device that asked for it, if one was named -- creating a
            // config for a controller and not attaching it would surprise.
            auto deviceIt = json.strings.find("device");
            if (deviceIt != json.strings.end()) {
                std::string ignored;
                rest_link_device(deviceIt->second, nameIt->second, &ignored);
            }
            *out = rest_http_response(200, rest_configs_json());
            return true;
        }
        *out = rest_error_response(405, "method not allowed", "Allow: GET, POST, OPTIONS\r\n");
        return true;
    }

    // /api/v1/configs/{name}[/settings|/archive|/autolink|/unautolink]
    static const char kCfgPrefix[] = "/api/v1/configs/";
    if (req.path.compare(0, sizeof(kCfgPrefix) - 1, kCfgPrefix) == 0) {
        const std::string rest = req.path.substr(sizeof(kCfgPrefix) - 1);
        const size_t slash = rest.find('/');
        const std::string name = rest.substr(0, slash);
        const std::string action = slash == std::string::npos ? std::string() : rest.substr(slash + 1);

        // The shared section is readable like any other config and writable by
        // no one. A UI must be able to SHOW what an unlinked device is reading
        // -- that invisibility is what made a stale value so hard to find --
        // without being able to change something that affects every device.
        if (config_store::lower(name) == kSharedName) {
            if (req.method == "GET" && action.empty()) {
                *out = rest_http_response(200, rest_shared_json(rest_collect_devices()));
            } else {
                *out = rest_error_response(403,
                    "the shared section is read-only here -- edit ctm-device-config.txt "
                    "by hand. It applies to every device with no config linked, so a "
                    "change to it should be deliberate rather than something a UI does.");
            }
            return true;
        }

        if (!config_store::valid_name(name)) {
            *out = rest_error_response(404, "unknown path");
            return true;
        }
        config_store::ConfigFile cfg;
        if (!config_store::find_config(name, &cfg)) {
            *out = rest_error_response(404, "no config named " + name);
            return true;
        }

        if (action.empty()) {
            if (req.method != "GET") {
                *out = rest_error_response(405, "method not allowed", "Allow: GET, OPTIONS\r\n");
                return true;
            }
            *out = rest_http_response(200, rest_config_detail_json(cfg));
            return true;
        }

        if (req.method != "POST") {
            *out = rest_error_response(405, "method not allowed", "Allow: POST, OPTIONS\r\n");
            return true;
        }
        RestJson json;
        std::string parseError;
        if (!req.body.empty() && !rest_parse_flat_json(req.body, &json, &parseError)) {
            *out = rest_error_response(400, parseError);
            return true;
        }
        std::string error;

        if (action == "settings") {
            // Numbers and bools arrive typed; the file wants text either way.
            for (const auto &entry : json.strings) {
                if (!config_store::set_setting(name, entry.first, entry.second, &error)) {
                    *out = rest_error_response(400, error);
                    return true;
                }
            }
            for (const auto &entry : json.numbers) {
                if (!config_store::set_setting(name, entry.first,
                                               std::to_string(entry.second), &error)) {
                    *out = rest_error_response(400, error);
                    return true;
                }
            }
            for (const auto &entry : json.bools) {
                if (!config_store::set_setting(name, entry.first,
                                               entry.second ? "true" : "false", &error)) {
                    *out = rest_error_response(400, error);
                    return true;
                }
            }
            config_store::find_config(name, &cfg);
            *out = rest_http_response(200, rest_config_detail_json(cfg));
            return true;
        }

        if (action == "rename") {
            auto nameIt = json.strings.find("name");
            if (nameIt == json.strings.end()) {
                *out = rest_error_response(400, "name is required");
                return true;
            }
            if (!config_store::rename_config(name, nameIt->second, &error)) {
                *out = rest_error_response(409, error);
                return true;
            }
            // ⚠️ Re-point every live session that was reading it. The file moved
            // and its settings section moved with it, so a session left holding
            // the old name would silently fall back to the shared section --
            // a rename that quietly unlinks is worse than one that fails.
            for (const RestDeviceView &d : rest_collect_devices()) {
                if (config_store::lower(d.linkedConfig) != config_store::lower(name)) continue;
                std::string ignored;
                rest_link_device(d.ordinal, nameIt->second, &ignored);
            }
            *out = rest_http_response(200, rest_configs_json());
            return true;
        }

        if (action == "archive") {
            std::string movedTo;
            if (!config_store::archive_config(name, &error, &movedTo)) {
                *out = rest_error_response(400, error);
                return true;
            }
            // ⚠️ Drop linked devices back to the shared section HERE, rather
            // than waiting for the directory watcher. Otherwise the response
            // goes out while a device is still pointed at a file that is gone.
            for (const RestDeviceView &d : rest_collect_devices()) {
                if (config_store::lower(d.linkedConfig) != config_store::lower(name)) continue;
                std::string ignored;
                rest_link_device(d.ordinal, std::string(), &ignored);
            }
            *out = rest_http_response(200,
                "{\"archived_to\":\"" + rest_json_escape(movedTo) + "\"}");
            return true;
        }

        if (action == "autolink" || action == "unautolink") {
            std::string serial;
            auto serialIt = json.strings.find("serial");
            if (serialIt != json.strings.end()) serial = serialIt->second;
            else {
                auto deviceIt = json.strings.find("device");
                if (deviceIt == json.strings.end()) {
                    *out = rest_error_response(400, "serial or device is required");
                    return true;
                }
                RestDeviceView view;
                if (!rest_find_device(deviceIt->second, &view)) {
                    *out = rest_error_response(400, deviceIt->second + " is not connected");
                    return true;
                }
                if (config_store::settings_kind_for(view.kind) != cfg.kind) {
                    *out = rest_error_response(400,
                        "kind mismatch: device is " + view.kind + ", config is " + cfg.kind);
                    return true;
                }
                serial = view.serial;
            }
            const bool ok = action == "autolink"
                ? config_store::add_auto_link(name, serial, &error)
                : config_store::remove_auto_link(name, serial, &error);
            if (!ok) {
                *out = rest_error_response(action == "autolink" ? 409 : 400, error);
                return true;
            }
            *out = rest_http_response(200, rest_configs_json());
            return true;
        }

        *out = rest_error_response(404, "unknown path");
        return true;
    }

    return false;                    // not ours; fall through to the existing routes
}
