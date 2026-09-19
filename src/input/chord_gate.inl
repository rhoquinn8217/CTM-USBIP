// The TV's overlay chord, kept out of the host's hands (T-216).
//
// ⭐ WHY THIS IS ON THE LISTENER AND NOT THE TV. An xpad-driven pad is
// deliberately NOT grabbed by the TV, so while it is bridged its reports reach
// the TV's SDL (which watches for the chord) AND Windows (over USB/IP) at the
// same time. A gate on the TV's Moonlight input path cannot help, because a
// bridged pad does not use that path -- measured 2026-09-18, when exactly such
// a gate shipped as build 367 and changed nothing.
//
// ⓘ The rule itself lives in button_layout.inl beside the bits it clears, so it
// is tested without this file, which reaches into the device.
//
// ⓘ Relies on its includer (main.cpp) for device_input_pad_for and device_log.

#pragma once

namespace ctm_chord_gate {

// ⓘ One line per device when the gate first fires, and not again until it stops
// firing: a chord is held for a moment, and a line per report would bury the log
// at 250 a second.
inline std::mutex g_mutex;
inline std::unordered_map<const void *, bool> g_gating;

inline void apply(const void *deviceKey,
                  const std::vector<unsigned char> &descriptor,
                  uint8_t *data, size_t len)
{
    const InputPad pad = device_input_pad_for(descriptor);
    if (pad.layout == nullptr) return;

    const bool gated = ctm_rebind::chord_gate_apply(*pad.layout, data, len);

    bool announce = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        bool &was = g_gating[deviceKey];
        announce = gated && !was;
        was = gated;
    }
    if (announce) {
        device_log::input(device_log::msg()
            << "chord gate: both bumpers held -- Select and Start held back from "
               "the host while the TV's overlay chord is formed");
    }
}

inline void forget_device(const void *deviceKey)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_gating.erase(deviceKey);
}

} // namespace ctm_chord_gate

void ctm_chord_gate_apply(const void *deviceKey,
                          const std::vector<unsigned char> &descriptor,
                          uint8_t *data, size_t len)
{
    ctm_chord_gate::apply(deviceKey, descriptor, data, len);
}

void ctm_chord_gate_forget(const void *deviceKey)
{
    ctm_chord_gate::forget_device(deviceKey);
}
