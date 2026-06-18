# L-path Haar-wavelet streaming — master plan (end-to-end)

Low-latency 4K path: PC (AMD 9070 / AMF encode) → LAN → LG webOS TV (own-decode + GLES recombine).
This is the complete design: host decomposition → transport → TV decode+recombine, the latency model, the
roadmap, and every locked decision. Authority for the locked design is the `lpath-keeper` agent
(`D:\Work\CTM\.claude\agents\lpath-keeper.md`); this doc transcribes it. **Task 10** in the task list is the
first concrete slice of this (host GPU decomp/recomp + numpy oracle).

> **Framing note (memory-updated):** the original keeper TV-side model assumed NDL v2 feed-and-forget +
> GStreamer `appsrc!lxvideodec!appsink`. On-device RE since 2026-06-16 moved the TV decode path to
> **own-the-decode via `liblxv` (lxvdec)**: user-DPB into our own dmabuf, zero-copy to GLES, display via
> `videooutput` luna + `wl_webos_foreign` punch-through. The latency-**priority** decisions remain binding;
> the transport-to-shader **plumbing** follows the proven lxvdec/GHW path, not the old GStreamer assumption.

---

## 1. Concept

Haar-decompose each captured frame into an **LL** band (a real small-resolution image — the clean 2×2-box
downscale) and **detail** bands (HL/LH/HH — high-frequency differences). Encode LL as its own small-res
elementary stream = the low-latency **L-path master**. Encode detail separately as best-effort refinement.
The TV decodes LL always (cheap, small), upscales+presents it immediately, and adds detail on top when it's
ready and still valid. **Latency is pinned to the LL path; quality floats** with available compute/bandwidth.

Haar is chosen for the cheap add/subtract and the property that LL = a clean 2×2 box downscale. **Do not
substitute another wavelet.** Default 1 level (LL/LH/HL/HH); deeper = recurse on the LL chain only.

---

## 2. Locked decisions (the numbered on-record set — exactly four)

1. **Encode LL as a real small-resolution stream** (1080p / 540p), NOT a 4K frame with zeroed regions — so
   the decoder genuinely does less work.
2. **A single decode session must suffice** (worst-case TV assumption): decode LL only → upscale → present.
   If 2+ concurrent sessions exist, also decode detail. Degrade gracefully.
3. **Keep LL untiled / single-tile.** Tiling only in detail bands — detail seams get smeared away by
   recombine/upscale (invisible); LL seams would NOT hide. **Never tile LL.**
4. **Modest tile counts** (level-1 / ~4-way) are the sweet spot — more tiles = more seams; per-frame fixed
   overhead caps the benefit of going deeper.

**Binding rules (not numbered, but locked):**
- **Scheduling:** L-chain always prioritized; a fresh LL is a *hard interrupt* — present it now, drop
  in-flight detail; present-first; latency pinned to LL, quality floats. (§5)
- **Deghosting:** stale-detail-over-fresh-LL handled by an LL-gradient gating mask (reuse detail in static
  regions, fall back to upscaled-LL in moving regions); optional LL motion-warp. (§4)
- **Capture timing:** t0 = present timestamp (anchor; sometimes 0 on a virtual display → fallback); t1
  (GPU-copy completion) not cheaply measurable, opt-in profiling only.
- **Codec-agnostic:** AV1/HEVC/H264 are interchangeable engines, not the point.

---

## 3. Host side — decomposition + encode

Current host pipeline: `DXGI duplication → f1 (FP16 scRGB) → convert shader → P010 (PQ BT.2020 4:2:0) →
AMF encode → CTMS/TCP`, today zero-copy into the AMF P010 surface (`Converter` class,
`CTM-USBIP\src\capture\main.cpp` ~950–1110). The wavelet stage inserts after the P010 convert.

**Decisions locked for the host (task 10):**
- **DWT input surface: P010** (post-convert, 10-bit YUV). LL subbands come out encode-ready; integer Haar is
  lossless; chroma handled at its own 4:2:0 res.
- **Buffers: ping-pong** dst surface (not in-place) — GPU-friendly, hazard-free, faster.
- **Encoders: 2 total** — LL = its own untiled low-latency session; all detail bands (HL/LH/HH) = one second
  untiled session. (Scale to ~4-way detail tiling later, gated on the TV decode side.)
- **Recomposition = host-side loopback verification only** (the TV does the real recombine in GLES).
- AMF tiled-output / frame-in-slice-out is the per-tile mechanism (AMF 1.4.30+) for when detail gets tiled.

