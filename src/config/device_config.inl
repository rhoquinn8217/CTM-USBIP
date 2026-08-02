// -----------------------------------------------------------------------------
// Device configuration store.
//
// Per-device settings, read from a plain text file beside the agent. Windows
// owns these settings: the TV sends nothing and needs to know nothing about
// them, so an unmodified client is unaffected.
//
// The cache is dropped whenever a bridge session starts, and refilled on the
// next lookup. That is what makes editing the file and re-plugging the
// controller apply the change -- no reload command and no protocol are needed.
// Within a session the values are stable, so a setting cannot change under a
// running game.
//
// The format deliberately mirrors the .map files -- same [section] and
// key = value shape -- so there is no new syntax to learn:
//
//   [ds5]
//   force_echo_cancel = true
//
// Unknown sections and unknown keys are ignored, so a newer file still works
// with an older build. A missing file means every setting takes its built-in
// default, which is exactly how the agent behaved before this existed.
//
// NOTE: trailing comments ARE stripped here, with '#' or ';'. The .map parser
// does not strip them, and a trailing comment there silently turns a flag off.
// The two formats look alike but this one does not carry that trap.
//
// The path is relative to the agent's working directory, which is where the
// maps folder also resolves from. If this ever needs to work for the installed
// service as well, it should move to the same asset-finding helper the maps
// use.
// -----------------------------------------------------------------------------

static const char *const kDeviceConfigFileName = "ctm-device-config.txt";

static std::mutex g_device_config_mutex;
static bool g_device_config_loaded = false;
static std::map<std::string, std::map<std::string, std::string>> g_device_config;

static std::string device_config_trim(const std::string &text)
{
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

static std::string device_config_lower(std::string text)
{
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] >= 'A' && text[i] <= 'Z') {
            text[i] = static_cast<char>(text[i] - 'A' + 'a');
        }
    }
    return text;
}

// Caller must hold g_device_config_mutex.
static void device_config_load_locked()
{
    g_device_config_loaded = true;
    std::ifstream file(kDeviceConfigFileName);
    if (!file.is_open()) {
        device_log::config(device_log::msg()
            << "no " << kDeviceConfigFileName << " found, using built-in defaults");
        return;
    }

    std::string section;
    std::string line;
    size_t entries = 0;
    while (std::getline(file, line)) {
        const size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        line = device_config_trim(line);
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = device_config_lower(device_config_trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos || section.empty()) {
            continue;
        }
        const std::string key = device_config_lower(device_config_trim(line.substr(0, equals)));
        if (key.empty()) {
            continue;
        }
        g_device_config[section][key] = device_config_trim(line.substr(equals + 1));
        ++entries;
    }

    device_log::config(device_log::msg()
        << "loaded " << entries << " setting(s) from " << kDeviceConfigFileName);
}

// Drop the cached copy so the next lookup re-reads the file. Called when a
// bridge session starts: that is what turns a virtual reseat into "apply my
// edited settings", which is the model this file exists to serve.
static void device_config_invalidate()
{
    std::lock_guard<std::mutex> guard(g_device_config_mutex);
    g_device_config_loaded = false;
    g_device_config.clear();
}

// Look up a whole-number setting. Returns fallback when the file, section, key
// or a parsable value is missing. Callers use a fallback outside the valid
// range (e.g. -1) to mean "not configured, do nothing".
static int device_config_int(const char *section, const char *key, int fallback)
{
    if (section == nullptr || key == nullptr) {
        return fallback;
    }
    std::lock_guard<std::mutex> guard(g_device_config_mutex);
    if (!g_device_config_loaded) {
        device_config_load_locked();
    }
    const auto sectionIt = g_device_config.find(device_config_lower(section));
    if (sectionIt == g_device_config.end()) {
        return fallback;
    }
    const auto keyIt = sectionIt->second.find(device_config_lower(key));
    if (keyIt == sectionIt->second.end()) {
        return fallback;
    }
    // The WHOLE value must be a number. Checking only that parsing got
    // started is not enough: std::stoi reads as far as it can, so the typo
    // "5O" (five, letter O) parses as 5 and looks like a deliberate setting.
    // Caught by device_config_test.cpp on its first run, 2026-08-01.
    const std::string &text = keyIt->second;
    try {
        size_t consumed = 0;
        const int parsed = std::stoi(text, &consumed);
        if (consumed != text.size()) {
            return fallback;   // trailing junk means a typo, not a value
        }
        return parsed;
    } catch (...) {
        return fallback;   // unparsable is "not configured", never an error
    }
}

// Look up a boolean setting. Returns fallback when the file, the section, the
// key, or a recognisable value is missing -- every failure path is "behave as
// before", never an error.
static bool device_config_bool(const char *section, const char *key, bool fallback)
{
    if (section == nullptr || key == nullptr) {
        return fallback;
    }
    std::lock_guard<std::mutex> guard(g_device_config_mutex);
    if (!g_device_config_loaded) {
        device_config_load_locked();
    }
    const auto sectionIt = g_device_config.find(device_config_lower(section));
    if (sectionIt == g_device_config.end()) {
        return fallback;
    }
    const auto keyIt = sectionIt->second.find(device_config_lower(key));
    if (keyIt == sectionIt->second.end()) {
        return fallback;
    }
    const std::string value = device_config_lower(keyIt->second);
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    }
    return fallback;
}
