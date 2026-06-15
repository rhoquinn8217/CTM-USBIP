// ctm-capture — grab a Windows display (e.g. the VDD virtual monitor) via DXGI
// Desktop Duplication and show it scaled in a resizable window.
//
//   ctm-capture --list                 enumerate outputs and exit
//   ctm-capture --shot <idx> <png>     capture one frame from output <idx> to a PNG
//   ctm-capture [--output <idx>]       live resizable preview (default: last output)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX        // keep windows.h from defining min()/max() macros
#endif
#include <winsock2.h>   // before windows.h
#include <ws2tcpip.h>
#include <windows.h>
#include <mmdeviceapi.h>   // WASAPI loopback audio
#include <audioclient.h>
#include <mmreg.h>         // WAVEFORMATEXTENSIBLE, WAVE_FORMAT_*
#include <unknwn.h>      // IStream for gdiplus under WIN32_LEAN_AND_MEAN
#include <objidl.h>      // PROPID for gdiplus
#include <gdiplus.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <timeapi.h>       // timeBeginPeriod/timeEndPeriod (1ms scheduler tick)
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")   // WASAPI (CoCreateInstance / CoInitializeEx)
#pragma comment(lib, "winmm.lib")   // timeBeginPeriod/timeEndPeriod

#include "ctm_stream_protocol.h"

using Microsoft::WRL::ComPtr;

// Raise the Windows scheduler tick to 1ms for this object's lifetime. Without
// this, every std::this_thread::sleep_for() and any sub-tick wait rounds up to
// the default ~15.6ms quantum, which silently inflates and quantizes every
// timing in the profiler (encode poll waits, --fps cadence). Process-wide.
struct TimerRes {
    bool on_ = false;
    TimerRes()  { on_ = (timeBeginPeriod(1) == TIMERR_NOERROR); }
    ~TimerRes() { if (on_) timeEndPeriod(1); }
};

// Per-window latency-sample accumulator: count + avg/p50/p99/min/max, instead of
// the old last-value .store() reporting that hid encode/decode spikes (e.g. the
// scene-change decode spike) by collapsing a whole second to one frame. Thread
// safe: producers add() from the codec/rx threads, the stats thread flush()es.
struct StatAcc {
    void add(double ms) { std::lock_guard<std::mutex> lk(m_); s_.push_back(ms); }
    struct Summary { int n = 0; double avg = 0, p50 = 0, p99 = 0, mn = 0, mx = 0; };
    Summary flush()
    {
        std::lock_guard<std::mutex> lk(m_);
        Summary r; r.n = (int)s_.size();
        if (r.n) {
            std::sort(s_.begin(), s_.end());
            double sum = 0; for (double v : s_) sum += v;
            r.avg = sum / r.n; r.mn = s_.front(); r.mx = s_.back();
            r.p50 = s_[(size_t)(0.50 * (r.n - 1) + 0.5)];
            r.p99 = s_[(size_t)(0.99 * (r.n - 1) + 0.5)];
        }
        s_.clear();
        return r;
    }
private:
    std::mutex m_;
    std::vector<double> s_;
};

struct OutputRef {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output1;
    DXGI_OUTPUT_DESC desc{};
    std::wstring adapterName;
};

static std::vector<OutputRef> enumerate_outputs()
{
    std::vector<OutputRef> outs;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return outs;
    ComPtr<IDXGIAdapter1> ad;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &ad) == S_OK; ++ai) {
        DXGI_ADAPTER_DESC1 adesc{};
        ad->GetDesc1(&adesc);
        ComPtr<IDXGIOutput> out;
        for (UINT oi = 0; ad->EnumOutputs(oi, &out) == S_OK; ++oi) {
            ComPtr<IDXGIOutput1> o1;
            if (SUCCEEDED(out.As(&o1))) {
                OutputRef r;
                r.adapter = ad;
                r.output1 = o1;
                o1->GetDesc(&r.desc);
                r.adapterName = adesc.Description;
                outs.push_back(std::move(r));
            }
            out.Reset();
        }
        ad.Reset();
    }
    return outs;
}

static void list_outputs(const std::vector<OutputRef> &outs)
{
    std::wcout << L"outputs (" << outs.size() << L"):\n";
    for (size_t i = 0; i < outs.size(); ++i) {
        const DXGI_OUTPUT_DESC &d = outs[i].desc;
        const int w = d.DesktopCoordinates.right - d.DesktopCoordinates.left;
        const int h = d.DesktopCoordinates.bottom - d.DesktopCoordinates.top;
        std::wcout << L"  [" << i << L"] " << d.DeviceName
                   << L"  " << w << L"x" << h
                   << L" @(" << d.DesktopCoordinates.left << L"," << d.DesktopCoordinates.top << L")"
                   << L"  attached=" << (d.AttachedToDesktop ? 1 : 0)
                   << L"  gpu=" << outs[i].adapterName << L"\n";
    }
}

static float half_to_float(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff, f;
    if (exp == 0) {
        if (man == 0) { f = sign << 31; }
        else { exp = 127 - 15 + 1; while (!(man & 0x400)) { man <<= 1; --exp; } man &= 0x3ff; f = (sign << 31) | (exp << 23) | (man << 13); }
    } else if (exp == 0x1f) {
        f = (sign << 31) | (0xffu << 23) | (man << 13);
    } else {
        f = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float r;
    std::memcpy(&r, &f, 4);
    return r;
}

struct Frame {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // 0x00RRGGBB, BI_RGB-ready
};

// SDR white level for an output, as the scRGB value that "SDR white" maps to
// (Windows "SDR content brightness"; 1.0 = 80 nits). Used to tone-map HDR
// (scRGB FP16) frames down for the SDR preview. Returns 1.0 if unavailable.
static float sdr_white_scale(const wchar_t *gdiName)
{
    UINT32 nPath = 0, nMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPath, &nMode) != ERROR_SUCCESS) return 1.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPath, paths.data(), &nMode, modes.data(), nullptr) != ERROR_SUCCESS) return 1.0f;
    for (UINT32 i = 0; i < nPath; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, gdiName) != 0) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
        wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        wl.header.size = sizeof(wl);
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) != ERROR_SUCCESS) return 1.0f;
        return wl.SDRWhiteLevel > 0 ? (wl.SDRWhiteLevel / 1000.0f) : 1.0f;
    }
    return 1.0f;
}

// HDR preview paperwhite (nits). scRGB SDR-white = nits/80. Used as the tone-map
// divisor when the OS reports no usable SDR white level (e.g. virtual displays).
static float g_paperwhite_nits = 200.0f;

class Capturer {
public:
    bool init(const OutputRef &ref, std::wstring *err)
    {
        ref_ = ref;
        whiteScale_ = sdr_white_scale(ref.desc.DeviceName);
        const float div = (whiteScale_ > 1.05f) ? whiteScale_ : (g_paperwhite_nits / 80.0f);
        std::wcout << L"SDR white scale=" << whiteScale_ << L" -> HDR tone-map divisor="
                   << div << L" (paperwhite " << g_paperwhite_nits << L" nits)\n";
        const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        HRESULT hr = D3D11CreateDevice(ref.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, ARRAYSIZE(fls),
                                       D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
        if (FAILED(hr)) { if (err) *err = L"D3D11CreateDevice failed"; return false; }
        return reinit_dup(err);
    }

    bool reinit_dup(std::wstring *err)
    {
        dup_.Reset();
        HRESULT hr = ref_.output1->DuplicateOutput(dev_.Get(), &dup_);
        if (FAILED(hr)) {
            if (err) *err = (hr == DXGI_ERROR_UNSUPPORTED)
                ? L"DuplicateOutput unsupported on this output"
                : L"DuplicateOutput failed";
            return false;
        }
        return true;
    }

    // Returns 1=frame, 0=no new frame (timeout), -1=lost (caller should reinit).
    int grab(Frame &out)
    {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(500, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
        if (hr == DXGI_ERROR_ACCESS_LOST) return -1;
        if (FAILED(hr)) return -1;

        // The cursor is delivered separately from the desktop image. Capture its
        // position + shape while the frame is held; composite it in below.
        if (fi.LastMouseUpdateTime.QuadPart != 0) {
            curVisible_ = fi.PointerPosition.Visible != 0;
            curPos_ = fi.PointerPosition.Position;
        }
        if (fi.PointerShapeBufferSize != 0) {
            curBuf_.resize(fi.PointerShapeBufferSize);
            UINT req = 0;
            if (SUCCEEDED(dup_->GetFramePointerShape((UINT)curBuf_.size(), curBuf_.data(), &req, &curInfo_)))
                curHave_ = true;
        }

        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(res.As(&tex))) { dup_->ReleaseFrame(); return 0; }
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);

        if (!staging_ || stW_ != (int)td.Width || stH_ != (int)td.Height || stFmt_ != td.Format) {
            staging_.Reset();
            D3D11_TEXTURE2D_DESC sd = td;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            if (FAILED(dev_->CreateTexture2D(&sd, nullptr, &staging_))) { dup_->ReleaseFrame(); return 0; }
            stW_ = td.Width; stH_ = td.Height; stFmt_ = td.Format;
        }
        ctx_->CopyResource(staging_.Get(), tex.Get());

        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) { dup_->ReleaseFrame(); return 0; }

        out.w = (int)td.Width;
        out.h = (int)td.Height;
        out.px.assign((size_t)out.w * out.h, 0);
        const uint8_t *base = static_cast<const uint8_t *>(m.pData);
        for (int y = 0; y < out.h; ++y) {
            const uint8_t *row = base + (size_t)y * m.RowPitch;
            uint32_t *dst = out.px.data() + (size_t)y * out.w;
            if (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                std::memcpy(dst, row, (size_t)out.w * 4);   // BGRA == BI_RGB layout
            } else if (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                // HDR: scRGB is LINEAR with SDR white at scRGB value = whiteScale_.
                // Normalize to white=1, then apply the sRGB OETF so the SDR
                // preview isn't blown out (HDR highlights above white still clip).
                const uint16_t *p = reinterpret_cast<const uint16_t *>(row);
                const float divisor = (whiteScale_ > 1.05f) ? whiteScale_ : (g_paperwhite_nits / 80.0f);
                const float inv = 1.0f / divisor;
                auto enc = [](float v) {
                    if (v < 0) v = 0; if (v > 1) v = 1;
                    v = (v <= 0.0031308f) ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
                    return (uint32_t)(v * 255.0f + 0.5f);
                };
                for (int x = 0; x < out.w; ++x) {
                    uint32_t r = enc(half_to_float(p[x * 4 + 0]) * inv);
                    uint32_t g = enc(half_to_float(p[x * 4 + 1]) * inv);
                    uint32_t b = enc(half_to_float(p[x * 4 + 2]) * inv);
                    dst[x] = (r << 16) | (g << 8) | b;
                }
            } else {
                for (int x = 0; x < out.w; ++x) dst[x] = 0x202020;   // unknown format: gray
            }
        }
        ctx_->Unmap(staging_.Get(), 0);
        dup_->ReleaseFrame();
        composite_cursor(out);
        return 1;
    }

    DXGI_FORMAT last_format() const { return stFmt_; }

private:
    // Draw the captured pointer shape onto the frame at the current position.
    void composite_cursor(Frame &f)
    {
        if (!curVisible_ || !curHave_ || curInfo_.Width == 0) return;
        const int cw = (int)curInfo_.Width;
        const int pitch = (int)curInfo_.Pitch;
        const bool mono = curInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        const int ch = mono ? (int)curInfo_.Height / 2 : (int)curInfo_.Height;
        const uint8_t *buf = curBuf_.data();
        for (int y = 0; y < ch; ++y) {
            const int sy = curPos_.y + y;
            if (sy < 0 || sy >= f.h) continue;
            for (int x = 0; x < cw; ++x) {
                const int sx = curPos_.x + x;
                if (sx < 0 || sx >= f.w) continue;
                uint32_t &d = f.px[(size_t)sy * f.w + sx];
                if (curInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                    const uint8_t *s = buf + (size_t)y * pitch + (size_t)x * 4; // BGRA
                    const uint32_t a = s[3];
                    if (a == 0) continue;
                    const uint32_t db = d & 0xff, dg = (d >> 8) & 0xff, dr = (d >> 16) & 0xff;
                    const uint32_t ob = (s[0] * a + db * (255 - a)) / 255;
                    const uint32_t og = (s[1] * a + dg * (255 - a)) / 255;
                    const uint32_t orr = (s[2] * a + dr * (255 - a)) / 255;
                    d = (orr << 16) | (og << 8) | ob;
                } else if (curInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                    const uint8_t *s = buf + (size_t)y * pitch + (size_t)x * 4;
                    const uint32_t rgb = (s[2] << 16) | (s[1] << 8) | s[0];
                    if (s[3] == 0) d = rgb;       // opaque
                    else d ^= rgb;                // mask set -> XOR with screen
                } else if (mono) {
                    const int byte = x / 8, bit = 7 - (x % 8);
                    const int andm = (buf[(size_t)y * pitch + byte] >> bit) & 1;
                    const int xorm = (buf[(size_t)(ch + y) * pitch + byte] >> bit) & 1;
                    if (andm == 0 && xorm == 0) d = 0x000000;        // black
                    else if (andm == 0 && xorm == 1) d = 0xFFFFFF;   // white
                    else if (andm == 1 && xorm == 1) d ^= 0xFFFFFF;  // invert
                    // andm==1, xorm==0 => transparent (leave d)
                }
            }
        }
    }

public:

private:
    OutputRef ref_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> staging_;
    int stW_ = 0, stH_ = 0;
    DXGI_FORMAT stFmt_ = DXGI_FORMAT_UNKNOWN;
    float whiteScale_ = 1.0f;
    std::vector<uint8_t> curBuf_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO curInfo_{};
    POINT curPos_{};
    bool curVisible_ = false;
    bool curHave_ = false;
};

// ---- PNG save (shot mode) ------------------------------------------------
static int encoder_clsid(const WCHAR *mime, CLSID *clsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (!size) return -1;
    std::vector<uint8_t> buf(size);
    auto *info = reinterpret_cast<Gdiplus::ImageCodecInfo *>(buf.data());
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i)
        if (!wcscmp(info[i].MimeType, mime)) { *clsid = info[i].Clsid; return (int)i; }
    return -1;
}

static int run_shot(const OutputRef &ref, const std::wstring &png)
{
    Capturer cap;
    std::wstring err;
    if (!cap.init(ref, &err)) { std::wcerr << L"init: " << err << L"\n"; return 3; }
    Frame f;
    for (int i = 0; i < 120; ++i) {        // poll up to ~60s of static screen
        int r = cap.grab(f);
        if (r == 1 && f.w > 0) break;
        if (r == -1) cap.reinit_dup(&err);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (f.w == 0) { std::wcerr << L"no frame captured\n"; return 4; }
    std::wcout << L"captured " << f.w << L"x" << f.h << L" fmt=" << (int)cap.last_format() << L"\n";

    ULONG_PTR tok = 0;
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&tok, &in, nullptr);
    {
        Gdiplus::Bitmap bmp(f.w, f.h, PixelFormat32bppRGB);
        Gdiplus::BitmapData bd;
        Gdiplus::Rect rc(0, 0, f.w, f.h);
        bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppRGB, &bd);
        for (int y = 0; y < f.h; ++y)
            std::memcpy((uint8_t *)bd.Scan0 + (size_t)y * bd.Stride, f.px.data() + (size_t)y * f.w, (size_t)f.w * 4);
        bmp.UnlockBits(&bd);
        CLSID png_clsid;
        if (encoder_clsid(L"image/png", &png_clsid) < 0) { Gdiplus::GdiplusShutdown(tok); return 5; }
        bmp.Save(png.c_str(), &png_clsid, nullptr);
    }
    Gdiplus::GdiplusShutdown(tok);
    std::wcout << L"saved " << png << L"\n";
    return 0;
}

// ---- accelerated D3D11 preview -------------------------------------------
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_resize{false};
static std::atomic<int> g_cw{1280}, g_ch{720};

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        g_cw.store(LOWORD(lp) ? LOWORD(lp) : 1);
        g_ch.store(HIWORD(lp) ? HIWORD(lp) : 1);
        g_resize.store(true);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static const char *kVS = R"(
struct VSOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o; float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p; o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1); return o;
})";
static const char *kPS = R"(
Texture2D tex : register(t0); SamplerState smp : register(s0);
cbuffer CB : register(b0) { int isHDR; int3 _pad; };
float3 s2l(float3 c){ return (c <= 0.04045) ? c/12.92 : pow((c+0.055)/1.055, 2.4); }
float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {
    float4 c = tex.Sample(smp, uv);
    if (isHDR == 0) c.rgb = s2l(saturate(c.rgb));   // SDR sRGB8 -> linear scRGB
    return float4(c.rgb, 1);                          // HDR scRGB passes through
})";
// Window A. Two stages: decode the captured pixel (FP16 scRGB or 8-bit sRGB)
// into linear scRGB light, then encode for the window's monitor. HDR display
// gets linear scRGB; SDR display gets sRGB via the _SRGB backbuffer, and an
// HDR source is ACES-tonemapped so highlights >1 roll off instead of clipping.
static const char *kPSSceneDisp = R"(
Texture2D tex : register(t0); SamplerState smp : register(s0);
cbuffer CB : register(b0) { int srcHDR; int dstHDR; int2 _pad; };
float3 s2l(float3 c){ return (c <= 0.04045) ? c/12.92 : pow((c+0.055)/1.055, 2.4); }
float3 aces(float3 x){ const float a=2.51,b=0.03,c=2.43,d=0.59,e=0.14; return saturate((x*(a*x+b))/(x*(c*x+d)+e)); }
float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {
    float4 c = tex.Sample(smp, uv);
    float3 L = (srcHDR != 0) ? c.rgb : s2l(saturate(c.rgb));            // decode -> linear scRGB
    float3 o = (dstHDR != 0) ? L : ((srcHDR != 0) ? aces(L) : saturate(L)); // encode -> display
    return float4(o, 1);
})";
static const char *kVSQuad = R"(
cbuffer Q : register(b0) { float4 rect; float4 extra; };
struct QO { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
QO qmain(uint id : SV_VertexID) {
    float2 t = float2(id & 1, (id >> 1) & 1); QO o; o.uv = t;
    o.pos = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0, 1); return o;
})";
static const char *kPSCursor = R"(
Texture2D ctex : register(t0); SamplerState csmp : register(s0);
cbuffer Q : register(b0) { float4 rect; float4 extra; };  // x=paperwhite, y=sRGB target
float3 s2l(float3 c){ return (c <= 0.04045) ? c/12.92 : pow((c+0.055)/1.055, 2.4); }
float4 cmain(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {
    float4 c = ctex.Sample(csmp, uv);
    // extra.y > 0.5: target frame is SDR sRGB8 -> blend the cursor as-is (sRGB).
    // else: target is scRGB linear -> linearize and scale to SDR-white.
    float3 rgb = (extra.y > 0.5) ? c.rgb : (s2l(saturate(c.rgb)) * extra.x);
    return float4(rgb, c.a);
})";

static ComPtr<ID3DBlob> compile_shader(const char *src, const char *entry, const char *target)
{
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob, &err))) {
        if (err) std::cerr << (const char *)err->GetBufferPointer() << "\n";
        return nullptr;
    }
    return blob;
}

