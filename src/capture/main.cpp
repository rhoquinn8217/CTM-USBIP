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
#include <unknwn.h>      // IStream for gdiplus under WIN32_LEAN_AND_MEAN
#include <objidl.h>      // PROPID for gdiplus
#include <gdiplus.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
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

#include "ctm_stream_protocol.h"

using Microsoft::WRL::ComPtr;

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
    enum Mode  { CBR = 0, VBR = 1, CQP = 2 } mode = VBR;
    enum Usage { ULL = 0, LL = 1, TRANSCODE = 2 } usage = ULL;
    int resIndex = 0;          // index into kResHeights; 0 == native
    int bitrateKbps = 40000;   // target
    int maxrateKbps = 60000;   // peak / ceiling (VBR, CBR)
    int qp = 24;               // CQP qp / qvbr quality level
    bool encoderDiffers(const EncConfig &o) const {
        return codec != o.codec || mode != o.mode || usage != o.usage ||
               bitrateKbps != o.bitrateKbps || maxrateKbps != o.maxrateKbps ||
               qp != o.qp || resIndex != o.resIndex;
    }
};
static const int kResHeights[] = {0, 2160, 1440, 1080, 720};
static const wchar_t *kCodecName[] = {L"HEVC", L"AV1"};
static const wchar_t *kModeName[]  = {L"CBR", L"VBR", L"CQP"};
static const wchar_t *kUsageName[] = {L"ultralowlatency", L"lowlatency", L"transcoding"};

// shared live control state (written by WndProc, applied by the render thread)
static std::mutex g_cfgMtx;
static EncConfig g_cfgDesired;
static std::atomic<bool> g_cfgDirty{false};
static std::atomic<bool> g_vsync{false};   // V toggles; off = present without vblank wait

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
};

// --- AMF encoder (libavcodec hevc_amf / av1_amf), 10-bit x2bgr10le input ---
static AVRational q_xy(float v) { return av_make_q((int)(v * 50000.0f + 0.5f), 50000); }

class Encoder {
public:
    bool open(const EncConfig &cfg, int w, int h, const HdrInfo &hdr, std::string *err)
    {
        close();
        const char *name = (cfg.codec == EncConfig::HEVC) ? "hevc_amf" : "av1_amf";
        const AVCodec *codec = avcodec_find_encoder_by_name(name);
        if (!codec) { if (err) *err = std::string("encoder not found: ") + name; return false; }
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) { if (err) *err = "alloc encoder ctx failed"; return false; }
        ctx_->width = w; ctx_->height = h;
        ctx_->pix_fmt = AV_PIX_FMT_P010LE;    // shader already produced PQ BT.2020 YCbCr
        ctx_->time_base = av_make_q(1, 60);
        ctx_->framerate = av_make_q(60, 1);
        // Long GOP: an IDR every second (~60) caused a visible 1 Hz hiccup on
        // the TV (multi-megabit burst + heavier decode). New clients don't
        // wait for the GOP anyway -- joining forces an IDR.
        ctx_->gop_size = 600;
        ctx_->max_b_frames = 0;
        ctx_->color_primaries = AVCOL_PRI_BT2020;
        ctx_->color_trc = AVCOL_TRC_SMPTE2084;
        ctx_->colorspace = AVCOL_SPC_BT2020_NCL;
        ctx_->color_range = AVCOL_RANGE_MPEG;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

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

        frame_ = av_frame_alloc();
        frame_->format = AV_PIX_FMT_P010LE; frame_->width = w; frame_->height = h;
        frame_->color_primaries = AVCOL_PRI_BT2020;
        frame_->color_trc = AVCOL_TRC_SMPTE2084;
        frame_->colorspace = AVCOL_SPC_BT2020_NCL;
        frame_->color_range = AVCOL_RANGE_MPEG;            // shader outputs limited range
        if (av_frame_get_buffer(frame_, 0) < 0) { if (err) *err = "frame buffer alloc failed"; close(); return false; }

