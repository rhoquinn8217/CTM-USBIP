// Synthetic mouse device: creation, export, attach, and the pump that turns
// mailbox deltas into HID mouse reports.
//
// LIFECYCLE DECISION (design doc §20). The mouse is created ONCE when the first
// DS5 session with a mouse gate becomes ready, and lives for the process. It is
// deliberately NOT created-per-session and NOT "appears on movement": config is
// read live, so a device that only existed when a gate was set at session start
// could never be turned on mid-session -- you cannot send movement to a device
// that does not exist. Always-present means the gate only decides whether the
// pump emits, never whether the device exists. Untidy (a mouse shows in Device
// Manager even with gyro off) but it is the only shape that preserves live
// tuning, which is the whole point.
//
// HOW IT SERVES REPORTS. CtmUsbipServer::handle_interrupt_in drains the
// device's own pendingInputReports_ queue and needs no backend -- so a device
// with nothing behind it works as-is. The pump thread below calls
// inject_synthetic_input() whenever the mailbox has movement, and the server's
// interrupt-IN worker delivers it to Windows. When idle it enqueues nothing, so
// the poll simply blocks -- correct for a mouse (no movement = no reports).

#pragma once

namespace ctm_mouse_device {

// Guards one-time creation and holds the long-lived objects.
inline std::mutex g_mutex;
inline std::shared_ptr<CtmUsbipDevice> g_device;   // the synthetic mouse
inline std::thread g_pump;
inline std::atomic_bool g_running{false};
inline std::atomic_bool g_started{false};

inline std::wstring mouse_profile_path()
{
    return find_relative_asset(L"profiles\\descriptors\\virtual_mouse.profile");
}

inline std::wstring mouse_map_path()
{
    return find_relative_asset(L"maps\\virtual_mouse.map");
}

// Drains the mailbox and pushes 4-byte boot-mouse reports. Runs until stop().
// ⭐ Buttons and wheel, set by the rebinder and read by the pump.
//
// ⓘ The device already declares all of this -- three buttons and a signed wheel
// byte -- so nothing about the profile changes. Only the pump was hardcoding
// zero for both.
//
// ⚠️ The wheel is a DELTA, not a state: it must be sent once and cleared, or the
// page would scroll forever after one press.
inline std::atomic<uint8_t> g_buttons{0};
inline std::atomic<int> g_wheelPending{0};

inline void set_buttons(uint8_t mask) { g_buttons.store(mask, std::memory_order_relaxed); }
inline void add_wheel(int clicks) { g_wheelPending.fetch_add(clicks, std::memory_order_relaxed); }

inline void pump_loop()
{
    // The mouse endpoint from the profile. Kept in one place so it matches the
    // profile's [usb.endpoints] hid_in.
    constexpr uint8_t kMouseInEndpoint = 0x81;

    while (g_running.load() && !g_stop.load()) {
        int8_t dx = 0, dy = 0;
        const bool moved = ctm_gyro_mouse::shared_mailbox().drain(&dx, &dy);

        // ⭐ A button or a wheel click is worth a report on its own -- waiting
        // for movement would mean a click did nothing while the pad was still.
        const uint8_t buttons = g_buttons.load(std::memory_order_relaxed);
        int wheel = g_wheelPending.exchange(0, std::memory_order_relaxed);
        if (wheel > 127) wheel = 127;
        if (wheel < -127) wheel = -127;

        static uint8_t lastButtons = 0;
        const bool buttonsChanged = buttons != lastButtons;
        lastButtons = buttons;

        if (moved || buttonsChanged || wheel != 0) {
            std::shared_ptr<CtmUsbipDevice> dev;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                dev = g_device;
            }
            if (dev) {
                CTM_INPUT_REPORT report = {};
                report.endpoint_address = kMouseInEndpoint;
                report.length = 4;
                report.data[0] = buttons;                    // left/right/middle
                report.data[1] = static_cast<uint8_t>(dx);   // relative X
                report.data[2] = static_cast<uint8_t>(dy);   // relative Y
                report.data[3] = static_cast<uint8_t>(wheel);
                dev->inject_synthetic_input(report);
            }
        } else {
            // Nothing pending: sleep a mouse poll interval rather than spin.
            // ~4ms keeps latency well under the 10ms endpoint bInterval while
            // costing almost nothing when idle.
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
}

// Create + export + attach the mouse, once. Safe to call on every DS5 session
// becoming ready; subsequent calls are no-ops. Returns true if the mouse is up
// (or already was).
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
    if (!device->load(mouse_profile_path(), mouse_map_path(),
                      /*audioLatency*/ 0,
                      /*hasAudioBlockOverride*/ false,
                      /*audioBlockOverride*/ 0,
                      &error)) {
        std::wcerr << L"gyro mouse: profile/map load failed: " << error << L"\n";
        return false;
    }

    // A fixed busid distinct from any controller session. 31-char limit; this
    // is well under and cannot collide with the "1-<port>" controller busids.
    const std::string busId = "ctm-gyro-mouse";
    if (!g_agent_usbip_server->add_device(device, busId, &error)) {
        std::wcerr << L"gyro mouse: USB/IP export failed: " << error << L"\n";
        return false;
    }

    // Attach locally so Windows binds its HID mouse driver.
    if (!run_usbip_attach(widen_ascii(busId.c_str(), busId.size()), kDefaultUsbipPort)) {
        std::wcerr << L"gyro mouse: local attach failed\n";
        // Leave it exported; a manual attach can still pick it up. Not fatal.
    }

    g_device = device;
    g_running.store(true);
    g_pump = std::thread(pump_loop);
    g_started.store(true);
    device_log::input_w() << L"gyro mouse: synthetic mouse up (busid=" << widen_ascii(busId.c_str(), busId.size()) << L")";
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

} // namespace ctm_mouse_device

// Free-function hook matching the forward declaration in main.cpp. agent.inl
// calls this (it is compiled before this file, so it cannot name the namespace
// function directly); the definition lives here where the server and asset
// helpers are in scope.
void ctm_gyro_mouse_ensure_mouse_started()
{
    ctm_mouse_device::ensure_started();
}