class GpuPreview {
public:
    bool init(const OutputRef &ref, HWND hwnd, std::wstring *err)
    {
        ref_ = ref;
        const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(ref.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, ARRAYSIZE(fls),
                                     D3D11_SDK_VERSION, &dev_, nullptr, &ctx_))) {
            if (err) *err = L"D3D11CreateDevice failed"; return false;
        }
        ComPtr<IDXGIFactory2> fac;
        if (FAILED(ref.adapter->GetParent(IID_PPV_ARGS(&fac)))) { if (err) *err = L"no IDXGIFactory2"; return false; }
        RECT rc; GetClientRect(hwnd, &rc);
        bbW_ = (std::max)(1L, rc.right); bbH_ = (std::max)(1L, rc.bottom);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = bbW_; sd.Height = bbH_;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;   // scRGB; lets HDR pass through
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Scaling = DXGI_SCALING_STRETCH;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(fac->CreateSwapChainForHwnd(dev_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) { if (err) *err = L"CreateSwapChainForHwnd failed"; return false; }
        sc1.As(&sc_);
        UINT sup = 0;
        if (sc_ && SUCCEEDED(sc_->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &sup)) &&
            (sup & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
            sc_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);   // scRGB HDR
        }
        make_rtv();

        auto vsb = compile_shader(kVS, "main", "vs_5_0");
        auto psb = compile_shader(kPS, "main", "ps_5_0");
        auto vqb = compile_shader(kVSQuad, "qmain", "vs_5_0");
        auto pqb = compile_shader(kPSCursor, "cmain", "ps_5_0");
        if (!vsb || !psb || !vqb || !pqb) { if (err) *err = L"shader compile failed"; return false; }
        dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
        dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
        dev_->CreateVertexShader(vqb->GetBufferPointer(), vqb->GetBufferSize(), nullptr, &vsQuad_);
        dev_->CreatePixelShader(pqb->GetBufferPointer(), pqb->GetBufferSize(), nullptr, &psCursor_);

        D3D11_SAMPLER_DESC samp{};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.MaxLOD = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&samp, &samp_);

        D3D11_BUFFER_DESC cb{};
        cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        cb.ByteWidth = 16; dev_->CreateBuffer(&cb, nullptr, &cbScene_);
        cb.ByteWidth = 32; dev_->CreateBuffer(&cb, nullptr, &cbQuad_);

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev_->CreateBlendState(&bd, &blendOver_);

        return reinit_dup(err);
    }

    bool render()
    {
        if (g_resize.exchange(false)) {
            rtv_.Reset();
            bbW_ = g_cw.load(); bbH_ = g_ch.load();
            sc_->ResizeBuffers(0, (UINT)bbW_, (UINT)bbH_, DXGI_FORMAT_UNKNOWN, 0);
            make_rtv();
        }
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(16, &fi, &res);
        bool got = false;
        if (hr == DXGI_ERROR_ACCESS_LOST) { std::wstring e; reinit_dup(&e); return true; }
        else if (hr == DXGI_ERROR_WAIT_TIMEOUT) { /* no change */ }
        else if (FAILED(hr)) { return false; }
        else {
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(res.As(&tex))) {
                D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
                if (!frameTex_ || frW_ != (int)td.Width || frH_ != (int)td.Height || frFmt_ != td.Format) {
                    frameTex_.Reset(); frameSRV_.Reset();
                    D3D11_TEXTURE2D_DESC d = td;
                    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    d.CPUAccessFlags = 0; d.MiscFlags = 0;
                    if (SUCCEEDED(dev_->CreateTexture2D(&d, nullptr, &frameTex_)))
                        dev_->CreateShaderResourceView(frameTex_.Get(), nullptr, &frameSRV_);
                    frW_ = td.Width; frH_ = td.Height; frFmt_ = td.Format;
                    srcHDR_ = (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
                }
                if (frameTex_) { ctx_->CopyResource(frameTex_.Get(), tex.Get()); got = true; haveFrame_ = true; }
            }
            update_cursor(fi);
            dup_->ReleaseFrame();
        }
        if (!got && haveFrame_ == false) return true;     // nothing yet
        draw();
        sc_->Present(1, 0);
        return true;
    }

private:
    bool reinit_dup(std::wstring *err)
    {
        dup_.Reset();
        // Force a single, stable capture format (FP16 scRGB) so DWM can't flip
        // the surface between 8-bit (SDR) and FP16 (HDR) per frame -- that flip
        // is what made the preview switch between SDR and HDR looks.
        ComPtr<IDXGIOutput5> o5;
        if (SUCCEEDED(ref_.output1.As(&o5))) {
            const DXGI_FORMAT fmts[] = {DXGI_FORMAT_R16G16B16A16_FLOAT};
            if (SUCCEEDED(o5->DuplicateOutput1(dev_.Get(), 0, ARRAYSIZE(fmts), fmts, &dup_)))
                return true;
        }
        if (FAILED(ref_.output1->DuplicateOutput(dev_.Get(), &dup_))) {
            if (err) *err = L"DuplicateOutput failed"; return false;
        }
        return true;
    }

    void make_rtv()
    {
        ComPtr<ID3D11Texture2D> bb;
        if (SUCCEEDED(sc_->GetBuffer(0, IID_PPV_ARGS(&bb))))
            dev_->CreateRenderTargetView(bb.Get(), nullptr, &rtv_);
    }

    void update_cursor(const DXGI_OUTDUPL_FRAME_INFO &fi)
    {
        if (fi.LastMouseUpdateTime.QuadPart != 0) {
            curVisible_ = fi.PointerPosition.Visible != 0;
            curPos_ = fi.PointerPosition.Position;
        }
        if (fi.PointerShapeBufferSize == 0) return;
        shapeBuf_.resize(fi.PointerShapeBufferSize);
        UINT req = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
        if (FAILED(dup_->GetFramePointerShape((UINT)shapeBuf_.size(), shapeBuf_.data(), &req, &info))) return;
        const int w = (int)info.Width;
        const bool mono = info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        const int h = mono ? (int)info.Height / 2 : (int)info.Height;
        const int pitch = (int)info.Pitch;
        std::vector<uint8_t> rgba((size_t)w * h * 4, 0);   // R,G,B,A
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint8_t *o = &rgba[((size_t)y * w + x) * 4];
            if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                const uint8_t *s = &shapeBuf_[(size_t)y * pitch + x * 4]; // BGRA
                o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3];
            } else if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                const uint8_t *s = &shapeBuf_[(size_t)y * pitch + x * 4];
                if (s[3] == 0) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = 255; }   // opaque; XOR approximated as transparent
            } else if (mono) {
                const int by = x / 8, bit = 7 - (x % 8);
                const int a = (shapeBuf_[(size_t)y * pitch + by] >> bit) & 1;
                const int xo = (shapeBuf_[(size_t)(h + y) * pitch + by] >> bit) & 1;
                if (a == 0) { uint8_t v = xo ? 255 : 0; o[0] = o[1] = o[2] = v; o[3] = 255; }
                else if (xo) { o[0] = o[1] = o[2] = 255; o[3] = 255; }                  // invert approximated as white
            }
        }
        if (!curTex_ || curW_ != w || curH_ != h) {
            curTex_.Reset(); curSRV_.Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev_->CreateTexture2D(&d, nullptr, &curTex_))) return;
            dev_->CreateShaderResourceView(curTex_.Get(), nullptr, &curSRV_);
            curW_ = w; curH_ = h;
        }
        ctx_->UpdateSubresource(curTex_.Get(), 0, nullptr, rgba.data(), w * 4, 0);
        curValid_ = true;
    }

    void set_cb(ComPtr<ID3D11Buffer> &cb, const void *data, size_t bytes)
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, data, bytes);
            ctx_->Unmap(cb.Get(), 0);
        }
    }

    void draw()
    {
        const float black[4] = {0, 0, 0, 1};
        ctx_->ClearRenderTargetView(rtv_.Get(), black);
        ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        if (!haveFrame_ || frW_ <= 0) return;

        const float scale = (std::min)((float)bbW_ / frW_, (float)bbH_ / frH_);
        const float vw = frW_ * scale, vh = frH_ * scale;
        const float vx = (bbW_ - vw) / 2, vy = (bbH_ - vh) / 2;

        D3D11_VIEWPORT vp{vx, vy, vw, vh, 0, 1};
        ctx_->RSSetViewports(1, &vp);
        int cbScene[4] = {srcHDR_ ? 1 : 0, 0, 0, 0};
        set_cb(cbScene_, cbScene, sizeof(cbScene));
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, cbScene_.GetAddressOf());
        ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
        ctx_->PSSetShaderResources(0, 1, frameSRV_.GetAddressOf());
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx_->Draw(3, 0);

        if (curVisible_ && curValid_ && curW_ > 0) {
            D3D11_VIEWPORT full{0, 0, (float)bbW_, (float)bbH_, 0, 1};
            ctx_->RSSetViewports(1, &full);
            const float dx = vx + curPos_.x * scale, dy = vy + curPos_.y * scale;
            const float dw = curW_ * scale, dh = curH_ * scale;
            float q[8] = {dx / bbW_ * 2 - 1, 1 - dy / bbH_ * 2,
                          (dx + dw) / bbW_ * 2 - 1, 1 - (dy + dh) / bbH_ * 2,
                          g_paperwhite_nits / 80.0f, 0, 0, 0};
            set_cb(cbQuad_, q, sizeof(q));
            ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ctx_->VSSetShader(vsQuad_.Get(), nullptr, 0);
            ctx_->VSSetConstantBuffers(0, 1, cbQuad_.GetAddressOf());
            ctx_->PSSetShader(psCursor_.Get(), nullptr, 0);
            ctx_->PSSetConstantBuffers(0, 1, cbQuad_.GetAddressOf());
            ctx_->PSSetShaderResources(0, 1, curSRV_.GetAddressOf());
            ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
            const float bf[4] = {0, 0, 0, 0};
            ctx_->OMSetBlendState(blendOver_.Get(), bf, 0xffffffff);
            ctx_->Draw(4, 0);
            ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        }
    }

    OutputRef ref_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain3> sc_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    long bbW_ = 1, bbH_ = 1;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> frameTex_;
    ComPtr<ID3D11ShaderResourceView> frameSRV_;
    int frW_ = 0, frH_ = 0; DXGI_FORMAT frFmt_ = DXGI_FORMAT_UNKNOWN; bool srcHDR_ = false; bool haveFrame_ = false;
    ComPtr<ID3D11VertexShader> vs_, vsQuad_;
    ComPtr<ID3D11PixelShader> ps_, psCursor_;
    ComPtr<ID3D11SamplerState> samp_;
    ComPtr<ID3D11Buffer> cbScene_, cbQuad_;
    ComPtr<ID3D11BlendState> blendOver_;
    ComPtr<ID3D11Texture2D> curTex_;
    ComPtr<ID3D11ShaderResourceView> curSRV_;
    int curW_ = 0, curH_ = 0; POINT curPos_{}; bool curVisible_ = false; bool curValid_ = false;
    std::vector<uint8_t> shapeBuf_;
};

