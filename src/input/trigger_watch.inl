// Watching the adaptive trigger fields.
//
// ⭐ WHY THIS EXISTS. To replace a trigger effect you first have to know what
// the game sends -- and it cannot be read from another tool. dualsense-tester
// was tried while Stellar Blade was running and every trigger setting REVERTED,
// because the game re-claims those fields on every report. Anything that wants
// to win has to sit in the path, which is here.
//
// ⛔ LOGS ONLY WHAT CHANGED. The game drives both triggers on every report at
// roughly 250 Hz, so logging each one gives 250 identical lines a second and
// buries the moment that matters. A transition is exactly when you aim, fire,
// or switch ammo -- the events worth seeing.
//
// ⓘ Field positions from dualsense-tester's own map, cross-checked against the
// 2026-07-31 C3 capture: mode at byte 11 for R2 and 22 for L2, parameters in
// the ten bytes after each. Claim bits 2 and 3 of byte 1 say the game is
// asserting them.

#pragma once

namespace ctm_trigger_watch {

// R2 occupies bytes 11-21, L2 bytes 22-32, of output report 0x02.
constexpr size_t kR2Offset = 11;
constexpr size_t kL2Offset = 22;
constexpr size_t kBlockLen = 11;
constexpr size_t kNeeded = kL2Offset + kBlockLen;   // 33

// ⭐ When a replacement last fired. The fire path gates on this, so R2 only
// clicks while the game is actually sending the effect we are replacing.
//
// ⛔ Without it, R2 clicks the mouse EVERYWHERE -- in menus, on the desktop,
// in other windows. The trigger is just a trigger; it has no idea a game is in
// front. The match does.
inline std::atomic<long long> g_lastMatchMs{0};

inline long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline std::mutex g_mutex;
inline std::map<const void *, std::array<uint8_t, kNeeded>> g_last;

inline std::string hex_block(const uint8_t *p, size_t n)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        if (i) out += ' ';
        out += digits[p[i] >> 4];
        out += digits[p[i] & 0x0f];
    }
    return out;
}

// ⚠️ Names for the modes we know. Anything else prints as a bare number rather
// than a guess -- a wrong name here would be worse than no name, because it
// would be believed.
inline const char *mode_name(uint8_t mode)
{
    switch (mode) {
        case 0x00: return "none";
        case 0x05: return "off";
        case 0x21: return "feedback";
        case 0x25: return "weapon";
        case 0x26: return "vibration";
        case 0x27: return "machine";
        default:   return "?";
    }
}

// Call with the full output report, report id included at data[0].
//
// !! Never changes anything. This is observation only -- the replace comes
// !! later, and only once there is something real to match against.
inline void observe(const void *deviceKey,
                    const std::vector<unsigned char> &descriptor,
                    const std::string &linkedConfig,
                    const uint8_t *data, size_t len)
{
    if (data == nullptr || len < kNeeded) return;

    // ⓘ Resolves its own section, the same way the gyro path does -- the kind
    // comes from the descriptor and the linked config namespaces it.
    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) return;                // not a DualSense
    const std::string section = device_settings_section(kind, linkedConfig);
    if (!device_config_bool(section.c_str(), "trigger_watch", false)) return;

    // ⛔ NO +1. The byte numbers above are absolute within the report, WITH the
    // report id at index 0 -- confirmed against a real capture:
    //
    //   02 0c 57 00 00 00 00 00 00 00 00 25 0c 00 ...
    //    0  1  2  3  4  5  6  7  8  9 10 11 12 13
    //
    // 0x25 is at index 11 and L2's mode at index 22, both exactly as
    // dualsense-tester documents. Skipping the id first read index 12 instead,
    // which is the second byte of the block, so nothing ever matched.
    std::array<uint8_t, kNeeded> now{};
    memcpy(now.data(), data, kNeeded);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_last.find(deviceKey);
        if (it != g_last.end()) {
            const bool sameR2 = memcmp(it->second.data() + kR2Offset,
                                       now.data() + kR2Offset, kBlockLen) == 0;
            const bool sameL2 = memcmp(it->second.data() + kL2Offset,
                                       now.data() + kL2Offset, kBlockLen) == 0;
            if (sameR2 && sameL2) return;          // nothing to say
        }
        g_last[deviceKey] = now;
    }

    const uint8_t *r2 = now.data() + kR2Offset;
    const uint8_t *l2 = now.data() + kL2Offset;

    // ⓘ The claim bits matter as much as the values: they say whether the game
    // is asserting the triggers this report or merely leaving the bytes set.
    const bool claimsTriggers = (now[1] & 0x0c) != 0;

    device_log::config(device_log::msg()
        << "[trigger] R2 " << mode_name(r2[0]) << " " << hex_block(r2, kBlockLen)
        << "  |  L2 " << mode_name(l2[0]) << " " << hex_block(l2, kBlockLen)
        << (claimsTriggers ? "  [claimed]" : "  [not claimed]"));
}

