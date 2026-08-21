// Per-controller config files.
//
// WHAT THIS ADDS. Settings are keyed by device TYPE today -- every DualSense
// reads [ds5]. Two controllers therefore cannot be tuned separately, which
// matters most for gyro sensitivity: a personal setting that is felt rather
// than reasoned about.
//
// A config file in configs/ can be LINKED to a specific bridged controller.
// Linked devices read that file; everything else keeps reading the shared
// section exactly as before, so a single-controller user never meets any of
// this.
//
// ⭐ THE DECISION THAT KEPT THIS SMALL. A config file's settings live under the
// DEVICE KIND -- a ds5 config holds a [ds5] block, the same shape as the shared
// ctm-device-config.txt. Loading namespaces it:
//
//     configs/couch.txt   [ds5] speaker_volume = 65
//         becomes         g_device_config["cfg:couch/ds5"]["speaker_volume"]
//
// So "link a device to a config" is just "look up a different section name",
// and device_config_str/int/bool are UNCHANGED and unaware any of this exists.
// The alternative -- a settings block with its own section name -- would have
// meant teaching every accessor about the indirection.
//
// FILE FORMAT
//     [config]
//     kind      = ds5                  ; must match the device it links to
//     auto_link = aabbccddeeff         ; serials linked automatically at bridge
//
//     [ds5]                            ; settings, named for the kind
//     speaker_volume = 65
//
// !! ABSENT IS NOT DEFAULTED. A key a file does not mention is left alone,
// !! exactly as with no config at all. That is the existing rule and nothing
// !! here changes it.

#pragma once