static int run_preview(OutputRef ref)
{
    TimerRes timerRes;   // 1ms scheduler tick so timings aren't tick-quantized (#1)
    const wchar_t *cls = L"CtmCapturePreview";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(cls, L"CTM Capture - virtual display preview (D3D11)", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);

    auto gp = std::make_shared<GpuPreview>();
    std::wstring err;
    if (!gp->init(ref, hwnd, &err)) { std::wcerr << L"preview init: " << err << L"\n"; return 3; }
    ShowWindow(hwnd, SW_SHOW);

    std::thread render([gp]() {
        while (g_running.load()) {
            if (!gp->render()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    MSG m;
    while (GetMessage(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    g_running.store(false);
    if (render.joinable()) render.join();
    return 0;
}

// ===================== live HDR encode -> decode -> display =====================
// Default mode. Window A shows the raw captured HDR frame; window B shows the
// same frame after AMF HEVC/AV1 encode + decode. Codec, rate-control mode,
// bitrate, max/peak rate, QP, usage and the encode resolution are all changeable
// live (hotkeys) -- on change we rebuild only the affected stages; capture and
// both windows stay up. The cursor is composited into the captured frame before
// display+encode (DDA delivers it separately), so it shows in both windows and
// is baked into the stream the TV will receive.

static double now_ms()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

// HDR mastering info from the source monitor (DXGI_OUTPUT_DESC1).
struct HdrInfo {
    bool valid = false;
    float rx = 0.708f, ry = 0.292f, gx = 0.170f, gy = 0.797f, bx = 0.131f, by = 0.046f;  // BT.2020 defaults
    float wx = 0.3127f, wy = 0.3290f;
    float minLum = 0.0f, maxLum = 1000.0f, maxFFLum = 400.0f;
};
static HdrInfo read_hdr_info(const OutputRef &ref)
{
    HdrInfo h;
    ComPtr<IDXGIOutput6> o6;
    if (SUCCEEDED(ref.output1.As(&o6))) {
        DXGI_OUTPUT_DESC1 d{};
        if (SUCCEEDED(o6->GetDesc1(&d))) {
            h.valid = true;
            h.rx = d.RedPrimary[0];   h.ry = d.RedPrimary[1];
            h.gx = d.GreenPrimary[0]; h.gy = d.GreenPrimary[1];
            h.bx = d.BluePrimary[0];  h.by = d.BluePrimary[1];
            h.wx = d.WhitePoint[0];   h.wy = d.WhitePoint[1];
            h.minLum = d.MinLuminance; h.maxLum = d.MaxLuminance; h.maxFFLum = d.MaxFullFrameLuminance;
        }
    }
    return h;
}

static const wchar_t *colorspace_name(DXGI_COLOR_SPACE_TYPE cs)
{
    switch (cs) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:   return L"sRGB G2.2 BT.709 (SDR)";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:   return L"scRGB G1.0 BT.709 (HDR linear)";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:return L"HDR10 PQ BT.2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020: return L"HDR10 PQ BT.2020 (studio)";
    default:                                        return L"other";
    }
}

static const wchar_t *dxgi_format_name(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return L"R16G16B16A16_FLOAT (FP16 scRGB)";
    case DXGI_FORMAT_B8G8R8A8_UNORM:     return L"B8G8R8A8_UNORM (sRGB)";
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return L"R8G8B8A8_UNORM (sRGB)";
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return L"R10G10B10A2_UNORM";
    default:                             return L"(other)";
    }
}

// Log the display's actual color settings so we adapt to it rather than
// forcing a format. ColorSpace G2084/G10 => HDR is on for this output.
static void log_display_mode(const OutputRef &ref)
{
    ComPtr<IDXGIOutput6> o6;
    if (FAILED(ref.output1.As(&o6))) { std::wcout << L"display: IDXGIOutput6 unavailable\n"; return; }
    DXGI_OUTPUT_DESC1 d{};
    if (FAILED(o6->GetDesc1(&d))) { std::wcout << L"display: GetDesc1 failed\n"; return; }
    const bool hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                     d.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020 ||
                     d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    std::wcout << L"display: " << (d.DesktopCoordinates.right - d.DesktopCoordinates.left) << L"x"
               << (d.DesktopCoordinates.bottom - d.DesktopCoordinates.top)
               << L" bpc=" << d.BitsPerColor
               << L" colorspace=" << colorspace_name(d.ColorSpace)
               << L" HDR=" << (hdr ? L"on" : L"off") << L"\n";
}

struct EncConfig {
    enum Codec { HEVC = 0, AV1 = 1 } codec = HEVC;
    enum Mode  { CBR = 0, VBR = 1, CQP = 2 } mode = CBR;
    enum Usage { ULL = 0, LL = 1, TRANSCODE = 2 } usage = ULL;
    int resIndex = 0;          // index into kResHeights; 0 == native
    int bitrateKbps = 40000;   // target
    int maxrateKbps = 99000;   // peak / ceiling (VBR); CBR locks this to 99 Mbps
    int qp = 24;               // CQP qp / qvbr quality level
    bool allIntra = false;     // true = every frame an IDR (no P-frames)
    bool intraRefresh = true;  // rolling intra-refresh: no IDR spike, no VBV latency (TV-friendly)
    int slices = 1;            // slices(HEVC)/tiles(AV1) per frame: smaller NALs, loss resilience
    int maxFrameKbits = 0;     // per-frame size cap in kbits (0 = off): clips spikes w/o a buffer
    int fps = 0;               // encoder framerate for rate-control budget (0 = use display refresh)
    bool encoderDiffers(const EncConfig &o) const {
        return codec != o.codec || mode != o.mode || usage != o.usage ||
               bitrateKbps != o.bitrateKbps || maxrateKbps != o.maxrateKbps ||
               qp != o.qp || resIndex != o.resIndex || allIntra != o.allIntra ||
               intraRefresh != o.intraRefresh || slices != o.slices || maxFrameKbits != o.maxFrameKbits ||
               fps != o.fps;
    }
};
static const int kResHeights[] = {0, 2160, 1440, 1080, 720, 540};
static const wchar_t *kCodecName[] = {L"HEVC", L"AV1"};
static const wchar_t *kModeName[]  = {L"CBR", L"VBR", L"CQP"};
static const wchar_t *kUsageName[] = {L"ultralowlatency", L"lowlatency", L"transcoding"};

// Current refresh of the captured display (Hz) -> the encoder's rate-control
// framerate, so its per-frame bit budget matches reality at 120Hz, not 60.
static int display_refresh_hz(const wchar_t *deviceName)
{
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (deviceName && EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return (int)dm.dmDisplayFrequency;
    return 60;   // safe fallback
}

// shared live control state (written by WndProc, applied by the render thread)
static std::mutex g_cfgMtx;
static EncConfig g_cfgDesired;
static std::atomic<bool> g_cfgDirty{false};
static std::atomic<bool> g_vsync{false};   // V toggles; off = present without vblank wait
static bool g_noPreview = false;           // --nopreview: stream only, windows hidden
static std::atomic<bool> g_noDecode{false}; // --nodecode / 'D' hotkey: hide window B, no local decode
static std::atomic<bool> g_decodeBench{false}; // 'B' hotkey: run the serialized decode bench once
static bool g_profile = false;             // --profile: measure true HW-decode GPU
                                           // time (forces decode completion via a
                                           // timestamp query); adds a sync, so off
                                           // by default to keep the live path async
static bool g_cpucopy = false;             // --cpucopy: force the old GPU->CPU->GPU
                                           // encode input (readback + AMF re-upload).
                                           // Default OFF = zero-copy: convert renders
                                           // straight into the encoder's D3D11 P010
                                           // hwframe, no readback, no re-upload.
static int g_maxFps = 0;                   // --fps N: optional encode cadence cap for
                                           // diagnostics; 0 (default) = uncapped, the
                                           // pipeline runs at capture rate (VDD refresh)

// --- GPU converter: captured FP16 scRGB -> P010 (PQ BT.2020 YCbCr 4:2:0) ---
// Y plane rendered full-res into R16_UNORM, interleaved CbCr half-res into
// R16G16_UNORM; both encode the 10-bit code << 6 exactly like P010, so the
// readback rows memcpy straight into a P010 AVFrame. Feeding the encoder
// P010 instead of RGB skips AMF's internal color conversion.
static const char *kPSConvert = R"(
Texture2D tex : register(t0); SamplerState smp : register(s0);
static const float3x3 M709to2020 = {
    0.627404, 0.329283, 0.043313,
    0.069097, 0.919541, 0.011362,
    0.016391, 0.088013, 0.895595 };
float3 pq_oetf(float3 L) {           // L normalized so 1.0 == 10000 nits
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 Lp = pow(max(L, 0.0), m1);
    return pow((c1 + c2 * Lp) / (1.0 + c3 * Lp), m2);
}
float3 pq2020(float2 uv) {
    float3 scrgb = max(tex.Sample(smp, uv).rgb, 0.0);   // linear BT.709, 1.0 == 80 nits
    float3 L = mul(M709to2020, scrgb) * (80.0 / 10000.0);
    return pq_oetf(saturate(L));                        // PQ BT.2020 R'G'B'
}
float lum(float3 p) { return dot(p, float3(0.2627, 0.6780, 0.0593)); }   // BT.2020 NCL
float4 mainY(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {
    float Y = lum(pq2020(uv));                          // limited range, code<<6
    return float4((876.0 * Y + 64.0) * (64.0 / 65535.0), 0, 0, 1);
}
float4 mainUV(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {
    float3 p = pq2020(uv);                              // half-res: linear tap ~= 2x2 box
    float Y = lum(p);
    float Cb = (p.b - Y) / 1.8814;
    float Cr = (p.r - Y) / 1.4746;
    return float4((896.0 * Cb + 512.0) * (64.0 / 65535.0),
                  (896.0 * Cr + 512.0) * (64.0 / 65535.0), 0, 1);
})";

class Converter {
public:
    bool init(ID3D11Device *dev, ID3D11DeviceContext *ctx, std::wstring *err)
    {
        dev_ = dev; ctx_ = ctx;
        auto vsb = compile_shader(kVS, "main", "vs_5_0");
        auto psy = compile_shader(kPSConvert, "mainY", "ps_5_0");
        auto psc = compile_shader(kPSConvert, "mainUV", "ps_5_0");
        if (!vsb || !psy || !psc) { if (err) *err = L"converter shader compile failed"; return false; }
        dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
        dev_->CreatePixelShader(psy->GetBufferPointer(), psy->GetBufferSize(), nullptr, &psY_);
        dev_->CreatePixelShader(psc->GetBufferPointer(), psc->GetBufferSize(), nullptr, &psUV_);
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&s, &samp_);
        return true;
    }

    static const int kSlots = 3;   // 1 mapped by the codec thread + 2 rotating (latest-wins)

    bool resize(int w, int h)
    {
        if (w == w_ && h == h_ && rtY_) return true;
        w_ = w; h_ = h;
        auto mkRT = [&](int tw, int th, DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D> &t, ComPtr<ID3D11RenderTargetView> &r) {
            t.Reset(); r.Reset();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = tw; td.Height = th; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = fmt; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t))) return false;
            return SUCCEEDED(dev_->CreateRenderTargetView(t.Get(), nullptr, &r));
        };
        auto mkStage = [&](int tw, int th, DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D> &t) {
            t.Reset();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = tw; td.Height = th; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = fmt; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            return SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &t));
        };
        if (!mkRT(w, h, DXGI_FORMAT_R16_UNORM, rtY_, rtvY_)) return false;
        if (!mkRT(w / 2, h / 2, DXGI_FORMAT_R16G16_UNORM, rtUV_, rtvUV_)) return false;
        for (int i = 0; i < kSlots; ++i) {
            if (!mkStage(w, h, DXGI_FORMAT_R16_UNORM, stagY_[i])) return false;
            if (!mkStage(w / 2, h / 2, DXGI_FORMAT_R16G16_UNORM, stagUV_[i])) return false;
        }
        // single P010 RT texture for the zero-copy encode path: render the convert
        // into this, then CopySubresourceRegion it into the encoder pool slice (the
        // pool can't be an RT array on AMD). Plane RTVs select Y/UV by format.
        encRt_.Reset(); encRtY_.Reset(); encRtUV_.Reset();
        {
            D3D11_TEXTURE2D_DESC pd{};
            pd.Width = w; pd.Height = h; pd.MipLevels = 1; pd.ArraySize = 1;
            pd.Format = DXGI_FORMAT_P010; pd.SampleDesc.Count = 1;
            pd.Usage = D3D11_USAGE_DEFAULT; pd.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (SUCCEEDED(dev_->CreateTexture2D(&pd, nullptr, &encRt_))) {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rd.Texture2D.MipSlice = 0;
                rd.Format = DXGI_FORMAT_R16_UNORM;
                dev_->CreateRenderTargetView(encRt_.Get(), &rd, &encRtY_);
                rd.Format = DXGI_FORMAT_R16G16_UNORM;
                dev_->CreateRenderTargetView(encRt_.Get(), &rd, &encRtUV_);
            } else {
                std::wcout << L"converter: single P010 RT texture create failed; zero-copy convert unavailable\n";
            }
        }
        return true;
    }

    // Render src -> P010 planes at target res and queue copies into slot's
    // staging pair. map() comes later (next loop), once the GPU has finished,
    // so the readback never stalls the render thread.
    bool submit(ID3D11ShaderResourceView *srcSRV, int slot)
    {
        if (!rtY_ || slot < 0 || slot >= kSlots) return false;
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
        ctx_->PSSetShaderResources(0, 1, &srcSRV);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);

        D3D11_VIEWPORT vpY{0, 0, (float)w_, (float)h_, 0, 1};
        ctx_->RSSetViewports(1, &vpY);
        ctx_->OMSetRenderTargets(1, rtvY_.GetAddressOf(), nullptr);
        ctx_->PSSetShader(psY_.Get(), nullptr, 0);
        ctx_->Draw(3, 0);

        D3D11_VIEWPORT vpC{0, 0, (float)(w_ / 2), (float)(h_ / 2), 0, 1};
        ctx_->RSSetViewports(1, &vpC);
        ctx_->OMSetRenderTargets(1, rtvUV_.GetAddressOf(), nullptr);
        ctx_->PSSetShader(psUV_.Get(), nullptr, 0);
        ctx_->Draw(3, 0);

        ID3D11ShaderResourceView *nullSRV = nullptr;
        ctx_->PSSetShaderResources(0, 1, &nullSRV);
        ctx_->CopyResource(stagY_[slot].Get(), rtY_.Get());
        ctx_->CopyResource(stagUV_[slot].Get(), rtUV_.Get());
        return true;
    }

    // Render src -> our single P010 RT texture (encRt_). The native AMF encoder
    // wraps encRt_ directly (true zero-copy); the ffmpeg fallback copies it into
    // its pool (submit_to_planes). Y/UV plane RTVs select planes by format.
    bool render_p010(ID3D11ShaderResourceView *srcSRV)
    {
        if (!srcSRV || !encRt_ || !encRtY_ || !encRtUV_) return false;
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
        ctx_->PSSetShaderResources(0, 1, &srcSRV);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);

        D3D11_VIEWPORT vpY{0, 0, (float)w_, (float)h_, 0, 1};
        ctx_->RSSetViewports(1, &vpY);
        ctx_->OMSetRenderTargets(1, encRtY_.GetAddressOf(), nullptr);   // Y plane
        ctx_->PSSetShader(psY_.Get(), nullptr, 0);
        ctx_->Draw(3, 0);

        D3D11_VIEWPORT vpC{0, 0, (float)(w_ / 2), (float)(h_ / 2), 0, 1};
        ctx_->RSSetViewports(1, &vpC);
        ctx_->OMSetRenderTargets(1, encRtUV_.GetAddressOf(), nullptr);  // UV plane
        ctx_->PSSetShader(psUV_.Get(), nullptr, 0);
        ctx_->Draw(3, 0);

        ID3D11ShaderResourceView *nullSRV = nullptr;
        ctx_->PSSetShaderResources(0, 1, &nullSRV);
        ID3D11RenderTargetView *nullRTV = nullptr;
        ctx_->OMSetRenderTargets(1, &nullRTV, nullptr);
        ctx_->Flush();                                     // submit before the encoder reads it
        return true;
    }
    ID3D11Texture2D *p010_texture() const { return encRt_.Get(); }

    // ffmpeg fallback: render into encRt_, then GPU-copy into the pool slice.
    bool submit_to_planes(ID3D11ShaderResourceView *srcSRV, ID3D11Texture2D *p010, UINT index)
    {
        if (!render_p010(srcSRV) || !p010) return false;
        ctx_->CopySubresourceRegion(p010, D3D11CalcSubresource(0, index, 1), 0, 0, 0, encRt_.Get(), 0, nullptr);
        ctx_->Flush();
        return true;
    }

    // 1 = both planes mapped, 0 = GPU still copying (retry), -1 = error
    int map(int slot, const uint8_t **y, int *yPitch, const uint8_t **uv, int *uvPitch)
    {
        if (slot < 0 || slot >= kSlots || !stagY_[slot] || !stagUV_[slot]) return -1;
        HRESULT hr = ctx_->Map(stagY_[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapY_);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return 0;
        if (FAILED(hr)) return -1;
        hr = ctx_->Map(stagUV_[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapUV_);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { ctx_->Unmap(stagY_[slot].Get(), 0); return 0; }
        if (FAILED(hr)) { ctx_->Unmap(stagY_[slot].Get(), 0); return -1; }
        *y = static_cast<const uint8_t *>(mapY_.pData);
        *yPitch = (int)mapY_.RowPitch;
        *uv = static_cast<const uint8_t *>(mapUV_.pData);
        *uvPitch = (int)mapUV_.RowPitch;
        return 1;
    }
    void unmap(int slot)
    {
        if (slot < 0 || slot >= kSlots) return;
        if (stagY_[slot]) ctx_->Unmap(stagY_[slot].Get(), 0);
        if (stagUV_[slot]) ctx_->Unmap(stagUV_[slot].Get(), 0);
    }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    ID3D11Device *dev_ = nullptr; ID3D11DeviceContext *ctx_ = nullptr;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psY_, psUV_;
    ComPtr<ID3D11SamplerState> samp_;
    ComPtr<ID3D11Texture2D> rtY_, rtUV_, stagY_[kSlots], stagUV_[kSlots];
    ComPtr<ID3D11RenderTargetView> rtvY_, rtvUV_;
    D3D11_MAPPED_SUBRESOURCE mapY_{}, mapUV_{};
    int w_ = 0, h_ = 0;
    // single P010 RT texture for the near-zero-copy encode path (submit_to_planes)
    ComPtr<ID3D11Texture2D> encRt_;
    ComPtr<ID3D11RenderTargetView> encRtY_, encRtUV_;
};

// --- AMF encoder (libavcodec hevc_amf / av1_amf), 10-bit x2bgr10le input ---
static AVRational q_xy(float v) { return av_make_q((int)(v * 50000.0f + 0.5f), 50000); }

class Encoder {
public:
    bool open(const EncConfig &cfg, int w, int h, const HdrInfo &hdr, AVBufferRef *hwDevice, std::string *err)
    {
        close();
        hdr_ = hdr;
        const char *name = (cfg.codec == EncConfig::HEVC) ? "hevc_amf" : "av1_amf";
        const AVCodec *codec = avcodec_find_encoder_by_name(name);
        if (!codec) { if (err) *err = std::string("encoder not found: ") + name; return false; }
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) { if (err) *err = "alloc encoder ctx failed"; return false; }
        ctx_->width = w; ctx_->height = h;
        // PTS carries real capture time in microseconds -- frames are encoded
        // only when the desktop changes, so frame-index timing would lie to
        // the receiver's renderer. framerate stays as a rate-control hint.
        ctx_->time_base = av_make_q(1, 1000000);
        ctx_->framerate = av_make_q(60, 1);
        // Long GOP: an IDR every second (~60) caused a visible 1 Hz hiccup on
        // the TV (multi-megabit burst + heavier decode). New clients don't
        // wait for the GOP anyway -- joining forces an IDR.
        // All-intra: every frame an IDR (no inter prediction) -- lowest latency,
        // resilient to loss, much higher bitrate.
        ctx_->gop_size = cfg.allIntra ? 1 : 600;
        ctx_->max_b_frames = 0;
        ctx_->color_primaries = AVCOL_PRI_BT2020;
        ctx_->color_trc = AVCOL_TRC_SMPTE2084;
        ctx_->colorspace = AVCOL_SPC_BT2020_NCL;
        ctx_->color_range = AVCOL_RANGE_MPEG;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

        // Zero-copy input (default): hand the encoder a D3D11 P010 frames pool so
        // the converter renders straight into encoder textures (BIND_RENDER_TARGET)
        // -- no GPU->CPU readback and no AMF re-upload. --cpucopy forces the CPU
        // path; we also fall back to it if the pool won't initialize.
        hw_ = false;
        if (hwDevice && !g_cpucopy) {
            hwFrames_ = av_hwframe_ctx_alloc(hwDevice);
            if (hwFrames_) {
                auto *fc = reinterpret_cast<AVHWFramesContext *>(hwFrames_->data);
                fc->format = AV_PIX_FMT_D3D11;
                fc->sw_format = AV_PIX_FMT_P010;
                fc->width = w; fc->height = h;
                fc->initial_pool_size = 16;
                auto *d3dfc = reinterpret_cast<AVD3D11VAFramesContext *>(fc->hwctx);
                // The AMD driver rejects a P010 texture ARRAY with RENDER_TARGET, so
                // the pool is SHADER_RESOURCE only (creatable, like the decode pool);
                // the converter renders into its own single P010 RT texture and
                // GPU-copies it into a pool slice (one blit, no readback).
                d3dfc->BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (av_hwframe_ctx_init(hwFrames_) >= 0) hw_ = true;
                else av_buffer_unref(&hwFrames_);
            }
            if (!hw_) std::wcout << L"encoder: D3D11 zero-copy pool init failed; using CPU copy\n";
        }
        ctx_->pix_fmt = hw_ ? AV_PIX_FMT_D3D11 : AV_PIX_FMT_P010LE;  // shader already PQ BT.2020 YCbCr
        if (hw_) {
            ctx_->sw_pix_fmt = AV_PIX_FMT_P010;
            ctx_->hw_frames_ctx = av_buffer_ref(hwFrames_);
        }

        const int64_t br = (int64_t)cfg.bitrateKbps * 1000;
        const int64_t mr = (int64_t)std::max(cfg.maxrateKbps, cfg.bitrateKbps) * 1000;
        const char *usage = kUsageNameA(cfg.usage);
        // 1 frame in flight: the default (16) holds frames inside AMF and only
        // releases packet N after ~16 more submits -- seconds of latency when
        // the screen is mostly static.
        av_opt_set_int(ctx_, "async_depth", 1, AV_OPT_SEARCH_CHILDREN);
        // honor pict_type=I as a full IDR (new stream clients sync on it)
        av_opt_set_int(ctx_, "forced_idr", 1, AV_OPT_SEARCH_CHILDREN);
        av_opt_set(ctx_, "usage", usage, AV_OPT_SEARCH_CHILDREN);
        av_opt_set(ctx_, "quality", cfg.usage == EncConfig::ULL ? "speed"
                                  : cfg.usage == EncConfig::LL ? "balanced" : "quality",
                   AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(ctx_, "bitdepth", 10, AV_OPT_SEARCH_CHILDREN);
        if (cfg.codec == EncConfig::HEVC)
            av_opt_set(ctx_, "profile", "main10", AV_OPT_SEARCH_CHILDREN);

        switch (cfg.mode) {
        case EncConfig::CBR:
            av_opt_set(ctx_, "rc", "cbr", AV_OPT_SEARCH_CHILDREN);
            ctx_->bit_rate = br; ctx_->rc_max_rate = br; ctx_->rc_buffer_size = (int)br;
            break;
        case EncConfig::VBR:
            av_opt_set(ctx_, "rc", "vbr_peak", AV_OPT_SEARCH_CHILDREN);
            ctx_->bit_rate = br; ctx_->rc_max_rate = mr; ctx_->rc_buffer_size = (int)mr;
            break;
        case EncConfig::CQP:
            if (cfg.codec == EncConfig::HEVC) {
                av_opt_set(ctx_, "rc", "cqp", AV_OPT_SEARCH_CHILDREN);
                av_opt_set_int(ctx_, "qp_i", cfg.qp, AV_OPT_SEARCH_CHILDREN);
                av_opt_set_int(ctx_, "qp_p", cfg.qp, AV_OPT_SEARCH_CHILDREN);
            } else {  // av1_amf has no plain cqp -> quality-targeted VBR
                av_opt_set(ctx_, "rc", "qvbr", AV_OPT_SEARCH_CHILDREN);
                av_opt_set_int(ctx_, "qvbr_quality_level", cfg.qp, AV_OPT_SEARCH_CHILDREN);
            }
            break;
        }

        if (avcodec_open2(ctx_, codec, nullptr) < 0) { if (err) *err = std::string("avcodec_open2 failed for ") + name; close(); return false; }

        if (!hw_) {   // CPU path: one persistent P010 frame, color + HDR stamped once
            frame_ = av_frame_alloc();
            frame_->format = AV_PIX_FMT_P010LE; frame_->width = w; frame_->height = h;
            if (av_frame_get_buffer(frame_, 0) < 0) { if (err) *err = "frame buffer alloc failed"; close(); return false; }
            stamp_frame(frame_);
        }
        pkt_ = av_packet_alloc();
        w_ = w; h_ = h;
        return true;
    }

    // Color + HDR10 static metadata (AMF writes it as SEI). On the CPU frame this
    // is done once; on each zero-copy hwframe it's re-stamped before send.
    void stamp_frame(AVFrame *f)
    {
        f->color_primaries = AVCOL_PRI_BT2020;
        f->color_trc = AVCOL_TRC_SMPTE2084;
        f->colorspace = AVCOL_SPC_BT2020_NCL;
        f->color_range = AVCOL_RANGE_MPEG;            // shader outputs limited range
        if (AVMasteringDisplayMetadata *md = av_mastering_display_metadata_create_side_data(f)) {
            md->display_primaries[0][0] = q_xy(hdr_.rx); md->display_primaries[0][1] = q_xy(hdr_.ry);
            md->display_primaries[1][0] = q_xy(hdr_.gx); md->display_primaries[1][1] = q_xy(hdr_.gy);
            md->display_primaries[2][0] = q_xy(hdr_.bx); md->display_primaries[2][1] = q_xy(hdr_.by);
            md->white_point[0] = q_xy(hdr_.wx); md->white_point[1] = q_xy(hdr_.wy);
            md->min_luminance = av_make_q((int)(hdr_.minLum * 10000.0f), 10000);
            md->max_luminance = av_make_q((int)hdr_.maxLum, 1);
            md->has_primaries = 1; md->has_luminance = 1;
        }
        if (AVContentLightMetadata *cl = av_content_light_metadata_create_side_data(f)) {
            cl->MaxCLL = (unsigned)hdr_.maxLum; cl->MaxFALL = (unsigned)hdr_.maxFFLum;
        }
    }

    // CPU path (--cpucopy): copy mapped P010 planes (Y full res, interleaved CbCr
    // half res) into the persistent frame and encode.
    int encode(const uint8_t *y, int yPitch, const uint8_t *uv, int uvPitch,
               int64_t pts, std::vector<AVPacket *> &out)
    {
        if (!ctx_ || !frame_) return -1;
        if (av_frame_make_writable(frame_) < 0) return -1;
        const int bytes = w_ * 2;              // both planes: w_*2 bytes per row
        for (int r = 0; r < h_; ++r)
            memcpy(frame_->data[0] + (size_t)r * frame_->linesize[0],
                   y + (size_t)r * yPitch, bytes);
        for (int r = 0; r < h_ / 2; ++r)
            memcpy(frame_->data[1] + (size_t)r * frame_->linesize[1],
                   uv + (size_t)r * uvPitch, bytes);
        frame_->pts = pts;
        frame_->pict_type = forceIdr_.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        int r = avcodec_send_frame(ctx_, frame_);
        if (r < 0) return r;
        return drain(out);
    }

    bool is_hw() const { return hw_; }

    // Zero-copy: borrow a P010 D3D11 texture from the encoder pool. The caller
    // renders into it (Converter::submit_to_planes) then passes it to encode_hw().
    // Returned frame: data[0]=ID3D11Texture2D*, data[1]=array slice index.
    AVFrame *get_hwframe()
    {
        if (!hw_ || !hwFrames_) return nullptr;
        AVFrame *f = av_frame_alloc();
        if (!f) return nullptr;
        if (av_hwframe_get_buffer(hwFrames_, f, 0) < 0) { av_frame_free(&f); return nullptr; }
        return f;
    }
    // Encode a hwframe the caller already rendered into; takes ownership (frees it).
    int encode_hw(AVFrame *f, int64_t pts, std::vector<AVPacket *> &out)
    {
        if (!ctx_ || !f) { if (f) av_frame_free(&f); return -1; }
        stamp_frame(f);
        f->pts = pts;
        f->pict_type = forceIdr_.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        int r = avcodec_send_frame(ctx_, f);
        av_frame_free(&f);                     // send_frame took its own ref
        if (r < 0) return r;
        return drain(out);
    }

    void force_idr() { forceIdr_.store(true); }

    void close()
    {
        if (pkt_) av_packet_free(&pkt_);
        if (frame_) av_frame_free(&frame_);
        if (ctx_) avcodec_free_context(&ctx_);
        if (hwFrames_) av_buffer_unref(&hwFrames_);
        hw_ = false;
        w_ = h_ = 0;
    }
    ~Encoder() { close(); }

private:
    // Drain ready packets (1-in/1-out low latency). Busy-poll, never sleep: a
    // sleep_for() rounds up to the OS timer tick and inflates the measured encMs
    // (the Fe budget reads this). The wall-clock deadline can never deadlock.
    int drain(std::vector<AVPacket *> &out)
    {
        const double encDeadlineMs = now_ms() + 200.0;
        for (;;) {
            int r = avcodec_receive_packet(ctx_, pkt_);
            if (r == 0) {
                AVPacket *p = av_packet_alloc();
                av_packet_move_ref(p, pkt_);
                out.push_back(p);
                continue;            // drain anything else that's ready
            }
            if (r == AVERROR(EAGAIN)) {
                if (!out.empty()) return 0;
                if (now_ms() >= encDeadlineMs) return 0;
                YieldProcessor();    // tight spin: keeps encMs == true encode latency
                continue;
            }
            return (r == AVERROR_EOF) ? 0 : r;
        }
    }

    static const char *kUsageNameA(int u) { return u == 0 ? "ultralowlatency" : u == 1 ? "lowlatency" : "transcoding"; }
    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *pkt_ = nullptr;
    AVBufferRef *hwFrames_ = nullptr;
    bool hw_ = false;
    HdrInfo hdr_{};
    std::atomic<bool> forceIdr_{false};
    int w_ = 0, h_ = 0;
};

#include "amf_encoder.inl"   // native AMD AMF encoder (HEVC+AV1), true zero-copy
#include "amf_decoder.inl"   // native AMD AMF decoder (HEVC+AV1), zero-copy D3D11

// --- decoder: D3D11VA hardware (zero-copy P010 textures), sw fallback ---
class Decoder {
public:
    bool open(EncConfig::Codec c, AVBufferRef *hwdev, std::string *err)
    {
        close();
        codec_ = c;
        hwFailed_.store(false);
        if (hwdev) {
            const char *name = (c == EncConfig::HEVC) ? "hevc" : "av1";
            if (const AVCodec *codec = avcodec_find_decoder_by_name(name)) {
                ctx_ = avcodec_alloc_context3(codec);
                if (ctx_) {
                    ctx_->opaque = this;
                    ctx_->get_format = &Decoder::get_hw_format;
                    ctx_->hw_device_ctx = av_buffer_ref(hwdev);
                    ctx_->thread_count = 1;          // hwaccel: no CPU threading
                    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
                    if (avcodec_open2(ctx_, codec, nullptr) >= 0) {
                        frame_ = av_frame_alloc();
                        hw_ = true;
                        return true;
                    }
                    avcodec_free_context(&ctx_);
                }
            }
        }
        return open_sw(err);
    }

    // Software path. FFmpeg's built-in "av1" decoder is hwaccel-only, so AV1
    // uses libdav1d here. Slice threads only -- frame threading buffers
    // ~core-count frames and turns a static screen into seconds of latency.
    bool open_sw(std::string *err)
    {
        close();
        const char *name = (codec_ == EncConfig::HEVC) ? "hevc" : "libdav1d";
        const AVCodec *codec = avcodec_find_decoder_by_name(name);
        if (!codec) { if (err) *err = std::string("decoder not found: ") + name; return false; }
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) { if (err) *err = "decoder alloc failed"; return false; }
        ctx_->thread_count = 0;
        ctx_->thread_type = FF_THREAD_SLICE;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (avcodec_open2(ctx_, codec, nullptr) < 0) { if (err) *err = "decoder open failed"; close(); return false; }
        frame_ = av_frame_alloc();
        hw_ = false;
        hwFailed_.store(false);
        return true;
    }

    bool is_hw() const { return hw_; }
    bool hw_failed() const { return hwFailed_.load(); }
    // Returns the decoded frame (owned by decoder, valid until next call) or null.
    // NOTE: avcodec_receive_frame() unrefs the frame at its start, so we must NOT
    // call it again before the caller consumes the result -- return on first hit.
    // Safe here because the low-delay stream is 1-in/1-out (no B-frames/reorder).
    AVFrame *decode(AVPacket *pkt)
    {
        if (!ctx_) return nullptr;
        if (avcodec_send_packet(ctx_, pkt) < 0) return nullptr;
        if (avcodec_receive_frame(ctx_, frame_) == 0) return frame_;
        return nullptr;
    }
    void close()
    {
        if (frame_) av_frame_free(&frame_);
        if (ctx_) avcodec_free_context(&ctx_);
    }
    ~Decoder() { close(); }

private:
    // Choose D3D11 output and size the frame pool ourselves so the decode
    // textures are also shader-bindable (sampled directly by the display pass).
    static AVPixelFormat get_hw_format(AVCodecContext *ctx, const AVPixelFormat *fmts)
    {
        Decoder *self = static_cast<Decoder *>(ctx->opaque);
        for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p != AV_PIX_FMT_D3D11) continue;
            AVBufferRef *fctx = nullptr;
            if (avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx, AV_PIX_FMT_D3D11, &fctx) >= 0) {
                auto *fc = reinterpret_cast<AVHWFramesContext *>(fctx->data);
                auto *hwf = reinterpret_cast<AVD3D11VAFramesContext *>(fc->hwctx);
                hwf->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
                fc->initial_pool_size += 4;          // display holds a few refs
                if (av_hwframe_ctx_init(fctx) >= 0) {
                    ctx->hw_frames_ctx = fctx;
                    return AV_PIX_FMT_D3D11;
                }
                av_buffer_unref(&fctx);
            }
        }
        if (self) self->hwFailed_.store(true);       // worker reopens software
        return fmts[0];
    }

    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    EncConfig::Codec codec_ = EncConfig::HEVC;
    bool hw_ = false;
    std::atomic<bool> hwFailed_{false};
};