        // HDR10 static metadata from the monitor, written by AMF as SEI.
        if (AVMasteringDisplayMetadata *md = av_mastering_display_metadata_create_side_data(frame_)) {
            md->display_primaries[0][0] = q_xy(hdr.rx); md->display_primaries[0][1] = q_xy(hdr.ry);
            md->display_primaries[1][0] = q_xy(hdr.gx); md->display_primaries[1][1] = q_xy(hdr.gy);
            md->display_primaries[2][0] = q_xy(hdr.bx); md->display_primaries[2][1] = q_xy(hdr.by);
            md->white_point[0] = q_xy(hdr.wx); md->white_point[1] = q_xy(hdr.wy);
            md->min_luminance = av_make_q((int)(hdr.minLum * 10000.0f), 10000);
            md->max_luminance = av_make_q((int)hdr.maxLum, 1);
            md->has_primaries = 1; md->has_luminance = 1;
        }
        if (AVContentLightMetadata *cl = av_content_light_metadata_create_side_data(frame_)) {
            cl->MaxCLL = (unsigned)hdr.maxLum; cl->MaxFALL = (unsigned)hdr.maxFFLum;
        }
        pkt_ = av_packet_alloc();
        w_ = w; h_ = h;
        return true;
    }

    // Copy mapped P010 planes (Y full res, interleaved CbCr half res) and
    // encode. Appends emitted packets (caller frees with av_packet_free).
    int encode(const uint8_t *y, int yPitch, const uint8_t *uv, int uvPitch,
               int64_t pts, std::vector<AVPacket *> &out)
    {
        if (!ctx_) return -1;
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
        // Low latency = 1-in/1-out: wait (bounded) for this frame's packet
        // instead of letting it ride out on a later call.
        for (int spins = 0; spins < 400; ++spins) {
            r = avcodec_receive_packet(ctx_, pkt_);
            if (r == 0) {
                AVPacket *p = av_packet_alloc();
                av_packet_move_ref(p, pkt_);
                out.push_back(p);
                continue;            // drain anything else that's ready
            }
            if (r == AVERROR(EAGAIN)) {
                if (!out.empty()) return 0;
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
            return (r == AVERROR_EOF) ? 0 : r;
        }
        return 0;                    // safety bound (~200ms); never deadlock
    }

    void force_idr() { forceIdr_.store(true); }

    void close()
    {
        if (pkt_) av_packet_free(&pkt_);
        if (frame_) av_frame_free(&frame_);
        if (ctx_) avcodec_free_context(&ctx_);
        w_ = h_ = 0;
    }
    ~Encoder() { close(); }

private:
    static const char *kUsageNameA(int u) { return u == 0 ? "ultralowlatency" : u == 1 ? "lowlatency" : "transcoding"; }
    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *pkt_ = nullptr;
    std::atomic<bool> forceIdr_{false};
    int w_ = 0, h_ = 0;
};

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
            for (SOCKET c : clients_) closesocket(c);
            clients_.clear();
        }
        if (acceptThread_.joinable()) acceptThread_.join();
    }
    ~StreamSender() { stop(); }

    // A new client must start on an IDR; the codec worker polls this.
    bool take_want_idr() { return wantIdr_.exchange(false); }

    void set_info(const CtmsStreamInfo &si)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        info_ = si; haveInfo_ = true;
        send_msg(CTMS_STREAM_INFO, 0, 0, &info_, sizeof(info_));
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
        send_msg(CTMS_CURSOR_POS, 0, (uint64_t)now_ms(), &p, sizeof(p));
    }
    void send_video(const uint8_t *data, int size, int64_t pts, bool idr)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        send_msg(CTMS_VIDEO_FRAME, idr ? CTMS_FLAG_IDR : 0, (uint64_t)pts, data, size);
    }
    bool connected() { std::lock_guard<std::mutex> lk(mtx_); return !clients_.empty(); }