namespace config_store {

inline const char *kDir = "configs";
inline const char *kArchiveDir = "configs\\archive";

struct ConfigFile {
    std::string name;                       // filename stem
    std::string kind;                       // "ds5" | "ds5_edge"
    std::vector<std::string> autoLink;      // normalised serials
    std::string path;
};

inline std::mutex g_mutex;
inline std::map<std::string, ConfigFile> g_files;   // keyed by lowered name

inline std::string trim(const std::string &text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

inline std::string lower(std::string text)
{
    for (char &c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// Safe as a filename AND as a section fragment. Rejecting here means never
// sanitising later, and stops a name escaping configs/ or forging a section.
inline bool valid_name(const std::string &name)
{
    if (name.empty() || name.size() > 48) return false;
    // ⛔ "archive" is where archived configs go. A config file of that name
    // would sit beside a directory of the same stem, which is legal on NTFS
    // and confusing to everyone.
    if (name == "archive" || name == "Archive" || name == "ARCHIVE") return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// ⛔ DS5 family only. Not because other devices cannot have settings, but
// because auto_link needs a trustworthy per-unit serial and only these are
// established to have one. Some devices report nothing; some report the SAME
// value for every unit, which would silently share settings between two
// controllers while looking like they were configured separately -- worse than
// not offering the feature. Manual linking could open up later, after
// measurement rather than assumption.
inline bool kind_supports_config(const std::string &kind)
{
    return kind == "ds5" || kind == "ds5_edge";
}

// ⭐ The namespaced section name -- the whole mechanism.
inline std::string section_for(const std::string &configName, const std::string &kind)
{
    if (configName.empty()) return kind;                 // shared section
    return "cfg:" + lower(configName) + "/" + lower(kind);
}

inline std::string path_for(const std::string &name)
{
    return std::string(kDir) + "\\" + name + ".txt";
}

// Normalises a serial identically everywhere: lower case, alphanumerics only,
// so a MAC matches however the TV punctuated it.
inline std::string normalise_serial(const std::string &raw)
{
    std::string out;
    for (char c : raw) {
        if (c >= 'A' && c <= 'Z') out.push_back(static_cast<char>(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
        if (out.size() >= 32) break;
    }
    return out;
}

// ⛔ A key or value goes straight into the file, so anything that could forge
// a line must be refused BEFORE the write. A newline plus a bracket writes a
// new section header into the middle of the file; a '#' comments out the rest
// of the line; an '=' in a key makes the line unparseable. The damage outlives
// the request and corrupts the file for every reader.
//
// ⓘ The line-protocol version of this feature had exactly this guard. It was
// lost when the transport changed to REST -- worth remembering that a rewrite
// drops safeguards silently.
inline bool valid_setting_key(const std::string &key)
{
    if (key.empty() || key.size() > 64) return false;
    for (char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

inline bool valid_setting_value(const std::string &value)
{
    if (value.size() > 256) return false;
    // '!' is allowed: the gate value "!touchpad" needs it.
    return value.find_first_of("\r\n[]=#;") == std::string::npos;
}

inline bool ensure_dir(const char *dir)
{
    const std::wstring w(dir, dir + strlen(dir));
    if (CreateDirectoryW(w.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// Parses one file and pushes its settings into g_device_config under the
// namespaced section. Caller holds g_mutex.
inline bool load_one_locked(const std::string &name, ConfigFile *out)
{
    std::ifstream file(path_for(name));
    if (!file.is_open()) return false;

    out->name = name;
    out->path = path_for(name);
    out->kind.clear();
    out->autoLink.clear();

    std::string section, line;
    std::vector<std::pair<std::string, std::string>> pending;

    while (std::getline(file, line)) {
        const size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) line.erase(comment);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos || section.empty()) continue;
        const std::string key = lower(trim(line.substr(0, equals)));
        const std::string value = trim(line.substr(equals + 1));

        if (section == "config") {
            if (key == "kind") out->kind = lower(value);
            else if (key == "auto_link") {
                std::string current;
                for (char c : value + ",") {
                    if (c == ',' || c == ' ' || c == '\t') {
                        const std::string s = normalise_serial(current);
                        if (!s.empty()) out->autoLink.push_back(s);
                        current.clear();
                    } else current.push_back(c);
                }
            }
        } else {
            pending.emplace_back(section, key + "=" + value);
        }
    }

    if (out->kind.empty()) return false;         // no kind, not a config

    // ⚠️ Only a block matching the declared kind is taken. A [ds5] block inside
    // a ds5_edge config is a mistake, and honouring it silently would make the
    // file behave differently from how it reads.
    for (const auto &entry : pending) {
        if (entry.first != out->kind) continue;
        const size_t eq = entry.second.find('=');
        g_device_config[section_for(name, out->kind)][entry.second.substr(0, eq)] =
            entry.second.substr(eq + 1);
    }
    return true;
}

// Rebuilds the registry from disk. Cheap -- these files are tiny.
inline void reload_all()
{
    // ⚠️ LOCK ORDER: g_mutex, then g_device_config_mutex. Never the reverse.
    // g_device_config is read by device_config_str/int/bool from the input
    // path at ~250 reports/sec per controller, while this runs on the agent
    // loop thread -- so mutating it unguarded was a genuine data race on a
    // std::map, whose failure mode is a crash or garbage rather than a clean
    // error.
    std::lock_guard<std::mutex> lock(g_mutex);
    std::lock_guard<std::mutex> configLock(g_device_config_mutex);

    // Drop every previously loaded config section first, so a deleted or
    // edited file cannot leave stale values behind.
    for (auto it = g_device_config.begin(); it != g_device_config.end(); ) {
        if (it->first.rfind("cfg:", 0) == 0) it = g_device_config.erase(it);
        else ++it;
    }
    g_files.clear();

    const std::wstring pattern = std::wstring(kDir, kDir + strlen(kDir)) + L"\\*.txt";
    WIN32_FIND_DATAW find = {};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &find);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring wname(find.cFileName);
        const std::string fname(wname.begin(), wname.end());
        if (fname.size() < 5) continue;
        const std::string stem = fname.substr(0, fname.size() - 4);
        if (!valid_name(stem)) continue;
        ConfigFile cfg;
        if (load_one_locked(stem, &cfg)) g_files[lower(stem)] = cfg;
    } while (FindNextFileW(handle, &find));
    FindClose(handle);

    device_log::config(device_log::msg()
        << "loaded " << g_files.size() << " controller config(s) from " << kDir);
}

inline std::vector<ConfigFile> list_configs()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<ConfigFile> out;
    for (const auto &entry : g_files) out.push_back(entry.second);
    return out;
}

inline bool find_config(const std::string &name, ConfigFile *out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_files.find(lower(name));
    if (it == g_files.end()) return false;
    *out = it->second;
    return true;
}

// The config a serial auto-links to, or empty. First match wins in whatever
// order the map gives -- deliberately not resolved with extra ordering logic,
// because add_auto_link refuses to create a duplicate claim in the first place.
inline std::string auto_link_for(const std::string &serial, const std::string &kind)
{
    const std::string s = normalise_serial(serial);
    if (s.empty()) return std::string();
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto &entry : g_files) {
        if (entry.second.kind != kind) continue;
        for (const std::string &claim : entry.second.autoLink) {
            if (claim == s) return entry.second.name;
        }
    }
    return std::string();
}

inline std::string claimed_by(const std::string &serial, const std::string &exceptName)
{
    const std::string s = normalise_serial(serial);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto &entry : g_files) {
        if (lower(entry.second.name) == lower(exceptName)) continue;
        for (const std::string &claim : entry.second.autoLink) {
            if (claim == s) return entry.second.name;
        }
    }
    return std::string();
}

inline bool create_config(const std::string &name, const std::string &kind, std::string *error)
{
    if (!valid_name(name)) { *error = "name must be letters, digits, _ or - (max 48)"; return false; }
    if (!kind_supports_config(kind)) { *error = "config unsupported for kind " + kind; return false; }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_files.count(lower(name))) { *error = name + " already exists"; return false; }
    }
    if (!ensure_dir(kDir)) { *error = "could not create " + std::string(kDir); return false; }

    std::ofstream file(path_for(name), std::ios::trunc);
    if (!file.is_open()) { *error = "could not write " + path_for(name); return false; }

    // ⭐ The settings block is written EMPTY, not pre-filled with defaults.
    // "All settings at defaults" and "nothing overridden" are the same thing
    // here, because an absent key is already left alone. Writing a default for
    // every key would need a registry of keys and defaults that does not exist,
    // and would go stale the moment someone adds a key and forgets to register
    // it.
    file << "# " << name << "\r\n#\r\n"
         << "# Settings for one or more " << kind << " controllers.\r\n"
         << "#\r\n"
         << "# A key that is ABSENT is left alone -- it is not defaulted. So an\r\n"
         << "# empty block below behaves exactly as no config at all, and every\r\n"
         << "# line added is a deliberate override.\r\n\r\n"
         << "[config]\r\n"
         << "kind = " << kind << "\r\n"
         << "# Serials linked to this config automatically at bridge time.\r\n"
         << "auto_link =\r\n\r\n"
         << "[" << kind << "]\r\n";
    file.close();
    reload_all();
    return true;
}

// ⭐ ARCHIVE, NOT DELETE. Moving the file to configs/archive/ means nothing is
// ever destroyed, the watcher's existing "file vanished" path handles the
// fallback for free, and restoring is a drag in Explorer rather than a verb
// nobody remembers.
inline bool archive_config(const std::string &name, std::string *error, std::string *movedTo)
{
    ConfigFile cfg;
    if (!find_config(name, &cfg)) { *error = "no config named " + name; return false; }
    if (!ensure_dir(kDir) || !ensure_dir(kArchiveDir)) {
        *error = "could not create " + std::string(kArchiveDir); return false;
    }
    // ⚠️ NEVER overwrite an already-archived copy. Archiving "couch",
    // recreating it and archiving again would otherwise destroy the first --
    // the exact loss that archive-instead-of-delete exists to prevent. Suffix
    // on collision instead.
    std::string target = std::string(kArchiveDir) + "\\" + cfg.name + ".txt";
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const std::wstring probe(target.begin(), target.end());
        if (GetFileAttributesW(probe.c_str()) == INVALID_FILE_ATTRIBUTES) break;
        target = std::string(kArchiveDir) + "\\" + cfg.name + "-" +
                 std::to_string(suffix) + ".txt";
    }
    const std::wstring wFrom(cfg.path.begin(), cfg.path.end());
    const std::wstring wTo(target.begin(), target.end());
    if (!MoveFileExW(wFrom.c_str(), wTo.c_str(), 0)) {
        *error = "could not move " + cfg.path; return false;
    }
    *movedTo = target;
    reload_all();
    return true;
}

// Rewrites one key in a config file, preserving everything else -- comments,
// ordering, blank lines, and any key not named. A key present but commented
// out is uncommented in place rather than duplicated.
inline bool set_setting(const std::string &name, const std::string &key,
                        const std::string &value, std::string *error)
{
    if (!valid_setting_key(key)) {
        *error = "key must be letters, digits or _ (max 64): " + key;
        return false;
    }
    if (!valid_setting_value(value)) {
        *error = "value may not contain [ ] = # ; or a newline (max 256)";
        return false;
    }
    ConfigFile cfg;
    if (!find_config(name, &cfg)) { *error = "no config named " + name; return false; }

    std::vector<std::string> lines;
    {
        std::ifstream in(cfg.path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    std::string current;
    int sectionStart = -1;
    int sectionEnd = static_cast<int>(lines.size());
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const std::string t = trim(lines[i]);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
            current = lower(trim(t.substr(1, t.size() - 2)));
            if (current == cfg.kind) sectionStart = i;
            else if (sectionStart >= 0 && sectionEnd == static_cast<int>(lines.size())) sectionEnd = i;
            continue;
        }
        if (current != cfg.kind) continue;
        std::string bare = t;
        if (!bare.empty() && (bare.front() == '#' || bare.front() == ';')) bare = trim(bare.substr(1));
        const size_t eq = bare.find('=');
        if (eq == std::string::npos) continue;
        if (lower(trim(bare.substr(0, eq))) != lower(key)) continue;
        std::string indent;
        for (char c : lines[i]) { if (c == ' ' || c == '\t') indent.push_back(c); else break; }
        lines[i] = indent + key + " = " + value;
        std::ofstream out(cfg.path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) { *error = "could not write " + cfg.path; return false; }
        for (const std::string &l : lines) out << l << "\r\n";
        out.close();
        reload_all();
        return true;
    }

    if (sectionStart < 0) {
        lines.push_back("");
        lines.push_back("[" + cfg.kind + "]");
        lines.push_back(key + " = " + value);
    } else {
        int insert = sectionEnd;
        while (insert > sectionStart + 1 && trim(lines[insert - 1]).empty()) --insert;
        lines.insert(lines.begin() + insert, key + " = " + value);
    }
    std::ofstream out(cfg.path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) { *error = "could not write " + cfg.path; return false; }
    for (const std::string &l : lines) out << l << "\r\n";
    out.close();
    reload_all();
    return true;
}

inline bool set_auto_link_line(const ConfigFile &cfg, const std::string &joined, std::string *error)
{
    std::vector<std::string> lines;
    {
        std::ifstream in(cfg.path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    bool wrote = false;
    for (std::string &line : lines) {
        const std::string t = trim(line);
        if (t.rfind("auto_link", 0) == 0 && t.find('=') != std::string::npos) {
            line = "auto_link = " + joined;
            wrote = true;
            break;
        }
    }
    if (!wrote) { *error = cfg.name + " has no auto_link line"; return false; }
    std::ofstream out(cfg.path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) { *error = "could not write " + cfg.path; return false; }
    for (const std::string &l : lines) out << l << "\r\n";
    out.close();
    reload_all();
    return true;
}

inline bool add_auto_link(const std::string &name, const std::string &serial, std::string *error)
{
    ConfigFile cfg;
    if (!find_config(name, &cfg)) { *error = "no config named " + name; return false; }
    const std::string s = normalise_serial(serial);
    if (s.empty()) { *error = "no usable serial"; return false; }

    // ⭐ THIS REFUSAL IS THE POINT OF THE VERB. Two configs claiming one serial
    // is resolvable but ambiguous; refusing here keeps it unreachable rather
    // than merely tolerable, which is worth more than the typing it saves.
    const std::string other = claimed_by(s, name);
    if (!other.empty()) { *error = s + " is already claimed by " + other; return false; }

    for (const std::string &existing : cfg.autoLink) if (existing == s) return true;

    std::vector<std::string> updated = cfg.autoLink;
    updated.push_back(s);
    std::string joined;
    for (size_t i = 0; i < updated.size(); ++i) { if (i) joined += ", "; joined += updated[i]; }
    return set_auto_link_line(cfg, joined, error);
}

inline bool remove_auto_link(const std::string &name, const std::string &serial, std::string *error)
{
    ConfigFile cfg;
    if (!find_config(name, &cfg)) { *error = "no config named " + name; return false; }
    const std::string s = normalise_serial(serial);
    std::string joined;
    for (const std::string &existing : cfg.autoLink) {
        if (existing == s) continue;
        if (!joined.empty()) joined += ", ";
        joined += existing;
    }
    return set_auto_link_line(cfg, joined, error);
}

} // namespace config_store
