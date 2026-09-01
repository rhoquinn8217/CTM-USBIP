# Changes from upstream

This is a fork of [`CTM-Bridge/CTM-USBIP`](https://github.com/CTM-Bridge/CTM-USBIP),
forked at `08df624` (2026-07-26), 31 commits after the `v0.0.1` tag.
Licensed GPL-3.0-or-later, the same as upstream.

Changes below are by rhoquinn8217. Each line names the commits that
carry it; upstream's own history is unchanged.

## Changes

| Date | Change | Commits |
|---|---|---|
| 2026-07-02 | Added `ds5_usb` device kind, dispatched to its own profile | `4f50540` |
| 2026-07-02 | Added `maps/ds5_usb_over_ds5_usb.map` for wired connections | `0f09507`, `8a31dbd`, `4b4630f` |
| 2026-07-02 | Added message type `MsgIsoAudio = 11` | `b8a754c` |
| 2026-07-02 | Build: allow the VS2026 toolchain; copy the new map | `5f30787` |
| 2026-07-28 | Added ISO passthrough map flag and backend send path | `3bc5624`, `67d8e4d` |
| 2026-07-28 | Route wired ISO audio through a separate path | `19f22a9` |
| 2026-07-28 | Added map parser test project and test harness | `04754b3`, `bdb14f0` |
| 2026-07-29 | Added PCM amplitude logging to the wired ISO audio path | `e3afa55`, `841d9a2`, `cbae4b4` |
| 2026-07-30 | Check the keepalive enable result instead of discarding it | `61302f7` |
| 2026-07-30 | Log bridge session transitions; flush log output so teardown lines are not lost | `dbec96f`, `87d1427` |
| 2026-08-01 | Windows-side device configuration. A text config file drives echo cancellation, audio routing, volumes and gains; edits apply live | `570ba61`, `20d14cb`, `fcc55ba`, `7cb8f1f`, `706b88c`, `382d18f`, `78dbdd1`, `2e6b42f`, `70c17dd`, `a7efc78`, `b496d08`, `a42dbe4`, `4fdf97f` |
| 2026-08-02 | Controller microphone audio, host side. Completions release after the audio they carry, ending a busy loop of ~28,000 requests a second | `76493f2`, `ec3c9de`, `99030fa`, `d7553ef` |
| 2026-08-04 | Microphone buffers are per session, not per process. One shared buffer let a second controller drain the first one's audio | `310df17`, `4a1d11e` |
| 2026-08-05 | Mute the controller's microphone from the host, so nothing else can leave it streaming. The bit layout is asserted in tests | `f85de1a` |
| 2026-08-05 | Accept the DualSense Edge. Its own captured USB descriptor makes Windows identify it as an Edge, which its extra controls need | `8fbb26d`, `9d8e3d7` |
| 2026-08-11 | Hold the audio stream open briefly after bridging. The controller's audio sleeps when idle, and waking it swallowed the start of a sound | `4edd287`, `e9eceea`, `0d0c907` |
| 2026-08-12 | A settings panel for the DualSense Edge, so its configuration can be edited without hand-writing the file | `e998d29` |
| 2026-08-12 | Drop the audio hold. The TV signals its own controllers now, so the host need not keep the stream awake for it | `f36f272` |
| 2026-08-13 | Drop microphone reports before anything reads them as pad state. Audio arrives in the button report, one flag apart, and reads as presses | `9bdbf81` |
| 2026-08-16 | The host owns the controller's Bluetooth audio buffer. `audio_latency_ms` reaches a live session in a second or two, with no reconnect | `c3e2496`, `1b12d9e` |
| 2026-08-19 | Stop preloading feature reports a Bluetooth DualSense never answers. They shared the link with the TV's tone and took the pad offline | `366b0de`, `08dabcd` |
| 2026-08-20 | Optional HTTP/JSON control API (`--rest <port>`): status, sessions, bridge start/stop, restart. Loopback-only unless `--rest-lan` | `4875cb3` |
| 2026-08-21 | Per-controller configuration. A controller links to its own config file, can claim a serial so it re-attaches at bridge time, updates live | `b08a90f`, `8b6e26a`, `3f63e49`, `e550c01`, `bf325cc`, `5abf076`, `71b6360`, `7132194`, `16c5054` |
| 2026-08-21 | Gyro-to-mouse. Motion drives a synthetic USB mouse using the pad's own calibration, so a sensitivity number means the same on every pad | `8556242`, `b02ea17`, `2cb9c26`, `395b8b3`, `a640c2e`, `25e9c76` |
| 2026-08-23 | The agent serves its own settings page (`--ui`), embedded in the executable and usable entirely from a controller | `ccb1f56`, `b7bf340`, `78c6095`, `c162412`, `21b5999`, `9b0d4ea`, `3a85ee7`, `ff5b164`, `022f131`, `46109c7`, `8ce9227`, `13fcd7a`, `a036db3`, `e0a5719`, `11f3301` |
| 2026-08-24 | A release script and launcher: one folder with a README. Upstream's release FFmpeg replaces the debug binaries, and the build fetches vcpkg | `27eea1e`, `ab8cb60`, `0b2202a`, `23d7ad4` |
| 2026-08-26 | One log format, tagged by layer and quiet by default, with repeated lines collapsed rather than printed hundreds of times | `fdc8325`, `f82d303` |
| 2026-08-28 | Button rebinding. A button sends a keyboard key or mouse action through synthetic USB devices, and the game stops seeing the button | `e1ae04d`, `693f00c`, `69635b6` |
| 2026-08-28 | Config mode and the chord that opens it. Two fingers plus Options opens the settings window; the pad drives it and the game hears nothing | `f9c2d8e`, `76ab5e8`, `a72c741`, `fa9bfa1`, `ce2aa02`, `f9270a5` |
| 2026-08-31 | The touchpad drives the mouse: one finger the cursor, two fingers scroll, a tap clicks. Clicking the pad in grabs a drag, lifting drops it | `229314e`, `6bddf3e` |
| 2026-08-31 | A stick drives the mouse, and a stick scrolls. Travel is speed times elapsed time, so it does not change with the report rate | `25527ef`, `b3cd198` |
| 2026-08-31 | An on-screen keyboard on a button. `OSKeyboard` toggles Steam's keyboard or Windows' `osk.exe`; only Steam's takes a controller | `470f1ea` |
| 2026-08-31 | Presets on New: gyro-to-mouse, stick-to-mouse, touchpad-mouse, L2-gyro-mouse-aiming. Ordinary configs, named after the preset | `6bddf3e`, `e44f113` |
| 2026-08-31 | One settings section at a time, and Safe Edit Mode names what the lock costs: basic controls only, or your inputs mirror in the game | `25527ef`, `988562f`, `b989322`, `35424b7` |
| 2026-08-31 | Tests stopped deleting the agent's configs. They ran where the agent reads and removed the user's files; they use a scratch dir now | `229314e` |

## Files changed

Generated from `git diff origin/main origin/rhqn-main --stat`, excluding
`third_party/` (9 files, +1333 -- the GamepadMotionHelpers library and
upstream's release FFmpeg binaries replacing the repo's debug ones).

```
 .gitignore                                    |   20 +-
 CHANGES.md                                    |   89 +
 LINK                                          |    0
 README.md                                     |   18 +
 app/ctm-usbip-tests.vcxproj                   |   94 +
 app/ctm-usbip.vcxproj                         |    4 +-
 attic/flydigi_apex4_identity.map              |   58 +
 attic/flydigi_apex4_usb.profile               |   24 +
 build-tests.ps1                               |   86 +
 build.ps1                                     |   87 +-
 device-config.md                              |  195 +++
 docs/rest_api.md                              |  139 ++
 include/ctm/map/runtime.h                     |    5 +
 maps/ds5_usb_over_ds5_usb.map                 |   61 +
 maps/virtual_keyboard.map                     |   46 +
 maps/virtual_mouse.map                        |   54 +
 profiles/descriptors/ds5e_composite.profile   |   30 +
 profiles/descriptors/virtual_keyboard.profile |   74 +
 profiles/descriptors/virtual_mouse.profile    |   59 +
 release.ps1                                   |  145 ++
 src/app/agent.inl                             |  331 +++-
 src/app/agent_session_sweep.inl               |  313 ++++
 src/app/cli.inl                               |   22 +-
 src/app/common.inl                            |    8 +
 src/app/open_ui.inl                           |  384 +++++
 src/app/rest.inl                              |  755 ++++++++
 src/app/rest_config.inl                       |  890 ++++++++++
 src/app/rest_config_sessions.inl              |   99 ++
 src/app/rest_sessions.inl                     |   33 +
 src/app/service.inl                           |   25 +-
 src/app/ui_page.inl                           |   73 +
 src/audio/audio_gain.inl                      |  178 ++
 src/audio/ds5_apply_settings.inl              |  169 ++
 src/audio/ds5_output_overrides.inl            |  550 ++++++
 src/audio/iso_in_pacing.inl                   |  221 +++
 src/audio/iso_in_test_tone.inl                |   95 +
 src/audio/mic_ring.inl                        |  202 +++
 src/audio/pcm_amplitude_log.inl               |  162 ++
 src/backend/backend.inl                       |   29 +
 src/backend/bridge.inl                        |  241 ++-
 src/backend/bridge_enet.inl                   |   35 +-
 src/backend/bt.inl                            |   16 +-
 src/config/config_presets.inl                 |  190 ++
 src/config/config_store.inl                   |  746 ++++++++
 src/config/config_watcher.inl                 |  170 ++
 src/config/device_config.inl                  |  218 +++
 src/input/gyro_calibration.inl                |  131 ++
 src/input/gyro_calibration_fetch.inl          |   95 +
 src/input/gyro_mouse.inl                      |  676 ++++++++
 src/input/keyboard_device.inl                 |  208 +++
 src/input/mouse_device.inl                    |  206 +++
 src/input/osk.inl                             |  120 ++
 src/input/rebind.inl                          |  803 +++++++++
 src/input/stick_mouse.inl                     |  338 ++++
 src/input/touch_mouse.inl                     |  359 ++++
 src/log/device_log.inl                        |  229 +++
 src/main.cpp                                  |  268 ++-
 src/map/runtime.cpp                           |    4 +
 src/usbip/device.inl                          |  412 ++++-
 src/usbip/server.inl                          |   55 +-
 tests/config_store_test.cpp                   |  567 ++++++
 tests/device_config_test.cpp                  |  463 +++++
 tests/gyro_mouse_test.cpp                     |  247 +++
 tests/harness.h                               |   55 +
 tests/host_audio_settings_test.cpp            |  125 ++
 tests/iso_in_pacing_test.cpp                  |  118 ++
 tests/map_defaults_test.cpp                   |  100 ++
 tests/osk_test.cpp                            |   91 +
 tests/rest_parser_test.cpp                    |  195 +++
 tests/stick_mouse_test.cpp                    |  512 ++++++
 tests/tests_main.cpp                          |   87 +
 tests/touch_mouse_test.cpp                    |  504 ++++++
 tests/units.h                                 |   54 +
 tools/controller-config-test-client.html      | 4194 +++++++++++++++++++++++++++++++++++++++++++++
 tools/device-config-panel-edge.bat            |    9 +
 tools/device-config-panel-edge.ps1            |  327 ++++
 tools/device-config-panel.bat                 |    4 +
 tools/device-config-panel.ps1                 |  303 ++++
 tools/start-ctm-usbip.bat                     |   67 +
 79 files changed, 19255 insertions(+), 114 deletions(-)
```
