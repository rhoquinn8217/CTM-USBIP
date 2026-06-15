// Native AMD AMF decoder (HEVC Main10 + AV1) -- zero-copy D3D11 P010 output.
//
// Feeds Annex-B frames straight to the AMF hardware decoder and gets back a D3D11
// P010 surface on OUR device, so window B samples it directly. Low-latency reorder
// mode (DPB delay 0) + a controlled surface pool -- the knobs ffmpeg's D3D11VA path
// didn't expose.
//
// Decode time is measured by a DEDICATED DRAIN THREAD, not on the network read
// thread. AMF decode is async/pipelined: when we submit frame N the output ready
// is an earlier frame. If we only drained once per arriving packet (~16.7ms apart
// at 60fps) the observed d1 would be quantized to the arrival cadence -- a frame
// the GPU finished in ~2ms wouldn't be *seen* done until the next packet, so
// d1-d0 measured the look-interval, not the decode. The drain thread polls
// QueryOutput continuously and stamps d1 the instant the surface emerges -> real
// decode compute. It does NOT sit in the path (pure observer), so g2g is unchanged
// (if anything lower: decoded frames are published to the display the instant they
// are ready instead of at the next read). Static screen -> no output -> no sample
// -> the number can't drift. ffmpeg stays the CPU fallback.
//
// Included after amf_encoder.inl (which pulls the AMF headers + `using namespace amf`).
#include <components/VideoDecoderUVD.h>
#include <unordered_map>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>

// Result of the serialized decode benchmark (decode_bench / 'B' hotkey).
struct DecodeBench {
    double latAvgMs = 0, latP50Ms = 0, latP99Ms = 0, latMaxMs = 0;
    double throughputFps = 0;
    int pipelineDepth = 0;   // frames in flight before the first output (1 = no hold)
    int frames = 0;
};

