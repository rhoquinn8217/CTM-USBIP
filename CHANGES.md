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

## Files changed
```
 .gitignore                      |   9 ++-
 CHANGES.md                      |  49 ++++++++++++
 app/ctm-usbip-tests.vcxproj     |  83 ++++++++++++++++++++
 build-tests.ps1                 |  86 +++++++++++++++++++++
 build.ps1                       |   3 +-
 include/ctm/map/runtime.h       |   2 +
 maps/ds5_usb_over_ds5_usb.map   |  61 +++++++++++++++
 src/app/agent.inl               |  12 ++-
 src/app/agent_session_sweep.inl | 153 +++++++++++++++++++++++++++++++++++++
 src/audio/pcm_amplitude_log.inl | 162 ++++++++++++++++++++++++++++++++++++++++
 src/backend/backend.inl         |   4 +
 src/backend/bridge.inl          |  23 +++++-
 src/main.cpp                    |   4 +
 src/map/runtime.cpp             |   1 +
 src/usbip/device.inl            |  42 ++++++++++-
 tests/harness.h                 |  55 ++++++++++++++
 tests/map_defaults_test.cpp     | 100 +++++++++++++++++++++++++
 17 files changed, 840 insertions(+), 9 deletions(-)
```
_Generated with `git diff upstream/main --stat`. Regenerate this block
rather than editing it by hand._

_`CHANGES.md`'s own line count above is a snapshot taken before this
revision was written, so it always lags by one edit. Expected, not a
discrepancy._
