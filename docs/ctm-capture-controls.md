# CTM Capture — Controls & Commands

`ctm-capture.exe` captures a display (default: the **last** output = the VDD
virtual monitor), encodes it (native AMF), streams over CTMS, and shows two
preview windows: **A** = raw capture, **B** = decoded loopback (the same path the
TV runs). Run from `out\x64\Release\ctm-capture.exe`.

## CLI flags

| Flag | Effect |
|------|--------|
| `--list` | List DXGI outputs and exit |
| `--shot <idx> <png>` | Save a screenshot of output `<idx>` and exit |
| `--output <idx>` | Capture output index (default: last = VDD) |
| `--single` | Single window: raw preview only (no encode/stream) |
| `--nopreview` | Stream only, no windows |
| `--nodecode` | No local decode; hide window B (encode-only profiling). Also toggle with `D` |
| `--profile` | Measure true HW-decode GPU time (adds a sync; off by default) |
| `--cpucopy` | Force CPU encode input (disable zero-copy) |
| `--fps <n>` | Cap output fps (0 = uncapped). NOTE: distinct from the encoder's rate-control fps, which auto-detects the display refresh |
| `--codec <hevc\|av1>` | Video codec |
| `--mode <cbr\|vbr\|cqp>` | Rate control |
| `--usage <ull\|ll\|transcode>` | AMF usage preset |
| `--bitrate <kbps>` | Target bitrate (CBR/VBR) |
| `--maxrate <kbps>` | Peak bitrate (VBR) |
| `--qp <n>` | Quantizer (CQP) |
| `--intra` | All-intra (every frame an IDR) |
| `--res <height>` | `0`(native) `2160` `1440` `1080` `720` `540` |
| `--paperwhite <nits>` | SDR paperwhite for HDR tonemap |

## Hotkeys (focus window A or B)

| Key | Action |
|-----|--------|
| `C` | Codec HEVC ⇄ AV1 |
| `M` | Rate mode CBR → VBR → CQP |
| `R` | Resolution (native/2160/1440/1080/720/540) |
| `U` | Usage ull → ll → transcode |
| `I` | All-intra toggle |
| `F` | Intra-refresh toggle (rolling I, no IDR spike) |
| `S` | Slices/tiles 1 → 2 → 4 → 8 |
| `A` | Per-frame size cap off → 1000 → 2000 → 4000 kbits |
| `V` | Vsync toggle |
| `D` | Decode-window toggle (hide B + stop local decode) |
| `B` | Decode bench — serialized; prints latency / throughput / hold to console |
| `Up`/`Down` | Bitrate ±5000k (CBR/VBR) **or** QP ∓1 (CQP) |
| `Left`/`Right` | Maxrate ±5000k |
| `Esc` | Quit |

## Notes

- **CQP**: `Up`/`Down` changes **QP**, not bitrate — there is no bitrate target
  (`br`/`max` are ignored). For a bitrate target, switch to **CBR/VBR** (`M`).
- **CBR**: the target applies, but actual Mb/s is **content-limited** — a static
  screen won't fill the target, so watch the *target* field, not just Mb/s.
- **Encoder framerate** auto-detects the display refresh (rate-control budget),
  shown as `@<fps>` in the `reconfigured:` log. At 120 Hz it budgets for 120.
- **Title / console** show: `codec · mode · usage · WxH · target(br/max or qp) ·
  flags(allI/IR/S/AU) · fps e/d/s · enc/dec/g2g ms · Mb/s`.
- **`B` decode bench**: needs a keyframe captured first (let it stream briefly).
  Runs on a *separate* decoder (live path untouched) and prints:
  - `LATENCY submit->exit` — real per-frame decode latency, free of the 60/120fps
    arrival pacing (avg/p50/p99/max).
  - `THROUGHPUT` — sustained decode fps (ms/frame) when fed back-to-back.
  - `pipeline depth` — frames in flight before the first output: **1 = no hold**,
    **>1 = the decoder holds frames** (the 1-frame-hold question).
- **Latency model**: `g2g = d2 - t0` (display draw − source present) is the
  glass-to-glass truth and tracks the frame interval; `decode d1-d0` on the live
  path is the decoder's queue/pacing wait (not compute) — use `B` for real decode
  timing.
