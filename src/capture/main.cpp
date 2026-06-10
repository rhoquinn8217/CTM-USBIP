// ctm-capture — grab a Windows display (e.g. the VDD virtual monitor) via DXGI
// Desktop Duplication and show it scaled in a resizable window.
//
//   ctm-capture --list                 enumerate outputs and exit
//   ctm-capture --shot <idx> <png>     capture one frame from output <idx> to a PNG
//   ctm-capture [--output <idx>]       live resizable preview (default: last output)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>      // IStream for gdiplus under WIN32_LEAN_AND_MEAN
#include <objidl.h>      // PROPID for gdiplus
#include <gdiplus.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "gdiplus.lib")

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
static const char *kVSQuad = R"(
cbuffer Q : register(b0) { float4 rect; float4 extra; };
struct QO { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
QO qmain(uint id : SV_VertexID) {
    float2 t = float2(id & 1, (id >> 1) & 1); QO o; o.uv = t;
    o.pos = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0, 1); return o;
})";
static const char *kPSCursor = R"(
Texture2D ctex : register(t0); SamplerState csmp : register(s0);
cbuffer Q : register(b0) { float4 rect; float4 extra; };  // extra.x = paperwhite scale
float3 s2l(float3 c){ return (c <= 0.04045) ? c/12.92 : pow((c+0.055)/1.055, 2.4); }
float4 cmain(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {
    float4 c = ctex.Sample(csmp, uv);
    return float4(s2l(saturate(c.rgb)) * extra.x, c.a);   // match desktop SDR-white
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

    // live preview: --output N, else default to the last output (the newly added one)
    int idx = (int)outs.size() - 1;
    if (a1 == L"--output" && argc >= 3) idx = _wtoi(argv[2]);
    if (idx < 0 || idx >= (int)outs.size()) { std::wcerr << L"bad output index\n"; return 2; }
    list_outputs(outs);
    std::wcout << L"capturing output [" << idx << L"] " << outs[idx].desc.DeviceName << L"\n";
    return run_preview(outs[idx]);
}