private:
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
            if (clients_.size() >= 4) { closesocket(c); continue; }
            clients_.push_back(c);
            if (haveInfo_) send_one(c, CTMS_STREAM_INFO, 0, 0, &info_, sizeof(info_));
            if (!shapePix_.empty()) {
                std::vector<uint8_t> buf = shape_buf();
                send_one(c, CTMS_CURSOR_SHAPE, 0, 0, buf.data(), (int)buf.size());
            }
            wantIdr_.store(true);
        }
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
        send_msg(CTMS_CURSOR_SHAPE, 0, 0, buf.data(), (int)buf.size());
    }
    void send_msg(uint16_t type, uint16_t flags, uint64_t pts, const void *payload, int len)   // mtx_ held
    {
        for (size_t i = 0; i < clients_.size();) {
            if (send_one(clients_[i], type, flags, pts, payload, len)) {
                ++i;
            } else {
                closesocket(clients_[i]);
                clients_.erase(clients_.begin() + i);   // dead client; drop it
            }
        }
    }
    bool send_one(SOCKET s, uint16_t type, uint16_t flags, uint64_t pts, const void *payload, int len)
    {
        CtmsHdr h{CTMS_MAGIC, type, flags, pts, (uint32_t)len};
        return send_all(s, &h, sizeof(h)) && send_all(s, payload, len);
    }
    bool send_all(SOCKET s, const void *p, int len)
    {
        const char *b = static_cast<const char *>(p);
        while (len > 0) {
            int n = send(s, b, len, 0);
            if (n <= 0) return false;
            b += n; len -= n;
        }
        return true;
    }

    SOCKET listenSock_ = INVALID_SOCKET;
    std::vector<SOCKET> clients_;
    std::thread acceptThread_;
    std::atomic<bool> exit_{false};
    std::atomic<bool> wantIdr_{false};
    std::mutex mtx_;
    CtmsStreamInfo info_{}; bool haveInfo_ = false;
    CtmsCursorShape shapeHdr_{};
    std::vector<uint8_t> shapePix_;
};

