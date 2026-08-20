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
| 2026-08-01 | Windows-side device configuration. A text config file, keyed by device type from the controller's USB ids, drives echo cancellation, audio routing, speaker and headset volumes, audio gain and rumble gain. Values are sent to the controller when a session starts and defended against later overwrites; a file watcher applies edits to a running session, so nothing needs a reseat. Adds a tagged, timestamped log of this fork's own output, and unit tests for the parser, the overrides and the log | `570ba61`, `20d14cb`, `fcc55ba`, `7cb8f1f`, `706b88c`, `382d18f`, `78dbdd1`, `2e6b42f`, `70c17dd`, `a7efc78`, `b496d08`, `a42dbe4`, `4fdf97f` |
| 2026-08-02 | Controller microphone audio, host side. The virtual controller's microphone endpoint answered every request instantly with silence, so the Windows audio driver resubmitted as fast as it was answered — measured at ~28,000 requests per second against the ~98 the stream needs, and the host's capture clock never advanced. Completions are now released after the duration of the audio they carry. Adds message type 12 for raw PCM travelling from the TV, a shallow ring that pads a shortfall with silence rather than waiting, and a diagnostic tone generator that is off by default. An unmodified client sees no change on the wire and gains the busy-loop fix | `76493f2`, `ec3c9de`, `99030fa`, `d7553ef` |
| 2026-08-04 | Microphone buffers are per session rather than per process. One buffer shared across every session meant a second controller drained the first one's audio, and a disconnect left stale audio for whatever connected next. Each session now owns its own, keyed by the backend it belongs to | `310df17`, `4a1d11e` |
| 2026-08-05 | Mute the controller's microphone from the host. The DualSense keeps microphone state in one byte of an output report; writing it from here means a controller cannot be left streaming by something else. The bit layout is asserted in tests rather than assumed | `f85de1a` |
| 2026-08-05 | Accept the DualSense Edge. It answers with its own USB descriptor, captured from real hardware, so Windows identifies it as an Edge rather than a standard DualSense -- which is what makes its extra controls work | `8fbb26d`, `9d8e3d7` |
| 2026-08-11 | Hold the audio stream open briefly after a controller is bridged, and let the TV ask for it to be held longer. The controller's audio subsystem sleeps when nothing is playing, and waking it takes long enough to swallow the start of a sound | `4edd287`, `e9eceea`, `0d0c907` |
| 2026-08-12 | A settings panel for the DualSense Edge, so its device configuration can be edited without hand-writing the file | `e998d29` |
| 2026-08-12 | Drop the audio hold. The TV signals its own controllers now, so the host no longer needs to keep the stream awake on its behalf | `f36f272` |
| 2026-08-13 | Drop microphone reports before anything reads them as pad state. A controller told to stream its microphone sends audio in the same report id and length it uses for buttons, one flag apart. Anything that does not check the flag reads audio as thousands of button presses a second | `9bdbf81` |
| 2026-08-16 | The host owns the controller's Bluetooth audio buffer. Set `audio_latency_ms` in the device config: it travels in the host configuration at handshake and again whenever the file changes, and the TV applies it to a live session, so a change is audible within a second or two without reconnecting the controller. Measured across its range, the value is a buffer in milliseconds -- below eight the controller has less than one Opus frame to play and falls silent, which the log now says plainly rather than leaving unexplained | `c3e2496`, `1b12d9e` |
| 2026-08-19 | Stop preloading the feature reports a Bluetooth DualSense never answers. Nine requests, each waited out, ran over the same Bluetooth link as the TV's confirmation tone and took the controller offline mid-bridge. The TV's log separated cause from symptom because two events run the same code: an unbridge delivered 600 ms of audio in 600 ms, while a bridge took 15,531, 5,454 and 5,401 ms for the same audio. Moving the probes to a background thread was not enough, and delaying them three seconds was worse because that lands in the tail of the tone. They are a cache, so a cold cache costs one live fetch instead. Improved rather than fixed: one bad call in seventeen remained, and every write still succeeded, so it is contention for the link rather than rejection. Seen on one television only, a C3, which is also the only set showing a slow device enumeration | `366b0de`, `08dabcd` |

## Files changed
```
 .gitignore                         |  11 +-
 CHANGES.md                         |  57 +++++
 app/ctm-usbip-tests.vcxproj        |  87 ++++++++
 build-tests.ps1                    |  86 ++++++++
 build.ps1                          |   3 +-
 device-config.md                   | 195 +++++++++++++++++
 include/ctm/map/runtime.h          |   2 +
 maps/ds5_usb_over_ds5_usb.map      |  61 ++++++
 src/app/agent.inl                  |  19 +-
 src/app/agent_session_sweep.inl    | 193 ++++++++++++++++
 src/audio/audio_gain.inl           | 178 +++++++++++++++
 src/audio/ds5_apply_settings.inl   | 112 ++++++++++
 src/audio/ds5_output_overrides.inl | 438 +++++++++++++++++++++++++++++++++++++
 src/audio/iso_in_pacing.inl        | 221 +++++++++++++++++++
 src/audio/iso_in_test_tone.inl     |  95 ++++++++
 src/audio/mic_ring.inl             | 146 +++++++++++++
 src/audio/pcm_amplitude_log.inl    | 162 ++++++++++++++
 src/backend/backend.inl            |   4 +
 src/backend/bridge.inl             |  30 ++-
 src/config/config_watcher.inl      | 153 +++++++++++++
 src/config/device_config.inl       | 200 +++++++++++++++++
 src/log/device_log.inl             | 126 +++++++++++
 src/main.cpp                       |  16 ++
 src/map/runtime.cpp                |   1 +
 src/usbip/device.inl               |  61 +++++-
 src/usbip/server.inl               |  28 +++
 tests/device_config_test.cpp       | 415 +++++++++++++++++++++++++++++++++++
 tests/harness.h                    |  55 +++++
 tests/iso_in_pacing_test.cpp       | 118 ++++++++++
 tests/map_defaults_test.cpp        | 100 +++++++++
 tests/tests_main.cpp               |  16 ++
 tests/units.h                      |  47 ++++
 tools/device-config-panel.bat      |   4 +
 tools/device-config-panel.ps1      | 303 +++++++++++++++++++++++++
 34 files changed, 3732 insertions(+), 11 deletions(-)
```
_Generated with `git diff upstream/main --stat`. Regenerate this block
rather than editing it by hand._

_`CHANGES.md`'s own line count above is a snapshot taken before this
revision was written, so it always lags by one edit. Expected, not a
discrepancy._