**Forward Haar (1-level) on a 2×2 group {a=TL, b=TR, c=BL, d=BR}:**
```
LL = (a+b+c+d)/4     (clean 2×2 box downscale — the master)
HL = (a−b+c−d)/4     (horizontal detail)
LH = (a+b−c−d)/4     (vertical detail)
HH = (a−b−c+d)/4     (diagonal detail)
```
**The numpy oracle (task 1) is the authority** on the exact normalization/scale factors — the GPU shader and
the TV recombine shader must match it bit-for-bit. Treat the *structure* as locked, the *scale factors* as
oracle-defined. Detail bands are signed → need a bias (e.g. +mid for 10-bit) to encode as unsigned video.

---

## 4. TV side — decode + GLES recombine

**Plumbing (memory-updated): own-decode via lxvdec → linear NV12 in our dmabuf → EGLImage → GLES shader →
display via videooutput punch-through.** The recombine math is path-agnostic; the plumbing follows the proven
lxvdec/GHW path, NOT the old `appsrc!lxvideodec!appsink`.

**Inverse Haar (TV shader), reconstruct the 2×2:**
```
a = LL + HL + LH + HH
b = LL − HL + LH − HH
c = LL + HL − LH − HH
d = LL − HL − LH + HH
```
(scale factors per the oracle — must match it bit-for-bit; that's the acceptance gate.)

- **LL upscale:** sample LL with the GPU bilinear sampler at 2× (it's a real small texture — HW bilinear is
  free). Reconstructed detail is the **correction added on top**. Detail absent/dropped → present
  **upscaled-LL alone** (the latency-floor output).
- **LL-gradient deghost mask** — *why:* detail refreshes slower than LL; adding stale detail (frame N−k) onto
  a region that moved in fresh LL (frame N) paints **ghosts**. *What:* a per-pixel blend weight between
  {upscaled-LL-only} and {LL + retained-detail}. *How:* compute the spatial/temporal gradient of LL (current
  vs previous LL, and/or local spatial gradient). High gradient ⇒ motion ⇒ fall back to upscaled-LL; low
  gradient ⇒ static ⇒ reuse detail (full inverse-Haar). Cheap shader pass over the small LL texture.
  *Optional (open):* LL-derived motion-warp to reposition retained detail instead of dropping it — not v1.
- **Sampling:** LL as small bilinear-upscaled texture; detail band(s) at detail res; shader runs inverse-Haar
  per output 2×2, applies the deghost weight to the detail contribution, writes the full-res pixel. Detail
  tile seams smeared away by recombine; LL single-tile = no seams.

**Known decode-plumbing bugs to clear before recombine (recorded on the own-decode path):** NV12 EGLImage
needs the YUV color-space/range/siting EGL hints or you get a black frame; DPB starvation if buffers aren't
returned on the right thread; cache-invalidate before any CPU read.

**OPEN/UNVERIFIED — 10-bit vs 8-bit:** host encodes P010 (10-bit); the lxvdec user-DPB path outputs linear
NV12 (`set_memory_format(RASTER)`). Whether it **preserves 10-bit (P010/NV12-10) or collapses to 8-bit** is
unverified — if 8-bit, the small-magnitude detail differences band. *Check:* decode a known 10-bit clip via
the lxvdec user-DPB path, inspect the output buffer's actual format enum. (Host stays 10-bit either way; TV
downconverts gracefully if needed — this does not change the host's P010 choice.)

---

## 5. L-path scheduling (end-to-end) — LOCKED, the core invariant

**Rule:** the L-chain is prioritized ALWAYS. A fresh LL band is a **hard interrupt** — present it immediately
with whatever detail is ready, and **DROP in-flight detail decode**.

**Host (encode/send order):** LL encoded + submitted FIRST, on its own session, ahead of detail for the same
frame; LL frame N on the wire before detail frame N (transport tag marks LL priority). Detail-send never
blocks LL-send. Host treats LL as the clock — never waits for detail before LL of frame N+1.

**TV (decode/present order):** LL has a dedicated decode session (decision 2); detail decode is secondary +
**preemptible**. On LL frame N arrival: present it (upscaled + whatever detail ≤N is ready and passes
deghost) **immediately**; abandon any in-flight detail decode for older frames (the hard interrupt).
**Present-first:** present driven solely by LL availability, never gated on detail. Late detail is applied as
a refinement to the still-displayed frame only if no newer LL has arrived, else dropped (deghost decides).
Present path: immediate/mailbox, bypass compositor/vsync.

---

## 6. Transport / protocol — LARGELY OPEN (constraints locked)

**Locked constraints:** LL and detail are **separate elementary streams** (decision 1 forces it; decision 2:
LL decodable without ever touching detail). Detail must be **independently discardable at the framing layer**
— losing detail must not corrupt the LL bitstream.

**Recommended (propose-and-lock, not yet locked):** reuse the existing **CTMS-over-TCP** framing
(`CTM-USBIP\src\capture\main.cpp` — **read the actual header layout there before committing field offsets;
do not invent them**). One TCP connection, two logical substreams via a **band-tag** in the CTMS frame header.
*Open:* single-tagged-connection vs dual-connection — gated by whether TCP head-of-line blocking lets a
stalled detail frame delay LL (measure; if so, split LL onto its own connection/QUIC stream).

**Per-frame metadata the TV needs (locked information content, wire encoding open):**
- `band_tag` — LL vs detail (and which detail band, if HL/LH/HH are separate).
- `level` — DWT level (1 now; reserved for deeper).
- `geometry` — full W×H and LL W×H (upscale factor + detail placement). Likely per-session, not per-frame.
- `tile_layout` (detail) — tile count + origin/size; per-tile so dropped/late tiles are individually
  identifiable. LL is single-tile.
- `frame_sequence` / `pts` — **the core of the interrupt.** LL frames carry a monotonic sequence; detail
  frames carry the LL sequence they refine → TV decides "still valid for the LL I'm about to present, or
  stale → drop/deghost."

*Open:* HL/LH/HH as one packed stream vs three; geometry per-session vs per-frame; exact CTMS field layout.

---

## 7. Latency model — model LOCKED, the three numbers UNVERIFIED

Anchor: ~1 ms network RTT floor. Stack only the irreducible on top; attack the biggest term first.

| Term | What | Attack |
|---|---|---|
| Encode compute | scales w/ resolution | L-path scaling → toward encode fixed overhead **Fe** |
| Encode submit/retrieve (**Fe**) | per-frame fixed | persistent session, pre-alloc, no per-frame reconfig |
| Surface copies | VRAM traffic both ends | GPU-resident zero-copy both ends; measure residual |
| Decode compute | scales w/ resolution | L-path scaling → toward decode fixed overhead **Fd** |
| Decode setup (**Fd**) | per-frame fixed | persistent pipeline, pre-rolled, push-mode, no renegotiation |
| Present | hand-off to display | immediate/mailbox, bypass compositor/vsync |
| Scanout | physical panel | hard physical term; attack only by going under the present path |

**Fixed-vs-scaling split:** Fe/Fd are the fixed overheads that remain when resolution→small. The L-path win =
make the *scaling* part tiny (LL is 540p/1080p, not 4K) so total ≈ fixed overhead. Anchors: ~10 ms 4K encode,
~20 ms 4K AV1 decode (HEVC faster); the 540p/1080p numbers give the slope.

**Three gating measurements:**
1. **Fe** — native 1080p/540p encode on the 9070 (AV1+HEVC). Encode-only submit→retrieve on a persistent AMF
   session, NOT contaminated by busy-wait/sleep. (The current `main.cpp` HUD contaminates Fe — fix first.)
2. **Fd** — native 1080p/540p decode. NOT measurable through NDL v2 (no decode-complete query) — **but the
   lxvdec own-decode path gives a real per-frame decode signal** (`write_data` → `lxvdec_read` →
   `read_picture`), so timestamp decode-in→picture-out on-device. PC loopback decode is only a sanity ref.
3. **Present-to-display** in immediate mode — glass-to-glass present→photons, on device. (vtcapture is
   post-display +~1 frame → a profiling tool, not the present-latency primitive.)

**Kill-criterion (LOCKED):** *if the TV decode path is strictly frame-atomic and cannot present incrementally
/ cannot run independent streams, the decode-side win collapses to baseline.* **Test FIRST.** Memory-updated:
lxvdec is **frame-oriented** (read_picture returns whole frames) → sub-frame incremental present is unproven
and likely unavailable; the win therefore rests on **independent sessions** (a separate small LL decode
decoupled from detail) — verify *that* specifically. The decode ASIC is frame-oriented (rigid term).

---

## 8. Roadmap

**Tasks 1–3 are recorded/locked order. Tasks 4–8 are the design-implied continuation (proposed, not
previously committed) — they follow necessarily but were not a numbered roadmap.**

1. **Numpy reference (RECORDED).** Haar split → quadrant-pack → recombine; validate; visualize upscale +
   seam behavior; prototype the LL-gradient deghost mask. *Accept:* inverse reconstructs bit-exact (within
   rounding); tiled-detail seams confirmed invisible after recombine; deghost demoed on a synthetic
   moving-region case. *Produces:* the **oracle** (constants + exact math the shaders must match) + deghost ref.
2. **Fe/Fd harness (RECORDED).** Repair encode/decode timing at 4K/1080p/540p; fix the contaminated HUD.
   *Accept:* clean Fe at all res (AV1+HEVC); uncontaminated decode-timing path; runnable in one sitting.
   *Produces:* **Fe** (all res), loopback decode sanity, encode fixed-vs-scaling slope.
3. **AMF tiled-encode + slice-out + TV decode (RECORDED).** AMF tile-output / frame-in-slice-out; stand up
   the TV decode = the proven **lxvdec own-decode** path (NOT `appsrc!lxvideodec!appsink`). *Accept:* AMF
   emits detail tiles/slices; TV decodes a real small-res LL stream end-to-end. *Produces:* AMF tiled-encode
   confirmation; **on-device Fd** via lxvdec per-frame timing.
4. **Kill-criterion test (test FIRST — runs before/with task 3's TV half).** Verify an independent LL decode
   session decoupled from detail; check whether any incremental present exists. *Accept:* a dedicated
   small-res LL session produces presented frames independent of a detail session, OR a documented LL-only
   fallback (decision 2). *Produces:* the decode-side go/no-go + **present-to-display** latency.
5. **Two-band transport (§6).** LL/detail substream framing (tag/sequence/geometry/tile-layout) over CTMS;
   LL-priority send. *Accept:* TV demuxes by tag; detail droppable without LL corruption; LL sequence drives
   the interrupt. *Produces:* confirmation LL-send never blocks behind detail (or data forcing dual-conn).
6. **TV GLES recombine shader (§4).** Inverse-Haar + LL upscale + deghost, sampling lxvdec EGLImages; match
   the oracle. *Accept:* shader == oracle on the same input; deghost suppresses ghosts on a moving clip;
   10-bit-vs-8-bit settled. *Produces:* the **10-bit preservation** answer; seam-hiding + deghost on real frames.
7. **End-to-end L-path scheduling (§5).** Wire the hard-interrupt host→TV; LL-first, drop-in-flight-detail,
   present-first. *Accept:* under induced detail backlog, latency stays pinned to LL; dropping detail is
   visible as quality float, not stall. *Produces:* **glass-to-glass** under full design vs baseline.
8. **Glass-to-glass measurement + budget close.** Measure real g2g; decompose into §7 terms; attack the
   largest residual. *Accept:* g2g decomposed; biggest term + next attack named. *Produces:* the **final
   budget** for this hardware.

---

## 9. Open / unverified (with the check that settles each)

- **Fe @1080p/@540p** (AV1,HEVC) — task 2 harness on the 9070.
- **Fd @1080p/@540p** — via lxvdec own-decode per-frame timing (task 3); NOT NDL v2.
- **Present-to-display (immediate)** — task 4, on device.
- **Residual surface-copy VRAM traffic** — measure on host pipeline.
- **Kill-criterion result** — can we run an independent low-res LL decode decoupled from detail? Test first
  (task 4); lxvdec is frame-oriented so the win rests on independent sessions, not incremental decode.
- **Transport: single-tagged-conn vs dual-conn** — gated by TCP HOL-blocking measurement (task 5).
- **Exact CTMS header field layout** — read from `main.cpp`, don't invent.
- **HL/LH/HH one packed stream vs three** — open.
- **Geometry signaling per-session vs per-frame** — open (likely per-session).
- **10-bit preservation through lxvdec decode** — decode a 10-bit clip, inspect format enum.
- **Exact Haar normalization constants** — oracle-defined (task 1); shaders match bit-for-bit.
- **LL motion-warp deghost extension** — open, not v1.
- **Own-decode known-bugs gating recombine** — EGLImage YUV hints (else black); DPB return-thread; cache-invalidate.

---

## 10. Relevant files

- `D:\Work\CTM\.claude\agents\lpath-keeper.md` — locked design / latency authority.
- `D:\Work\CTM\CTM-USBIP\src\capture\main.cpp` — host pipeline (DXGI → AMF P010 → CTMS/TCP → loopback HUD);
  source of the CTMS framing + the Fe/Fd harness to repair; `Converter` ~950–1110 = wavelet insertion point.
- `D:\Work\CTM\CTM-USBIP\src\capture\amf_encoder.inl` — AMF encoder (multi-instance for LL + detail sessions).
- `D:\Work\CTM\re\` — TV RE artifacts (`probe.c`, `build.sh`, liblxv recipe).
- `D:\Work\CTM\ctm-bridge-webos\src\stream\` — TV app: `lxvdec_player.{c,h}` (own-decode — NOTE: removed from
  the shipping app this session as a dead-end *display* backend, but the liblxv own-decode technique is the
  task-3/6 decode path; recover from git history `ca2ab79`/earlier if needed), `gst_gl.c` (EGLImage/GLES),
  `gst_player.c` (videooutput punch-through), `vt_capture.c` (vtcapture profiling).
- `D:\Work\CTM\ctm-bridge-webos\docs\tv-media-stack.md` — lxvideodec/lxvideosink inventory.
