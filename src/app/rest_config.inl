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
    std::string nickname;       // "Titan" -- for people; never a handle
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
    out += ",\"nickname\":\"" + rest_json_escape(d.nickname) + "\"";
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
    // ⭐ The REAL gate state, alongside the devices the page already polls.
    //
    // ⛔ The page cannot read this from the config file: the endpoints set the
    // flag directly and never write to disk, so a ticked or unticked box there
    // says nothing about what is actually happening. A checkbox that lies is
    // worse than no checkbox.
    // ⛔ THE EFFECTIVE GATE, not the flag.
    //
    // Measured 2026-08-29: with the window behind, the flag was on and the
    // footer read "controllers to page" -- while the agent was NOT gating,
    // because it checks the foreground on every report. The footer said one
    // thing and the pad did another.
    //
    // ⭐ Reporting what is actually happening also fixes the ungated warning for
    // free: it keys off this, so it stops firing in a state where nothing is
    // gated anyway.
    out += "],\"config_mode\":";
    out += (ctm_rebind_config_mode() && ctm_ui_has_foreground()) ? "true" : "false";
    // ⭐ Which window the agent considers current. A page carrying a different
    // token is stale.
    out += ",\"ui_token\":\"" + rest_json_escape(ctm_open_ui::current_ui_token()) + "\"";
    // ⓘ Constant for this listener run; the page keeps its remembered place
    // only while this value is unchanged.
    out += ",\"run_id\":\"" + rest_json_escape(ctm_open_ui::agent_run_id()) + "\"";
    out += ",\"gate_hold\":";
    out += ctm_rebind_gate_hold() ? "true" : "false";
    return out + "}";
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
    // ⛔ SCAN DISK FIRST, the same as the detail endpoint below.
    //
    // Without it this answered from whatever happened to be loaded, and at
    // startup that is nothing but the shared section -- the config files are
    // only read when a device bridges. So a page opened alongside the listener
    // was told there were no configs, correctly reported it, and looked like a
    // bug in the page. Measured 2026-08-25.
    //
    // Cheap: the files are tiny and this is a human-paced call.
    config_store::reload_all();

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
    // ⛔ TWO LITERALS, JOINED AT RUNTIME. MSVC caps a single string literal at
    // 16,380 bytes -- NOT the 65,535 quoted elsewhere -- and this list crossed
    // it on 2026-08-31 when the stick keys landed -- "string too big, trailing characters truncated", which would
    // have shipped a keys list cut off mid-entry and a settings page missing
    // whatever came after the cut. Same limit build.ps1 already works around
    // for the embedded page, which is why that one is a byte array.
    //
    // ⚠️ Three pieces, split at structural boundaries -- buttons, pointer
    // settings, key names -- so each stays a whole readable thing. Two pieces
    // left under 400 bytes of headroom, which two more keys would have eaten,
    // and the failure is a silently truncated list rather than a loud error at
    // the point of use. Split again at the next boundary if one nears 16,000.
    // ⛔ CUSTOM DELIMITER, and it is load-bearing. A default raw string ends at
    // the first `)"` -- so a help line reading "...held down)" or a button
    // named "(A)" TERMINATES THE LITERAL EARLY, and the compiler then fails
    // somewhere unrelated. Both happened on 2026-09-01. With CTMKEYS the text
    // can contain anything except that exact delimiter.
    const std::string settings = R"CTMKEYS({"keys":[
{"key":"osk_program","type":"choice","choices":["steam","osk","overlay"],"default":"steam","help":"Which on-screen keyboard the OSKeyboard binding opens. Steam's is navigable with the controller and closes cleanly, but needs Steam running. Windows' own osk.exe always works, but its keys must be clicked with the cursor -- the d-pad cannot walk them. Overlay is this program's own: the d-pad walks it, it never takes focus from the game, and it needs nothing else installed."},
{"key":"rebind_debug","type":"bool","default":false,"help":"Logs what each bound button is doing, twice a second: whether it was seen as pressed, and the raw button bytes. For working out why a rebind does nothing."},
{"key":"rebind_0","type":"string","name":"(✕) | (A)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_0","type":"int","min":0,"max":1000,"name":"(✕) | (A)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_1","type":"string","name":"(○) | (B)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_1","type":"int","min":0,"max":1000,"name":"(○) | (B)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_2","type":"string","name":"(□) | (X)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_2","type":"int","min":0,"max":1000,"name":"(□) | (X)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_3","type":"string","name":"(△) | (Y)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_3","type":"int","min":0,"max":1000,"name":"(△) | (Y)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_4","type":"string","name":"(L1) | (LB)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_4","type":"int","min":0,"max":1000,"name":"(L1) | (LB)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_5","type":"string","name":"(R1) | (RB)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_5","type":"int","min":0,"max":1000,"name":"(R1) | (RB)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_6","type":"string","name":"(L2) | (LT)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_6","type":"int","min":0,"max":1000,"name":"(L2) | (LT)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_7","type":"string","name":"(R2) | (RT)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_7","type":"int","min":0,"max":1000,"name":"(R2) | (RT)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_8","type":"string","name":"(Create) | (Select)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_8","type":"int","min":0,"max":1000,"name":"(Create) | (Select)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_9","type":"string","name":"(Options) | (Start)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_9","type":"int","min":0,"max":1000,"name":"(Options) | (Start)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_10","type":"string","name":"(L3)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_10","type":"int","min":0,"max":1000,"name":"(L3)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_11","type":"string","name":"(R3)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_11","type":"int","min":0,"max":1000,"name":"(R3)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_12","type":"string","name":"(↑)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_12","type":"int","min":0,"max":1000,"name":"(↑)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_13","type":"string","name":"(↓)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_13","type":"int","min":0,"max":1000,"name":"(↓)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_14","type":"string","name":"(←)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_14","type":"int","min":0,"max":1000,"name":"(←)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_15","type":"string","name":"(→)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_15","type":"int","min":0,"max":1000,"name":"(→)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
{"key":"rebind_16","type":"string","name":"(PS) | (Home)","default":"","help":"Remap button to keyboard key or mouse action"},
{"key":"turbo_16","type":"int","min":0,"max":1000,"name":"(PS) | (Home)","default":0,"help":"Set milliseconds between rapid fire presses, 0 means held down"},
)CTMKEYS";
    // The button rebinds and turbos are the bulk of the list, so they are
    // their own piece -- the next-largest structural boundary after the key
    // names.
    const std::string pointers = R"CTMKEYS(
{"key":"stick_to_mouse","type":"choice","choices":["","right","left","both"],"default":"","help":"Which stick moves the mouse cursor. Blank is off. With both, whichever stick is pushed further drives, so the two never fight. The cursor moves at a speed while the stick is held, unlike gyro and the touchpad which follow movement."},
{"key":"stick_to_mouse_gate","type":"choice","choices":["always","L2","R2","L1","R1","touchpad","!touchpad","touchpad_click","PS"],"default":"always","help":"What must be held for the stick to move the cursor. Blank means always -- the stick works whenever it is enabled. Useful when the touchpad or gyro is also driving the cursor."},
{"key":"stick_mouse_speed","type":"int","min":50,"max":5000,"default":1200,"help":"Cursor pixels per second at full stick deflection. Time-based, so the speed does not change with the controller's report rate."},
{"key":"stick_mouse_deadzone","type":"int","min":0,"max":50,"default":15,"help":"Percent of stick travel ignored around the centre, so a resting stick does not drift the cursor. Movement grows from zero at the edge of it rather than jumping."},
{"key":"stick_mouse_curve","type":"choice","choices":["linear","quadratic","cubic"],"default":"quadratic","help":"How deflection maps to speed. Linear is direct. Quadratic slows the low end for precision. Cubic slows it further, so small pushes are very fine and the edge is still fast."},
{"key":"stick_to_scroll","type":"choice","choices":["","right","left","both"],"default":"","help":"Which stick scrolls, pushed up or down. Blank is off. Useful with gyro or stick cursor, where both thumbs are already committed and reaching the touchpad means regripping."},
{"key":"stick_to_scroll_gate","type":"choice","choices":["always","L2","R2","L1","R1","touchpad","!touchpad","touchpad_click","PS"],"default":"always","help":"What must be held for the stick to scroll. Blank means always."},
{"key":"stick_scroll_speed","type":"int","min":1,"max":60,"default":10,"help":"Wheel clicks per second at full push. Time-based, so it does not change with the controller's report rate."},
{"key":"stick_scroll_deadzone","type":"int","min":0,"max":60,"default":25,"help":"Percent of stick travel ignored around the centre. Larger than the cursor's by default, so a stick used for aiming does not scroll by accident."},
{"key":"stick_scroll_natural","type":"bool","default":false,"help":"Scroll direction. Off: pushing up scrolls the content up, like a wheel. On: inverted, the phone convention."},
{"key":"stick_mouse_invert","type":"int","min":0,"max":3,"default":0,"help":"1 inverts horizontal, 2 inverts vertical, 3 inverts both."},
{"key":"mouse_exclusive","type":"bool","default":false,"help":"Hide from the game whatever is driving the mouse. If the gyro moves the cursor, the game stops seeing the gyro; the same for a stick that points or scrolls, and for the touchpad. Without this you are aiming twice at once. The mouse-mode presets turn it on."},
{"key":"touchpad_to_mouse_gate","type":"choice","choices":["always","L2","R2","L1","R1","touchpad_click","PS"],"default":"always","help":"What must be held for the touchpad to move the cursor, scroll or tap. Blank means always -- the touchpad settings have their own switches, so an absent gate is not off."},
{"key":"touchpad_to_mouse","type":"bool","default":false,"help":"One finger on the touchpad moves the cursor, laptop-trackpad style. Relative: lift and reposition without the cursor jumping. Feeds the same virtual mouse as gyro."},
{"key":"touchpad_mouse_speed","type":"int","min":1,"max":400,"default":100,"help":"Touchpad cursor speed, percent. At 100 a full-pad swipe crosses roughly a full screen width."},
{"key":"touchpad_scroll","type":"bool","default":false,"help":"Two fingers on the touchpad scroll, like a laptop trackpad. Vertical only."},
{"key":"touchpad_scroll_speed","type":"int","min":1,"max":400,"default":100,"help":"Scroll speed, percent."},
{"key":"touchpad_scroll_natural","type":"bool","default":false,"help":"Scroll direction. Off: fingers down scrolls the page down, the classic wheel. On: content follows your fingers, the phone convention."},
{"key":"touchpad_click_drag","type":"bool","default":false,"help":"Click the touchpad in with a finger on it to grab, move to drag, then LIFT THE FINGER to drop -- the click itself can be released straight away. Trackpads call this drag lock; three-finger drag is not possible here because the pad reports only two touches."},
{"key":"touchpad_tap_click","type":"bool","default":false,"help":"A quick tap clicks: one finger is left click, two fingers is right click. Double-click is just tapping twice. Turn off if taps misfire in your grip."},
{"key":"gyro_to_mouse_gate","type":"choice","choices":["","always","L2","R2","L1","R1","touchpad","!touchpad","touchpad_click","PS"],"default":"","help":"What must be held for gyro to move the mouse. Blank is off."},
{"key":"gyro_mouse_px_per_360","type":"int","min":1000,"max":200000,"default":1920,"help":"Pixels the cursor travels for one full turn. 1920 means one turn crosses a 1080p screen, which is the calibrated figure -- if you need far more than that, the sensitivity settings are usually what is actually wrong."},
{"key":"gyro_mouse_min_sens","type":"int","min":0,"max":60,"default":8,"help":"Sensitivity for slow, precise movement. ⭐ 8 is the recommended starting point and 4-10 is the useful band; below about 4 slow movement becomes almost dead, which people then compensate for by raising everything else."},
{"key":"gyro_mouse_sens","type":"int","min":1,"max":200,"default":50,"help":"Legacy single sensitivity. Scales both tiers; 50 leaves them as shipped."},
{"key":"gyro_mouse_debug_gate","type":"bool","default":false,"help":"Logs the raw trigger and touchpad bytes twice a second, for diagnosing a gate that never opens."},
{"key":"gyro_mouse_debug_scale","type":"bool","default":false,"help":"Logs measured degrees turned, pixels emitted, report interval and rate twice a second. For finding why a usable speed needs a px_per_360 far above the calibrated figure."},
{"key":"gyro_mouse_max_sens","type":"int","min":0,"max":60,"default":16,"help":"Sensitivity for fast turns. ⭐ Set it EQUAL to min to turn acceleration off, and start there -- with a wide gap between them the same setting feels too slow and too fast depending on how fast you happen to be moving."},
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
])CTMKEYS";
    const std::string names = R"CTMKEYS(,