// Receiver: exactly what the TV will run -- connect, parse CTMS, feed the
// decoder, track the remote cursor. Here it feeds window B over loopback so
// B shows the true protocol+decode path (minus only network ping).
class StreamReceiver {
public:
    // dispatchT: pts&127 -> capture time ring (same process), for the lat stat
    void start(uint16_t port, AVBufferRef *hwDev, const double *dispatchT)
    {
        hwDev_ = hwDev; dispatchT_ = dispatchT;
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
    std::atomic<bool> hwdec{false};

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
                    std::string e;
                    dec_.open(info_.codec == 2 ? EncConfig::AV1 : EncConfig::HEVC, hwDev_, &e);
                    hwdec.store(dec_.is_hw());
                }
                break;
            case CTMS_VIDEO_FRAME: {
                AVPacket *p = av_packet_alloc();
                if (p && av_new_packet(p, (int)h.payloadLen) == 0) {
                    memcpy(p->data, payload.data(), h.payloadLen);
                    p->pts = p->dts = (int64_t)h.pts;
                    const double t0 = now_ms();
                    if (AVFrame *f = dec_.decode(p)) {
                        if (AVFrame *cl = av_frame_clone(f)) {
                            std::lock_guard<std::mutex> lk(decMtx_);
                            if (decodedLatest_) av_frame_free(&decodedLatest_);
                            decodedLatest_ = cl;
                        }
                        if (f->pts >= 0 && dispatchT_)
                            latMs.store(now_ms() - dispatchT_[f->pts & 127]);
                        decFrames.fetch_add(1);
                    }
                    decMs.store(now_ms() - t0);
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

    std::thread thread_;
    std::atomic<bool> exit_{false};
    std::mutex sockMtx_;
    SOCKET sock_ = INVALID_SOCKET;
    AVBufferRef *hwDev_ = nullptr;
    const double *dispatchT_ = nullptr;
    Decoder dec_;
    CtmsStreamInfo info_{};
    std::mutex decMtx_;
    AVFrame *decodedLatest_ = nullptr;
    std::mutex curMtx_;
    int curX_ = 0, curY_ = 0, curW_ = 0, curH_ = 0;
    bool curVis_ = false, shapeDirty_ = false;
    std::vector<uint8_t> curPix_;
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
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    EncConfig &c = g_cfgDesired;
    switch (key) {
    case 'C': c.codec = (c.codec == EncConfig::HEVC) ? EncConfig::AV1 : EncConfig::HEVC; break;
    case 'M': c.mode  = (EncConfig::Mode)(((int)c.mode + 1) % 3); break;
    case 'R': c.resIndex = (c.resIndex + 1) % (int)(sizeof(kResHeights) / sizeof(int)); break;
    case 'U': c.usage = (EncConfig::Usage)(((int)c.usage + 1) % 3); break;
    case VK_UP:
        if (c.mode == EncConfig::CQP) c.qp = std::max(1, c.qp - 1);
        else c.bitrateKbps = std::min(200000, c.bitrateKbps + 5000);
        break;
    case VK_DOWN:
        if (c.mode == EncConfig::CQP) c.qp = std::min(51, c.qp + 1);
        else c.bitrateKbps = std::max(2000, c.bitrateKbps - 5000);
        break;
    case VK_RIGHT: c.maxrateKbps = std::min(300000, c.maxrateKbps + 5000); break;
    case VK_LEFT:  c.maxrateKbps = std::max(c.bitrateKbps, c.maxrateKbps - 5000); break;
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
        ref_ = ref; wndB_ = wndB; wsA_ = wsA; wsB_ = wsB;
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
        if (!sender_.start(CTMS_PORT, err)) return false;
        rx_.start(CTMS_PORT, hwDev_, dispatchT_);
        std::wcout << L"CTMS stream on port " << CTMS_PORT << L" (local receiver attached)\n";

        { std::lock_guard<std::mutex> lk(g_cfgMtx); applied_ = g_cfgDesired; }
        if (!reconfigure(applied_, err)) return false;
        log_display_mode(ref_);
        std::wcout << L"HDR monitor info: valid=" << hdr_.valid << L" maxLum=" << hdr_.maxLum
                   << L" maxFALL=" << hdr_.maxFFLum << L" minLum=" << hdr_.minLum << L"\n";
        lastStat_ = now_ms();
        codecThread_ = std::thread(&DualPipeline::codec_worker, this);
        return true;
    }

    ~DualPipeline()
    {
        { std::lock_guard<std::mutex> lk(jobMtx_); workerExit_ = true; }
        jobCv_.notify_all();
        if (codecThread_.joinable()) codecThread_.join();
        rx_.stop();          // receiver owns the decoder; stop before hwDev unref
        sender_.stop();
        if (inFlightSlot_ >= 0) conv_.unmap(inFlightSlot_);
        if (hwDev_) av_buffer_unref(&hwDev_);   // decoder/frames hold own refs
        WSACleanup();
    }

    bool render_once()
    {
        const double loopT0 = now_ms();

        // 1. collect a finished codec job (worker is done with the mapped slot)
        if (jobDone_.exchange(false)) {
            if (inFlightSlot_ >= 0) { conv_.unmap(inFlightSlot_); inFlightSlot_ = -1; }
            jobInFlight_.store(false);
        }

        // 2. live reconfigure, only while the codec worker is idle (it owns
        //    enc_/dec_ during a job and reads a mapped staging slot)
        if (g_cfgDirty.load() && !jobInFlight_.load()) {
            g_cfgDirty.store(false);
            EncConfig want; { std::lock_guard<std::mutex> lk(g_cfgMtx); want = g_cfgDesired; }
            if (want.encoderDiffers(applied_)) {
                std::wstring e;
                reconfigure(want, &e);
                subNew_ = subOld_ = -1;   // staged frames are at the old resolution
            }
        }

        // 3. dispatch to the codec worker: newest completed copy wins. Try the
        //    most recent submission first; if its GPU copy isn't finished yet,
        //    fall back to the older one. Whatever is staler gets dropped.
        if (!jobInFlight_.load() && (subNew_ >= 0 || subOld_ >= 0)) {
            const int order[2] = {subNew_, subOld_};
            for (int k = 0; k < 2; ++k) {
                const int s = order[k];
                if (s < 0) continue;
                const uint8_t *dy = nullptr, *duv = nullptr; int py = 0, puv = 0;
                const double m0 = now_ms();
                const int mr = conv_.map(s, &dy, &py, &duv, &puv);
                if (mr == 1) {
                    mapMs_ = now_ms() - m0;
                    {
                        std::lock_guard<std::mutex> lk(jobMtx_);
                        jobY_ = dy; jobPY_ = py; jobUV_ = duv; jobPUV_ = puv; jobPts_ = pts_++;
                        dispatchT_[jobPts_ & 127] = submitT_[s];   // latency from capture, not dispatch
                        jobReady_ = true;
                    }
                    inFlightSlot_ = s;
                    if (s == subNew_) {
                        subNew_ = -1;
                        if (subOld_ >= 0) { subOld_ = -1; drops_++; }   // older content: drop
                    } else {
                        subOld_ = -1;          // newest still copying; it dispatches next
                    }
                    jobInFlight_.store(true);
                    jobCv_.notify_one();
                    break;
                }
                if (mr < 0) { (s == subNew_ ? subNew_ : subOld_) = -1; }
                // mr == 0: copy not finished; try the older slot / next loop
            }
        }

        resize_backbuffers();

        // 4. grab a frame (FP16 scRGB)
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(4, &fi, &res);
        bool fresh = false;
        if (hr == DXGI_ERROR_ACCESS_LOST) { std::wstring e; reinit_dup(&e); }
        else if (hr == DXGI_ERROR_WAIT_TIMEOUT) { /* no change */ }
        else if (SUCCEEDED(hr)) {
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(res.As(&tex))) {
                D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
                // Follow the display's real format: recreate our copy on a
                // format change (HDR<->SDR), not just on resize.
                if (!frameTex_ || frW_ != (int)td.Width || frH_ != (int)td.Height || frFmt_ != td.Format) {
                    frameTex_.Reset(); frameSRV_.Reset(); rtvFrame_.Reset();
                    D3D11_TEXTURE2D_DESC d = td;
                    d.Usage = D3D11_USAGE_DEFAULT;
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;  // RTV for cursor compositing
                    d.CPUAccessFlags = 0; d.MiscFlags = 0;
                    if (SUCCEEDED(dev_->CreateTexture2D(&d, nullptr, &frameTex_))) {
                        dev_->CreateShaderResourceView(frameTex_.Get(), nullptr, &frameSRV_);
                        dev_->CreateRenderTargetView(frameTex_.Get(), nullptr, &rtvFrame_);
                    }
                    frW_ = td.Width; frH_ = td.Height; frFmt_ = td.Format;
                    frameHDR_ = (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);  // FP16 == scRGB/HDR
                }
                if (!fmtLogged_) {
                    fmtLogged_ = true;
                    std::wcout << L"captured surface: " << dxgi_format_name(td.Format)
                               << L" -> treating as " << (frameHDR_ ? L"HDR/scRGB linear" : L"SDR/sRGB") << L"\n";
                }
                if (frameTex_) {
                    ctx_->CopyResource(frameTex_.Get(), tex.Get());
                    update_cursor(fi);         // refresh shape; capture stays cursorless
                    haveFrame_ = true; fresh = true;
                }
            }
            dup_->ReleaseFrame();
        }

        // 5. window A: raw scRGB passthrough (never waits on the codec path)
        const UINT sync = g_vsync.load() ? 1 : 0;
        const UINT pflags = (sync == 0 && tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        if (haveFrame_) draw_scene(rtvA_.Get(), bbAW_, bbAH_, frameSRV_.Get(), frW_, frH_);
        draw_cursor_overlay(rtvA_.Get(), bbAW_, bbAH_);
        scA_->Present(sync, pflags);
        send_cursor_pos();   // CTMS: position on change (shape goes in update_cursor)

        // 6. reservoir, latest-wins: every fresh frame is submitted; when both
        //    rotating slots hold submissions, overwrite the older one (drop).
        //    Never re-copy onto the slot dispatch is probing -- rotation keeps
        //    the probe target stable, so no Map livelock.
        if (fresh && haveFrame_) {
            int slot;
            if (subOld_ >= 0) { slot = subOld_; drops_++; }      // superseded; reuse
            else { slot = 0; while (slot == inFlightSlot_ || slot == subNew_) ++slot; }
            if (conv_.submit(frameSRV_.Get(), slot)) {
                subOld_ = subNew_;
                subNew_ = slot;
                submitT_[slot] = now_ms();
            }
        }

        // 7. window B: newest decoded frame from the CTMS receiver (the same
        //    path the TV runs); cursor drawn from RECEIVED metadata, so B
        //    shows the cursor's true through-the-pipe latency.
        AVFrame *take = rx_.take_decoded();
        if (take) {
            if (take->format == AV_PIX_FMT_D3D11) decoded_.set_hw(take);
            else { decoded_.upload(take); av_frame_free(&take); }
        }
        if (decoded_.valid()) {
            const float black[4] = {0, 0, 0, 1};
            ctx_->ClearRenderTargetView(rtvB_.Get(), black);
            ctx_->OMSetRenderTargets(1, rtvB_.GetAddressOf(), nullptr);
            decoded_.draw(bbBW_, bbBH_);
        }
        draw_rx_cursor_overlay(rtvB_.Get(), bbBW_, bbBH_);
        scB_->Present(sync, pflags);

        const double dt = now_ms() - loopT0;
        statLoops_++;
        if (dt > worstLoop_) worstLoop_ = dt;
        update_stats();
        return true;
    }

    // Codec worker: encode + decode off the render thread, so neither window
    // ever stalls on the codecs (this is where the old per-frame hiccups and
    // the 142ms scene-change decode spike used to land).
    void codec_worker()
    {
        for (;;) {
            const uint8_t *dy = nullptr, *duv = nullptr; int py = 0, puv = 0; int64_t pts = 0;
            {
                std::unique_lock<std::mutex> lk(jobMtx_);
                jobCv_.wait(lk, [&] { return jobReady_ || workerExit_; });
                if (workerExit_) return;
                jobReady_ = false;
                dy = jobY_; py = jobPY_; duv = jobUV_; puv = jobPUV_; pts = jobPts_;
            }
            std::vector<AVPacket *> pkts;
            wstage_.store(1);                  // encoding
            if (sender_.take_want_idr()) enc_.force_idr();   // new client joined
            double t0 = now_ms();
            int er = enc_.encode(dy, py, duv, puv, pts, pkts);
            if (er < 0) encErrs_.fetch_add(1);
            encMsA_.store(now_ms() - t0);
            size_t bytes = 0;
            for (AVPacket *p : pkts) {         // ship over CTMS; the receiver decodes
                bytes += p->size;
                if (p->pts != AV_NOPTS_VALUE && p->dts != AV_NOPTS_VALUE && p->pts != p->dts)
                    reorder_.fetch_add(1);   // pts!=dts => B-frame style reordering
                sender_.send_video(p->data, p->size, p->pts, (p->flags & AV_PKT_FLAG_KEY) != 0);
                av_packet_free(&p);
            }
            winBytes_.fetch_add(bytes);
            wstage_.store(0);                  // idle
            jobDone_.store(true);
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

    bool reconfigure(const EncConfig &c, std::wstring *err)
    {
        int w, h; target_res(c, &w, &h);
        conv_.resize(w, h);
        std::string e8;
        if (!enc_.open(c, w, h, hdr_, &e8)) {
            std::wcerr << L"encoder open failed: " << std::wstring(e8.begin(), e8.end()) << L"\n";
            if (err) *err = L"encoder open failed"; return false;
        }
        CtmsStreamInfo si{};
        si.codec = (c.codec == EncConfig::AV1) ? 2 : 1;
        si.width = (uint16_t)w; si.height = (uint16_t)h;
        si.fps = 60;
        si.isHDR = 1;   // encoder is PQ BT.2020 for now (task #14: follow source)
        const float prim[8] = {hdr_.rx, hdr_.ry, hdr_.gx, hdr_.gy, hdr_.bx, hdr_.by, hdr_.wx, hdr_.wy};
        memcpy(si.primaries, prim, sizeof(prim));
        si.maxLum = hdr_.maxLum; si.minLum = hdr_.minLum;
        si.maxCLL = hdr_.maxLum; si.maxFALL = hdr_.maxFFLum;
        sender_.set_info(si);
        applied_ = c; encW_ = w; encH_ = h;
        std::wcout << L"reconfigured: " << kCodecName[c.codec] << L" " << kModeName[c.mode]
                   << L" " << kUsageName[c.usage] << L" " << w << L"x" << h
                   << L" br=" << c.bitrateKbps << L"k max=" << c.maxrateKbps
                   << L"k qp=" << c.qp << L"\n";
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
        const double worst = worstLoop_;
        worstLoop_ = 0;
        const double mbps = winBytes_.exchange(0) * 8.0 / 1000.0 / el;   // window-averaged
        const double encMs = encMsA_.load(), decMs = rx_.decMs.load(), latMs = rx_.latMs.load();
        wchar_t buf[360];
        swprintf(buf, 360, L"CTM decoded | %ls %ls %ls %dx%d br %dk/max %dk qp %d | loop %.0ffps worst %.1fms | enc %.1f dec %.1f lat %.0f ms | ~%.1f Mb/s vsync=%d",
                 kCodecName[applied_.codec], kModeName[applied_.mode], kUsageName[applied_.usage],
                 encW_, encH_, applied_.bitrateKbps, applied_.maxrateKbps, applied_.qp,
                 fps, worst, encMs, decMs, latMs, mbps, g_vsync.load() ? 1 : 0);
        SetWindowTextW(wndB_, buf);
        std::wcout << L"[stat] " << kCodecName[applied_.codec] << L" " << kModeName[applied_.mode]
                   << L" " << kUsageName[applied_.usage] << L" " << encW_ << L"x" << encH_
                   << L" | loop " << fps << L"fps worst " << worst << L"ms map " << mapMs_
                   << L"ms | enc " << encMs << L"ms dec " << decMs << L"ms lat " << latMs
                   << L"ms | ~" << mbps
                   << L" Mb/s enc#=" << pts_ << L" dec#=" << rx_.decFrames.load()
                   << L" shown=" << (decoded_.valid() ? 1 : 0)
                   << L" vsync=" << (g_vsync.load() ? 1 : 0)
                   << L" wstage=" << wstage_.load() << L" inflight=" << (jobInFlight_.load() ? 1 : 0)
                   << L" encErr=" << encErrs_.load()
                   << L" skp=" << drops_ << L" bfr=" << reorder_.load()
                   << L" hwdec=" << (rx_.hwdec.load() ? 1 : 0)
                   << L" rxconn=" << (sender_.connected() ? 1 : 0) << L"\n";
    }

    OutputRef ref_;
    HWND wndB_ = nullptr;
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
    DXGI_FORMAT rtvFmt_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> frameTex_;
    ComPtr<ID3D11ShaderResourceView> frameSRV_;
    int frW_ = 0, frH_ = 0; bool haveFrame_ = false;
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
    Encoder enc_;
    EncConfig applied_;
    int capW_ = 0, capH_ = 0, encW_ = 0, encH_ = 0;
    int64_t pts_ = 0;

    // CTMS stream: sender (host) + local receiver feeding window B
    StreamSender sender_;
    StreamReceiver rx_;
    int lastSentX_ = INT_MIN, lastSentY_ = INT_MIN; bool lastSentVis_ = false;
    ComPtr<ID3D11Texture2D> rxCurTex_;
    ComPtr<ID3D11ShaderResourceView> rxCurSRV_;
    int rxCurW_ = 0, rxCurH_ = 0;

    // codec worker handoff
    std::thread codecThread_;
    std::mutex jobMtx_;
    std::condition_variable jobCv_;
    bool jobReady_ = false, workerExit_ = false;
    const uint8_t *jobY_ = nullptr, *jobUV_ = nullptr;
    int jobPY_ = 0, jobPUV_ = 0; int64_t jobPts_ = 0;
    std::atomic<bool> jobInFlight_{false};   // dispatch .. collect
    std::atomic<bool> jobDone_{false};       // worker -> render thread
    int subNew_ = -1, subOld_ = -1, inFlightSlot_ = -1;   // latest-wins reservoir
    double submitT_[8] = {};                 // slot -> capture-submit time
    int drops_ = 0;                          // frames replaced before encoding

    // stats
    std::atomic<double> encMsA_{0};
    std::atomic<uint64_t> winBytes_{0};
    std::atomic<int> wstage_{0};               // 0 idle, 1 encoding
    std::atomic<int> encErrs_{0};
    std::atomic<int> reorder_{0};              // packets with pts != dts (B-frames)
    double dispatchT_[128] = {};               // pts & 127 -> dispatch time
    int statLoops_ = 0;
    double worstLoop_ = 0, mapMs_ = 0, lastStat_ = 0;
};

static int run_dual(OutputRef ref)
{
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
    ShowWindow(wndA, SW_SHOW);
    ShowWindow(wndB, SW_SHOW);
    std::wcout << L"\nHotkeys (focus either window):  C=codec  M=rate-mode  R=resolution  U=usage"
                  L"  V=vsync  Up/Down=bitrate(or qp)  Left/Right=maxrate  Esc=quit\n";

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
