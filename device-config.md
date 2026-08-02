# Device configuration

Per-device settings for controllers bridged to Windows, read from
`ctm-device-config.txt` beside the agent.

**Windows owns these settings.** The TV sends nothing and needs to know nothing
about them, so an unmodified client is unaffected.

---

## How it works

**A key that is present is ours. A key that is absent leaves the game alone.**
Absence means "not our business", never "use a default" — so every field the
file does not mention behaves exactly as it did before any of this existed.

Settings reach the controller two ways, and both apply **on save**, within about
a second. No reseat, no restart.

| | How it gets there |
|---|---|
| **Set** | Written to the controller when a session starts, and again whenever the file changes |
| **Defend** | A game that later overwrites a field gets corrected on the way past |

Neither covers the other's case. A setting a game never touches only needs the
first; a setting a game writes needs both.

---

## Sections

Sections are **device types**, matched on the controller's USB ids.

| Section | Device |
|---|---|
| `[ds5]` | DualSense |
| `[ds5_edge]` | DualSense Edge |

**Nothing is inherited.** An Edge does not fall back to `[ds5]` — it is a
different product, and a config that inherits is a config you cannot read. To
make an Edge behave like a DualSense, copy the lines.

A device with no section is left completely untouched.

---

## Gain and volume are different

| | Meaning | `100` means |
|---|---|---|
| `*_gain` | Scales a signal on its way through | **Unchanged** |
| `*_volume` | Sets the device's own output level | **Maximum** |

So `audio_gain = 100` does nothing, while `speaker_volume = 100` is full blast.
Both conventions are standard in audio work; the collision is inherent, not a
naming mistake.

Values are percentages used as multipliers, not decibels. Silence is negative
infinity dB, which is awkward in a text file — and "turn rumble off" is the most
likely thing anyone sets.

---

## Audio

### `audio_output`

The controller has **one mono speaker and a stereo headset jack**, and the
audio-control field routes three sinks rather than picking a destination. Four
routings exist:

| Value | Headset left | Headset right | Speaker |
|---|---|---|---|
| `headset` | Left | Right | muted |
| `headset_mono` | Left | Left | muted |
| `both` | Left | Left | Right |
| `speaker` | muted | muted | Right |

**There is no stereo-headset-plus-speaker mode.** The speaker is mono and is fed
the right channel, so anything using the speaker costs the headset its right
channel — `both` is unavoidably mono in the ears.

| Value | Effect |
|---|---|
| `auto` | Leave route and volumes exactly as the game set them |
| `headset` | Stereo headset, speaker muted |
| `headset_mono` | Headset with the left channel in both ears |
| `speaker` | Speaker only, headset muted |
| `both` | Speaker plus mono headset |
| `off` | Everything muted |

**The mode also decides which volume keys apply** — the output a mode does not
use is set to zero.

**The controller does not switch on its own** when a headset is plugged in, even
across a reseat. Routing has to be told. Its power-on default is the headset,
whether or not anything is plugged into the jack.

ⓘ Routing values measured on the wired path 2026-08-01 and cross-checked against
the Linux `hid-playstation` audio jack patch series.

### `speaker_volume`, `headset_volume`

`0`–`100`. The controller's own output levels — separate controls on separate
bytes, with **different full scales**: the speaker's raw maximum is `0x64` and
the headset's is `0x7f`. The percentage hides that.

⭐ **The percentage maps onto the usable range, not onto zero-to-maximum.** The
outputs have a high hardware floor — the Linux driver records the speaker's
accepted range as `0x3d`–`0x64`, so roughly the bottom 60% of a naive mapping is
below the floor and simply silent. `0` is off; `1`–`100` spans floor to full.

⚠️ **The headset's floor has not been measured.** The value in use is the
speaker's floor scaled to the headset's larger range — a reasonable guess,
nothing more.

### `audio_gain`

`0`–`100`. Digital scaling applied to the audio stream before it reaches the
controller.

**Not "speaker gain".** It scales the channels that feed whichever output the
routing selects, so with `audio_output = headset` it is the headset's gain.

Capped at 100 on purpose: game audio already runs near full scale, so boosting
digitally only clips.

Scaling is linear in amplitude and hearing is not, so a value sounds
considerably quieter than its number suggests.

### `force_echo_cancel`

`true` or `false`. The controller **mutes its own speaker** when echo
cancellation is off, because the microphone sits beside it.

Only applied when the speaker is actually in use. With audio going to the
headset there is no speaker output and nothing to cancel.

Games have been observed clearing this on exit, which is what leaves the speaker
silent afterwards.

---

## Rumble

The controller has **two separate rumble systems**, and both are called rumble.

**Two spinning weights** — a big one for deep thuds, a small one for fine buzz.
Two different feelings. Used by older and cross-platform titles.

**Audio-driven haptics** — two small speakers against the grips playing a
vibration waveform. The fine-grained system PS5-native titles use, and it is
stereo: left grip and right grip. It has no heavy/soft split by nature.

### `master_rumble_gain`

`0`–`500`, where `100` is unchanged. Scales **everything** — both weights and
both haptic channels.

**Boosting helps one system and not the other.** A game may drive the spinning
weights at a low level, so multiplying that is a real difference. The audio
haptics already run near full scale, so most of a boost there is clipped away —
measured as "200 felt similar to 100, if slightly stronger".

Reduction works cleanly on both.

ⓘ The upper bound of 500 is inherited from the Bluetooth haptics path's clamp
rather than chosen from evidence. It is a safety bound, not a recommendation.

### `rumble_gain_heavy`, `rumble_gain_soft`

`0`–`500`. The spinning weights only. **These multiply with the master**, like a
mixing desk: master 50 with heavy 50 gives the big weight 25%.

⚠️ **Unverified on hardware.** No game available for testing drives the weights
at all, so these have only ever been checked by unit test.

---

## Known limits

**Two identical controllers share one section.** Settings are keyed by device
type, so two DualSenses get the same values. Nothing available identifies one of
two identical controllers stably.

**The gains are not per-section.** `audio_gain` and `master_rumble_gain` read
`[ds5]` regardless of which device is connected — they are cached once for a
path that runs a hundred times a second. Only relevant with two *different*
device types connected at once.

**Wired only.** Everything here applies to controllers bridged over USB.

**The speaker preamp gain is untouched.** The controller has a separate preamp
setting for the speaker that nothing here writes. It is a second reason speaker
audio may be quieter than expected.