"key_names":[
"KeyA","KeyB","KeyC","KeyD","KeyE","KeyF","KeyG","KeyH","KeyI","KeyJ","KeyK",
"KeyL","KeyM","KeyN","KeyO","KeyP","KeyQ","KeyR","KeyS","KeyT","KeyU","KeyV",
"KeyW","KeyX","KeyY","KeyZ",
"Digit1","Digit2","Digit3","Digit4","Digit5","Digit6","Digit7","Digit8",
"Digit9","Digit0",
"Enter","Escape","Backspace","Tab","Space","Minus","Equal","BracketLeft",
"BracketRight","Backslash","Semicolon","Quote","Backquote","Comma","Period",
"Slash","CapsLock",
"F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
"Insert","Home","PageUp","Delete","End","PageDown",
"ArrowUp","ArrowDown","ArrowLeft","ArrowRight",
"Numpad0","Numpad1","Numpad2","Numpad3","Numpad4","Numpad5","Numpad6",
"Numpad7","Numpad8","Numpad9","NumpadEnter",
"F13","F14","F15","F16","F17","F18","F19","F20","F21","F22","F23","F24",
"ControlLeft","ShiftLeft","AltLeft","MetaLeft",
"ControlRight","ShiftRight","AltRight","MetaRight",
"MouseLeft","MouseRight","MouseMiddle","MouseWheelUp","MouseWheelDown",
"OSKeyboard"
]})CTMKEYS";
    return settings + pointers + names;
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

    // ---- Config mode -------------------------------------------------------
    //
    // ⭐ Four endpoints, all the same shape: set a flag, maybe touch a window.
    //
    // ⛔ RELEASE THE GATE BEFORE TOUCHING THE WINDOW, in every path. If the
    // window work throws or hangs, the release has already happened -- the gate
    // is what can strand someone mid-game; a leftover window is only untidy.
    //
    // ⓘ The TV drives these. Until the TV side exists they are called by hand
    // with curl, which is deliberate: it lets both halves be proven separately.
    if (req.path.rfind("/api/v1/ui/", 0) == 0) {
        if (req.method != "POST") {
            *out = rest_error_response(405, "method not allowed", "Allow: POST, OPTIONS\r\n");
            return true;
        }

        const std::string what = req.path.substr(11);

        // ⭐ RESET, not "open" -- named for what it does rather than what you
        // hoped for. It kills whatever window exists, sets the gate, and opens a
        // fresh one. Opening the first window is a special case of that, not a
        // separate action.
        //
        // ⓘ A separate reset endpoint was designed and dropped as redundant:
        // there is no state that survives this, so there is nothing left for one
        // to clear.
        //
        // ⚠️ Kill and recreate rather than focusing an existing window -- every
        // call then lands in a known state, with nothing carried over from a
        // window left in the middle of something.
        if (what == "reset" || what == "open") {
            // ⛔ CLEAR THE HOLD FIRST. Hold beats config mode by design, so a
            // reset with one still set would close the window, fail to gate,
            // and leave the caller believing it had worked.
            //
            // ⓘ This is what makes it a reset rather than an open: every path
            // into it lands in the same known state.
            // ⭐ Same call the chord makes, so the two cannot drift apart.
            ctm_chord_show_ui(std::string());   // no controller in hand here
            *out = rest_http_response(200, R"({"ok":true,"config_mode":true})");
            return true;
        }

        // ⭐ The page reporting FOCUS. The gate follows it, so clicking away
        // hands the pad straight back to the game -- and the window stays,
        // because one vanishing mid-edit is worse than the bookkeeping is good.
        if (what == "focus") {
            RestJson json;
            std::string parseError;
            if (!rest_parse_flat_json(req.body, &json, &parseError)) {
                // ⚠️ Says so rather than failing quietly. The page sends this
                // with a .catch that swallows errors, so a 400 here would
                // vanish entirely and look like the request was never made.
                device_log::input(device_log::msg()
                    << "ui/focus: body did not parse -- " << parseError
                    << " (body was: " << req.body << ")");
                *out = rest_error_response(400, parseError);
                return true;
            }
            device_log::input(device_log::msg()
                << "ui/focus: " << req.body);
            // ⓘ RestJson keeps parsed booleans in their own map, so a JSON
            // `true` arrives as a bool -- no string comparison needed.
            //
            // ⚠️ Absent means FALSE, deliberately: a malformed body should
            // release the gate rather than engage it. Releasing wrongly is a
            // nuisance; gating wrongly leaves someone unable to play.
            auto it = json.bools.find("focused");
            const bool on = (it != json.bools.end()) && it->second;
            // ⭐ A page claiming focus CONFIRMS a provisional gate -- the
            // window came forward, so the failsafe can stand down.
            if (on) ctm_rebind_clear_provisional();
            ctm_rebind_set_config_mode(on);
            *out = rest_http_response(200, on ? R"({"ok":true,"config_mode":true})"
                                              : R"({"ok":true,"config_mode":false})");
            return true;
        }

        // The page reporting its own teardown, sent with navigator.sendBeacon
        // so it survives the page being torn down.
        // ⭐ THE MANUAL ESCAPE HATCH. Stops gating and STAYS off, whatever
        // focus does -- otherwise clicking the page would turn it straight back
        // on and the control would look broken.
        //
        // ⓘ Two reasons to want it: adjusting settings at the desk while not
        // minding that the game sees the pad, and getting out when something is
        // stuck. The second is why it must not live only in a config file --
        // one you would have to find and hand-edit is not reachable in the
        // moment you need it.
        if (what == "hold") {
            RestJson json;
            std::string parseError;
            if (!rest_parse_flat_json(req.body, &json, &parseError)) {
                *out = rest_error_response(400, parseError);
                return true;
            }
            auto it = json.bools.find("hold");
            const bool hold = (it != json.bools.end()) && it->second;
            ctm_rebind_set_gate_hold(hold);
            *out = rest_http_response(200, hold ? R"({"ok":true,"hold":true})"
                                                : R"({"ok":true,"hold":false})");
            return true;
        }

        // ⭐ OPEN ONLY, never close. For a page that has found itself in an
        // ordinary browser tab and wants a proper window.
        //
        // ⛔ It must NOT use reset: close_existing matches windows by TITLE, and
        // the browser window holding that tab has the same title -- so reset
        // closed the person's entire browser. Measured 2026-08-29.
        if (what == "spawn") {
            ctm_open_ui::open_new(g_rest_port);
            *out = rest_http_response(200, R"({"ok":true})");
            return true;
        }

        if (what == "closed") {
            // ⓘ Logged because it was silent: a close produced a focus report
            // but no beacon line, so there was no way to tell whether the
            // beacon fired at all. Focus happening to fire on close is luck --
            // this is the path meant to be reliable.
            device_log::input(device_log::msg()
                << "ui/closed: the page reported its own teardown");
            ctm_rebind_set_config_mode(false);
            *out = rest_http_response(200, R"({"ok":true,"config_mode":false})");
            return true;
        }

        *out = rest_error_response(404, "unknown ui action");
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
    // ⭐ The presets a UI can offer. ⓘ Listed by the agent rather than
    // hardcoded in the page, for the same reason /api/v1/keys exists: two
    // places naming the same thing is two places to disagree.
    if (req.path == "/api/v1/presets") {
        if (req.method != "GET") {
            *out = rest_error_response(405, "method not allowed", "Allow: GET, OPTIONS\r\n");
            return true;
        }
        std::string body = "{\"presets\":[";
        for (size_t i = 0; i < ctm_presets::preset_count(); ++i) {
            const ctm_presets::Preset &p = ctm_presets::kPresets[i];
            if (i) body += ",";
            body += "{\"name\":\"" + rest_json_escape(p.name) + "\"";
            body += ",\"help\":\"" + rest_json_escape(p.help) + "\"";
            // ⭐ The settings themselves, so a UI can show what the preset
            // DOES rather than a sentence about it. ⓘ Derived display beats a
            // written description: the two cannot drift apart.
            body += ",\"settings\":[";
            for (size_t s = 0; s < p.count; ++s) {
                if (s) body += ",";
                body += "{\"key\":\"" + rest_json_escape(p.settings[s].key) + "\"";
                body += ",\"value\":\"" + rest_json_escape(p.settings[s].value) + "\"}";
            }
            body += "]";
            body += ",\"kinds\":[";
            bool first = true;
            if (p.ds5) { body += "\"ds5\""; first = false; }
            if (p.ds5_edge) { body += first ? "\"ds5_edge\"" : ",\"ds5_edge\""; }
            body += "]}";
        }
        body += "]}";
        *out = rest_http_response(200, body);
        return true;
    }

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
            // ⭐ An optional preset to start from. Absent means blank, which
            // is what create has always done -- so the plain path is
            // unchanged and a blank config is still one press away.
            const ctm_presets::Preset *preset = nullptr;
            auto presetIt = json.strings.find("preset");
            if (presetIt != json.strings.end() && !presetIt->second.empty()) {
                preset = ctm_presets::find(presetIt->second);
                if (preset == nullptr) {
                    *out = rest_error_response(400, "no preset named " + presetIt->second);
                    return true;
                }
                const std::string settingsKind = config_store::settings_kind_for(kind);
                if (!ctm_presets::suits(*preset, settingsKind)) {
                    // ⛔ Named rather than ignored: a preset that cannot act on
                    // this controller would be a config that silently does
                    // nothing, which is the worst shape a setting can take.
                    *out = rest_error_response(400,
                        std::string(preset->name) + " is not for " + settingsKind);
                    return true;
                }
            }

            if (!config_store::create_config(nameIt->second, kind, &error)) {
                *out = rest_error_response(409, error);
                return true;
            }

            if (preset != nullptr) {
                // ⓘ Through set_setting, the same comment-preserving writer
                // everything else uses -- so a preset config reads like any
                // hand-written one and can be edited the same way.
                for (size_t i = 0; i < preset->count; ++i) {
                    std::string writeError;
                    if (!config_store::set_setting(nameIt->second,
                                                   preset->settings[i].key,
                                                   preset->settings[i].value,
                                                   &writeError)) {
                        // ⚠️ The config EXISTS at this point. Reporting the
                        // failure and leaving it is honest; deleting it behind
                        // the user's back would be worse.
                        *out = rest_error_response(500,
                            "created " + nameIt->second + " but could not write " +
                            preset->settings[i].key + ": " + writeError);
                        return true;
                    }
                }
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

        if (action == "copy") {
            auto nameIt = json.strings.find("name");
            if (nameIt == json.strings.end()) {
                *out = rest_error_response(400, "name is required");
                return true;
            }
            if (!config_store::copy_config(name, nameIt->second, &error)) {
                *out = rest_error_response(409, error);
                return true;
            }
            // \u24d8 The copy is NOT linked here. Linking is a separate verb the
            // UI can call, and a copy made from Overview belongs to no
            // controller -- deciding for it would be guessing.
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