// --- decoded-view: upload yuv420p10le planes, reconstruct scRGB for window B ---
static const char *kPSDecoded = R"(
Texture2D texY : register(t0); Texture2D texU : register(t1); Texture2D texV : register(t2);
SamplerState smp : register(s0);
cbuffer DV : register(b0) { float2 uvScale; int dstHDR; int _pad; };
static const float3x3 M2020to709 = {
     1.660491, -0.587641, -0.072850,
    -0.124550,  1.132900, -0.008349,
    -0.018151, -0.100579,  1.118730 };
float3 pq_eotf(float3 E) {            // returns L normalized so 1.0 == 10000 nits
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 Ep = pow(max(E, 0.0), 1.0 / m2);
    return pow(max(Ep - c1, 0.0) / (c2 - c3 * Ep), 1.0 / m1);
}
float3 aces(float3 x){ const float a=2.51,b=0.03,c=2.43,d=0.59,e=0.14; return saturate((x*(a*x+b))/(x*(c*x+d)+e)); }
float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {
    float k = 65535.0;                                   // R16 UNORM sample -> 10-bit code
    float Y = texY.Sample(smp, uv).r * k;
    float Cb = texU.Sample(smp, uv).r * k;
    float Cr = texV.Sample(smp, uv).r * k;
    float yn = (Y - 64.0) / 876.0;                       // limited-range luma
    float cb = (Cb - 512.0) / 896.0;
    float cr = (Cr - 512.0) / 896.0;
    float3 rgbpq;                                        // BT.2020 NCL inverse matrix
    rgbpq.r = yn + 1.47460 * cr;
    rgbpq.g = yn - 0.16455 * cb - 0.57135 * cr;
    rgbpq.b = yn + 1.88140 * cb;
    float3 L = pq_eotf(saturate(rgbpq));                 // linear BT.2020, 1.0==10000nit
    float3 scrgb = mul(M2020to709, L * 125.0);           // -> scRGB units (1.0==80nit)
    return float4(dstHDR != 0 ? scrgb : aces(scrgb), 1.0);
})";

// Zero-copy variant: sample the decoder's P010 texture array slice directly
// (Y as R16, interleaved CbCr as R16G16; values are the 10-bit code << 6).
// uvScale compensates for codec surface alignment padding.
static const char *kPSDecodedHW = R"(
Texture2D texY : register(t0); Texture2D texUV : register(t1);
SamplerState smp : register(s0);
cbuffer DV : register(b0) { float2 uvScale; int dstHDR; int _pad; };
static const float3x3 M2020to709 = {
     1.660491, -0.587641, -0.072850,
    -0.124550,  1.132900, -0.008349,
    -0.018151, -0.100579,  1.118730 };
float3 pq_eotf(float3 E) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 Ep = pow(max(E, 0.0), 1.0 / m2);
    return pow(max(Ep - c1, 0.0) / (c2 - c3 * Ep), 1.0 / m1);
}
float3 aces(float3 x){ const float a=2.51,b=0.03,c=2.43,d=0.59,e=0.14; return saturate((x*(a*x+b))/(x*(c*x+d)+e)); }
float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {
    float2 tuv = uv * uvScale;
    float k = 65535.0 / 64.0;                            // P010 -> 10-bit code
    float Y = texY.Sample(smp, tuv).r * k;
    float2 C = texUV.Sample(smp, tuv).rg * k;
    float yn = (Y - 64.0) / 876.0;
    float cb = (C.x - 512.0) / 896.0;
    float cr = (C.y - 512.0) / 896.0;
    float3 rgbpq;
    rgbpq.r = yn + 1.47460 * cr;
    rgbpq.g = yn - 0.16455 * cb - 0.57135 * cr;
    rgbpq.b = yn + 1.88140 * cb;
    float3 L = pq_eotf(saturate(rgbpq));
    float3 scrgb = mul(M2020to709, L * 125.0);
    return float4(dstHDR != 0 ? scrgb : aces(scrgb), 1.0);
})";

class DecodedView {
public:
    bool init(ID3D11Device *dev, ID3D11DeviceContext *ctx, std::wstring *err)
    {
        dev_ = dev; ctx_ = ctx;
        auto vsb = compile_shader(kVS, "main", "vs_5_0");
        auto psb = compile_shader(kPSDecoded, "main", "ps_5_0");
        auto psh = compile_shader(kPSDecodedHW, "main", "ps_5_0");
        if (!vsb || !psb || !psh) { if (err) *err = L"decoded-view shader compile failed"; return false; }
        dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
        dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
        dev_->CreatePixelShader(psh->GetBufferPointer(), psh->GetBufferSize(), nullptr, &psHw_);
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&s, &samp_);
        D3D11_BUFFER_DESC cb{};
        cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; cb.ByteWidth = 16;
        dev_->CreateBuffer(&cb, nullptr, &cbUv_);
        return true;
    }

    // Take ownership of a hardware (AV_PIX_FMT_D3D11) frame. The previous
    // frame is kept one generation as the GPU may still read it in flight.
    void set_hw(AVFrame *f)
    {
        if (hwFramePrev_) av_frame_free(&hwFramePrev_);
        hwFramePrev_ = hwFrame_;
        hwFrame_ = f;
        hwMode_ = true;
        valid_ = true;
    }
    ~DecodedView()
    {
        if (hwFrame_) av_frame_free(&hwFrame_);
        if (hwFramePrev_) av_frame_free(&hwFramePrev_);
    }

    void upload(const AVFrame *f)
    {
        if (!f || f->width <= 0) return;
        ensure(f->width, f->height);
        ctx_->UpdateSubresource(texY_.Get(), 0, nullptr, f->data[0], f->linesize[0], 0);
        ctx_->UpdateSubresource(texU_.Get(), 0, nullptr, f->data[1], f->linesize[1], 0);
        ctx_->UpdateSubresource(texV_.Get(), 0, nullptr, f->data[2], f->linesize[2], 0);
        hwMode_ = false;
        valid_ = true;
    }

    // Draw the decoded image letterboxed into a bbW x bbH render target.
    void draw(int bbW, int bbH)
    {
        if (!valid_) return;
        if (hwMode_) { draw_hw(bbW, bbH); return; }
        const float scale = std::min((float)bbW / w_, (float)bbH / h_);
        const float vw = w_ * scale, vh = h_ * scale;
        D3D11_VIEWPORT vp{(bbW - vw) / 2, (bbH - vh) / 2, vw, vh, 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        set_dv(1.0f, 1.0f);
        ctx_->PSSetConstantBuffers(0, 1, cbUv_.GetAddressOf());
        ID3D11ShaderResourceView *srvs[3] = {srvY_.Get(), srvU_.Get(), srvV_.Get()};
        ctx_->PSSetShaderResources(0, 3, srvs);
        ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView *n3[3] = {nullptr, nullptr, nullptr};
        ctx_->PSSetShaderResources(0, 3, n3);
    }
    bool valid() const { return valid_; }
    void set_dst_hdr(bool h) { dstHDR_ = h; }

private:
    void set_dv(float ux, float uy)   // upload uvScale + dstHDR to b0
    {
        struct { float ux, uy; int dstHDR, pad; } cb{ux, uy, dstHDR_ ? 1 : 0, 0};
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(cbUv_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof(cb)); ctx_->Unmap(cbUv_.Get(), 0); }
    }
    // Sample the decoder's pool texture (array slice) directly -- no copy.
    void draw_hw(int bbW, int bbH)
    {
        if (!hwFrame_) return;
        auto *tex = reinterpret_cast<ID3D11Texture2D *>(hwFrame_->data[0]);
        const UINT slice = (UINT)(uintptr_t)hwFrame_->data[1];
        if (!tex) return;
        if (tex != cachedTex_) {                 // new pool: rebuild SRV cache
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            sliceSrv_.clear();
            sliceSrv_.resize(d.ArraySize);
            texW_ = (int)d.Width; texH_ = (int)d.Height;
            cachedTex_ = tex;
        }
        if (slice >= sliceSrv_.size()) return;
        if (!sliceSrv_[slice].first) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MipLevels = 1;
            sd.Texture2DArray.FirstArraySlice = slice;
            sd.Texture2DArray.ArraySize = 1;
            sd.Format = DXGI_FORMAT_R16_UNORM;
            dev_->CreateShaderResourceView(tex, &sd, &sliceSrv_[slice].first);
            sd.Format = DXGI_FORMAT_R16G16_UNORM;
            dev_->CreateShaderResourceView(tex, &sd, &sliceSrv_[slice].second);
        }
        if (!sliceSrv_[slice].first || !sliceSrv_[slice].second) return;
        const int fw = hwFrame_->width, fh = hwFrame_->height;
        set_dv((float)fw / texW_, (float)fh / texH_);
        const float scale = std::min((float)bbW / fw, (float)bbH / fh);
        const float vw = fw * scale, vh = fh * scale;
        D3D11_VIEWPORT vp{(bbW - vw) / 2, (bbH - vh) / 2, vw, vh, 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(psHw_.Get(), nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, cbUv_.GetAddressOf());
        ID3D11ShaderResourceView *srvs[2] = {sliceSrv_[slice].first.Get(), sliceSrv_[slice].second.Get()};
        ctx_->PSSetShaderResources(0, 2, srvs);
        ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView *n2[2] = {nullptr, nullptr};
        ctx_->PSSetShaderResources(0, 2, n2);
    }

public:
    // Draw a single AMF-decoded P010 texture (one texture, both planes) -- not an
    // ffmpeg array-pool slice. SRVs are per-frame (cheap) so we never pin a pool
    // surface. imgW/imgH = the coded picture size (texture may be padded larger).
    void draw_amf(ID3D11Texture2D *tex, int imgW, int imgH, int bbW, int bbH)
    {
        if (!tex || imgW <= 0 || imgH <= 0) return;
        D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
        ComPtr<ID3D11ShaderResourceView> srvY, srvUV;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R16_UNORM;
        if (FAILED(dev_->CreateShaderResourceView(tex, &sd, &srvY))) return;
        sd.Format = DXGI_FORMAT_R16G16_UNORM;
        if (FAILED(dev_->CreateShaderResourceView(tex, &sd, &srvUV))) return;
        set_dv((float)imgW / (float)d.Width, (float)imgH / (float)d.Height);
        const float scale = std::min((float)bbW / imgW, (float)bbH / imgH);
        const float vw = imgW * scale, vh = imgH * scale;
        D3D11_VIEWPORT vp{(bbW - vw) / 2, (bbH - vh) / 2, vw, vh, 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(psHw_.Get(), nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, cbUv_.GetAddressOf());
        ID3D11ShaderResourceView *srvs[2] = {srvY.Get(), srvUV.Get()};
        ctx_->PSSetShaderResources(0, 2, srvs);
        ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView *n2[2] = {nullptr, nullptr};
        ctx_->PSSetShaderResources(0, 2, n2);
        valid_ = true;
    }

private:
    void ensure(int w, int h)
    {
        if (w == w_ && h == h_ && texY_) return;
        w_ = w; h_ = h;
        auto mk = [&](int pw, int ph, ComPtr<ID3D11Texture2D> &t, ComPtr<ID3D11ShaderResourceView> &s) {
            t.Reset(); s.Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = pw; d.Height = ph; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R16_UNORM; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            dev_->CreateTexture2D(&d, nullptr, &t);
            dev_->CreateShaderResourceView(t.Get(), nullptr, &s);
        };
        mk(w, h, texY_, srvY_);
        mk((w + 1) / 2, (h + 1) / 2, texU_, srvU_);
        mk((w + 1) / 2, (h + 1) / 2, texV_, srvV_);
    }
    ID3D11Device *dev_ = nullptr; ID3D11DeviceContext *ctx_ = nullptr;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_, psHw_;
    ComPtr<ID3D11SamplerState> samp_;
    ComPtr<ID3D11Buffer> cbUv_;
    ComPtr<ID3D11Texture2D> texY_, texU_, texV_;
    ComPtr<ID3D11ShaderResourceView> srvY_, srvU_, srvV_;
    AVFrame *hwFrame_ = nullptr, *hwFramePrev_ = nullptr;   // owned D3D11 frames
    ID3D11Texture2D *cachedTex_ = nullptr;                  // pool identity only
    std::vector<std::pair<ComPtr<ID3D11ShaderResourceView>, ComPtr<ID3D11ShaderResourceView>>> sliceSrv_;
    int texW_ = 1, texH_ = 1;
    int w_ = 0, h_ = 0; bool valid_ = false, hwMode_ = false, dstHDR_ = false;
};

// ===================== CTMS stream transport (loopback now, TV later) ======
// Host side: listens on CTMS_PORT, one client at a time. Caches STREAM_INFO
// and the cursor shape so a (re)connecting client always gets them first.
class StreamSender {
public:
    bool start(uint16_t port, std::wstring *err)
    {
        listenSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock_ == INVALID_SOCKET) { if (err) *err = L"socket() failed"; return false; }
        BOOL yes = TRUE;
        setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listenSock_, (sockaddr *)&a, sizeof(a)) != 0 || listen(listenSock_, 1) != 0) {
            if (err) *err = L"bind/listen failed"; return false;
        }
        acceptThread_ = std::thread(&StreamSender::accept_loop, this);
        return true;
    }

    void stop()
    {
        exit_.store(true);
        if (listenSock_ != INVALID_SOCKET) { closesocket(listenSock_); listenSock_ = INVALID_SOCKET; }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto &cl : clients_) {
                cl->dead.store(true);
                cl->qcv.notify_all();
                closesocket(cl->sock);   // unblock writer send + reader recv
            }
            for (auto &cl : clients_) {
                if (cl->writer.joinable()) cl->writer.join();
                if (cl->reader.joinable()) cl->reader.join();
            }
            clients_.clear();
        }
        if (acceptThread_.joinable()) acceptThread_.join();
    }
    ~StreamSender() { stop(); }

    // A new client must start on an IDR; the codec worker polls this.
    bool take_want_idr() { return wantIdr_.exchange(false); }

    // host clock in us since stream epoch (set via set_epoch); PONG uses this
    void set_epoch(double t0ms) { epochMs_ = t0ms; }
    uint64_t host_us() { return (uint64_t)((now_ms() - epochMs_) * 1000.0); }

    void set_info(const CtmsStreamInfo &si)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        info_ = si; haveInfo_ = true;
        send_msg(CTMS_STREAM_INFO, 0, 0, 0, 0, &info_, sizeof(info_));
    }
    void set_cursor_shape(int w, int h, int hotX, int hotY, const uint8_t *rgba)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        shapeHdr_ = {(uint16_t)w, (uint16_t)h, (int16_t)hotX, (int16_t)hotY};
        shapePix_.assign(rgba, rgba + (size_t)w * h * 4);
        send_shape();
    }
    void send_cursor_pos(int x, int y, bool visible)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        CtmsCursorPos p{x, y, visible ? (uint8_t)1 : (uint8_t)0, {}};
        send_msg(CTMS_CURSOR_POS, 0, host_us(), 0, 0, &p, sizeof(p));
    }
    void send_video(const uint8_t *data, int size, int64_t t0Us, int64_t tEncUs, int64_t t1Us, bool idr)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        send_msg(CTMS_VIDEO_FRAME, idr ? CTMS_FLAG_IDR : 0, (uint64_t)t0Us, (uint64_t)tEncUs, (uint64_t)t1Us, data, size);
    }
    void send_audio(const uint8_t *pcm, int bytes, int64_t ptsUs)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        send_msg(CTMS_AUDIO_FRAME, 0, (uint64_t)ptsUs, 0, 0, pcm, bytes);
    }
    bool connected() { std::lock_guard<std::mutex> lk(mtx_); return !clients_.empty(); }