// ---- Replacement ------------------------------------------------------------
//
// ⭐ MATCH AND REPLACE, not a blanket override. The game sends a different
// block for each weapon, so matching on the exact bytes changes ONE weapon and
// leaves every other alone -- switch ammo and the block no longer matches, so
// nothing happens.
//
// ⓘ Measured in Stellar Blade, 2026-08-24:
//     machine gun  R2 = 25 0c 00 ...     mode 0x25, weapon
//     shotgun      R2 = 25 04 01 07 ...  mode 0x25, weapon
//
// ⚠️ Both are WEAPON mode, which is why the machine gun gives one bump: weapon
// mode resists to a position and breaks, like a gun hammer. It has no
// oscillator, so it physically cannot rattle. A continuous stream needs mode
// 0x26 (vibration) or 0x27 (machine), which take a frequency and keep going.
//
// ⛔ RAW BYTES, deliberately. The parameter layout within each mode is not
// something this project has verified, and naming fields we have not confirmed
// would be a guess presented as fact. Raw hex lets the values be found by feel
// -- the config watcher pushes to a live session in about a second, so it is
// change, save, squeeze.

// Parses "25 0c 00" into bytes. Anything unparseable yields empty, which the
// caller treats as "not configured" rather than as an error -- a typo should
// leave the game's own effect alone, not break the trigger.
inline std::vector<uint8_t> parse_hex(const std::string &text)
{
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !isxdigit(static_cast<unsigned char>(text[i]))) ++i;
        if (i + 1 >= text.size()) break;
        if (!isxdigit(static_cast<unsigned char>(text[i + 1]))) return {};
        out.push_back(static_cast<uint8_t>(std::stoi(text.substr(i, 2), nullptr, 16)));
        i += 2;
    }
    return out;
}

// Rewrites the R2 block in place when it matches. Returns true if it did.
//
// ⚠️ The match is a PREFIX. The game may vary later bytes with ammo count or
// heat, and requiring the whole block to match would then work intermittently
// -- which is worse than not working, because it looks like a different fault
// every time.
inline bool maybe_replace(const std::string &section, uint8_t *body, size_t len)
{
    if (body == nullptr || len < kR2Offset + kBlockLen) return false;

    // ⓘ device_config_str lowercases what it returns, which is harmless for hex.
    const std::string matchText = device_config_str(section.c_str(), "trigger_r2_match");
    const std::string withText = device_config_str(section.c_str(), "trigger_r2_replace");
    if (matchText.empty() || withText.empty()) return false;

    const std::vector<uint8_t> want = parse_hex(matchText);
    const std::vector<uint8_t> with = parse_hex(withText);
    if (want.empty() || with.empty() || want.size() > kBlockLen) return false;

    if (memcmp(body + kR2Offset, want.data(), want.size()) != 0) return false;

    // Replace the WHOLE block, not just the matched prefix: leaving the tail of
    // a weapon effect behind a machine-mode header would feed one mode's
    // parameters to another.
    g_lastMatchMs.store(now_ms(), std::memory_order_relaxed);

    memset(body + kR2Offset, 0, kBlockLen);
    memcpy(body + kR2Offset, with.data(),
           (std::min)(with.size(), kBlockLen));

    // ⭐ Say so, once per second at most.
    //
    // ⛔ Without this, "nothing changed" has two causes that look identical:
    // the match never fired, or it fired and the replacement bytes are wrong.
    // Those need completely different fixes, so the log has to separate them.
    {
        static auto lastSaid = std::chrono::steady_clock::now() - std::chrono::hours(1);
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSaid).count() >= 1000) {
            lastSaid = now;
            device_log::config(device_log::msg()
                << "[trigger] R2 replaced -- matched " << matchText
                << ", sent " << hex_block(body + kR2Offset, kBlockLen));
        }
    }
    return true;
}

// Is a replacement configured at all? Checked before copying the report, so
// the common case -- no override -- costs two config lookups and no allocation.
inline bool wants_replace(const std::vector<unsigned char> &descriptor,
                          const std::string &linkedConfig)
{
    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) return false;
    const std::string section = device_settings_section(kind, linkedConfig);
    return !device_config_str(section.c_str(), "trigger_r2_match").empty() &&
           !device_config_str(section.c_str(), "trigger_r2_replace").empty();
}

// Resolves the section, then replaces. Separate from maybe_replace so the
// matching logic can be reasoned about without the config plumbing.
inline bool replace_if_matched(const std::vector<unsigned char> &descriptor,
                               const std::string &linkedConfig,
                               uint8_t *data, size_t len)
{
    if (data == nullptr || len < kNeeded) return false;
    const char *kind = device_section_for(descriptor);
    if (kind == nullptr) return false;
    const std::string section = device_settings_section(kind, linkedConfig);
    return maybe_replace(section, data, len);
}

// Was a replacement active recently?
//
// ⚠️ 250 ms, not "this instant". A game re-sends the effect only when it
// changes, so between sends there is nothing to observe -- requiring a match
// in the same report would mean firing stopped a millisecond after aiming.
inline bool replacement_active()
{
    const long long last = g_lastMatchMs.load(std::memory_order_relaxed);
    return last != 0 && (now_ms() - last) < 250;
}

inline void forget(const void *deviceKey)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_last.erase(deviceKey);
}

} // namespace ctm_trigger_watch
