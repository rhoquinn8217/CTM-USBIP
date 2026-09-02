#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <winsvc.h>

#include "ctm/hid.h"
#include "ctm/map/runtime.h"
#include "ctm/profile.h"
#include "ctm/version.h"

#include <hidsdi.h>

#include <enet/enet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <random>      // nickname.inl picks one from a pool
#include <vector>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

#include "app/common.inl"
#include "usb/descriptors.inl"
#include "audio/reservoir.inl"
#include "map/diagnostics.inl"
#include "log/device_log.inl"
#include "config/device_config.inl"
#include "audio/ds5_output_overrides.inl"
#include "input/gyro_calibration.inl"   // read before gyro_mouse.inl uses it
// ⓘ gyro_mouse.inl gates on this, and it is defined in rebind.inl which comes
// later -- so it is declared here, above its user.
bool ctm_rebind_config_mode_effective();

#include "input/gyro_mouse.inl"          // needs device_config_* and device_section_for
#include "backend/backend.inl"
#include "backend/bt.inl"
/* Defined in audio/mic_ring.inl, which must come AFTER bridge.inl because it
 * needs monotonic_us from it. Declared here so bridge.inl can call it. */
static void mic_ring_push(const CtmBackend *owner, const uint8_t *data, size_t len);
static void mic_ring_reset(const CtmBackend *owner);
#include "backend/bridge.inl"
#include "backend/bridge_enet.inl"
#include "audio/mic_ring.inl"
#include "audio/audio_gain.inl"
#include "audio/pcm_amplitude_log.inl"  // needs monotonic_us from backend/bridge.inl; must precede its caller
#include "audio/iso_in_test_tone.inl"
// ⓘ And the same for the rebind hook itself: device.inl calls it on the input
// path but is included long before rebind.inl, which needs the keyboard device
// and the config helpers.
// ⓘ rest_config.inl is included before rebind.inl and needs to flip the gate
// from its endpoints, so the setter is forward declared here too.
void ctm_rebind_set_config_mode(bool on);
bool ctm_rebind_config_mode();
bool ctm_rebind_gate_hold();
void ctm_rebind_clear_provisional();
// ⓘ rebind.inl runs on the input path and needs the window check from open_ui.
bool ctm_ui_has_foreground();
// ⓘ The chord calls this from the input path; the REST endpoint calls it too.
// ⓘ Takes the controller that ran the chord, so the window can come up on its
// tab. Empty means "no particular one" -- the REST spawn path has no controller
// in hand.
void ctm_chord_show_ui(const std::string &ordinal);

// Which controller a device belongs to, or empty if it is not bridged. The
// input path holds a device pointer; the ordinal lives with the session.
std::string ctm_ordinal_for_device(const void *deviceKey);
void ctm_rebind_set_gate_hold(bool hold);

void ctm_rebind_apply(const void *deviceKey,
                      const std::vector<unsigned char> &descriptor,
                      const std::string &linkedConfig,
                      uint8_t *data, size_t len);
// ℹ Same shape for the touchpad hook and its forget: device.inl calls both
// on the input path, and touch_mouse.inl is included far later because it
// needs the mouse device and the gyro mailbox.
void ctm_touch_mouse_apply(const void *deviceKey,
                           const std::vector<unsigned char> &descriptor,
                           const std::string &linkedConfig,
                           const uint8_t *data, size_t len);
void ctm_touch_mouse_forget(const void *deviceKey);
// ⓘ Releases only THIS controller's held keys -- they are kept per device so
// two gated pads cannot cancel each other.
void ctm_keyboard_forget_device(const void *deviceKey);
// Swallow whatever is held, so a button that dismissed the overlay cannot also
// reach the game on the next report.
void ctm_rebind_swallow_held();
void ctm_stick_mouse_apply(const void *deviceKey,
                           const std::vector<unsigned char> &descriptor,
                           const std::string &linkedConfig,
                           const uint8_t *data, size_t len);