private:
    // One queue + writer thread per client: a slow link (TV on wifi) must
    // never block the codec worker -- enqueue is wait-free for the producer.
    // If a client falls more than kMaxQueue behind it is dropped.
    struct Client {
        SOCKET sock = INVALID_SOCKET;
        std::thread writer;
        std::thread reader;      // handles client->host msgs (PING; later input)
        std::mutex qm;
        std::condition_variable qcv;
        std::deque<std::vector<uint8_t>> q;
        size_t qBytes = 0;
        std::atomic<bool> dead{false};
    };
    static const size_t kSoftQueue = 4u * 1024u * 1024u;   // drop backlog past this (bounds latency)
    static const size_t kMaxQueue = 32u * 1024u * 1024u;   // hard cap: drop the client

    // Multiple simultaneous clients (local preview + TV). Each new client gets
    // the cached STREAM_INFO + cursor shape, and triggers an IDR so it can
    // start decoding immediately instead of waiting out the GOP.
    void accept_loop()
    {
        while (!exit_.load()) {
            SOCKET c = accept(listenSock_, nullptr, nullptr);
            if (c == INVALID_SOCKET) { if (exit_.load()) return; continue; }
            BOOL nd = TRUE;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof(nd));
            std::lock_guard<std::mutex> lk(mtx_);
            reap_dead();
            if (clients_.size() >= 4) { closesocket(c); continue; }
            auto cl = std::make_unique<Client>();
            cl->sock = c;
            cl->writer = std::thread(&StreamSender::writer_loop, this, cl.get());
            cl->reader = std::thread(&StreamSender::reader_loop, this, cl.get());
            if (haveInfo_) enqueue_one(cl.get(), CTMS_STREAM_INFO, 0, 0, 0, 0, &info_, sizeof(info_));
            if (!shapePix_.empty()) {
                std::vector<uint8_t> buf = shape_buf();
                enqueue_one(cl.get(), CTMS_CURSOR_SHAPE, 0, 0, 0, 0, buf.data(), (int)buf.size());
            }
            clients_.push_back(std::move(cl));
            wantIdr_.store(true);
        }
    }

    // client->host: currently just PING (reply PONG with the host clock so the
    // client can resolve our clock offset + RTT). Reused later for TV input.
    void reader_loop(Client *cl)
    {
        for (;;) {
            CtmsHdr h{};
            if (!recv_all(cl->sock, &h, sizeof(h)) || h.magic != CTMS_MAGIC) break;
            std::vector<uint8_t> payload(h.payloadLen);
            if (h.payloadLen && !recv_all(cl->sock, payload.data(), h.payloadLen)) break;
            if (h.type == CTMS_PING && h.payloadLen >= sizeof(CtmsPing)) {
                CtmsPing ping; memcpy(&ping, payload.data(), sizeof(ping));
                CtmsPong pong{ping.clientUs, host_us()};
                // Priority: push PONG to the FRONT so clock sync isn't skewed
                // by sitting behind queued video (that caused negative net).
                std::lock_guard<std::mutex> lk(cl->qm);
                enqueue_front_locked(cl, CTMS_PONG, 0, 0, 0, 0, &pong, sizeof(pong));
            }
        }
        cl->dead.store(true);
        cl->qcv.notify_all();
    }
    static bool recv_all(SOCKET s, void *p, int len)
    {
        char *b = static_cast<char *>(p);
        while (len > 0) { int n = recv(s, b, len, 0); if (n <= 0) return false; b += n; len -= n; }
        return true;
    }

    void writer_loop(Client *cl)
    {
        for (;;) {
            std::vector<uint8_t> msg;
            {
                std::unique_lock<std::mutex> lk(cl->qm);
                cl->qcv.wait(lk, [&] { return !cl->q.empty() || cl->dead.load() || exit_.load(); });
                if (cl->dead.load() || exit_.load()) break;
                msg = std::move(cl->q.front());
                cl->q.pop_front();
                cl->qBytes -= msg.size();
            }
            const char *b = reinterpret_cast<const char *>(msg.data());
            int len = (int)msg.size();
            while (len > 0) {
                int n = send(cl->sock, b, len, 0);
                if (n <= 0) { cl->dead.store(true); break; }
                b += n; len -= n;
            }
            if (cl->dead.load()) break;
        }
        // socket close is owned by reap_dead()/stop() (single close unblocks
        // both this writer and the reader's recv)
    }

    std::vector<uint8_t> shape_buf()   // mtx_ held
    {
        std::vector<uint8_t> buf(sizeof(CtmsCursorShape) + shapePix_.size());
        memcpy(buf.data(), &shapeHdr_, sizeof(shapeHdr_));
        memcpy(buf.data() + sizeof(shapeHdr_), shapePix_.data(), shapePix_.size());
        return buf;
    }
    void send_shape()   // mtx_ held
    {
        std::vector<uint8_t> buf = shape_buf();
        send_msg(CTMS_CURSOR_SHAPE, 0, 0, 0, 0, buf.data(), (int)buf.size());
    }
    void send_msg(uint16_t type, uint16_t flags, uint64_t pts, uint64_t tEnc, uint64_t t1, const void *payload, int len)   // mtx_ held
    {
        for (auto &cl : clients_) enqueue_one(cl.get(), type, flags, pts, tEnc, t1, payload, len);
        reap_dead();
    }
    void enqueue_one(Client *cl, uint16_t type, uint16_t flags, uint64_t pts, uint64_t tEnc, uint64_t t1, const void *payload, int len)
    {
        if (cl->dead.load()) return;
        std::lock_guard<std::mutex> lk(cl->qm);
        enqueue_locked(cl, type, flags, pts, tEnc, t1, payload, len);
    }
    void enqueue_locked(Client *cl, uint16_t type, uint16_t flags, uint64_t pts, uint64_t tEnc, uint64_t t1, const void *payload, int len)   // cl->qm held
    {
        CtmsHdr h{CTMS_MAGIC, type, flags, pts, tEnc, t1, (uint32_t)len};
        std::vector<uint8_t> msg(sizeof(h) + len);
        memcpy(msg.data(), &h, sizeof(h));
        if (len) memcpy(msg.data() + sizeof(h), payload, len);
        // Latest-wins on the wire: a client that can't keep up (e.g. intra at
        // 4K = 100+ Mbps over a slower link) has its BACKLOG dropped rather than
        // growing latency until the hard cap disconnects it. Dropping breaks the
        // GOP, so request a fresh IDR to give the client a clean resync point.
        bool dropped = false;
        while (!cl->q.empty() && cl->qBytes + msg.size() > kSoftQueue) {
            cl->qBytes -= cl->q.front().size();
            cl->q.pop_front();
            dropped = true;
        }
        if (dropped) wantIdr_.store(true);
        if (cl->qBytes + msg.size() > kMaxQueue) {   // single msg bigger than the hard cap: give up
            cl->dead.store(true);
            cl->qcv.notify_one();
            return;
        }
        cl->qBytes += msg.size();
        cl->q.push_back(std::move(msg));
        cl->qcv.notify_one();
    }
    void enqueue_front_locked(Client *cl, uint16_t type, uint16_t flags, uint64_t pts, uint64_t tEnc, uint64_t t1, const void *payload, int len)   // cl->qm held
    {
        CtmsHdr h{CTMS_MAGIC, type, flags, pts, tEnc, t1, (uint32_t)len};
        std::vector<uint8_t> msg(sizeof(h) + len);
        memcpy(msg.data(), &h, sizeof(h));
        if (len) memcpy(msg.data() + sizeof(h), payload, len);
        cl->qBytes += msg.size();
        cl->q.push_front(std::move(msg));   // ahead of queued video
        cl->qcv.notify_one();
    }
    void reap_dead()   // mtx_ held
    {
        for (size_t i = 0; i < clients_.size();) {
            if (clients_[i]->dead.load()) {
                clients_[i]->qcv.notify_all();
                if (clients_[i]->writer.joinable()) clients_[i]->writer.join();
                if (clients_[i]->reader.joinable()) {
                    closesocket(clients_[i]->sock);   // unblock the reader's recv
                    clients_[i]->reader.join();
                }
                clients_.erase(clients_.begin() + i);
            } else {
                ++i;
            }
        }
    }

    SOCKET listenSock_ = INVALID_SOCKET;
    std::vector<std::unique_ptr<Client>> clients_;
    std::thread acceptThread_;
    std::atomic<bool> exit_{false};
    std::atomic<bool> wantIdr_{false};
    std::mutex mtx_;
    double epochMs_ = 0;     // host stream epoch (now_ms at start)
    CtmsStreamInfo info_{}; bool haveInfo_ = false;
    CtmsCursorShape shapeHdr_{};
    std::vector<uint8_t> shapePix_;
};

// Receiver: exactly what the TV will run -- connect, parse CTMS, feed the
// decoder, track the remote cursor. Here it feeds window B over loopback so
// B shows the true protocol+decode path (minus only network ping).
class StreamReceiver {
public:
    // t0: sender stream epoch (now_ms at init) -- pts is us since t0, and we
    // share the clock (same machine), so lat = (now - t0) - pts/1000.
    void start(uint16_t port, AVBufferRef *hwDev, double t0)
    {
        exit_.store(false);                 // re-armable: the 'D' toggle restarts us
        hwDev_ = hwDev; t0_ = t0;
        thread_ = std::thread(&StreamReceiver::run, this, port);
    }
    void stop()
    {
        exit_.store(true);
        {
            std::lock_guard<std::mutex> lk(sockMtx_);
            if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
        }
        if (thread_.joinable()) thread_.join();
        amfdec_.close();                    // stop the decode drain thread + free the AMF decoder
        if (decodedLatest_) av_frame_free(&decodedLatest_);
    }
    ~StreamReceiver() { stop(); }

    AVFrame *take_decoded()
    {
        std::lock_guard<std::mutex> lk(decMtx_);
        AVFrame *f = decodedLatest_; decodedLatest_ = nullptr; return f;
    }
    // Copies out cursor state; returns true if the shape changed since last call.
    bool cursor_state(int *x, int *y, bool *visible, int *w, int *h, std::vector<uint8_t> *rgbaIfDirty)
    {
        std::lock_guard<std::mutex> lk(curMtx_);
        *x = curX_; *y = curY_; *visible = curVis_; *w = curW_; *h = curH_;
        if (!shapeDirty_) return false;
        shapeDirty_ = false;
        *rgbaIfDirty = curPix_;
        return true;
    }

    std::atomic<double> decMs{0}, latMs{0};
    std::atomic<int64_t> decFrames{0};
    std::atomic<uint64_t> decGpuDrops_{0};   // --profile: frames whose GPU sample was disjoint/timed-out
    std::atomic<bool> hwdec{false};
    // Per-window distributions (#6). decAcc = decode submit (CPU); decGpuAcc =
    // true GPU-inclusive decode, populated only under --profile; latAcc =
    // source-present -> decode-submit-returned (NOT glass-to-glass: excludes the
    // GPU decode, window-B present and TV scanout -- see update_stats label).
    StatAcc decAcc_, latAcc_, decGpuAcc_;

    // native AMF decoder (primary HW decode path); ffmpeg dec_ is the fallback
    AmfDecoder amfdec_;
    std::atomic<bool> useAmfDec_{false};
    std::mutex benchMtx_;                              // guards benchGop_
    std::vector<std::vector<uint8_t>> benchGop_;       // latest GOP for the 'B' decode bench
    int decW_ = 0, decH_ = 0;
    bool use_amf_dec() const { return useAmfDec_.load(); }
    AmfDecoder &amfdec() { return amfdec_; }
    int dec_w() const { return decW_; }
    int dec_h() const { return decH_; }

    // 'B' hotkey: serialized decode bench on the latest captured GOP, using a
    // SEPARATE decoder (no drain thread) so the live path is untouched. Prints
    // real decode latency (submit->exit), throughput, and the pipeline depth.
    void run_decode_bench()
    {
        std::vector<std::vector<uint8_t>> gop;
        { std::lock_guard<std::mutex> lk(benchMtx_); gop = benchGop_; }
        if (gop.size() < 2) { std::wcout << L"[bench] no GOP yet (need a keyframe + frames first)\n"; return; }
        ID3D11Device *dev = nullptr;
        if (hwDev_) { auto *dc = reinterpret_cast<AVHWDeviceContext *>(hwDev_->data);
                      dev = reinterpret_cast<AVD3D11VADeviceContext *>(dc->hwctx)->device; }
        const EncConfig::Codec codec = info_.codec == 2 ? EncConfig::AV1 : EncConfig::HEVC;
        AmfDecoder bench; std::string e;
        if (!bench.open(codec, dev, decW_, decH_, &e, /*startDrain=*/false)) {
            std::wcout << L"[bench] decoder open failed: " << std::wstring(e.begin(), e.end()) << L"\n"; return;
        }
        std::wcout << L"[bench] serialized decode: " << gop.size() << L"-frame GOP, target 300 outputs...\n";
        DecodeBench r{};
        if (!bench.decode_bench(gop, 300, &r, &e)) {
            std::wcout << L"[bench] failed: " << std::wstring(e.begin(), e.end()) << L"\n"; bench.close(); return;
        }
        std::wcout << L"[bench] LATENCY submit->exit: avg " << r.latAvgMs << L" p50 " << r.latP50Ms
                   << L" p99 " << r.latP99Ms << L" max " << r.latMaxMs << L" ms (n=" << r.frames << L")\n"
                   << L"[bench] THROUGHPUT: " << r.throughputFps << L" fps ("
                   << (r.throughputFps > 0 ? 1000.0 / r.throughputFps : 0.0) << L" ms/frame)\n"
                   << L"[bench] pipeline depth: " << r.pipelineDepth << L" frame(s) before first output -> "
                   << (r.pipelineDepth <= 1 ? L"no hold" : L"decoder HOLDS frames") << L"\n";
        bench.close();
    }

private:
    void run(uint16_t port)
    {
        while (!exit_.load()) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a{};
            a.sin_family = AF_INET; a.sin_port = htons(port);
            inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
            if (connect(s, (sockaddr *)&a, sizeof(a)) != 0) {
                closesocket(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            BOOL nd = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof(nd));
            { std::lock_guard<std::mutex> lk(sockMtx_); sock_ = s; }
            read_loop(s);
            { std::lock_guard<std::mutex> lk(sockMtx_); if (sock_ == s) sock_ = INVALID_SOCKET; }
            closesocket(s);
            dec_.close();
        }
    }

    void read_loop(SOCKET s)
    {
        std::vector<uint8_t> payload;
        while (!exit_.load()) {
            CtmsHdr h{};
            if (!recv_all(s, &h, sizeof(h)) || h.magic != CTMS_MAGIC) return;
            payload.resize(h.payloadLen);
            if (h.payloadLen && !recv_all(s, payload.data(), h.payloadLen)) return;
            switch (h.type) {
            case CTMS_STREAM_INFO:
                if (h.payloadLen >= sizeof(CtmsStreamInfo)) {
                    memcpy(&info_, payload.data(), sizeof(info_));
                    const EncConfig::Codec codec = info_.codec == 2 ? EncConfig::AV1 : EncConfig::HEVC;
                    decW_ = info_.width; decH_ = info_.height;
                    std::string e;
                    // HW decode = native AMF (own decoder, real decode-time, low-latency
                    // DPB, controlled pool). ffmpeg D3D11VA/sw is the fallback.
                    ID3D11Device *dev = nullptr;
                    if (hwDev_) {
                        auto *dc = reinterpret_cast<AVHWDeviceContext *>(hwDev_->data);
                        dev = reinterpret_cast<AVD3D11VADeviceContext *>(dc->hwctx)->device;
                    }
                    // startDrain=false: we drive decode SYNCHRONOUSLY, one frame at a
                    // time (submit -> wait for it to exit) so d1-d0 is the real decode.
                    useAmfDec_.store(amfdec_.open(codec, dev, decW_, decH_, &e, /*startDrain=*/false));
                    if (useAmfDec_.load()) {
                        hwdec.store(true);
                        std::wcout << L"[rx] decoder: native AMF zero-copy (synchronous, one frame at a time)\n";
                    } else {
                        std::wcout << L"[rx] AMF decoder unavailable (" << std::wstring(e.begin(), e.end())
                                   << L"); ffmpeg fallback\n";
                        dec_.open(codec, hwDev_, &e);
                        hwdec.store(dec_.is_hw());
                    }
                }
                break;
            case CTMS_VIDEO_FRAME: {
                if (useAmfDec_.load()) {       // synchronous: submit + wait for this frame to exit
                    double decMsV = 0.0; int64_t outPts = -1;
                    const int got = amfdec_.decode_sync(payload.data(), (int)h.payloadLen, (int64_t)h.pts, &decMsV, &outPts, 12);
                    if (got > 0) {
                        decFrames.fetch_add(got);                              // real decoded-frame count
                        if (decMsV > 0) { decMs.store(decMsV); decAcc_.add(decMsV); }   // d1 - d0
                        if (outPts >= 0) {                                     // src -> decode-exit
                            const double lat = (now_ms() - t0_) - (double)outPts / 1000.0;
                            latMs.store(lat); latAcc_.add(lat);
                        }
                    }
                    // Keep the latest GOP (from a keyframe) for the 'B' decode bench.
                    {
                        std::lock_guard<std::mutex> lk(benchMtx_);
                        if (h.flags & CTMS_FLAG_IDR) benchGop_.clear();
                        if ((!benchGop_.empty() || (h.flags & CTMS_FLAG_IDR)) && benchGop_.size() < 240)
                            benchGop_.emplace_back(payload.begin(), payload.begin() + h.payloadLen);
                    }
                    break;
                }
                AVPacket *p = av_packet_alloc();
                if (p && av_new_packet(p, (int)h.payloadLen) == 0) {
                    memcpy(p->data, payload.data(), h.payloadLen);
                    p->pts = p->dts = (int64_t)h.pts;
                    const double t0 = now_ms();
                    // HW decode is async: avcodec_receive_frame returning a D3D11
                    // surface does NOT mean the GPU finished -- so dec-submit is the
                    // CPU submit cost only, measured up to tSubmitDone (before any
                    // GPU wait), which keeps it and lat comparable across --profile
                    // on/off. Under --profile, decode_timed_gpu also brackets the
                    // decode with GPU timestamp queries for the true GPU decode (Fd).
                    double gpuMs = -1.0, tSubmitDone = 0.0;
                    AVFrame *f;
                    if (g_profile && dec_.is_hw()) {
                        f = decode_timed_gpu(p, &gpuMs, &tSubmitDone);
                    } else {
                        f = dec_.decode(p);
                        tSubmitDone = now_ms();
                    }
                    if (f) {
                        if (AVFrame *cl = av_frame_clone(f)) {
                            std::lock_guard<std::mutex> lk(decMtx_);
                            if (decodedLatest_) av_frame_free(&decodedLatest_);
                            decodedLatest_ = cl;
                        }
                        if (f->pts >= 0) {
                            const double lat = (tSubmitDone - t0_) - (double)f->pts / 1000.0;
                            latMs.store(lat);
                            latAcc_.add(lat);
                        }
                        decFrames.fetch_add(1);
                        if (gpuMs >= 0.0) decGpuAcc_.add(gpuMs);          // produced frame + valid GPU sample
                        else if (g_profile && dec_.is_hw()) decGpuDrops_.fetch_add(1);   // disjoint/timeout
                    }
                    const double dms = tSubmitDone - t0;   // CPU submit only
                    decMs.store(dms);
                    decAcc_.add(dms);
                    if (dec_.hw_failed()) {
                        std::string e;
                        dec_.open_sw(&e);
                        hwdec.store(false);
                        std::wcout << L"[rx] hw decode unavailable, software fallback\n";
                    }
                }
                av_packet_free(&p);
                break;
            }
            case CTMS_CURSOR_POS:
                if (h.payloadLen >= sizeof(CtmsCursorPos)) {
                    CtmsCursorPos p;
                    memcpy(&p, payload.data(), sizeof(p));
                    std::lock_guard<std::mutex> lk(curMtx_);
                    curX_ = p.x; curY_ = p.y; curVis_ = p.visible != 0;
                }
                break;
            case CTMS_CURSOR_SHAPE:
                if (h.payloadLen >= sizeof(CtmsCursorShape)) {
                    CtmsCursorShape sh;
                    memcpy(&sh, payload.data(), sizeof(sh));
                    const size_t need = (size_t)sh.width * sh.height * 4;
                    if (h.payloadLen >= sizeof(sh) + need) {
                        std::lock_guard<std::mutex> lk(curMtx_);
                        curW_ = sh.width; curH_ = sh.height;
                        curPix_.assign(payload.begin() + sizeof(sh), payload.begin() + sizeof(sh) + need);
                        shapeDirty_ = true;
                    }
                }
                break;
            }
        }
    }

    static bool recv_all(SOCKET s, void *p, int len)
    {
        char *b = static_cast<char *>(p);
        while (len > 0) {
            int n = recv(s, b, len, 0);
            if (n <= 0) return false;
            b += n; len -= n;
        }
        return true;
    }

    // Lazily create timestamp queries on the decoder's own D3D11 device (the one
    // wrapped in hwDev_). device_context is the immediate context ffmpeg uses for
    // decode submission, so a timestamp pair on it brackets the decode's GPU work.
    bool ensure_gpu_queries()
    {
        if (qInit_) return qCtx_ != nullptr;
        qInit_ = true;
        if (!hwDev_) return false;
        auto *dc = reinterpret_cast<AVHWDeviceContext *>(hwDev_->data);
        auto *d3d = reinterpret_cast<AVD3D11VADeviceContext *>(dc->hwctx);
        if (!d3d->device || !d3d->device_context) return false;
        qCtx_ = d3d->device_context;            // ComPtr AddRefs; hwDev_ owns the original
        if (FAILED(qCtx_.As(&qMt_))) { qCtx_.Reset(); return false; }
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(d3d->device->CreateQuery(&qd, &qDisjoint_))) { qCtx_.Reset(); return false; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(d3d->device->CreateQuery(&qd, &qT0_)) ||
            FAILED(d3d->device->CreateQuery(&qd, &qT1_))) { qCtx_.Reset(); return false; }
        return true;
    }

    // Decode with a GPU timestamp bracket held under the D3D11 multithread lock,
    // so only the decode's GPU work lands between the two timestamps (the render
    // thread's commands can't interleave). --profile path only; it adds a sync the
    // normal async path never pays. *submitDoneMs is stamped the instant the
    // CPU decode call returns, BEFORE the GPU wait -- so the caller's dec-submit
    // and lat stay CPU-only and comparable across --profile on/off. *gpuMs is the
    // isolated GPU decode (Fd), -1 if unavailable (disjoint / timeout / sw).
    AVFrame *decode_timed_gpu(AVPacket *p, double *gpuMs, double *submitDoneMs)
    {
        *gpuMs = -1.0;
        if (!ensure_gpu_queries()) { AVFrame *f = dec_.decode(p); *submitDoneMs = now_ms(); return f; }
        qMt_->Enter();
        qCtx_->Begin(qDisjoint_.Get());
        qCtx_->End(qT0_.Get());
        AVFrame *f = dec_.decode(p);            // submits the decode GPU work
        *submitDoneMs = now_ms();               // CPU submit ends here (pre GPU wait)
        qCtx_->End(qT1_.Get());
        qCtx_->End(qDisjoint_.Get());
        qMt_->Leave();
        // Bounded wait for GPU completion. gpuMs is read from the GPU timestamps
        // below, NOT from this poll, so a coarse poll cadence doesn't blur it --
        // sleep between polls to spare the device multithread lock the render
        // thread shares (each GetData re-takes it).
        const double dl = now_ms() + 50.0;
        while (qCtx_->GetData(qDisjoint_.Get(), nullptr, 0, 0) == S_FALSE && now_ms() < dl)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0;
        if (qCtx_->GetData(qDisjoint_.Get(), &dj, sizeof(dj), 0) == S_OK && !dj.Disjoint && dj.Frequency &&
            qCtx_->GetData(qT0_.Get(), &a, sizeof(a), 0) == S_OK &&
            qCtx_->GetData(qT1_.Get(), &b, sizeof(b), 0) == S_OK && b > a)
            *gpuMs = (double)(b - a) * 1000.0 / (double)dj.Frequency;
        return f;
    }

    std::thread thread_;
    std::atomic<bool> exit_{false};
    std::mutex sockMtx_;
    SOCKET sock_ = INVALID_SOCKET;
    AVBufferRef *hwDev_ = nullptr;
    double t0_ = 0;
    Decoder dec_;
    // GPU decode-timing state (--profile); created lazily on the first HW decode.
    bool qInit_ = false;
    ComPtr<ID3D11DeviceContext> qCtx_;
    ComPtr<ID3D11Multithread> qMt_;
    ComPtr<ID3D11Query> qDisjoint_, qT0_, qT1_;
    CtmsStreamInfo info_{};
    std::mutex decMtx_;
    AVFrame *decodedLatest_ = nullptr;
    std::mutex curMtx_;
    int curX_ = 0, curY_ = 0, curW_ = 0, curH_ = 0;
    bool curVis_ = false, shapeDirty_ = false;
    std::vector<uint8_t> curPix_;
};