class AmfDecoder {
public:
    // startDrain=false => no async drain thread (the serialized bench drives
    // SubmitInput/QueryOutput itself on the calling thread).
    bool open(EncConfig::Codec codec, ID3D11Device *dev, int w, int h, std::string *err, bool startDrain = true)
    {
        stop_thread();                               // tear the drain thread down before touching dec_
        if (dec_) { dec_->Terminate(); dec_->Release(); dec_ = nullptr; }
        ok_ = false;
        { std::lock_guard<std::mutex> lk(mtx_); latest_ = nullptr; }
        { std::lock_guard<std::mutex> lk(stMtx_); submitTime_.clear(); }
        if (!dev) { if (err) *err = "no D3D11 device"; return false; }
        if (!dll_) dll_ = LoadLibraryW(AMF_DLL_NAME);
        if (!dll_) { if (err) *err = "amfrt64.dll not found"; return false; }
        if (!factory_) {
            auto initFn = reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll_, AMF_INIT_FUNCTION_NAME));
            if (!initFn || initFn(AMF_FULL_VERSION, &factory_) != AMF_OK || !factory_) { if (err) *err = "AMFInit failed"; return false; }
        }
        if (!ctx_) {
            if (factory_->CreateContext(&ctx_) != AMF_OK || !ctx_) { if (err) *err = "CreateContext failed"; return false; }
            if (ctx_->InitDX11(dev) != AMF_OK) { if (err) *err = "InitDX11 failed"; return false; }
        }
        const wchar_t *id = (codec == EncConfig::AV1) ? AMFVideoDecoderHW_AV1 : AMFVideoDecoderHW_H265_MAIN10;
        if (factory_->CreateComponent(ctx_, id, &dec_) != AMF_OK || !dec_) { if (err) *err = "CreateComponent(decoder) failed"; return false; }
        // DPB delay 0: our stream has no frame reordering -> minimum decode latency.
        dec_->SetProperty(AMF_VIDEO_DECODER_REORDER_MODE, (amf_int64)AMF_VIDEO_DECODER_MODE_LOW_LATENCY);
        // Output frames in DECODE order, the instant they're done -- not paced to pts.
        dec_->SetProperty(AMF_TIMESTAMP_MODE, (amf_int64)AMF_TS_DECODE);
        if (dec_->Init(AMF_SURFACE_P010, w, h) != AMF_OK) { if (err) *err = "decoder Init failed"; return false; }
        ok_ = true;
        if (startDrain) { run_.store(true); drainThread_ = std::thread([this] { drain_loop(); }); }
        return true;
    }

    // Bench (no drain thread): non-blocking pull of one decoded surface; returns
    // its pts (us) via outPtsUs. true if a surface came out.
    bool poll(int64_t *outPtsUs)
    {
        if (!ok_) return false;
        AMFDataPtr out;
        if (dec_->QueryOutput(&out) != AMF_OK || !out) return false;
        AMFSurfacePtr surf(out);
        if (!surf) return false;
        surf->Convert(AMF_MEMORY_DX11);
        if (outPtsUs) *outPtsUs = (int64_t)(surf->GetPts() / 10);
        return true;
    }

    // Synchronous ONE-FRAME-AT-A-TIME live decode (open with startDrain=false).
    // Submit this frame (d0), then drain until the first decoded surface comes
    // out; publish it as latest_ for the display and return its real submit->exit
    // time d1-d0 + pts. If the decoder HOLDS the frame, the surface that appears
    // is the PREVIOUS frame (released by this submit), so d1-d0 then spans ~one
    // arrival interval -- which makes the hold visible instead of hidden. Bounded
    // by timeoutMs so a held frame never deadlocks the caller.
    int decode_sync(const uint8_t *data, int size, int64_t ptsUs, double *decMs, int64_t *outPtsUs, int timeoutMs)
    {
        if (decMs) *decMs = 0; if (outPtsUs) *outPtsUs = -1;
        if (!ok_ || !data || size <= 0) return 0;
        AMFBufferPtr inBuf;
        if (ctx_->AllocBuffer(AMF_MEMORY_HOST, (amf_size)size, &inBuf) != AMF_OK || !inBuf) return 0;
        memcpy(inBuf->GetNative(), data, size);
        inBuf->SetPts((amf_pts)ptsUs * 10);
        { std::lock_guard<std::mutex> lk(stMtx_); submitTime_[ptsUs] = now_ms(); }   // d0
        for (int g = 0; g < 100000; ++g) {
            AMF_RESULT r = dec_->SubmitInput(inBuf);
            if (r == AMF_OK || r == AMF_NEED_MORE_INPUT) break;
            if (r == AMF_INPUT_FULL) { if (pull_publish(decMs, outPtsUs)) return 1; YieldProcessor(); continue; }
            return 0;
        }
        const double dl = now_ms() + timeoutMs;
        while (now_ms() < dl) {
            if (pull_publish(decMs, outPtsUs)) return 1;   // one frame out -> done (one at a time)
            YieldProcessor();
        }
        return 0;   // in flight, nothing out yet -> caller reads the next frame
    }

    // Serialized decode benchmark. Feeds `gop` (must start with a keyframe)
    // back-to-back -- as fast as the decoder accepts, NOT at the 60fps arrival
    // rate -- and tight-polls for each output until `targetOutputs` frames exit.
    // Measures the REAL per-frame decode latency (submit->exit, free of arrival
    // pacing), sustained throughput, and the pipeline depth: how many frames are
    // in flight before the first output (1 = the decoder emits as it decodes;
    // >1 = it holds frames). Requires open(..., startDrain=false).
    bool decode_bench(const std::vector<std::vector<uint8_t>> &gop, int targetOutputs, DecodeBench *res, std::string *err)
    {
        if (!ok_ || gop.empty() || !res) { if (err) *err = "no decoder / empty GOP"; return false; }
        std::unordered_map<int64_t, double> tsub;
        std::vector<double> lats; lats.reserve(targetOutputs + 16);
        int64_t pts = 0;
        int produced = 0, submitted = 0, depth = 0;
        double firstSubMs = 0, lastOutMs = 0; bool first = true;
        auto reap = [&] {
            int64_t op;
            while (poll(&op)) {
                auto it = tsub.find(op);
                if (it != tsub.end()) { lats.push_back(now_ms() - it->second); tsub.erase(it); }
                if (++produced == 1) depth = submitted;     // in-flight count at first output
                lastOutMs = now_ms();
            }
        };
        const double benchT0 = now_ms();
        while (produced < targetOutputs && now_ms() - benchT0 < 5000.0) {
            for (const auto &f : gop) {
                if (produced >= targetOutputs) break;
                AMFBufferPtr inBuf;
                if (ctx_->AllocBuffer(AMF_MEMORY_HOST, (amf_size)f.size(), &inBuf) != AMF_OK || !inBuf) { if (err) *err = "AllocBuffer failed"; return false; }
                memcpy(inBuf->GetNative(), f.data(), f.size());
                inBuf->SetPts((amf_pts)pts * 10);
                const double ts = now_ms();
                tsub[pts] = ts;
                if (first) { firstSubMs = ts; first = false; }
                bool submittedOk = false;
                for (int guard = 0; guard < 1000000; ++guard) {
                    AMF_RESULT r = dec_->SubmitInput(inBuf);
                    if (r == AMF_OK || r == AMF_NEED_MORE_INPUT) { submittedOk = true; break; }
                    if (r == AMF_INPUT_FULL) { reap(); YieldProcessor(); continue; }
                    break;
                }
                if (!submittedOk) { if (err) *err = "SubmitInput failed"; return false; }
                ++submitted;
                reap();
                ++pts;
                if (tsub.size() > 1024) tsub.clear();
            }
        }
        const double dl = now_ms() + 300.0;          // final drain
        while (now_ms() < dl) { int before = produced; reap(); if (produced == before) YieldProcessor(); }

        if (lats.empty()) { if (err) *err = "no decoded output"; return false; }
        std::sort(lats.begin(), lats.end());
        double sum = 0; for (double v : lats) sum += v;
        res->frames = (int)lats.size();
        res->latAvgMs = sum / lats.size();
        res->latP50Ms = lats[lats.size() / 2];
        res->latP99Ms = lats[std::min(lats.size() - 1, lats.size() * 99 / 100)];
        res->latMaxMs = lats.back();
        res->pipelineDepth = depth;
        const double span = (lastOutMs - firstSubMs) / 1000.0;
        res->throughputFps = span > 0 ? produced / span : 0;
        return true;
    }

    bool is_open() const { return ok_; }

    // Per-decoded-frame callback, invoked on the drain thread the moment a surface
    // emerges: (decMs = d1-d0 real compute, outPtsUs = that frame's t0). The
    // receiver wires this to bump the decoded-frame count and the decode/latency
    // distributions -- so stats are stamped at decode-exit, never at read cadence.
    void set_on_frame(std::function<void(double, int64_t)> cb)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        onFrame_ = std::move(cb);
    }

    // Network thread: submit ONE Annex-B frame and return. No draining here -- the
    // drain thread pulls outputs. d0 (submit wall time) is recorded by pts so the
    // drain thread can compute d1-d0 for the exact frame.
    bool submit(const uint8_t *data, int size, int64_t ptsUs)
    {
        if (!ok_ || !data || size <= 0) return false;
        AMFBufferPtr inBuf;
        if (ctx_->AllocBuffer(AMF_MEMORY_HOST, (amf_size)size, &inBuf) != AMF_OK || !inBuf) return false;
        memcpy(inBuf->GetNative(), data, size);
        inBuf->SetPts((amf_pts)ptsUs * 10);            // amf_pts is 100ns
        { std::lock_guard<std::mutex> lk(stMtx_); submitTime_[ptsUs] = now_ms(); }   // d0 for this pts
        for (int tries = 0; tries < 4096; ++tries) {   // drain thread frees space if full
            AMF_RESULT r = dec_->SubmitInput(inBuf);
            if (r == AMF_OK || r == AMF_NEED_MORE_INPUT) return true;
            if (r == AMF_INPUT_FULL) { YieldProcessor(); continue; }
            break;
        }
        return false;
    }

    // Display thread: borrow the latest decoded texture (kept alive by the held
    // surface) under the lock; also returns the frame's pts (= its t0) and a
    // generation id so the caller can tell a NEW frame from a redraw of the same
    // one (only NEW frames sample d2 - t0). Call release_latest() after the draw.
    ID3D11Texture2D *lock_latest(int64_t *ptsUs, uint64_t *gen)
    {
        mtx_.lock();
        if (!latest_) { mtx_.unlock(); return nullptr; }
        if (ptsUs) *ptsUs = latestPts_;
        if (gen) *gen = latestGen_;
        AMFPlane *p = latest_->GetPlaneAt(0);
        return p ? reinterpret_cast<ID3D11Texture2D *>(p->GetNative()) : nullptr;
    }
    void release_latest() { mtx_.unlock(); }

    void close()
    {
        stop_thread();
        { std::lock_guard<std::mutex> lk(mtx_); latest_ = nullptr; }
        if (dec_) { dec_->Terminate(); dec_->Release(); dec_ = nullptr; }
        if (ctx_) { ctx_->Terminate(); ctx_->Release(); ctx_ = nullptr; }
        ok_ = false;
    }
    ~AmfDecoder() { close(); }

