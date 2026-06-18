// Native AMD AMF encoder (HEVC + AV1) -- true zero-copy.
//
// Wraps OUR OWN single D3D11 P010 texture (the one the converter renders into) as
// an AMF input surface via CreateSurfaceFromDX11Native and encodes it directly --
// no ffmpeg hwframe pool (AMD rejects a P010 texture ARRAY), no copy, no readback.
// amfrt64.dll ships with the AMD driver. ffmpeg is only the CPU fallback now.
//
// Included into main.cpp after EncConfig/HdrInfo/now_ms are defined.
#include <core/Factory.h>
#include <core/Context.h>
#include <core/Surface.h>
#include <core/Buffer.h>
#include <core/Data.h>
#include <components/Component.h>
#include <components/VideoEncoderHEVC.h>
#include <components/VideoEncoderAV1.h>

using namespace amf;   // amf_int64/amf_pts/AMF_OK/AMFConstructSize/enum values/etc.

class AmfEncoder {
public:
    // dll/factory/context are created once and reused across reconfigures; only
    // the encoder component is rebuilt. Returns false (and leaves is_open()==false)
    // if AMF is unavailable -- the caller then falls back to ffmpeg/CPU.
    bool open(const EncConfig &cfg, int w, int h, const HdrInfo &hdr, ID3D11Device *dev, std::string *err)
    {
        if (enc_) { enc_->Terminate(); enc_->Release(); enc_ = nullptr; }
        ok_ = false;
        if (!dev) { if (err) *err = "no D3D11 device"; return false; }
        if (!dll_) dll_ = LoadLibraryW(AMF_DLL_NAME);
        if (!dll_) { if (err) *err = "amfrt64.dll not found (AMD driver missing?)"; return false; }
        if (!factory_) {
            auto initFn = reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll_, AMF_INIT_FUNCTION_NAME));
            if (!initFn || initFn(AMF_FULL_VERSION, &factory_) != AMF_OK || !factory_) {
                if (err) *err = "AMFInit failed"; return false;
            }
        }
        if (!ctx_) {
            if (factory_->CreateContext(&ctx_) != AMF_OK || !ctx_) { if (err) *err = "CreateContext failed"; return false; }
            if (ctx_->InitDX11(dev) != AMF_OK) { if (err) *err = "InitDX11 failed"; return false; }
        }
        av1_ = (cfg.codec == EncConfig::AV1);
        if (factory_->CreateComponent(ctx_, av1_ ? AMFVideoEncoder_AV1 : AMFVideoEncoder_HEVC, &enc_) != AMF_OK || !enc_) {
            if (err) *err = "CreateComponent failed"; return false;
        }

        const amf_int64 br = (amf_int64)cfg.bitrateKbps * 1000;
        const amf_int64 mr = (amf_int64)std::max(cfg.maxrateKbps, cfg.bitrateKbps) * 1000;
        const amf_int64 fps = cfg.fps > 0 ? cfg.fps : 60;   // rate-control budget framerate
        // all-intra: IDR every frame. intra-refresh: push the periodic IDR far out
        // so the rolling refresh is the only intra (no keyframe spike).
        const amf_int64 gop = cfg.allIntra ? 1 : (cfg.intraRefresh ? 100000 : 600);
        if (av1_) {
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_USAGE,
                cfg.usage == EncConfig::ULL ? (amf_int64)AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY
              : cfg.usage == EncConfig::LL  ? (amf_int64)AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY
                                            : (amf_int64)AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING);
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMESIZE, AMFConstructSize(w, h));
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_COLOR_BIT_DEPTH, (amf_int64)AMF_COLOR_BIT_DEPTH_10);
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_PROFILE, (amf_int64)AMF_VIDEO_ENCODER_AV1_PROFILE_MAIN);
            // HDR VUI: converter outputs PQ BT.2020 (limited range) -> tag it (see HEVC note).
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PROFILE, (amf_int64)AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020);
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_TRANSFER_CHARACTERISTIC, (amf_int64)AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084);
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PRIMARIES, (amf_int64)AMF_COLOR_PRIMARIES_BT2020);
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_INPUT_COLOR_PROFILE, (amf_int64)AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020);
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET,
                cfg.usage == EncConfig::ULL ? (amf_int64)AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED
              : cfg.usage == EncConfig::LL  ? (amf_int64)AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED
                                            : (amf_int64)AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY);
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE, (amf_int64)AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS);
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
                cfg.mode == EncConfig::CBR ? (amf_int64)AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR
              : cfg.mode == EncConfig::CQP ? (amf_int64)AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CONSTANT_QP
                                           : (amf_int64)AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR);
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, br);
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, mr);
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMERATE, AMFConstructRate((amf_int32)fps, 1));
            enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, gop);
            if (cfg.slices > 1)        enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_TILES_PER_FRAME, (amf_int64)cfg.slices);
            if (cfg.maxFrameKbits > 0) enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_MAX_COMPRESSED_FRAME_SIZE, (amf_int64)cfg.maxFrameKbits * 1000);
            if (cfg.intraRefresh)      enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_INTRA_REFRESH_MODE, (amf_int64)AMF_VIDEO_ENCODER_AV1_INTRA_REFRESH_MODE__CONTINUOUS);
        } else {
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE,
                cfg.usage == EncConfig::ULL ? (amf_int64)AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY
              : cfg.usage == EncConfig::LL  ? (amf_int64)AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY
                                            : (amf_int64)AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING);
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, AMFConstructSize(w, h));
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, (amf_int64)AMF_COLOR_BIT_DEPTH_10);
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE, (amf_int64)AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10);
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TIER, (amf_int64)AMF_VIDEO_ENCODER_HEVC_TIER_MAIN);
            // HDR VUI: the converter always outputs PQ BT.2020 (limited-range) P010, but
            // the AMF HEVC stream is otherwise untagged -> the TV decodes it as SDR
            // (getStatus videoInfo:null, no panel HDR). Tag colour so lxvideodec reports
            // BT.2020 PQ. The ffmpeg encoder path already does this; the AMF path didn't.
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE, (amf_int64)AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020);
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC, (amf_int64)AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084);
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES, (amf_int64)AMF_COLOR_PRIMARIES_BT2020);
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PROFILE, (amf_int64)AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020);
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
                cfg.usage == EncConfig::ULL ? (amf_int64)AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED
              : cfg.usage == EncConfig::LL  ? (amf_int64)AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED
                                            : (amf_int64)AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY);
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
                cfg.mode == EncConfig::CBR ? (amf_int64)AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR
              : cfg.mode == EncConfig::CQP ? (amf_int64)AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP
                                           : (amf_int64)AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR);
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, br);
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, mr);
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, AMFConstructRate((amf_int32)fps, 1));
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, gop);
            enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, (amf_int64)1);
            if (cfg.slices > 1)        enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_SLICES_PER_FRAME, (amf_int64)cfg.slices);
            if (cfg.maxFrameKbits > 0) enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_MAX_AU_SIZE, (amf_int64)cfg.maxFrameKbits * 1000);
            if (cfg.intraRefresh) {    // spread intra over ~1s (fps frames) of 64x64 CTBs -> no spike
                const amf_int64 ctbs = (amf_int64)((w + 63) / 64) * ((h + 63) / 64);
                const amf_int64 perSlot = (ctbs + fps - 1) / fps;
                enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_INTRA_REFRESH_NUM_CTBS_PER_SLOT, perSlot > 0 ? perSlot : 1);
            }
        }
        // mastering-display + content-light SEI (HDR10 statics) from the source monitor.
        if (hdr.valid) {
            AMFHDRMetadata md{};
            md.redPrimary[0]   = (amf_uint16)(hdr.rx * 50000.f); md.redPrimary[1]   = (amf_uint16)(hdr.ry * 50000.f);
            md.greenPrimary[0] = (amf_uint16)(hdr.gx * 50000.f); md.greenPrimary[1] = (amf_uint16)(hdr.gy * 50000.f);
            md.bluePrimary[0]  = (amf_uint16)(hdr.bx * 50000.f); md.bluePrimary[1]  = (amf_uint16)(hdr.by * 50000.f);
            md.whitePoint[0]   = (amf_uint16)(hdr.wx * 50000.f); md.whitePoint[1]   = (amf_uint16)(hdr.wy * 50000.f);
            md.maxMasteringLuminance = (amf_uint32)(hdr.maxLum * 10000.f);   // AMF: nits x 10000
            md.minMasteringLuminance = (amf_uint32)(hdr.minLum * 10000.f);
            md.maxContentLightLevel      = (amf_uint16)hdr.maxLum;
            md.maxFrameAverageLightLevel = (amf_uint16)hdr.maxFFLum;
            amf::AMFBufferPtr hbuf;
            if (ctx_->AllocBuffer(AMF_MEMORY_HOST, sizeof(md), &hbuf) == AMF_OK && hbuf) {
                memcpy(hbuf->GetNative(), &md, sizeof(md));
                enc_->SetProperty(av1_ ? AMF_VIDEO_ENCODER_AV1_INPUT_HDR_METADATA
                                       : AMF_VIDEO_ENCODER_HEVC_INPUT_HDR_METADATA, hbuf);
            }
        }
        if (enc_->Init(AMF_SURFACE_P010, w, h) != AMF_OK) { if (err) *err = "encoder Init failed"; return false; }
        w_ = w; h_ = h; ok_ = true;
        return true;
    }

    bool is_open() const { return ok_; }
    void force_idr() { forceIdr_.store(true); }

    // Encode our D3D11 P010 texture (zero-copy). Appends each Annex-B packet's
    // bytes to `out` with its keyframe flag in `keys`. 1-in/1-out low latency:
    // polls (bounded) for this frame's packet. Returns false on a hard error.
    bool encode(ID3D11Texture2D *p010, int64_t ptsUs,
                std::vector<std::vector<uint8_t>> &out, std::vector<bool> &keys)
    {
        if (!ok_ || !p010) return false;
        amf::AMFSurfacePtr surf;
        if (ctx_->CreateSurfaceFromDX11Native(p010, &surf, nullptr) != AMF_OK || !surf) return false;
        surf->SetPts((amf_pts)ptsUs * 10);   // amf_pts is 100ns units
        if (forceIdr_.exchange(false)) {
            if (av1_) surf->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, (amf_int64)AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY);
            else      surf->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, (amf_int64)AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
        }
        AMF_RESULT r = enc_->SubmitInput(surf);
        if (r != AMF_OK && r != AMF_NEED_MORE_INPUT && r != AMF_INPUT_FULL) return false;

        const double dl = now_ms() + 200.0;   // wall-clock safety bound
        bool got = false;
        for (;;) {
            amf::AMFDataPtr data;
            AMF_RESULT q = enc_->QueryOutput(&data);
            if (q == AMF_OK && data) {
                amf::AMFBufferPtr buf(data);
                if (buf && buf->GetNative() && buf->GetSize()) {
                    const uint8_t *p = static_cast<const uint8_t *>(buf->GetNative());
                    out.emplace_back(p, p + buf->GetSize());
                    amf_int64 t = -1;
                    if (av1_) data->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &t);
                    else      data->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &t);
                    keys.push_back(av1_ ? (t == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY)
                                        : (t == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR));
                    got = true;
                }
                continue;            // drain anything else ready
            }
            if (q == AMF_REPEAT) {   // not produced yet
                if (got || now_ms() >= dl) break;
                YieldProcessor();
                continue;
            }
            break;                   // AMF_EOF or error
        }
        return true;
    }

    void close()
    {
        if (enc_) { enc_->Terminate(); enc_->Release(); enc_ = nullptr; }
        if (ctx_) { ctx_->Terminate(); ctx_->Release(); ctx_ = nullptr; }
        // factory_ is owned by the runtime; dll_ stays loaded for process life.
        ok_ = false; w_ = h_ = 0;
    }
    ~AmfEncoder() { close(); }

private:
    HMODULE dll_ = nullptr;
    amf::AMFFactory *factory_ = nullptr;
    amf::AMFContext *ctx_ = nullptr;
    amf::AMFComponent *enc_ = nullptr;
    std::atomic<bool> forceIdr_{false};
    bool av1_ = false, ok_ = false;
    int w_ = 0, h_ = 0;
};
