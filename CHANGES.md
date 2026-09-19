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
| 2026-09-08 | Options moves the settings window and R3 sizes it, the same gesture the on-screen keyboard uses. A tap places it, a hold steers it with the stick or the mouse | `764e06c`, `b8ffac7`, `01f247d`, `239000b`, `79c03b4` |
| 2026-09-09 | Three layouts for the settings window: SIMPLE, one controller and its config; QUICK, the same cut to a name and a selector for reaching mid-game; ADVANCED, the full page. Create goes straight to Quick, and the listener remembers the layout, size, place and controller across a close | `339ef21`, `6f83ccf`, `fc6982a`, `9097921`, `faa5760`, `36dae19`, `241e69a` |
| 2026-09-09 | One Options mover per controller: two bridged pads shared one, so a hold on either snapped the window on every report of the other. The stick that steers a window no longer also drives the mouse, and travel is speed times elapsed time, so it does not change with the report rate | `1526c97`, `1256eea`, `5f9e4db` |
| 2026-09-10 | The touchpad preset scrolls naturally, and its description says so | `f8ad9ac` |
| 2026-09-10 | Adaptive trigger effects: a resistance break, a wall, or a climb that lets go, placed anywhere in the pull | `bbfcd5f`, `9f9b847`, `6cbe348`, `eaf2e55`, `33ff84d` |
| 2026-09-10 | R2 as a mouse click: the cursor freezes for the whole gesture, so a double click lands twice on one pixel, and a held click becomes a drag | `890fab0`, `1db8342` |
| 2026-09-11 | A trigger is bound in one place. `rebind_6` and `rebind_7` say what it sends, as for any other button, and a per-side mode says what pulling it does to the cursor. Setting both no longer sends two presses per pull | `c948bad`, `7abdd1e`, `6e873e6`, `962e7fc`, `b285c83`, `9262f50`, `f0d70f6` |
| 2026-09-11 | The press lands on the break the finger feels. The controller is asked rather than a travel number guessed: byte 42 for the right trigger, byte 43 for the left, both confirmed on hardware. Two thresholds so a resting trigger cannot chatter | `aa00e2a`, `4950379`, `573d079`, `f8ab6eb`, `e935859`, `dac514b`, `8a9cd41`, `fd15276` |
| 2026-09-11 | Four trigger feels, each measured rather than assumed: a break, a wall, a wall with a detent, and a break that returns itself. A notch and a snap press on travel, because the hardware cannot name their moment. An effect of "off" now clears the trigger whenever it is asked for | `64efd09`, `86317a5`, `265418c`, `8f1a672`, `856e615`, `5325e54`, `458f4f6`, `7e7001e`, `ca47551`, `033758f` |
| 2026-09-11 | The gyro gate and the trigger's steady are separate settings, so one trigger can open the gate on a light hold and click at its break. The drag and double-click windows are per side, because which trigger wants them follows what it is bound to | `258c07b`, `fdbc0ca`, `ccfa2d0` |
| 2026-09-11 | Two ways a mouse button could be left held down, both closed: the rebinder went silent once it gave a trigger up, and the pump recorded a release as sent before checking there was a device to send it to | `012fef6` |
| 2026-09-11 | Settings page 2.64.48: trigger settings grouped and ordered, the compact config picker steps in place instead of opening a list taller than its window, preset previews drop tuning numbers and passthroughs, and the trigger log reports both sides | `dad60d1`, `f763fe1`, `e0b521d`, `91b2c38`, `b06d011`, `683b091`, `3daa7a3`, `3c33c4f`, `9d74ebd`, `012d986`, `9006b65` |
| 2026-09-12 | Check which pad it is before reading the pad. The mouse hooks, the rebinder and the gates read DualSense offsets on whatever arrived, so a DS4's timestamp read as a button; each path now asks the layout first | `5e270d1` |
| 2026-09-12 | A button layout per pad, resolved once per report, with an Xbox table read off its own map file. Config mode rests a pad at its own offsets, where DualSense positions had frozen an Xbox report instead of resting it | `96ecdcf`, `f60b096` |
| 2026-09-12 | A config is no longer tied to a controller type. Any config links to any controller, each setting checks the pad before it acts, and an existing DS5 config keeps working unchanged | `3800cff` |
| 2026-09-14 | A new bridge retires an older one only when the serial and the TV's node both match, so a second pad no longer displaces the first | `e99dcf5` |
| 2026-09-14 | A mouse, keyboard or other bridged part is never dropped for being idle. Only a pad has an idle timeout, because only a pad is expected to keep talking | `8177f0e` |
| 2026-09-15 | A cabled DualShock 4 is a controller in its own right: its own kind, map and button table, steering the mouse by gyro, touchpad and stick from its own layout, named as itself in the text and the presets | `c3e7852`, `a926df5`, `ef0c5a9`, `53bd775`, `d4c1d5b`, `dbbcd83` |
| 2026-09-15 | An Xbox trigger pulled past a threshold presses like a button. Its triggers carry no digital bit, so every binding on them had been silent | `192e2d1` |
| 2026-09-15 | Each pad keeps its own held mouse buttons and its own chord state, so a pad lying at rest no longer releases another pad's drag | `f955fd4`, `449003f` |
| 2026-09-15 | The left stick scrolls even when no stick is moving the cursor | `d709b51` |
| 2026-09-15 | A trigger whose steady switch is off fires its remap again | `15da386` |
| 2026-09-15 | A map's handshake outranks the pad's own input in the queue, and every bridged pad announces its own id rather than the captured one | `55b6a79`, `581efbe` |
| 2026-09-15 | Diagnostics: the first fifty served packets are written out, the queue cap says when it eats one, and the log says whether the host is collecting what we queue | `833029e`, `5bf617d` |
| 2026-09-16 | The microphone guard writes to a DualSense, not to every device whose report is the same shape | `1f84fec` |
| 2026-09-17 | The gyro's hold has one definition, shared with the trigger tests rather than copied | `5ce687a` |
| 2026-09-17 | `device.log` is capped at 20 MB with one older file kept, and the every-report lines move behind their own switch. An unattended run had written 227 MB | `757b51a`, `61b0ddd` |
| 2026-09-17 | DS5-USBIP has a name and a version of its own, 0.1.0, shown from one place: the command line, the file properties, the status endpoint and the page | `827556c` |
| 2026-09-17 | A device is named by its model, then its type, and "hid" only when nothing else is known | `f407995` |
| 2026-09-17 | A serial of ALL ZEROS is no identity. One refusal in `normalise_serial`, so a config file's claim is dropped as it is read, nothing auto-links to it, and adding such a claim is refused. All zeros, not leading zeros: a Switch Pro Controller's `000000000001` is a real per-unit serial | `49084cc` |
| 2026-09-18 | The pad's own charge, read off the report the listener already receives, and shown as a battery beside the controller's name in all three views and as a column in both device tables. Offsets measured on the pads, not taken from a header; a pad with no battery byte, or in a fault state, shows nothing at all rather than zero | `929760c`, `88bc398`, `4e17c9b`, `a69278f`, `01f8d67`, `2823912`, `73f298c` |
| 2026-09-18 | The settings page talks to the port it was SERVED from instead of a hardcoded 48055, so a listener on any other REST port no longer looks dead while answering | `98046e8` |
| 2026-09-18 | The tray icon is a controller rather than a keyboard, and a click opens a menu -- settings or the keyboard -- instead of opening the keyboard outright | `cfefe25`, `c211ae4` |
| 2026-09-18 | The TV's overlay chord is taken out of a bridged Xbox pad's report: while both bumpers are held, Select and Start are cleared before Windows is served them, so the chord opens the TV's overlay without Steam opening its keyboard behind it. The bumpers, the face buttons and the d-pad are untouched | `0063ce8` |
| 2026-09-19 | The parts of one device share one nickname. A dongle arrives as two or three separate bridges and each was named on its own, so one device answered to two words that exist only to be said out loud. Measured across three multi-part devices: every part reaches the listener with an identical product string, because the TV sends the USB device's iProduct and not the part's own name | `c799ad3` |
| 2026-09-19 | A rumble floor, so a faint cue stays faint rather than vanishing. A straight multiply at low gain leaves a motor below the amplitude that starts it turning. `rumble_floor` is a percentage, default 12, which is the floor DS4Windows settled on. A motor the game asked to stop, and a motor the person gained to zero, are both left silent | `ae13560` |
| 2026-09-19 | A game's own adaptive trigger effect no longer replaces the config's. The config's effect is sent once, when a config links, while a DualSense-aware game sends its own trigger blocks in every output report -- which took the trigger remaps with them, since a click remap fires on a break that was no longer there. Only a trigger the host is actually claiming is rewritten | `33bcede` |
| 2026-09-19 | A gear on the on-screen keyboard's tab, on all three faces, opening the config window. The keyboard swallows the pad while it is up, so the window was unreachable without knowing a chord. Each tab row must now span its face exactly, checked at compile time rather than noticed on a television | `3b6383c` |