private:
    // Pull ONE ready decoded surface (non-blocking), publish it as latest_ (+gen)
    // for the display, and compute d1-d0 from submitTime_. Returns 1 if one came
    // out. Used by the synchronous one-frame-at-a-time live path.
    int pull_publish(double *decMs, int64_t *outPtsUs)
    {
        AMFDataPtr out;
        if (dec_->QueryOutput(&out) != AMF_OK || !out) return 0;
        AMFSurfacePtr surf(out);
        if (!surf) return 0;
        surf->Convert(AMF_MEMORY_DX11);
        const double d1 = now_ms();
        const int64_t op = (int64_t)(surf->GetPts() / 10);
        double d0 = -1.0;
        {
            std::lock_guard<std::mutex> lk(stMtx_);
            auto it = submitTime_.find(op);
            if (it != submitTime_.end()) { d0 = it->second; submitTime_.erase(it); }
            if (submitTime_.size() > 256) submitTime_.clear();
        }
        { std::lock_guard<std::mutex> lk(mtx_); latest_ = surf; latestPts_ = op; ++latestGen_; }
        if (decMs) *decMs = (d0 >= 0.0) ? (d1 - d0) : 0.0;
        if (outPtsUs) *outPtsUs = op;
        return 1;
    }

    // Continuously pull every ready output (non-blocking) and stamp d1 the instant
    // it emerges. Match each to its input by pts -> d1 - d0 (real decode compute).
    // Publish the newest as latest_ (+ pts + a new generation id) for the display,
    // and hand (decMs, pts) to the receiver via onFrame_. When nothing is ready,
    // nap 100us: keeps a core free yet stamps d1 within ~0.1ms of decode-done.
    void drain_loop()
    {
        while (run_.load()) {
            bool any = false;
            for (;;) {
                AMFDataPtr out;
                if (dec_->QueryOutput(&out) != AMF_OK || !out) break;   // nothing ready
                AMFSurfacePtr surf(out);
                if (!surf) continue;
                surf->Convert(AMF_MEMORY_DX11);
                const double d1 = now_ms();
                const int64_t op = (int64_t)(surf->GetPts() / 10);      // output frame's pts (us)
                double d0 = -1.0;
                {
                    std::lock_guard<std::mutex> lk(stMtx_);
                    auto it = submitTime_.find(op);
                    if (it != submitTime_.end()) { d0 = it->second; submitTime_.erase(it); }
                    if (submitTime_.size() > 256) submitTime_.clear();  // never grow unbounded
                }
                const double dms = (d0 >= 0.0) ? (d1 - d0) : 0.0;       // d1 - d0
                std::function<void(double, int64_t)> cb;
                { std::lock_guard<std::mutex> lk(mtx_); latest_ = surf; latestPts_ = op; ++latestGen_; cb = onFrame_; }
                if (cb) cb(dms, op);
                any = true;
            }
            if (!any) std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    void stop_thread()
    {
        run_.store(false);
        if (drainThread_.joinable()) drainThread_.join();
    }

    HMODULE dll_ = nullptr;
    AMFFactory *factory_ = nullptr;
    AMFContext *ctx_ = nullptr;
    AMFComponent *dec_ = nullptr;
    std::thread drainThread_;
    std::atomic<bool> run_{false};
    std::function<void(double, int64_t)> onFrame_;    // per-decoded-frame stats sink
    std::mutex mtx_;                              // guards latest_/latestPts_/latestGen_/onFrame_
    std::mutex stMtx_;                            // guards submitTime_
    AMFSurfacePtr latest_;                        // holds the displayed surface alive
    int64_t latestPts_ = -1;                      // its pts (= t0), us since stream epoch
    uint64_t latestGen_ = 0;                      // bumped per NEW decoded frame
    std::unordered_map<int64_t, double> submitTime_;  // pts -> d0 (submit wall time)
    bool ok_ = false;
};