void ctm_stick_mouse_forget(const void *deviceKey);
// ⓘ rebind.inl fires the on-screen keyboard toggle, and osk.inl is included
// after it because it reads config through the same accessors.
// ⓘ `button` is the standard index that fired it, so an overlay keyboard can be
// dismissed by the same button that opened it.
void ctm_osk_toggle(const std::string &section, int button);
#include "usbip/device.inl"
#include "audio/iso_in_pacing.inl"
#include "usbip/server.inl"
#include "app/open_ui.inl"      // --ui: open the settings page, or focus one already open
#include "app/cli.inl"
#include "audio/ds5_apply_settings.inl"
#include "config/config_store.inl"   // per-controller config files; needs device_log + g_device_config
#include "config/config_presets.inl"  // what a new config can start from; needs nothing but strings
#include "config/config_watcher.inl" // follows config_store: apply_change refreshes it too
#include "app/rest.inl"
#include "app/ui_page_generated.inl"   // GENERATED by build.ps1 from the HTML
#include "app/ui_page.inl"             // serves it, disk first then embedded
#include "app/rest_config.inl"
        // config routes; needs rest.inl's helpers, so it follows it
// Forward declaration: agent.inl calls this when a DS5 session becomes ready to
// bring up the synthetic mouse; mouse_device.inl (just below) defines it. This
// breaks the include cycle -- the mouse device needs agent.inl's server and
// asset helpers, while agent.inl needs only this one symbol.
void ctm_gyro_mouse_ensure_mouse_started();
// ⓘ Same cycle, same shape: the keyboard device needs agent.inl's server and
// asset helpers, while agent.inl needs only this one symbol from it.
void ctm_rebind_ensure_keyboard_started();
#include "input/gyro_calibration_fetch.inl"   // needs CtmBackend; agent.inl calls it
#include "app/nickname.inl"      // controller nicknames; agent.inl assigns one per session
#include "app/agent.inl"
#include "input/mouse_device.inl"      // needs g_agent_usbip_server, find_relative_asset, run_usbip_attach
#include "input/keyboard_device.inl"   // same dependencies as the mouse above
// ⛔ AFTER keyboard_device.inl, which it types through, and BEFORE rebind.inl,
//    which calls into it. Both directions matter: the overlay needs the
//    keyboard to exist, and rebind needs the overlay to exist.
#include "app/overlay_window.inl"  // --overlay-test: the always-on-top, never-focused window
// ⚠️ AFTER keyboard_device: rebind pushes key state into it, so it must be
// defined first. And after gyro_mouse, for device_section_for and the config
// helpers.
#include "input/rebind.inl"
#include "input/touch_mouse.inl"   // touchpad cursor/scroll/taps; needs the mouse device and the gyro mailbox
#include "input/stick_mouse.inl"   // stick cursor; needs the gyro gate and mailbox
#include "input/osk.inl"          // the on-screen keyboard toggle
#include "app/rest_sessions.inl"
#include "app/rest_config_sessions.inl"   // defines what rest_config.inl declares; needs agent.inl's sessions
#include "app/service.inl"

bool ctm_ui_has_foreground()
{
    return ctm_open_ui::window_has_foreground();
}


