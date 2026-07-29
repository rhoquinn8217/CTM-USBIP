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
| 2026-07-29 | Ported T-028 PCM amplitude logging into the wired ISO path | `e3afa55`, `841d9a2` |

## Files changed

```
 .gitignore                      |   9 +--
 CHANGES.md                      |  42 ++++++++++++
 app/ctm-usbip-tests.vcxproj     |  83 ++++++++++++++++++++++++
 build-tests.ps1                 |  86 ++++++++++++++++++++++++
 build.ps1                       |   3 +-
 include/ctm/map/runtime.h       |   2 +
 maps/ds5_usb_over_ds5_usb.map   |  61 +++++++++++++++++
 src/app/agent.inl               |   8 ++-
 src/audio/pcm_amplitude_log.inl | 140 ++++++++++++++++++++++++++++++++++++++++
 src/backend/backend.inl         |   4 ++
 src/backend/bridge.inl          |  10 +++
 src/main.cpp                    |   4 ++
 src/map/runtime.cpp             |   1 +
 src/usbip/device.inl            |  42 +++++++++-
 tests/harness.h                 |  55 ++++++++++++++
 tests/map_defaults_test.cpp     | 100 ++++++++++++++++++++++++++++
 16 files changed, 643 insertions(+), 7 deletions(-)
```

_Generated with `git diff upstream/main --stat`. Regenerate this block
rather than editing it by hand._

_`CHANGES.md`'s own line count above is a snapshot taken before this
revision was written, so it always lags by one edit. Expected, not a
discrepancy._