// ---- WASAPI loopback -> Opus -> CTMS audio --------------------------------
// Captures what the PC is playing (default render endpoint, loopback), encodes
// Opus (48k stereo, low-delay), and sends CTMS_AUDIO_FRAME stamped with host
// time. Forces a steady ~20ms cadence (zero-fill on silence) so the TV's
// NDL A/V pipeline never starves on audio.
class AudioLoopback {
public:
    bool start(StreamSender *sender, std::wstring *err)
    {
        sender_ = sender;
        const AVCodec *c = avcodec_find_encoder_by_name("libopus");
        if (!c) { if (err) *err = L"libopus encoder not found"; return false; }
        enc_ = avcodec_alloc_context3(c);
        enc_->sample_rate = 48000;
        av_channel_layout_default(&enc_->ch_layout, 2);
        enc_->sample_fmt = AV_SAMPLE_FMT_S16;
        enc_->bit_rate = 160000;
        enc_->time_base = av_make_q(1, 48000);
        av_opt_set(enc_->priv_data, "application", "lowdelay", 0);
        av_opt_set(enc_->priv_data, "frame_duration", "20", 0);
        if (avcodec_open2(enc_, c, nullptr) < 0) { if (err) *err = L"opus open failed"; avcodec_free_context(&enc_); return false; }
        frameSamples_ = enc_->frame_size > 0 ? enc_->frame_size : 960;   // per channel
        frame_ = av_frame_alloc();
        frame_->format = AV_SAMPLE_FMT_S16; frame_->sample_rate = 48000; frame_->nb_samples = frameSamples_;
        av_channel_layout_default(&frame_->ch_layout, 2);
        av_frame_get_buffer(frame_, 0);
        pkt_ = av_packet_alloc();
        run_.store(true);
        thread_ = std::thread(&AudioLoopback::run, this);
        return true;
    }
    void stop()
    {
        run_.store(false);
        if (thread_.joinable()) thread_.join();
        if (pkt_) av_packet_free(&pkt_);
        if (frame_) av_frame_free(&frame_);
        if (enc_) avcodec_free_context(&enc_);
    }
    ~AudioLoopback() { stop(); }
    int rate() const { return 48000; }
    int channels() const { return 2; }
    bool active() const { return ok_.load(); }

private:
    void run()
    {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        if (!setup_wasapi()) { CoUninitialize(); return; }
        ok_.store(true);
        client_->Start();
        const int chOut = 2;
        std::vector<int16_t> acc;             // interleaved stereo S16
        double lastEmitMs = now_ms();
        const double frameMs = 1000.0 * frameSamples_ / 48000.0;
        while (run_.load()) {
            // drain available packets
            UINT32 packet = 0;
            while (capture_->GetNextPacketSize(&packet) == S_OK && packet > 0) {
                BYTE *data = nullptr; UINT32 nFrames = 0; DWORD flags = 0;
                if (capture_->GetBuffer(&data, &nFrames, &flags, nullptr, nullptr) != S_OK) break;
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                append_stereo_s16(acc, silent ? nullptr : data, nFrames);
                capture_->ReleaseBuffer(nFrames);
            }
            // emit full frames
            while ((int)acc.size() >= frameSamples_ * chOut) {
                encode_send(acc.data());
                acc.erase(acc.begin(), acc.begin() + frameSamples_ * chOut);
                lastEmitMs = now_ms();
            }
            // keep cadence during silence so NDL audio doesn't underrun
            if (now_ms() - lastEmitMs >= frameMs) {
                std::vector<int16_t> sil((size_t)frameSamples_ * chOut, 0);
                encode_send(sil.data());
                lastEmitMs = now_ms();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        client_->Stop();
        if (mix_) { CoTaskMemFree(mix_); mix_ = nullptr; }
        CoUninitialize();
    }

    bool setup_wasapi()
    {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void **)&enum_))) return false;
        if (FAILED(enum_->GetDefaultAudioEndpoint(eRender, eConsole, &dev_))) return false;
        if (FAILED(dev_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client_))) return false;
        if (FAILED(client_->GetMixFormat(&mix_))) return false;
        // shared loopback uses the device mix format; we convert to 48k stereo S16
        if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                       1000000 /*100ms*/, 0, mix_, nullptr))) return false;
        if (FAILED(client_->GetService(__uuidof(IAudioCaptureClient), (void **)&capture_))) return false;
        return true;
    }

    // Convert the device mix buffer to interleaved stereo S16 at 48k. Assumes
    // the mix is 48k (typical); float32 or 16-bit input, >=2 channels.
    void append_stereo_s16(std::vector<int16_t> &acc, const BYTE *data, UINT32 nFrames)
    {
        const int ch = mix_->nChannels;
        // float subtype GUID Data1 == 3 (IEEE_FLOAT), == 1 (PCM); compare Data1
        // to avoid pulling in the ksmedia GUID symbol.
        bool isFloat = mix_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (mix_->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            isFloat = ((const WAVEFORMATEXTENSIBLE *)mix_)->SubFormat.Data1 == 3;
        for (UINT32 i = 0; i < nFrames; ++i) {
            int16_t l = 0, r = 0;
            if (data) {
                if (isFloat) {
                    const float *f = (const float *)(data + (size_t)i * ch * 4);
                    auto cvt = [](float v) { v = v < -1 ? -1 : v > 1 ? 1 : v; return (int16_t)(v * 32767.0f); };
                    l = cvt(f[0]); r = cvt(f[ch > 1 ? 1 : 0]);
                } else {
                    const int16_t *s = (const int16_t *)(data + (size_t)i * ch * 2);
                    l = s[0]; r = s[ch > 1 ? 1 : 0];
                }
            }
            acc.push_back(l); acc.push_back(r);
        }
    }

    void encode_send(const int16_t *interleavedStereo)
    {
        if (av_frame_make_writable(frame_) < 0) return;
        memcpy(frame_->data[0], interleavedStereo, (size_t)frameSamples_ * 2 * sizeof(int16_t));
        frame_->pts = framePts_; framePts_ += frameSamples_;
        if (avcodec_send_frame(enc_, frame_) < 0) return;
        while (avcodec_receive_packet(enc_, pkt_) == 0) {
            sender_->send_audio(pkt_->data, pkt_->size, (int64_t)sender_->host_us());
            av_packet_unref(pkt_);
        }
    }

    StreamSender *sender_ = nullptr;
    ComPtr<IMMDeviceEnumerator> enum_;
    ComPtr<IMMDevice> dev_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    WAVEFORMATEX *mix_ = nullptr;
    AVCodecContext *enc_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *pkt_ = nullptr;
    int frameSamples_ = 960;
    int64_t framePts_ = 0;
    std::thread thread_;
    std::atomic<bool> run_{false}, ok_{false};
};

// per-window resize state (real size arrives via WM_SIZE on ShowWindow; the
// swapchain is first created at the actual client size, so start un-dirty)
struct WinState {
    std::atomic<int> w{1280}, h{720};
    std::atomic<bool> resized{false};
};

static void apply_hotkey(WPARAM key)
{
    if (key == 'V') { g_vsync.store(!g_vsync.load()); return; }
    if (key == 'D') { g_noDecode.store(!g_noDecode.load()); return; }   // hide window B / stop local decode
    if (key == 'B') { g_decodeBench.store(true); return; }              // run serialized decode bench once
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    EncConfig &c = g_cfgDesired;
    switch (key) {
    case 'C': c.codec = (c.codec == EncConfig::HEVC) ? EncConfig::AV1 : EncConfig::HEVC; break;
    case 'M': c.mode  = (EncConfig::Mode)(((int)c.mode + 1) % 3);
              if (c.mode == EncConfig::CBR) c.maxrateKbps = 99000; break;   // CBR: locked 99 Mbps peak
    case 'R': c.resIndex = (c.resIndex + 1) % (int)(sizeof(kResHeights) / sizeof(int)); break;
    case 'U': c.usage = (EncConfig::Usage)(((int)c.usage + 1) % 3); break;
    case 'I': c.allIntra = !c.allIntra; break;   // inter (P-frames) <-> all-intra
    case 'F': c.intraRefresh = !c.intraRefresh; break;                       // rolling intra-refresh (no IDR spike)
    case 'S': c.slices = (c.slices >= 8) ? 1 : c.slices * 2; break;          // slices/tiles: 1,2,4,8
    case 'A': c.maxFrameKbits = (c.maxFrameKbits == 0) ? 1000               // per-frame cap kbits: off,1000,2000,4000
            : (c.maxFrameKbits >= 4000) ? 0 : c.maxFrameKbits * 2; break;
    case VK_UP:
        if (c.mode == EncConfig::CQP) c.qp = std::max(1, c.qp - 1);
        else c.bitrateKbps = std::min(200000, c.bitrateKbps + 5000);
        break;
    case VK_DOWN:
        if (c.mode == EncConfig::CQP) c.qp = std::min(51, c.qp + 1);
        else c.bitrateKbps = std::max(2000, c.bitrateKbps - 5000);
        break;
    case VK_RIGHT: if (c.mode != EncConfig::CBR) c.maxrateKbps = std::min(300000, c.maxrateKbps + 5000); break;   // locked in CBR
    case VK_LEFT:  if (c.mode != EncConfig::CBR) c.maxrateKbps = std::max(c.bitrateKbps, c.maxrateKbps - 5000); break;
    default: return;
    }
    g_cfgDirty.store(true);
}

static LRESULT CALLBACK DualWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WinState *ws = reinterpret_cast<WinState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_SIZE:
        if (ws) { ws->w.store(LOWORD(lp) ? LOWORD(lp) : 1); ws->h.store(HIWORD(lp) ? HIWORD(lp) : 1); ws->resized.store(true); }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { g_running.store(false); PostQuitMessage(0); return 0; }
        apply_hotkey(wp);
        return 0;
    case WM_DESTROY:
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

class DualPipeline {
public:
    bool init(const OutputRef &ref, HWND wndA, HWND wndB, WinState *wsA, WinState *wsB, std::wstring *err)
    {
        ref_ = ref; wndA_ = wndA; wndB_ = wndB; wsA_ = wsA; wsB_ = wsB;
        hdr_ = read_hdr_info(ref);
        const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(ref.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, ARRAYSIZE(fls),
                                     D3D11_SDK_VERSION, &dev_, nullptr, &ctx_))) {
            if (err) *err = L"D3D11CreateDevice failed"; return false;
        }
        // Wrap our D3D11 device for libavcodec D3D11VA decode: decoded frames
        // are textures on THIS device, so the display pass samples them
        // directly (true zero-copy). FFmpeg only multithread-protects devices
        // it creates itself -- for a wrapped device we must do it, or the
        // decoder's video-context calls (worker thread) race the render
        // thread's context calls and corrupt the driver (0xC0000005).
        {
            ComPtr<ID3D11Multithread> mt;
            if (SUCCEEDED(ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);
        }
        hwDev_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (hwDev_) {
            auto *dc = reinterpret_cast<AVHWDeviceContext *>(hwDev_->data);
            auto *d3d = reinterpret_cast<AVD3D11VADeviceContext *>(dc->hwctx);
            d3d->device = dev_.Get();
            dev_->AddRef();                       // ctx takes ownership of one ref
            if (av_hwdevice_ctx_init(hwDev_) < 0) av_buffer_unref(&hwDev_);
        }
        if (FAILED(ref.adapter->GetParent(IID_PPV_ARGS(&fac_)))) { if (err) *err = L"no IDXGIFactory2"; return false; }
        {
            // uncapped windowed presents (otherwise Present(0) blocks once the
            // DWM present queue fills, hard-locking the loop to the refresh rate)
            ComPtr<IDXGIFactory5> fac5;
            BOOL allow = FALSE;
            if (SUCCEEDED(fac_.As(&fac5)) &&
                SUCCEEDED(fac5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
                tearing_ = allow != 0;
        }
        if (!make_swapchain(wndA, scA_, rtvA_, &bbAW_, &bbAH_, err)) return false;
        if (!make_swapchain(wndB, scB_, rtvB_, &bbBW_, &bbBH_, err)) return false;
        decoded_.set_dst_hdr(dstHDR_);
        std::wcout << L"preview windows: " << (dstHDR_ ? L"HDR scRGB" : L"SDR (sRGB, HDR tonemapped)") << L"\n";

        auto vsb = compile_shader(kVS, "main", "vs_5_0");
        auto psb = compile_shader(kPSSceneDisp, "main", "ps_5_0");
        if (!vsb || !psb) { if (err) *err = L"scene shader compile failed"; return false; }
        dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vsScene_);
        dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &psScene_);
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&s, &samp_);
        D3D11_BUFFER_DESC cb{};
        cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; cb.ByteWidth = 16;
        dev_->CreateBuffer(&cb, nullptr, &cbScene_);

        // cursor compositing (reuses the single-preview quad + cursor shaders)
        auto vqb = compile_shader(kVSQuad, "qmain", "vs_5_0");
        auto pqb = compile_shader(kPSCursor, "cmain", "ps_5_0");
        if (!vqb || !pqb) { if (err) *err = L"cursor shader compile failed"; return false; }
        dev_->CreateVertexShader(vqb->GetBufferPointer(), vqb->GetBufferSize(), nullptr, &vsCursor_);
        dev_->CreatePixelShader(pqb->GetBufferPointer(), pqb->GetBufferSize(), nullptr, &psCursor_);
        cb.ByteWidth = 32;
        dev_->CreateBuffer(&cb, nullptr, &cbCursor_);
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev_->CreateBlendState(&bd, &blendOver_);

        if (!conv_.init(dev_.Get(), ctx_.Get(), err)) return false;
        if (!decoded_.init(dev_.Get(), ctx_.Get(), err)) return false;
        if (!reinit_dup(err)) return false;

        capW_ = ref.desc.DesktopCoordinates.right - ref.desc.DesktopCoordinates.left;
        capH_ = ref.desc.DesktopCoordinates.bottom - ref.desc.DesktopCoordinates.top;
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        t0_ = now_ms();   // stream epoch: pts = us since here
        sender_.set_epoch(t0_);
        if (!sender_.start(CTMS_PORT, err)) return false;
        if (!g_noPreview && !g_noDecode.load()) { rx_.start(CTMS_PORT, hwDev_, t0_); decodeOn_ = true; }
        std::wcout << L"CTMS stream on port " << CTMS_PORT
                   << (g_noPreview ? L" (no preview)\n"
                                   : (g_noDecode.load() ? L" (decode off -- window B hidden)\n"
                                                        : L" (local receiver attached)\n"));
        { std::wstring ae; audioOn_ = audio_.start(&sender_, &ae);
          std::wcout << (audioOn_ ? L"audio: WASAPI loopback -> Opus 48k stereo\n"
                                  : L"audio: disabled (" + ae + L")\n"); }

        { std::lock_guard<std::mutex> lk(g_cfgMtx); applied_ = g_cfgDesired; }
        if (!reconfigure(applied_, err)) return false;
        log_display_mode(ref_);
        std::wcout << L"HDR monitor info: valid=" << hdr_.valid << L" maxLum=" << hdr_.maxLum
                   << L" maxFALL=" << hdr_.maxFFLum << L" minLum=" << hdr_.minLum << L"\n";
        lastStat_ = now_ms();
        codecThread_ = std::thread(&DualPipeline::capture_encode_loop, this);
        return true;
    }

    ~DualPipeline()
    {
        { std::lock_guard<std::mutex> lk(jobMtx_); workerExit_ = true; }
        jobCv_.notify_all();
        if (codecThread_.joinable()) codecThread_.join();
        audio_.stop();
        rx_.stop();          // receiver owns the decoder; stop before hwDev unref
        sender_.stop();
        if (inFlightSlot_ >= 0) conv_.unmap(inFlightSlot_);
        if (jobFrame_) av_frame_free(&jobFrame_);   // zero-copy hwframe never consumed
        if (hwDev_) av_buffer_unref(&hwDev_);   // decoder/frames hold own refs
        WSACleanup();
    }

    // 'D' hotkey / --nodecode: start or stop the local decode path at runtime.
    // Stopping it closes the AMF decoder (drain thread + GPU) and hides window B,
    // so the host runs capture+encode only -- clean encode-side profiling.
    void reconcile_decode()
    {
        if (g_noPreview) return;
        const bool want = !g_noDecode.load();
        if (want == decodeOn_) return;
        if (want) { rx_.start(CTMS_PORT, hwDev_, t0_); ShowWindow(wndB_, SW_SHOW); }
        else      { rx_.stop(); lastShownGen_ = 0; ShowWindow(wndB_, SW_HIDE); }
        decodeOn_ = want;
    }

    bool render_once()
    {
        const double loopT0 = now_ms();
        reconcile_decode();
        if (g_decodeBench.exchange(false) && decodeOn_ && !benchRunning_.exchange(true))
            std::thread([this] { rx_.run_decode_bench(); benchRunning_.store(false); }).detach();
        { std::lock_guard<std::mutex> g(gpuMtx_); resize_backbuffers(); }
        const UINT sync = g_vsync.load() ? 1 : 0;
        const UINT pflags = (sync == 0 && tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0;

        // window A: latest captured f1, produced by the capture/encode thread.
        // gpuMtx_ serializes our ctx_ draws against the capture thread; f1Mtx_
        // (nested) guards the f1 buffer the capture thread may be republishing.
        if (!g_noPreview) {
            {
                std::lock_guard<std::mutex> g(gpuMtx_);
                {
                    std::lock_guard<std::mutex> lk(f1Mtx_);
                    if (f1Display_ >= 0 && f1SRV_[f1Display_])
                        draw_scene(rtvA_.Get(), bbAW_, bbAH_, f1SRV_[f1Display_].Get(), f1W_, f1H_);
                    else {
                        const float black[4] = {0, 0, 0, 1};
                        ctx_->ClearRenderTargetView(rtvA_.Get(), black);
                    }
                }
                draw_cursor_overlay(rtvA_.Get(), bbAW_, bbAH_);
            }
            scA_->Present(sync, pflags);
        }
        send_cursor_pos();   // CTMS: cursor position on change

        // 7. window B: newest decoded frame from the CTMS receiver (the same
        //    path the TV runs); cursor drawn from RECEIVED metadata, so B
        //    shows the cursor's true through-the-pipe latency. Skipped when
        //    decode is off ('D'/--nodecode): no decoder, window hidden.
        // Serialized to the decode output: draw + present window B ONCE per newly
        // decoded frame, so the window's update rate IS the decode rate (not the
        // ~600fps render loop). Makes the decode cadence + latency legible.
        if (decodeOn_) {
            bool present = false;
            const float black[4] = {0, 0, 0, 1};
            std::lock_guard<std::mutex> g(gpuMtx_);
            if (rx_.use_amf_dec()) {                     // native AMF decoded texture
                int64_t fpts = -1; uint64_t gen = 0;
                ID3D11Texture2D *tex = rx_.amfdec().lock_latest(&fpts, &gen);
                if (tex && gen != lastShownGen_) {       // NEW frame -> draw + present once
                    ctx_->ClearRenderTargetView(rtvB_.Get(), black);
                    ctx_->OMSetRenderTargets(1, rtvB_.GetAddressOf(), nullptr);
                    decoded_.draw_amf(tex, rx_.dec_w(), rx_.dec_h(), bbBW_, bbBH_);  // d2 = now
                    draw_rx_cursor_overlay(rtvB_.Get(), bbBW_, bbBH_);
                    lastShownGen_ = gen;
                    if (fpts >= 0) { g2gAcc_.add((now_ms() - t0_) - (double)fpts / 1000.0); dispFrames_++; }
                    present = true;
                }
                if (tex) rx_.amfdec().release_latest();
            } else {                                     // ffmpeg path
                AVFrame *take = rx_.take_decoded();
                if (take) {
                    if (take->format == AV_PIX_FMT_D3D11) decoded_.set_hw(take);
                    else { decoded_.upload(take); av_frame_free(&take); }
                    ctx_->ClearRenderTargetView(rtvB_.Get(), black);
                    ctx_->OMSetRenderTargets(1, rtvB_.GetAddressOf(), nullptr);
                    if (decoded_.valid()) decoded_.draw(bbBW_, bbBH_);
                    draw_rx_cursor_overlay(rtvB_.Get(), bbBW_, bbBH_);
                    dispFrames_++;
                    present = true;
                }
            }
            if (present) scB_->Present(sync, pflags);   // present only on a new decoded frame
        }

        const double dt = now_ms() - loopT0;
        statLoops_++;
        loopAcc_.add(dt);
        if (dt > worstLoop_) worstLoop_ = dt;
        update_stats();
        if (g_noPreview) std::this_thread::sleep_for(std::chrono::milliseconds(2));  // nothing to present
        return true;
    }

    // Capture+encode thread: OWNS the duplication. Blocking AcquireNextFrame
    // (event-driven, no busy poll) -> copy f1 -> convert (f2) -> encode (f3), all
    // on ONE thread, so t0->t3 (present -> packet-out) has no handoff and no
    // --fps gate. Publishes f1 (double-buffered) for the preview thread to draw;
    // never presents (presents stay on the render thread, off the t0->t3 path).
    void capture_encode_loop()
    {
        while (!workerExit_.load()) {
            // live reconfigure (this thread owns enc_/conv_)
            if (g_cfgDirty.load()) {
                g_cfgDirty.store(false);
                EncConfig want; { std::lock_guard<std::mutex> lk(g_cfgMtx); want = g_cfgDesired; }
                if (want.encoderDiffers(applied_)) { std::wstring e; reconfigure(want, &e); }
            }

            DXGI_OUTDUPL_FRAME_INFO fi{};
            ComPtr<IDXGIResource> res;
            HRESULT hr = dup_->AcquireNextFrame(100, &fi, &res);   // blocks until a frame
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;           // re-check exit/reconfig
            if (hr == DXGI_ERROR_ACCESS_LOST) { std::wstring e; reinit_dup(&e); continue; }
            if (FAILED(hr)) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }

            ComPtr<ID3D11Texture2D> tex;
            if (FAILED(res.As(&tex))) { dup_->ReleaseFrame(); continue; }
            D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);

            // (re)create the f1 double-buffer on size/format change
            if (!f1Tex_[0] || f1W_ != (int)td.Width || f1H_ != (int)td.Height || f1Fmt_ != td.Format) {
                { std::lock_guard<std::mutex> lk(f1Mtx_); f1Display_ = -1; }
                for (int i = 0; i < 2; ++i) {
                    f1Tex_[i].Reset(); f1SRV_[i].Reset();
                    D3D11_TEXTURE2D_DESC d = td;
                    d.Usage = D3D11_USAGE_DEFAULT;
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    d.CPUAccessFlags = 0; d.MiscFlags = 0;
                    if (SUCCEEDED(dev_->CreateTexture2D(&d, nullptr, &f1Tex_[i])))
                        dev_->CreateShaderResourceView(f1Tex_[i].Get(), nullptr, &f1SRV_[i]);
                }
                f1Write_ = 0;
                f1W_ = (int)td.Width; f1H_ = (int)td.Height; f1Fmt_ = td.Format;
                f1HDR_ = (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
                frameHDR_ = f1HDR_;   // draw_scene reads frameHDR_ for the source tonemap
                if (!fmtLogged_) {
                    fmtLogged_ = true;
                    std::wcout << L"captured surface: " << dxgi_format_name(td.Format)
                               << L" -> " << (f1HDR_ ? L"HDR/scRGB" : L"SDR/sRGB") << L"\n";
                }
            }
            const int w = f1Write_;
            if (!f1Tex_[w]) { dup_->ReleaseFrame(); continue; }

            {
                std::lock_guard<std::mutex> g(gpuMtx_);          // serialize ctx_ vs preview draws
                ctx_->CopyResource(f1Tex_[w].Get(), tex.Get());  // f1
                update_cursor(fi);                               // cursor metadata (sent via CTMS)
            }
            LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
            const bool havePresentTs = fi.LastPresentTime.QuadPart != 0;
            const double t0 = havePresentTs                     // t0 = present (frame ready)
                ? (double)fi.LastPresentTime.QuadPart * 1000.0 / qf.QuadPart : now_ms();
            freshFrames_++; if (!havePresentTs) presentFallback_++;
            dup_->ReleaseFrame();

            const int64_t pts = (int64_t)((t0 - t0_) * 1000.0);  // us since stream epoch

            // convert (f2) + encode (f3); encMs brackets the encode call only.
            std::vector<AVPacket *> pkts;
            wstage_.store(1);
            if (sender_.take_want_idr()) { if (useAmf_) amf_.force_idr(); else enc_.force_idr(); }
            const int64_t tEncUs = (int64_t)((now_ms() - t0_) * 1000.0);
            double encMs = 0; bool encoded = false;
            if (useAmf_) {   // native AMF: render P010 into encRt_, AMF wraps it directly
                bool cvOk;
                { std::lock_guard<std::mutex> g(gpuMtx_); cvOk = conv_.render_p010(f1SRV_[w].Get()); }
                if (cvOk) {
                    amfOut_.clear(); amfKeys_.clear();
                    const double te0 = now_ms();
                    amf_.encode(conv_.p010_texture(), pts, amfOut_, amfKeys_);
                    encMs = now_ms() - te0; encoded = !amfOut_.empty();
                }
            } else if (enc_.is_hw()) {
                if (AVFrame *hf = enc_.get_hwframe()) {
                    ID3D11Texture2D *htex = reinterpret_cast<ID3D11Texture2D *>(hf->data[0]);
                    const UINT hidx = (UINT)(intptr_t)hf->data[1];
                    bool cvOk;
                    { std::lock_guard<std::mutex> g(gpuMtx_); cvOk = conv_.submit_to_planes(f1SRV_[w].Get(), htex, hidx); }
                    if (cvOk) {
                        const double te0 = now_ms();
                        if (enc_.encode_hw(hf, pts, pkts) < 0) encErrs_.fetch_add(1);
                        encMs = now_ms() - te0; encoded = true;
                    } else { av_frame_free(&hf); }
                }
            } else {   // --cpucopy fallback: convert to staging, map, encode
                const uint8_t *dy = nullptr, *duv = nullptr; int py = 0, puv = 0; int mr = 0; bool cvOk = false;
                {
                    std::lock_guard<std::mutex> g(gpuMtx_);
                    cvOk = conv_.submit(f1SRV_[w].Get(), 0);
                    if (cvOk) { const double dl = now_ms() + 50.0; while ((mr = conv_.map(0, &dy, &py, &duv, &puv)) == 0 && now_ms() < dl) YieldProcessor(); }
                }
                if (cvOk && mr == 1) {
                    const double te0 = now_ms();
                    if (enc_.encode(dy, py, duv, puv, pts, pkts) < 0) encErrs_.fetch_add(1);
                    encMs = now_ms() - te0; encoded = true;
                    { std::lock_guard<std::mutex> g(gpuMtx_); conv_.unmap(0); }
                }
            }
            const double t3 = now_ms();
            const int64_t encUs = (int64_t)((t3 - t0_) * 1000.0);  // packet-out
            if (encoded) {
                encMsA_.store(encMs); encAcc_.add(encMs);
                const double produce = t3 - t0;                    // t0->t3, no handoff/cadence
                hostMsA_.store(produce); hostAcc_.add(produce);
                pts_++;
            }
            // publish f1 for the preview thread, then flip the write buffer
            { std::lock_guard<std::mutex> lk(f1Mtx_); f1Display_ = w; }
            f1Write_ = 1 - w;

            size_t bytes = 0;
            if (useAmf_) {                     // ship native-AMF packets
                for (size_t i = 0; i < amfOut_.size(); ++i) {
                    bytes += amfOut_[i].size();
                    sender_.send_video(amfOut_[i].data(), (int)amfOut_[i].size(), pts, tEncUs, encUs, amfKeys_[i]);
                    txFrames_.fetch_add(1);
                }
            } else {
                for (AVPacket *p : pkts) {     // ship ffmpeg packets
                    bytes += p->size;
                    if (p->pts != AV_NOPTS_VALUE && p->dts != AV_NOPTS_VALUE && p->pts != p->dts)
                        reorder_.fetch_add(1);
                    sender_.send_video(p->data, p->size, p->pts, tEncUs, encUs, (p->flags & AV_PKT_FLAG_KEY) != 0);
                    txFrames_.fetch_add(1);
                    av_packet_free(&p);
                }
            }
            winBytes_.fetch_add(bytes);
            wstage_.store(0);
        }
    }