// ⭐ Show the settings window and take the controllers.
//
// One implementation for the chord and the REST endpoint, so the two cannot
// drift apart. Defined here because it needs the window helpers AND the gate,
// which live in files included at different points.
//
// ⛔ Clears the hold FIRST. Hold beats config mode by design, so showing the
// window with one set would gate nothing and report success anyway.
void ctm_chord_show_ui(const std::string &ordinal)
{
    ctm_rebind_set_gate_hold(false);
    // ⓘ Before any close: the target is read when the new URL is built.
    if (!ordinal.empty()) ctm_open_ui::open_on_tab(ordinal);
    // ⛔ ONE AT A TIME. Whoever takes the claim does the open; anyone who
    // cannot has ALREADY LEFT ITS TARGET above, and the open in flight will
    // use it. Two of these running at once close each other's windows -- see
    // the note beside claim_open.
    if (!ctm_open_ui::claim_open()) {
        device_log::session_w() << L"window open already in flight -- retargeted";
        return;
    }
    struct Release { ~Release() { ctm_open_ui::release_open(); } } release;

    // ⛔ WAIT FOR THE CLOSE. Measured 2026-08-29: with a window already open,
    // WM_CLOSE was posted and open_new ran immediately -- so the old window was
    // still there, the new one did not come forward, and the taskbar icon just
    // flashed.
    //
    // ⚠️ That failure is worse than it looks. The gate takes the pad, but the
    // KEYSTROKES go to whatever has focus -- and with the game still in front,
    // Ctrl+Alt+Shift+W is just W to anything reading scancodes and ignoring
    // modifiers. The gate was handing the game keyboard input in place of the
    // pad.
    //
    // ⓘ Polls rather than sleeping a fixed time: a window that closes quickly
    // should not cost anyone 300ms.
    if (ctm_open_ui::close_existing()) {
        int waited = 0;
        for (; waited < 40; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            if (!ctm_open_ui::window_exists()) break;
        }
        device_log::input_w() << L"ui: waited " << (waited * 25)
                              << L"ms for the old window to go";
    }

    ctm_rebind_set_config_mode(true);
    device_log::input_w() << L"ui: launching a new window";
    ctm_open_ui::open_new(g_rest_port);
    // ⭐ Focus is not visibility. A borderless game paints over a focused
    // window, so it has to be lifted in the DRAWING order as well.
    ctm_open_ui::raise_when_ready();

}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    // ⭐⭐ TELL WINDOWS WE UNDERSTAND HIGH-DPI, before any window exists.
    //
    // ⚠️ Without this the process gets VIRTUALISED screen metrics: on a 4K
    // desktop at 300% scaling, GetSystemMetrics(SM_CXSCREEN) answers 1280
    // rather than 3840. The overlay keyboard would size itself in those
    // pretend pixels and Windows would then stretch the result -- the right
    // physical size, but blurry, which is the one thing that hurts on a
    // television across a room.
    //
    // ⓘ Safe for everything else here: the only other absolute screen use is
    // warping the cursor to the CENTRE (gyro_mouse.inl), and the centre is the
    // centre in either coordinate space. Nothing else in the listener reads
    // screen positions -- the browser window is found by title, not location.
    //
    // ⓘ The capture program already does this, for DuplicateOutput1.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    EnetGlobalGuard enetGuard;
    if (!enetGuard.ok) {
        std::wcerr << L"enet_initialize failed\n";
        return 2;
    }
    if (argc < 2) {
        print_usage();
        return 2;
    }

    std::wstring mode = argv[1];
    if (mode == L"version" || mode == L"--version" || mode == L"-v") {
        std::wcout << L"ctm-usbip " << widen_ascii(CTM_VERSION_DISPLAY, strlen(CTM_VERSION_DISPLAY)) << L"\n";
        return 0;
    }

    if (mode == L"list-bt" || mode == L"list-hid") {
        // JSON device inventory for GUI front-ends (Ciprian's Bridge). The GUI
        // must never re-implement enumeration/classification — this output is
        // the single host-side source of device knowledge.
        const std::vector<CtmBtDevice> devices =
            mode == L"list-bt" ? ctm_list_bluetooth_hid_devices() : ctm_list_hid_devices();
        auto esc = [](const std::wstring &w) {
            const std::string s = narrow_ascii(w);
            std::string out;
            for (const char c : s) {
                if (c == '"' || c == '\\') { out += '\\'; out += c; }
                else if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else out += c;
            }
            return out;
        };
        std::cout << "[";
        for (size_t i = 0; i < devices.size(); ++i) {
            const CtmBtDevice &d = devices[i];
            std::cout << (i ? ",\n " : "\n ")
                << "{\"index\":" << i
                << ",\"product\":\"" << esc(d.product) << "\""
                << ",\"manufacturer\":\"" << esc(d.manufacturer) << "\""
                << ",\"serial\":\"" << esc(d.serial) << "\""
                << ",\"device_type\":\"" << esc(d.device_type) << "\""
                << ",\"unavailable_reason\":\"" << esc(d.unavailable_reason) << "\""
                << ",\"instance_id\":\"" << esc(d.instance_id) << "\""
                << ",\"parent_instance_id\":\"" << esc(d.parent_instance_id) << "\""
                << ",\"vendor_id\":" << d.vendor_id
                << ",\"product_id\":" << d.product_id
                << ",\"usage_page\":" << d.usage_page
                << ",\"usage\":" << d.usage
                << ",\"input_report_length\":" << d.input_report_length
                << ",\"output_report_length\":" << d.output_report_length
                << ",\"feature_report_length\":" << d.feature_report_length
                << ",\"is_bluetooth\":" << (d.is_bluetooth ? "true" : "false")
                << ",\"is_game_controller\":" << (d.is_game_controller ? "true" : "false")
                << ",\"is_supported\":" << (d.is_supported ? "true" : "false")
                << ",\"can_open\":" << (d.can_open_read_write ? "true" : "false")
                << "}";
        }
        std::cout << "\n]" << std::endl;
        return 0;
    }
    bool noAttach = false;
    std::wstring profileOverride;
    std::wstring mapOverride;
    std::wstring busId = kDefaultBusId;
    uint8_t audioLatency = 0x60;
    bool hasAudioBlockOverride = false;
    uint8_t audioBlockOverride = 0;
    uint16_t usbipPort = kDefaultUsbipPort;
    unsigned long number = 0;

    if (mode == L"agent") {
        unsigned long port = kAgentDefaultPort;
        int argIndex = 2;
        if (argc >= 3 && argv[2][0] != L'-') {
            if (!parse_uint_arg(argv[2], 65535, &port)) {
                print_usage();
                return 2;
            }
            argIndex = 3;
        }
        if (port < 1024) {
            print_usage();
            return 2;
        }
        for (int i = argIndex; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--enet") {
                g_use_enet.store(true);
            } else if (arg == L"--rest" && i + 1 < argc) {
                unsigned long value = 0;
                if (!parse_uint_arg(argv[++i], 65535, &value) || value < 1024) {
                    print_usage();
                    return 2;
                }
                g_rest_port = static_cast<uint16_t>(value);
            } else if (arg == L"--verbose") {
                // ⓘ Everything the agent can say. Off by default: the per-report
                // lines run at roughly 250 a second and bury the handful a
                // person actually needs.
                g_verbose_flag = true;
            } else if (arg == L"--ui") {
                // ⭐ Opens the settings page once the agent is up, or brings an
                // already-open one forward. The launcher used to do this and
                // could not check for an existing window, so every rebuild left
                // another one behind.
                ctm_open_ui::g_open_ui = true;
            } else if (arg == L"--overlay-test") {
                // ⓘ TEMPORARY, and named so. Step one of the overlay keyboard
                // is a window with the right styles and a placeholder inside;
                // this flag is how it gets looked at before anything is wired
                // to it. It goes when the OSKeyboard action opens the real one.
                ctm_overlay::show();
            } else if (arg == L"--rest-lan") {
                g_rest_bind_lan = true;
            } else if (arg == L"--rest-token" && i + 1 < argc) {
                const std::wstring token = argv[++i];
                if (token.empty() || token.find(L' ') != std::wstring::npos ||
                    token.find(L'"') != std::wstring::npos) {
                    std::wcerr << L"--rest-token must be non-empty with no spaces or quotes\n";
                    return 2;
                }
                g_rest_token = narrow_ascii(token);
            } else {
                print_usage();
                return 2;
            }
        }
        // ⭐ ONE exe, ONE set of flags, four situations:
        //
        //   listener up,  page open  -> bring the page forward, exit
        //   listener up,  no page    -> open a page, exit
        //   listener off, page open  -> start the listener, bring the page forward
        //   listener off, no page    -> start the listener, open a page
        //
        // The page is FOCUSED first because that works whether or not the agent
        // starts. Opening a NEW one waits until we know which branch we are in:
        // if the agent is not going to run, a page must still appear here; if it
        // is, run_agent opens one only once it is actually listening, so a
        // failed start never leaves a window reporting an unreachable agent.
        if (ctm_open_ui::g_open_ui) {
            // ⭐ --ui IMPLIES --rest. The page is served by the agent now, so
            // without a REST port there is nothing to serve it from and --ui
            // could only fail silently. Turning the port on is the useful
            // reading of "give me the settings page".
            if (g_rest_port == 0) {
                g_rest_port = 48055;
                device_log::config_w() << L"--ui needs the settings API; enabling it on port "
                           << g_rest_port ;
            }
            // ⛔ ASK WHO ELSE IS RUNNING FIRST.
            //
            // This focused any existing window BEFORE checking for another
            // listener -- so a window left behind by a DEAD one was adopted,
            // and this listener never opened its own. The adopted window then
            // closed itself on the token check, because its token belonged to a
            // listener that no longer exists. Result: no window at all.
            //
            // ⓘ A window only counts as "already open" when the listener that
            // opened it is still there. Otherwise it is a leftover, and closing
            // it is the right thing to do.
            if (ctm_open_ui::agent_already_running(static_cast<uint16_t>(port))) {
                const bool focused = ctm_open_ui::focus_only();
                // ⓘ No token: the OTHER listener serves the API and would not
                // recognise one minted here, so the window would close itself.
                if (!focused) ctm_open_ui::open_new(g_rest_port, false);
                std::wcout << L"a listener is already running on port " << port
                           << L" -- left it alone\n";
                return 0;
            }

            // ⭐ Nobody else is running, so any window out there is stale.
            if (ctm_open_ui::close_existing()) {
                for (int waited = 0; waited < 40; ++waited) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    if (!ctm_open_ui::window_exists()) break;
                }
            }
            ctm_open_ui::g_ui_already_focused = false;
        }
        return run_agent(static_cast<uint16_t>(port));
    }

    if (mode == L"service-run") {
        unsigned long port = kAgentDefaultPort;
        int argIndex = 2;
        if (argc >= 3 && argv[2][0] != L'-') {
            if (!parse_uint_arg(argv[2], 65535, &port) || port < 1024) {
                print_usage();
                return 2;
            }
            argIndex = 3;
        }
        for (int i = argIndex; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--enet") {
                g_use_enet.store(true);
            } else if (arg == L"--rest" && i + 1 < argc) {
                unsigned long value = 0;
                if (!parse_uint_arg(argv[++i], 65535, &value) || value < 1024) {
                    print_usage();
                    return 2;
                }
                g_rest_port = static_cast<uint16_t>(value);
            } else if (arg == L"--rest-lan") {
                g_rest_bind_lan = true;
            } else if (arg == L"--rest-token" && i + 1 < argc) {
                const std::wstring token = argv[++i];
                if (token.empty() || token.find(L' ') != std::wstring::npos ||
                    token.find(L'"') != std::wstring::npos) {
                    std::wcerr << L"--rest-token must be non-empty with no spaces or quotes\n";
                    return 2;
                }
                g_rest_token = narrow_ascii(token);
            } else {
                print_usage();
                return 2;
            }
        }
        g_service_port = static_cast<uint16_t>(port);
        g_running_as_service.store(true);
        return run_service();
    }

    if (mode == L"install" || mode == L"uninstall") {
        if (mode == L"uninstall") {
            return service_uninstall();
        }
        unsigned long port = kAgentDefaultPort;
        bool useEnet = false;
        unsigned long restPort = 0;
        bool restLan = false;
        std::wstring restToken;
        int argIndex = 2;
        if (argc >= 3 && argv[2][0] != L'-') {
            if (!parse_uint_arg(argv[2], 65535, &port) || port < 1024) {
                print_usage();
                return 2;
            }
            argIndex = 3;
        }
        for (int i = argIndex; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--enet") {
                useEnet = true;
            } else if (arg == L"--rest" && i + 1 < argc) {
                if (!parse_uint_arg(argv[++i], 65535, &restPort) || restPort < 1024) {
                    print_usage();
                    return 2;
                }
            } else if (arg == L"--rest-lan") {
                restLan = true;
            } else if (arg == L"--rest-token" && i + 1 < argc) {
                // Embedded verbatim in the service image path, so no spaces or
                // quotes (it is visible via `sc qc`, like any service argument).
                restToken = argv[++i];
                if (restToken.empty() || restToken.find(L' ') != std::wstring::npos ||
                    restToken.find(L'"') != std::wstring::npos) {
                    std::wcerr << L"--rest-token must be non-empty with no spaces or quotes\n";
                    return 2;
                }
            } else {
                print_usage();
                return 2;
            }
        }
        return service_install(static_cast<uint16_t>(port), useEnet,
                               static_cast<uint16_t>(restPort), restLan, restToken);
    }

    if (mode == L"bt") {
        if (argc < 3 || !parse_uint_arg(argv[2], 31, &number)) {
            print_usage();
            return 2;
        }
    } else if (mode == L"bridge") {
        if (argc < 3 || !parse_uint_arg(argv[2], 65535, &number) || number < 1024) {
            print_usage();
            return 2;
        }
    } else {
        print_usage();
        return 2;
    }

    for (int i = 3; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--no-attach") {
            noAttach = true;
        } else if (arg == L"--enet") {
            g_use_enet.store(true);
        } else if (arg == L"--profile" && i + 1 < argc) {
            profileOverride = argv[++i];
        } else if (arg == L"--map" && i + 1 < argc) {
            mapOverride = argv[++i];
        } else if (arg == L"--busid" && i + 1 < argc) {
            busId = argv[++i];
        } else if (arg == L"--audio-latency" && i + 1 < argc) {
            unsigned long value = 0;
            if (!parse_uint_arg(argv[++i], 255, &value)) {
                std::wcerr << L"invalid --audio-latency\n";
                return 2;
            }
            audioLatency = static_cast<uint8_t>(value);
        } else if (arg == L"--audio-block" && i + 1 < argc) {
            unsigned long value = 0;
            if (!parse_uint_arg(argv[++i], 255, &value)) {
                std::wcerr << L"invalid --audio-block\n";
                return 2;
            }
            hasAudioBlockOverride = true;
            audioBlockOverride = static_cast<uint8_t>(value);
        } else if (arg == L"--usbip-port" && i + 1 < argc) {
            unsigned long value = 0;
            if (!parse_uint_arg(argv[++i], 65535, &value) || value < 1024) {
                std::wcerr << L"invalid --usbip-port (1024..65535)\n";
                return 2;
            }
            usbipPort = static_cast<uint16_t>(value);
        } else {
            std::wcerr << L"invalid argument: " << arg << L"\n";
            return 2;
        }
    }

    std::wstring error;
    CtmUsbipDevice device;
    // "auto" = build the USB profile from the backend's caps (identity
    // pass-through): the default for bridge mode, OPT-IN for local bt mode so
    // generic BT HID devices without a curated profile can be plugged too.
    const bool dynamicBridgeProfile =
        (mode == L"bridge" && (profileOverride.empty() || profileOverride == L"auto")) ||
        (mode == L"bt" && profileOverride == L"auto");
    const std::wstring profilePath = dynamicBridgeProfile
        ? L"auto"
        : (profileOverride.empty() ? find_ds5_descriptor_profile() : profileOverride);
    const std::wstring mapPath = mapOverride.empty()
        ? (dynamicBridgeProfile ? find_hid_identity_map_file() : find_ds5_map_file())
        : mapOverride;
    if (dynamicBridgeProfile) {
        if (!device.load_map(mapPath, audioLatency, hasAudioBlockOverride, audioBlockOverride, &error)) {
            std::wcerr << L"load failed: " << error << L"\n";
            return 3;
        }
    } else if (!device.load(profilePath, mapPath, audioLatency, hasAudioBlockOverride, audioBlockOverride, &error)) {
        std::wcerr << L"load failed: " << error << L"\n";
        return 3;
    }
    std::wcout << L"profile: " << profilePath << L"\n";
    std::wcout << L"map: " << mapPath << L"\n";

    std::unique_ptr<CtmBackend> backend;
    EnetBridgeBackend *enetBackend = nullptr;
    if (mode == L"bt") {
        backend = std::make_unique<LocalBtBackend>(number, device.bt_audio_pace_ms());
    } else if (g_use_enet.load()) {
        auto enet = std::make_unique<EnetBridgeBackend>(static_cast<uint16_t>(number), device.bt_audio_pace_ms());
        enetBackend = enet.get();
        backend = std::move(enet);
        std::wcout << L"transport: ENet/UDP (--enet)\n";
    } else {
        backend = std::make_unique<BridgeBackend>(static_cast<uint16_t>(number), device.bt_audio_pace_ms());
    }

    if (!backend->start([&](const uint8_t *data, size_t length, uint8_t endpoint) {
            device.on_physical_input(data, length, endpoint);
        }, &error)) {
        std::wcerr << L"backend start failed: " << error << L"\n";
        return 4;
    }

    if (dynamicBridgeProfile) {
        CtmDescriptorProfile dynamicProfile;
        if (!make_dynamic_hid_profile(backend->caps(), &dynamicProfile, &error) ||
            !device.set_profile(dynamicProfile, &error)) {
            backend->stop();
            std::wcerr << L"dynamic profile failed: " << error << L"\n";
            return 3;
        }
    }

    if (!device.attach_backend(backend.get(), &error)) {
        backend->stop();
        std::wcerr << L"backend attach failed: " << error << L"\n";
        return 5;
    }

    const std::string busIdAscii = narrow_ascii(busId);
    if (busIdAscii.empty() || busIdAscii.size() > 31) {
        std::wcerr << L"invalid --busid: must be non-empty ASCII up to 31 bytes\n";
        backend->stop();
        return 2;
    }

    CtmUsbipServer server(&device, busIdAscii);
    if (!server.start(usbipPort, &error)) {
        backend->stop();
        std::wcerr << L"usbip server start failed: " << error << L"\n";
        return 6;
    }

    // Explicit plug-out / plug-in for the ENet transport on link transitions:
    // on link loss detach the virtual USB device (Windows sees an unplug) and
    // log the unplugged state; on reconnect re-attach it. The TCP BridgeBackend
    // keeps its existing reconnect-in-place behavior and sets no callbacks.
    if (enetBackend != nullptr) {
        const std::wstring attachBusId = busId;
        const uint16_t attachPort = usbipPort;
        const std::string detachBusId = busIdAscii;
        const bool autoAttach = !noAttach;
        enetBackend->set_disconnect_callback([&server, detachBusId]() {
            const bool detached = server.detach_device(detachBusId);
            device_log::session_w() << L"bridge link down: virtual device UNPLUGGED busid="
                       << widen_ascii(detachBusId.c_str(), detachBusId.size())
                       << (detached ? L" (usb/ip client detached)" : L" (no active import)");
        });
        enetBackend->set_reconnect_callback([attachBusId, attachPort, autoAttach]() {
            device_log::session_w() << L"bridge link up: virtual device PLUGGED IN busid=" << attachBusId;
            if (autoAttach && !run_usbip_attach(attachBusId, attachPort)) {
                std::wcerr << L"re-attach failed; server remains running for manual attach\n";
            }
        });
    }

    if (!noAttach) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!run_usbip_attach(busId, usbipPort)) {
            std::wcerr << L"attach failed; server remains running for manual attach\n";
        }
    }

    std::cout << "ctm-usbip running; press Ctrl+C to stop" << std::endl;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server.stop();
    device.stop();
    backend->stop();
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    return 0;
}