## Files changed

Generated from `git diff origin/main origin/rhqn-main --stat`, excluding
`third_party/` (9 files, +1333 -- the GamepadMotionHelpers library and
upstream's release FFmpeg binaries replacing the repo's debug ones).

```
 .gitattributes                                     |   48 +
 .gitignore                                         |   30 +-
 CHANGES.md                                         |  205 +
 LINK                                               |    0
 README.md                                          |   18 +
 app/ctm-usbip-tests.vcxproj                        |  107 +
 app/ctm-usbip.rc                                   |   11 +-
 app/ctm-usbip.vcxproj                              |    4 +-
 attic/flydigi_apex4_identity.map                   |   58 +
 attic/flydigi_apex4_usb.profile                    |   24 +
 build-tests.ps1                                    |   86 +
 build.ps1                                          |   88 +-
 device-config.md                                   |  195 +
 docs/rest_api.md                                   |  139 +
 include/ctm/map/runtime.h                          |   30 +
 include/ctm/product.h                              |   32 +
 maps/ds4_usb_over_ds4_usb.map                      |   61 +
 maps/ds5_usb_over_ds5_usb.map                      |   61 +
 maps/virtual_keyboard.map                          |   46 +
 maps/virtual_mouse.map                             |   54 +
 maps/xbox_gip_usb_over_xbox_bt.map                 |    5 +
 profiles/descriptors/ds5e_composite.profile        |   30 +
 profiles/descriptors/virtual_keyboard.profile      |   74 +
 profiles/descriptors/virtual_mouse.profile         |   59 +
 release.ps1                                        |  152 +
 src/app/agent.inl                                  |  566 +-
 src/app/agent_session_sweep.inl                    |  320 +
 src/app/cli.inl                                    |   24 +-
 src/app/common.inl                                 |   32 +
 src/app/config_move.inl                            |  538 ++
 src/app/device_type.inl                            |   70 +
 src/app/nickname.inl                               |   90 +
 src/app/open_ui.inl                                |  489 ++
 src/app/overlay_window.inl                         | 1995 ++++++
 src/app/rest.inl                                   |  760 +++
 src/app/rest_config.inl                            | 1128 ++++
 src/app/rest_config_sessions.inl                   |  201 +
 src/app/rest_sessions.inl                          |   33 +
 src/app/same_controller.inl                        |   38 +
 src/app/same_device.inl                            |   79 +
 src/app/service.inl                                |   25 +-
 src/app/tray_icon.inl                              |  238 +
 src/app/ui_page.inl                                |   73 +
 src/app/window_move.inl                            |  245 +
 src/audio/audio_gain.inl                           |  178 +
 src/audio/ds5_apply_settings.inl                   |  310 +
 src/audio/ds5_output_overrides.inl                 |  699 ++
 src/audio/iso_in_pacing.inl                        |  221 +
 src/audio/iso_in_test_tone.inl                     |   95 +
 src/audio/mic_ring.inl                             |  202 +
 src/audio/pcm_amplitude_log.inl                    |  162 +
 src/audio/rumble_floor.inl                         |   58 +
 src/backend/backend.inl                            |   29 +
 src/backend/bridge.inl                             |  256 +-
 src/backend/bridge_enet.inl                        |   35 +-
 src/backend/bt.inl                                 |   16 +-
 src/config/config_presets.inl                      |  387 ++
 src/config/config_store.inl                        |  820 +++
 src/config/config_watcher.inl                      |  170 +
 src/config/device_config.inl                       |  218 +
 src/input/battery.inl                              |  105 +
 src/input/button_layout.inl                        |  919 +++
 src/input/chord_gate.inl                           |   66 +
 src/input/gyro_calibration.inl                     |  168 +
 src/input/gyro_calibration_fetch.inl               |   99 +
 src/input/gyro_hold.inl                            |   48 +
 src/input/gyro_mouse.inl                           |  787 +++
 src/input/keyboard_device.inl                      |  301 +
 src/input/mic_report.inl                           |   41 +
 src/input/mouse_device.inl                         |  304 +
 src/input/mouse_exclusive.inl                      |  122 +
 src/input/mouse_held.inl                           |   99 +
 src/input/osk.inl                                  |  200 +
 src/input/rebind.inl                               | 1232 ++++
 src/input/stick_mouse.inl                          |  471 ++
 src/input/touch_mouse.inl                          |  409 ++
 src/input/trigger_click.inl                        |  801 +++
 src/input/trigger_effect.inl                       |  630 ++
 src/log/capped_log.inl                             |  117 +
 src/log/device_log.inl                             |  233 +
 src/main.cpp                                       |  439 +-
 src/map/runtime.cpp                                |   68 +-
 src/usbip/device.inl                               |  650 +-
 src/usbip/server.inl                               |   55 +-
 tests/button_layout_test.cpp                       |  953 +++
 tests/capped_log_test.cpp                          |  140 +
 tests/config_store_test.cpp                        |  777 +++
 tests/device_config_test.cpp                       |  521 ++
 tests/device_type_test.cpp                         |   89 +
 tests/gyro_mouse_test.cpp                          |  450 ++
 tests/harness.h                                    |   55 +
 tests/host_audio_settings_test.cpp                 |  125 +
 tests/iso_in_pacing_test.cpp                       |  118 +
 tests/map_defaults_test.cpp                        |  100 +
 tests/mic_report_test.cpp                          |   64 +
 tests/mouse_held_test.cpp                          |  162 +
 tests/nickname_test.cpp                            |   98 +
 tests/osk_test.cpp                                 |   81 +
 tests/product_version_test.cpp                     |   33 +
 tests/rest_parser_test.cpp                         |  195 +
 tests/rumble_floor_test.cpp                        |   87 +
 tests/same_controller_test.cpp                     |   49 +
 tests/same_device_test.cpp                         |   95 +
 tests/schema_json_test.cpp                         |  147 +
 tests/stick_mouse_test.cpp                         |  710 ++
 tests/tests_main.cpp                               |  115 +
 tests/touch_mouse_test.cpp                         |  664 ++
 tests/trigger_click_test.cpp                       |  835 +++
 tests/trigger_effect_test.cpp                      |  529 ++
 tests/units.h                                      |   54 +
 .../ffmpeg/x64/release/bin/swresample-6.dll        |  Bin 722944 -> 835584 bytes
 tools/controller-config-test-client.html           | 6959 ++++++++++++++++++++
 tools/device-config-panel-edge.bat                 |    9 +
 tools/device-config-panel-edge.ps1                 |  327 +
 tools/device-config-panel.bat                      |    4 +
 tools/device-config-panel.ps1                      |  303 +
 tools/osk-mockups.py                               |  103 +
 tools/start-ctm-usbip.bat                          |   67 +
 126 files changed, 36987 insertions(+), 145 deletions(-)
```