private:
    bool make_swapchain(HWND hwnd, ComPtr<IDXGISwapChain3> &sc, ComPtr<ID3D11RenderTargetView> &rtv,
                        long *w, long *h, std::wstring *err)
    {
        RECT rc; GetClientRect(hwnd, &rc);
        *w = std::max(1L, rc.right); *h = std::max(1L, rc.bottom);
        auto create = [&](DXGI_FORMAT fmt) -> bool {
            sc.Reset();
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = *w; sd.Height = *h; sd.Format = fmt; sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            sd.Scaling = DXGI_SCALING_STRETCH;
            sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
            ComPtr<IDXGISwapChain1> sc1;
            if (FAILED(fac_->CreateSwapChainForHwnd(dev_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) return false;
            return SUCCEEDED(sc1.As(&sc));
        };
        // Follow the window's monitor: scRGB FP16 if it can present HDR, else an
        // 8-bit backbuffer with an _SRGB RTV (hardware sRGB-encodes our linear
        // output). dstHDR_ tells the shaders which to produce.
        if (!create(DXGI_FORMAT_R16G16B16A16_FLOAT)) { if (err) *err = L"CreateSwapChainForHwnd failed"; return false; }
        UINT sup = 0;
        const bool hdrOut = SUCCEEDED(sc->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &sup)) &&
                            (sup & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT);
        if (hdrOut) {
            sc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
            dstHDR_ = true; rtvFmt_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
        } else {
            if (!create(DXGI_FORMAT_B8G8R8A8_UNORM)) { if (err) *err = L"CreateSwapChainForHwnd failed"; return false; }
            dstHDR_ = false; rtvFmt_ = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        }
        make_rtv(sc, rtv);
        return true;
    }

    void make_rtv(ComPtr<IDXGISwapChain3> &sc, ComPtr<ID3D11RenderTargetView> &rtv)
    {
        ComPtr<ID3D11Texture2D> bb;
        sc->GetBuffer(0, IID_PPV_ARGS(&bb));
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = rtvFmt_; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        dev_->CreateRenderTargetView(bb.Get(), &rd, &rtv);
    }

    void resize_one(ComPtr<IDXGISwapChain3> &sc, ComPtr<ID3D11RenderTargetView> &rtv, WinState *ws, long *w, long *h)
    {
        if (!ws->resized.exchange(false)) return;
        rtv.Reset();
        *w = ws->w.load(); *h = ws->h.load();
        sc->ResizeBuffers(0, (UINT)*w, (UINT)*h, DXGI_FORMAT_UNKNOWN,
                          tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        make_rtv(sc, rtv);
    }
    void resize_backbuffers()
    {
        resize_one(scA_, rtvA_, wsA_, &bbAW_, &bbAH_);
        resize_one(scB_, rtvB_, wsB_, &bbBW_, &bbBH_);
    }

    void draw_scene(ID3D11RenderTargetView *rtv, long bbW, long bbH, ID3D11ShaderResourceView *srv, int fw, int fh)
    {
        const float black[4] = {0, 0, 0, 1};
        ctx_->ClearRenderTargetView(rtv, black);
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        if (!srv || fw <= 0) return;
        const float scale = std::min((float)bbW / fw, (float)bbH / fh);
        const float vw = fw * scale, vh = fh * scale;
        D3D11_VIEWPORT vp{(bbW - vw) / 2, (bbH - vh) / 2, vw, vh, 0, 1};
        ctx_->RSSetViewports(1, &vp);
        // {srcHDR, dstHDR}: decode by source, encode (tonemap) by display
        int cb[4] = {frameHDR_ ? 1 : 0, dstHDR_ ? 1 : 0, 0, 0};
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(cbScene_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, cb, sizeof(cb)); ctx_->Unmap(cbScene_.Get(), 0); }
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vsScene_.Get(), nullptr, 0);
        ctx_->PSSetShader(psScene_.Get(), nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, cbScene_.GetAddressOf());
        ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
        ctx_->PSSetShaderResources(0, 1, &srv);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView *n = nullptr;
        ctx_->PSSetShaderResources(0, 1, &n);
    }

    // Build/refresh the cursor texture from the DDA pointer shape (delivered
    // separately from the desktop image).
    void update_cursor(const DXGI_OUTDUPL_FRAME_INFO &fi)
    {
        if (fi.LastMouseUpdateTime.QuadPart != 0) {
            curVisible_ = fi.PointerPosition.Visible != 0;
            curPos_ = fi.PointerPosition.Position;
        }
        if (fi.PointerShapeBufferSize == 0) return;
        shapeBuf_.resize(fi.PointerShapeBufferSize);
        UINT req = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
        if (FAILED(dup_->GetFramePointerShape((UINT)shapeBuf_.size(), shapeBuf_.data(), &req, &info))) return;
        curHotX_ = (int)info.HotSpot.x;
        curHotY_ = (int)info.HotSpot.y;
        const int w = (int)info.Width;
        const bool mono = info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        const int h = mono ? (int)info.Height / 2 : (int)info.Height;
        const int pitch = (int)info.Pitch;
        std::vector<uint8_t> rgba((size_t)w * h * 4, 0);   // R,G,B,A
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint8_t *o = &rgba[((size_t)y * w + x) * 4];
            if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                const uint8_t *s = &shapeBuf_[(size_t)y * pitch + x * 4]; // BGRA
                o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3];
            } else if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                const uint8_t *s = &shapeBuf_[(size_t)y * pitch + x * 4];
                if (s[3] == 0) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = 255; }   // opaque
            } else if (mono) {
                const int by = x / 8, bit = 7 - (x % 8);
                const int a = (shapeBuf_[(size_t)y * pitch + by] >> bit) & 1;
                const int xo = (shapeBuf_[(size_t)(h + y) * pitch + by] >> bit) & 1;
                if (a == 0) { uint8_t v = xo ? 255 : 0; o[0] = o[1] = o[2] = v; o[3] = 255; }
                else if (xo) { o[0] = o[1] = o[2] = 255; o[3] = 255; }                  // invert -> white
            }
        }
        if (!curTex_ || curW_ != w || curH_ != h) {
            curTex_.Reset(); curSRV_.Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev_->CreateTexture2D(&d, nullptr, &curTex_))) return;
            dev_->CreateShaderResourceView(curTex_.Get(), nullptr, &curSRV_);
            curW_ = w; curH_ = h;
        }
        ctx_->UpdateSubresource(curTex_.Get(), 0, nullptr, rgba.data(), w * 4, 0);
        curValid_ = true;
        sender_.set_cursor_shape(w, h, curHotX_, curHotY_, rgba.data());   // CTMS
    }

    // CTMS: send the cursor position (capture-space top-left) when it changes.
    void send_cursor_pos()
    {
        if (!curValid_ || capW_ <= 0) return;
        POINT live;
        if (!GetCursorPos(&live)) return;
        const int cx = live.x - ref_.desc.DesktopCoordinates.left;
        const int cy = live.y - ref_.desc.DesktopCoordinates.top;
        const bool vis = curVisible_ && cx >= 0 && cy >= 0 && cx < capW_ && cy < capH_;
        const int x = cx - curHotX_, y = cy - curHotY_;
        if (x == lastSentX_ && y == lastSentY_ && vis == lastSentVis_) return;
        lastSentX_ = x; lastSentY_ = y; lastSentVis_ = vis;
        sender_.send_cursor_pos(x, y, vis);
    }

    // Window B's cursor: drawn from RECEIVED CTMS metadata (position + shape),
    // exactly as the TV will do it.
    void draw_rx_cursor_overlay(ID3D11RenderTargetView *rtv, long bbW, long bbH)
    {
        int x = 0, y = 0, w = 0, h = 0;
        bool vis = false;
        std::vector<uint8_t> rgba;
        if (rx_.cursor_state(&x, &y, &vis, &w, &h, &rgba) && w > 0) {   // shape changed
            if (!rxCurTex_ || rxCurW_ != w || rxCurH_ != h) {
                rxCurTex_.Reset(); rxCurSRV_.Reset();
                D3D11_TEXTURE2D_DESC d{};
                d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
                d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(dev_->CreateTexture2D(&d, nullptr, &rxCurTex_))) return;
                dev_->CreateShaderResourceView(rxCurTex_.Get(), nullptr, &rxCurSRV_);
                rxCurW_ = w; rxCurH_ = h;
            }
            ctx_->UpdateSubresource(rxCurTex_.Get(), 0, nullptr, rgba.data(), w * 4, 0);
        }
        if (!vis || !rxCurSRV_ || rxCurW_ <= 0 || capW_ <= 0) return;
        const float s = std::min((float)bbW / capW_, (float)bbH / capH_);
        const float vw = capW_ * s, vh = capH_ * s;
        const float vx = (bbW - vw) / 2, vy = (bbH - vh) / 2;
        const float x0 = vx + x * s, y0 = vy + y * s;
        const float x1 = x0 + rxCurW_ * s, y1 = y0 + rxCurH_ * s;
        const float q[8] = {
            x0 / bbW * 2 - 1, 1 - y0 / bbH * 2, x1 / bbW * 2 - 1, 1 - y1 / bbH * 2,
            dstHDR_ ? g_paperwhite_nits / 80.0f : 1.0f, 0.0f, 0, 0};
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(cbCursor_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, q, sizeof(q)); ctx_->Unmap(cbCursor_.Get(), 0); }
        D3D11_VIEWPORT vp{0, 0, (float)bbW, (float)bbH, 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx_->VSSetShader(vsCursor_.Get(), nullptr, 0);
        ctx_->VSSetConstantBuffers(0, 1, cbCursor_.GetAddressOf());
        ctx_->PSSetShader(psCursor_.Get(), nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, cbCursor_.GetAddressOf());
        ctx_->PSSetShaderResources(0, 1, rxCurSRV_.GetAddressOf());
        ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
        const float bf[4] = {0, 0, 0, 0};
        ctx_->OMSetBlendState(blendOver_.Get(), bf, 0xffffffff);
        ctx_->Draw(4, 0);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ID3D11ShaderResourceView *n = nullptr;
        ctx_->PSSetShaderResources(0, 1, &n);
    }

    // Draw the cursor as an OVERLAY on a window's swapchain (not baked into the
    // captured frame / stream). Position is the live GetCursorPos minus the DDA
    // shape hotspot, mapped through the same letterbox as the content -- so it's
    // crisp (window-res) and snappy (redrawn every present, decoupled from
    // capture). Same call for both windows: both show the full source.
    void draw_cursor_overlay(ID3D11RenderTargetView *rtv, long bbW, long bbH)
    {
        if (!curVisible_ || !curValid_ || curW_ <= 0 || capW_ <= 0) return;
        POINT live;
        if (!GetCursorPos(&live)) return;
        int cx = live.x - ref_.desc.DesktopCoordinates.left;
        int cy = live.y - ref_.desc.DesktopCoordinates.top;
        if (cx < 0 || cy < 0 || cx >= capW_ || cy >= capH_) return;   // not over this display
        cx -= curHotX_; cy -= curHotY_;
        const float s = std::min((float)bbW / capW_, (float)bbH / capH_);   // letterbox source
        const float vw = capW_ * s, vh = capH_ * s;
        const float vx = (bbW - vw) / 2, vy = (bbH - vh) / 2;
        const float x0 = vx + cx * s, y0 = vy + cy * s;
        const float x1 = x0 + curW_ * s, y1 = y0 + curH_ * s;
        const float q[8] = {
            x0 / bbW * 2 - 1, 1 - y0 / bbH * 2, x1 / bbW * 2 - 1, 1 - y1 / bbH * 2,
            dstHDR_ ? g_paperwhite_nits / 80.0f : 1.0f, 0.0f, 0, 0};   // extra.x scale, extra.y=0: linear out
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(cbCursor_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, q, sizeof(q)); ctx_->Unmap(cbCursor_.Get(), 0); }
        D3D11_VIEWPORT vp{0, 0, (float)bbW, (float)bbH, 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx_->VSSetShader(vsCursor_.Get(), nullptr, 0);
        ctx_->VSSetConstantBuffers(0, 1, cbCursor_.GetAddressOf());
        ctx_->PSSetShader(psCursor_.Get(), nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, cbCursor_.GetAddressOf());
        ctx_->PSSetShaderResources(0, 1, curSRV_.GetAddressOf());
        ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
        const float bf[4] = {0, 0, 0, 0};
        ctx_->OMSetBlendState(blendOver_.Get(), bf, 0xffffffff);
        ctx_->Draw(4, 0);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ID3D11ShaderResourceView *n = nullptr;
        ctx_->PSSetShaderResources(0, 1, &n);
    }

    bool reinit_dup(std::wstring *err)
    {
        dup_.Reset();
        fmtLogged_ = false;
        // Offer BOTH formats: DXGI hands back whichever the desktop is actually
        // composing in -- FP16 scRGB when HDR is on, BGRA8 sRGB when SDR. Native
        // to each, no up-convert. Plain DuplicateOutput can't do this: it only
        // ever returns 8-bit BGRA, so HDR cannot be captured through it.
        // An HDR<->SDR toggle raises ACCESS_LOST -> we reinit and re-pick.
        ComPtr<IDXGIOutput5> o5;
        if (SUCCEEDED(ref_.output1.As(&o5))) {
            const DXGI_FORMAT fmts[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
            if (SUCCEEDED(o5->DuplicateOutput1(dev_.Get(), 0, ARRAYSIZE(fmts), fmts, &dup_)))
                return true;
        }
        if (FAILED(ref_.output1->DuplicateOutput(dev_.Get(), &dup_))) { if (err) *err = L"DuplicateOutput failed"; return false; }
        return true;
    }

    void target_res(const EncConfig &c, int *w, int *h)
    {
        if (c.resIndex == 0 || kResHeights[c.resIndex] >= capH_) { *w = capW_ & ~1; *h = capH_ & ~1; return; }
        int th = kResHeights[c.resIndex];
        int tw = (int)((double)capW_ * th / capH_ + 0.5);
        *w = tw & ~1; *h = th & ~1;
    }

    bool reconfigure(const EncConfig &cIn, std::wstring *err)
    {
        EncConfig c = cIn;
        // Tell the encoder the real framerate (display refresh) so rate-control
        // budgets per-frame correctly -- otherwise at 120Hz it doubles the rate.
        c.fps = (cIn.fps > 0) ? cIn.fps : display_refresh_hz(ref_.desc.DeviceName);
        if (c.mode == EncConfig::CBR) c.maxrateKbps = 99000;   // CBR peak locked to 99 Mbps
        int w, h; target_res(c, &w, &h);
        conv_.resize(w, h);
        std::string e8;
        // HW encode = native AMF (true zero-copy from our P010 texture). ffmpeg is
        // only the CPU fallback if AMF is unavailable.
        useAmf_ = amf_.open(c, w, h, hdr_, dev_.Get(), &e8);
        if (useAmf_) {
            std::wcout << L"encoder: native AMF zero-copy (" << kCodecName[c.codec] << L")\n";
        } else {
            std::wcout << L"encoder: AMF unavailable (" << std::wstring(e8.begin(), e8.end())
                       << L"); ffmpeg/CPU fallback\n";
            if (!enc_.open(c, w, h, hdr_, hwDev_, &e8)) {
                std::wcerr << L"encoder open failed: " << std::wstring(e8.begin(), e8.end()) << L"\n";
                if (err) *err = L"encoder open failed"; return false;
            }
        }
        CtmsStreamInfo si{};
        si.codec = (c.codec == EncConfig::AV1) ? 2 : 1;
        si.width = (uint16_t)w; si.height = (uint16_t)h;
        si.fps = (uint16_t)c.fps;
        si.isHDR = 1;   // encoder is PQ BT.2020 for now (task #14: follow source)
        const float prim[8] = {hdr_.rx, hdr_.ry, hdr_.gx, hdr_.gy, hdr_.bx, hdr_.by, hdr_.wx, hdr_.wy};
        memcpy(si.primaries, prim, sizeof(prim));
        si.maxLum = hdr_.maxLum; si.minLum = hdr_.minLum;
        si.maxCLL = hdr_.maxLum; si.maxFALL = hdr_.maxFFLum;
        si.hasAudio = audioOn_ ? 1 : 0;
        si.audioChannels = (uint8_t)audio_.channels();
        si.audioRate = (uint32_t)audio_.rate();
        sender_.set_info(si);
        applied_ = c; encW_ = w; encH_ = h;
        std::wcout << L"reconfigured: " << kCodecName[c.codec] << L" " << kModeName[c.mode]
                   << L" " << kUsageName[c.usage] << L" " << (c.allIntra ? L"INTRA " : L"inter ")
                   << (c.intraRefresh ? L"IR " : L"") << w << L"x" << h << L"@" << c.fps
                   << L" br=" << c.bitrateKbps << L"k max=" << c.maxrateKbps
                   << L"k qp=" << c.qp
                   << L" slices=" << c.slices << L" auCap=" << c.maxFrameKbits << L"k\n";
        return true;
    }

    void update_stats()
    {
        const double t = now_ms();
        const double el = t - lastStat_;
        if (el < 1000.0) return;
        lastStat_ = t;
        const double fps = statLoops_ * 1000.0 / el;
        statLoops_ = 0;
        // Real per-stage fps -- COUNT actual frames over the wall-clock window, per
        // stage. Each ticks only on a genuine new-frame event, so all go to 0 when
        // the source is static (no drift, no approximating one stage from another).
        const uint64_t tx = txFrames_.load();
        const double txfps = (double)(tx - lastTx_) * 1000.0 / el;          // encode fps
        lastTx_ = tx;
        const uint64_t dcf = rx_.decFrames.load();
        const double decfps = (double)(dcf - lastDec_) * 1000.0 / el;       // decode-output fps
        lastDec_ = dcf;
        const double dispfps = (double)(dispFrames_ - lastDisp_) * 1000.0 / el;  // window-B new-frame fps
        lastDisp_ = dispFrames_;
        const double worst = worstLoop_;
        worstLoop_ = 0;
        const double mbps = winBytes_.exchange(0) * 8.0 / 1000.0 / el;   // window-averaged

        // Flush per-window distributions (avg/p50/p99/max + n). A single
        // last-value sample hid the spikes that decide a latency budget.
        const StatAcc::Summary enc = encAcc_.flush(),  host = hostAcc_.flush(),
                               mp  = mapAcc_.flush(),  lp   = loopAcc_.flush(),
                               dec = rx_.decAcc_.flush(), lat = rx_.latAcc_.flush(),
                               dgpu = rx_.decGpuAcc_.flush(), g2g = g2gAcc_.flush();
        const uint64_t fresh = freshFrames_, fb = presentFallback_;
        freshFrames_ = presentFallback_ = 0;

        // Window title: codec/mode/rate up front so every hotkey change is visible.
        wchar_t rate[48];
        if (applied_.mode == EncConfig::CQP) swprintf(rate, 48, L"qp%d", applied_.qp);
        else swprintf(rate, 48, L"%dk/%dk", applied_.bitrateKbps, applied_.maxrateKbps);
        wchar_t flags[48];
        swprintf(flags, 48, L"%ls%ls%ls%ls",
                 applied_.allIntra ? L" allI" : L"", applied_.intraRefresh ? L" IR" : L"",
                 applied_.slices > 1 ? L" S" : L"", applied_.maxFrameKbits > 0 ? L" AU" : L"");
        wchar_t buf[360];
        swprintf(buf, 360, L"CTM | %ls %ls %ls %dx%d %ls%ls | fps e%.0f d%.0f s%.0f | enc %.1f dec %.1f ms | g2g %.0f ms | %.1f Mb/s",
                 kCodecName[applied_.codec], kModeName[applied_.mode], kUsageName[applied_.usage], encW_, encH_, rate, flags,
                 txfps, decfps, dispfps,
                 enc.n ? enc.avg : -1.0, dec.n ? dec.avg : -1.0,
                 g2g.n ? g2g.avg : -1.0, mbps);
        SetWindowTextW(wndB_, buf);
        SetWindowTextW(wndA_, buf);   // window A stays visible when decode is off

        // Console: full distributions. n/a (not 0) when a term had no samples --
        // e.g. dec/lat under --nopreview, where there is no local receiver (#7).
        auto dump = [](const wchar_t *nm, const StatAcc::Summary &s) {
            if (s.n) std::wcout << L" | " << nm << L" avg " << s.avg << L" p50 " << s.p50
                                << L" p99 " << s.p99 << L" max " << s.mx << L" n=" << s.n;
            else     std::wcout << L" | " << nm << L" n/a";
        };
        std::wcout << L"[stat] " << kCodecName[applied_.codec] << L" " << kModeName[applied_.mode]
                   << L" " << kUsageName[applied_.usage] << L" " << encW_ << L"x" << encH_;
        if (applied_.mode == EncConfig::CQP) std::wcout << L" qp" << applied_.qp;
        else                                 std::wcout << L" " << applied_.bitrateKbps << L"k/" << applied_.maxrateKbps << L"k";
        if (applied_.allIntra)        std::wcout << L" allI";
        if (applied_.intraRefresh)    std::wcout << L" IR";
        if (applied_.slices > 1)      std::wcout << L" S" << applied_.slices;
        if (applied_.maxFrameKbits>0) std::wcout << L" AU" << applied_.maxFrameKbits << L"k";
        std::wcout << L" | loop " << fps << L"fps worst " << worst << L"ms";
        dump(L"enc(ms)", enc);
        dump(L"host present->enc(ms)", host);
        dump(L"decode d1-d0(ms)", dec);                     // real per-frame decode time
        if (g_profile) { dump(L"dec-GPU(ms)", dgpu);        // true Fd; --profile only
            std::wcout << L" gpu-drops=" << rx_.decGpuDrops_.exchange(0); }
        dump(L"lat src->dec-out(ms)", lat);
        dump(L"g2g d2-t0(ms)", g2g);                        // glass-to-glass (local), new frames only
        dump(L"map(ms)", mp);
        dump(L"loop(ms)", lp);
        std::wcout << L" | ~" << mbps << L" Mb/s | fps enc=" << txfps << L" dec=" << decfps << L" disp=" << dispfps
                   << L" enc#=" << pts_ << L" dec#=" << rx_.decFrames.load() << L" disp#=" << dispFrames_
                   << L" present-fallback=" << fb << L"/" << fresh   // #8: anchor trust
                   << L" shown=" << (decoded_.valid() ? 1 : 0)
                   << L" vsync=" << (g_vsync.load() ? 1 : 0)
                   << L" wstage=" << wstage_.load() << L" inflight=" << (jobInFlight_.load() ? 1 : 0)
                   << L" encErr=" << encErrs_.load()
                   << L" skp=" << drops_ << L" bfr=" << reorder_.load()
                   << L" hwdec=" << (rx_.hwdec.load() ? 1 : 0)
                   << L" prof=" << (g_profile ? 1 : 0)
                   << L" rxconn=" << (sender_.connected() ? 1 : 0) << L"\n";
    }

    OutputRef ref_;
    HWND wndA_ = nullptr;
    HWND wndB_ = nullptr;
    bool decodeOn_ = false;   // local decode path running + window B shown (toggled by 'D')
    std::atomic<bool> benchRunning_{false};   // a one-shot decode bench ('B') is in progress
    WinState *wsA_ = nullptr, *wsB_ = nullptr;
    HdrInfo hdr_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    AVBufferRef *hwDev_ = nullptr;           // D3D11VA wrap of dev_
    ComPtr<IDXGIFactory2> fac_;
    ComPtr<IDXGISwapChain3> scA_, scB_;
    ComPtr<ID3D11RenderTargetView> rtvA_, rtvB_;
    long bbAW_ = 1, bbAH_ = 1, bbBW_ = 1, bbBH_ = 1;
    bool tearing_ = false;
    bool dstHDR_ = false;                       // window's monitor presents HDR?
    bool audioOn_ = false;
    DXGI_FORMAT rtvFmt_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> frameTex_;
    ComPtr<ID3D11ShaderResourceView> frameSRV_;
    int frW_ = 0, frH_ = 0; bool haveFrame_ = false;
    double framePresentMs_ = 0;                 // Windows present time of last fresh frame
    DXGI_FORMAT frFmt_ = DXGI_FORMAT_UNKNOWN;   // real duplicated format
    bool frameHDR_ = false;                     // FP16 scRGB vs 8-bit sRGB
    bool fmtLogged_ = false;                     // log the captured format once
    ComPtr<ID3D11VertexShader> vsScene_;
    ComPtr<ID3D11PixelShader> psScene_;
    ComPtr<ID3D11SamplerState> samp_;
    ComPtr<ID3D11Buffer> cbScene_;
    ComPtr<ID3D11VertexShader> vsCursor_;
    ComPtr<ID3D11PixelShader> psCursor_;
    ComPtr<ID3D11Buffer> cbCursor_;
    ComPtr<ID3D11BlendState> blendOver_;
    ComPtr<ID3D11RenderTargetView> rtvFrame_;
    ComPtr<ID3D11Texture2D> curTex_;
    ComPtr<ID3D11ShaderResourceView> curSRV_;
    int curW_ = 0, curH_ = 0, curHotX_ = 0, curHotY_ = 0;
    POINT curPos_{}; bool curVisible_ = false, curValid_ = false;
    std::vector<uint8_t> shapeBuf_;
    Converter conv_;
    DecodedView decoded_;
    Encoder enc_;                            // ffmpeg CPU fallback
    AmfEncoder amf_;                         // native AMD zero-copy (primary HW path)
    bool useAmf_ = false;
    std::vector<std::vector<uint8_t>> amfOut_;
    std::vector<bool> amfKeys_;
    EncConfig applied_;
    int capW_ = 0, capH_ = 0, encW_ = 0, encH_ = 0;
    int64_t pts_ = 0;

    // CTMS stream: sender (host) + local receiver feeding window B
    StreamSender sender_;
    StreamReceiver rx_;
    AudioLoopback audio_;
    int lastSentX_ = INT_MIN, lastSentY_ = INT_MIN; bool lastSentVis_ = false;
    ComPtr<ID3D11Texture2D> rxCurTex_;
    ComPtr<ID3D11ShaderResourceView> rxCurSRV_;
    int rxCurW_ = 0, rxCurH_ = 0;

    // capture+encode thread (owns the duplication); legacy handoff fields kept
    // unused so the dtor/teardown stays intact.
    std::thread codecThread_;
    std::mutex jobMtx_;
    std::condition_variable jobCv_;
    bool jobReady_ = false;
    std::atomic<bool> workerExit_{false};
    const uint8_t *jobY_ = nullptr, *jobUV_ = nullptr;
    int jobPY_ = 0, jobPUV_ = 0; int64_t jobPts_ = 0;
    AVFrame *jobFrame_ = nullptr;
    std::atomic<bool> jobInFlight_{false};
    std::atomic<bool> jobDone_{false};
    int subNew_ = -1, subOld_ = -1, inFlightSlot_ = -1;
    double submitT_[8] = {};
    int drops_ = 0;
    // f1 double-buffer: the capture thread writes f1Tex_[f1Write_], converts +
    // encodes from it, then publishes it (f1Display_) for the preview thread to
    // draw window A -- so the encode path owns the one duplication and preview is
    // a separate reader, never in the t0->t3 path.
    std::mutex f1Mtx_;
    ComPtr<ID3D11Texture2D> f1Tex_[2];
    ComPtr<ID3D11ShaderResourceView> f1SRV_[2];
    int f1Write_ = 0, f1Display_ = -1;
    int f1W_ = 0, f1H_ = 0; DXGI_FORMAT f1Fmt_ = DXGI_FORMAT_UNKNOWN; bool f1HDR_ = false;
    // Serializes immediate-context draw SEQUENCES across the capture thread
    // (copy/convert) and the preview thread (window A/B draws). The context is a
    // single state machine; without this their sequences interleave -> flicker.
    std::mutex gpuMtx_;

    // stats
    std::atomic<double> encMsA_{0};    // AMF encode call duration
    std::atomic<double> hostMsA_{0};   // present->encoded (matches TV "enc")
    std::atomic<uint64_t> winBytes_{0};
    std::atomic<int> wstage_{0};               // 0 idle, 1 encoding
    std::atomic<int> encErrs_{0};
    std::atomic<int> reorder_{0};              // packets with pts != dts (B-frames)
    std::atomic<uint64_t> txFrames_{0};        // video frames handed to the sender
    double t0_ = 0;                            // stream epoch (pts = us since t0)
    double lastDispatchT_ = 0;                 // --fps cadence cap
    uint64_t lastTx_ = 0;
    int statLoops_ = 0;
    double worstLoop_ = 0, mapMs_ = 0, lastStat_ = 0;
    // per-window distributions (#6) + present-anchor fallback rate (#8)
    StatAcc encAcc_, hostAcc_, mapAcc_, loopAcc_;
    uint64_t freshFrames_ = 0, presentFallback_ = 0;
    StatAcc g2gAcc_;                          // glass-to-glass (local): d2 - t0, new frames only
    uint64_t dispFrames_ = 0, lastDisp_ = 0;  // window-B new-frame displays (display-fps)
    uint64_t lastDec_ = 0;                     // decode-fps window delta
    uint64_t lastShownGen_ = 0;                // last AMF decode gen displayed (new vs redraw)
};

