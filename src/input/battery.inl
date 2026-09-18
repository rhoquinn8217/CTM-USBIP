// What a pad says about its own charge, kept per device for the settings page.
//
// ⭐ WHY THERE IS A STORE AT ALL. The reading arrives in the input report, at
// 250 a second, on the relay thread. The page asks for it over REST, on another
// thread, a few times a minute. So the value is sampled where it arrives and
// read where it is wanted, and the two never wait on each other.
//
// ⛔⛔ KEYED BY DEVICE, AND LOCKED. Every hook that remembered anything between
// reports has been bitten by keeping ONE value for every bridged pad: the
// rapid-fire fault of 2026-09-01, the chord and the on-screen keyboard in
// 449003f, the mouse's held buttons in f955fd4. Each device has its own relay
// thread, so a plain global here would be both wrong and a race.
//
// ⓘ Relies on its includer for device_input_pad_for() and device_log.

#pragma once

namespace ctm_battery {

struct Entry {
    ctm_rebind::BatteryReading reading;
    bool                        logged;
};

inline std::mutex g_mutex;
inline std::unordered_map<const void *, Entry> g_entries;

// ⭐ Sampled on every report, stored only when it CHANGES. A pad reports the
// same byte hundreds of times between one step of charge and the next, and the
// page redrawing on each of them was the thing T-195 asked us not to do.
inline void sample(const void *deviceKey,
                   const std::vector<unsigned char> &descriptor,
                   const uint8_t *data, size_t len)
{
    const InputPad pad = device_input_pad_for(descriptor);
    if (pad.layout == nullptr) return;
    const ctm_rebind::BatteryReading now =
        ctm_rebind::battery_reading(*pad.layout, data, len);
    if (!now.known) return;   // a pad with no battery byte, or a fault state

    bool announce = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Entry &entry = g_entries[deviceKey];
        const bool changed = !entry.reading.known ||
                             entry.reading.percent != now.percent ||
                             entry.reading.state != now.state;
        if (!changed) return;
        announce = !entry.logged;
        entry.reading = now;
        entry.logged = true;
    }
    // ⓘ The first reading of a session is worth a line; after that the page is
    // where it is read, and a log line per step would say nothing new.
    if (announce) {
        device_log::input(device_log::msg()
            << "battery: " << now.percent << "% "
            << (now.state == ctm_rebind::kBatteryFull ? "full"
                : now.state == ctm_rebind::kBatteryCharging ? "charging"
                : "on its own battery"));
    }
}

inline void forget_device(const void *deviceKey)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_entries.erase(deviceKey);
}

// The reading for a device, or not-known. ⛔ Not-known must reach the page as
// ABSENT rather than as zero: a flat pad and a pad that never said are opposite
// facts (rhoquinn8217 on T-195: every other kind shows nothing at all).
inline ctm_rebind::BatteryReading reading_for(const void *deviceKey)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_entries.find(deviceKey);
    if (it == g_entries.end()) return ctm_rebind::BatteryReading{ false, 0, ctm_rebind::kBatteryUnknown };
    return it->second.reading;
}

// The word the API uses for a state. ⓘ "discharging" is the kernel's word and
// is kept here because it is what anyone reading the JSON will expect.
inline const char *state_word(ctm_rebind::BatteryState state)
{
    switch (state) {
    case ctm_rebind::kBatteryCharging:    return "charging";
    case ctm_rebind::kBatteryFull:        return "full";
    case ctm_rebind::kBatteryDischarging: return "discharging";
    default:                              return "";
    }
}

} // namespace ctm_battery

void ctm_battery_sample(const void *deviceKey,
                        const std::vector<unsigned char> &descriptor,
                        const uint8_t *data, size_t len)
{
    ctm_battery::sample(deviceKey, descriptor, data, len);
}

void ctm_battery_forget(const void *deviceKey)
{
    ctm_battery::forget_device(deviceKey);
}
