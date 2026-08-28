// The synthetic keyboard, mirroring mouse_device.inl.
//
// ⭐ WHY A DEVICE. Button rebinding turns a controller button into a keyboard
// key, and something has to BE a keyboard for that key to come from. Windows
// binds this to its stock HID keyboard driver exactly as it would a real one.
//
// ⛔ WHY NOT SendInput. That is synthetic injection and Windows flags it as
// such; games using raw input can see the flag, and anti-cheat often blocks it.
// It would fail in exactly the games people care about, and fail invisibly. A
// virtual USB keyboard is indistinguishable from hardware -- which is this
// project's whole premise.
//
// SHAPE. 8-byte boot-keyboard reports: [0] modifiers, [1] reserved,
// [2..7] up to six simultaneous key usages.

#pragma once

namespace ctm_keyboard_device {

// Matches the endpoint the profile declares.
inline constexpr uint8_t kKeyboardInEndpoint = 0x81;

inline std::mutex g_mutex;
inline std::shared_ptr<CtmUsbipDevice> g_device;
inline std::thread g_pump;
inline std::atomic_bool g_running{false};
inline std::atomic_bool g_started{false};

// ⭐ The state the pump publishes. Written by the rebind path, read here.
//
// ⚠️ A whole report rather than a queue of events: HID keyboards are STATE, not
// events. "These keys are down right now" is the whole protocol, and a queue
// would have to be flattened back into this anyway -- with the added chance of
// getting the order wrong on release.
inline std::mutex g_stateMutex;
inline uint8_t g_modifiers = 0;
inline uint8_t g_keys[6] = {0, 0, 0, 0, 0, 0};
inline std::atomic_bool g_dirty{false};

inline std::wstring keyboard_profile_path()
{
    return find_relative_asset(L"profiles\\descriptors\\virtual_keyboard.profile");
}

inline std::wstring keyboard_map_path()
{
    return find_relative_asset(L"maps\\virtual_keyboard.map");
}

// Replace the whole held-key set. Six keys maximum, which is what a boot
// keyboard carries; anything past that is dropped rather than rolled over.
inline void set_state(uint8_t modifiers, const uint8_t *keys, size_t count)
{
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_modifiers = modifiers;
        for (size_t i = 0; i < 6; ++i) {
            g_keys[i] = (i < count && keys != nullptr) ? keys[i] : 0;
        }
    }
    g_dirty.store(true, std::memory_order_relaxed);
}

// ⭐ Everything up. Called when a controller unbridges and when rebinds are
// turned off -- a key left down would repeat forever and look like a stuck
// keyboard, which is the same class of fault as a stuck mouse button.
inline void release_all()
{
    set_state(0, nullptr, 0);
}

inline void pump_loop()
{
    while (g_running.load()) {
        if (!g_dirty.exchange(false, std::memory_order_relaxed)) {
            // ⓘ Nothing changed. A HID keyboard does not need to repeat itself:
            // the host holds the last report until a new one arrives, so a
            // steady state costs nothing.
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }

        CTM_INPUT_REPORT report = {};
        report.length = 8;
        report.endpoint_address = kKeyboardInEndpoint;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            report.data[0] = g_modifiers;
            report.data[1] = 0;
            for (size_t i = 0; i < 6; ++i) {
                report.data[2 + i] = g_keys[i];
            }
        }

        std::shared_ptr<CtmUsbipDevice> device;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            device = g_device;
        }
        if (device) {
            device->inject_synthetic_input(report);
        }
    }
}

inline bool ensure_started()
{
    if (g_started.load()) {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_started.load()) {
        return true;
    }

    if (!g_agent_usbip_server) {
        return false;               // server not up yet; try again next session
    }

    auto device = std::make_shared<CtmUsbipDevice>();
    std::wstring error;
    if (!device->load(keyboard_profile_path(), keyboard_map_path(),
                      /*audioLatency*/ 0,
                      /*hasAudioBlockOverride*/ false,
                      /*audioBlockOverride*/ 0,
                      &error)) {
        device_log::input_w() << L"rebind keyboard: profile/map load failed: " << error;
        return false;
    }

    const std::string busId = "ctm-rebind-kbd";
    if (!g_agent_usbip_server->add_device(device, busId, &error)) {
        device_log::input_w() << L"rebind keyboard: USB/IP export failed: " << error;
        return false;
    }

    if (!run_usbip_attach(widen_ascii(busId.c_str(), busId.size()), kDefaultUsbipPort)) {
        // ⚠️ Not fatal: it stays exported, so a manual attach can still take it.
        device_log::input_w() << L"rebind keyboard: local attach failed";
    }

    g_device = device;
    g_running.store(true);
    g_pump = std::thread(pump_loop);
    g_started.store(true);
    device_log::input_w() << L"rebind keyboard: synthetic keyboard up (busid="
                          << widen_ascii(busId.c_str(), busId.size()) << L")";
    return true;
}

inline void stop()
{
    g_running.store(false);
    if (g_pump.joinable()) {
        g_pump.join();
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_device.reset();
    g_started.store(false);
}

} // namespace ctm_keyboard_device

// Defined out here for main.cpp's forward declaration -- see the note beside
// ctm_gyro_mouse_ensure_mouse_started().
void ctm_rebind_ensure_keyboard_started()
{
    ctm_keyboard_device::ensure_started();
}