static int run_dual(OutputRef ref)
{
    TimerRes timerRes;   // 1ms scheduler tick so timings aren't tick-quantized (#1)
    const wchar_t *clsA = L"CtmCaptureRaw", *clsB = L"CtmCaptureDecoded";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DualWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = clsA;
    RegisterClassW(&wc);
    wc.lpszClassName = clsB;
    RegisterClassW(&wc);

    static WinState wsA, wsB;
    HWND wndA = CreateWindowW(clsA, L"CTM raw capture", WS_OVERLAPPEDWINDOW,
                              80, 80, 960, 540, nullptr, nullptr, wc.hInstance, nullptr);
    HWND wndB = CreateWindowW(clsB, L"CTM decoded", WS_OVERLAPPEDWINDOW,
                              1060, 80, 960, 540, nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(wndA, GWLP_USERDATA, (LONG_PTR)&wsA);
    SetWindowLongPtrW(wndB, GWLP_USERDATA, (LONG_PTR)&wsB);

    std::wcout << std::unitbuf;   // flush diagnostics line-by-line (debug tool)
    auto dp = std::make_shared<DualPipeline>();
    std::wstring err;
    if (!dp->init(ref, wndA, wndB, &wsA, &wsB, &err)) { std::wcerr << L"dual init: " << err << L"\n"; return 3; }
    if (!g_noPreview) {
        ShowWindow(wndA, SW_SHOW);
        ShowWindow(wndB, g_noDecode.load() ? SW_HIDE : SW_SHOW);
    }
    std::wcout << L"\nHotkeys (focus either window):  C=codec  M=rate-mode  R=resolution  U=usage"
                  L"  I=all-intra  F=intra-refresh  S=slices  A=frame-cap  V=vsync  D=decode-window  B=decode-bench"
                  L"  Up/Down=bitrate(or qp)  Left/Right=maxrate  Esc=quit\n";

    std::thread render([dp]() {
        while (g_running.load()) {
            if (!dp->render_once()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    MSG m;
    while (GetMessage(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    g_running.store(false);
    if (render.joinable()) render.join();
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // required for DuplicateOutput1
    for (int i = 1; i + 1 < argc; ++i)
        if (std::wstring(argv[i]) == L"--paperwhite") g_paperwhite_nits = (float)_wtof(argv[i + 1]);

    std::vector<OutputRef> outs = enumerate_outputs();
    if (outs.empty()) { std::wcerr << L"no DXGI outputs found\n"; return 2; }

    std::wstring a1 = argc >= 2 ? argv[1] : L"";

    if (a1 == L"--list") { list_outputs(outs); return 0; }

    if (a1 == L"--shot") {
        if (argc < 4) { std::wcerr << L"usage: ctm-capture --shot <idx> <png>\n"; return 2; }
        const int idx = _wtoi(argv[2]);
        if (idx < 0 || idx >= (int)outs.size()) { std::wcerr << L"bad output index\n"; return 2; }
        list_outputs(outs);
        return run_shot(outs[idx], argv[3]);
    }

    // Default output = the last one (the newly added VDD monitor); flags override.
    int idx = (int)outs.size() - 1;
    bool single = false;
    EncConfig cfg;
    auto eqi = [](const std::wstring &a, const wchar_t *b) { return _wcsicmp(a.c_str(), b) == 0; };
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&](const wchar_t *def) -> std::wstring { return (i + 1 < argc) ? argv[++i] : def; };
        if (a == L"--single") single = true;
        else if (a == L"--nopreview") g_noPreview = true;
        else if (a == L"--nodecode") g_noDecode = true;
        else if (a == L"--profile") g_profile = true;   // measure true HW-decode GPU time
        else if (a == L"--cpucopy") g_cpucopy = true;    // force CPU encode input (no zero-copy)
        else if (a == L"--fps") g_maxFps = _wtoi(next(L"0").c_str());   // 0/absent = uncapped
        else if (a == L"--intra") cfg.allIntra = true;
        else if (a == L"--output") idx = _wtoi(next(L"0").c_str());
        else if (a == L"--codec") cfg.codec = eqi(next(L"hevc"), L"av1") ? EncConfig::AV1 : EncConfig::HEVC;
        else if (a == L"--bitrate") cfg.bitrateKbps = std::max(500, _wtoi(next(L"40000").c_str()));
        else if (a == L"--maxrate") cfg.maxrateKbps = std::max(500, _wtoi(next(L"60000").c_str()));
        else if (a == L"--qp") cfg.qp = _wtoi(next(L"24").c_str());
        else if (a == L"--mode") { std::wstring m = next(L"vbr"); cfg.mode = eqi(m, L"cbr") ? EncConfig::CBR : eqi(m, L"cqp") ? EncConfig::CQP : EncConfig::VBR; }
        else if (a == L"--usage") { std::wstring u = next(L"ull"); cfg.usage = eqi(u, L"ll") ? EncConfig::LL : eqi(u, L"transcode") ? EncConfig::TRANSCODE : EncConfig::ULL; }
        else if (a == L"--res") {
            int rh = _wtoi(next(L"0").c_str());
            for (int k = 0; k < (int)(sizeof(kResHeights) / sizeof(int)); ++k) if (kResHeights[k] == rh) cfg.resIndex = k;
        }
    }
    if (cfg.maxrateKbps < cfg.bitrateKbps) cfg.maxrateKbps = cfg.bitrateKbps;
    if (idx < 0 || idx >= (int)outs.size()) { std::wcerr << L"bad output index\n"; return 2; }
    list_outputs(outs);
    std::wcout << L"capturing output [" << idx << L"] " << outs[idx].desc.DeviceName << L"\n";

    if (single) return run_preview(outs[idx]);
    { std::lock_guard<std::mutex> lk(g_cfgMtx); g_cfgDesired = cfg; }
    return run_dual(outs[idx]);
}
