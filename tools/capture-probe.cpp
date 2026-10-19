// Screen capture, stage 0 (docs/design-capture.md, section 6): measures the
// numbers that decide the branches of section 4 before any real capture code
// is written. Plain Win32 + C++/WinRT, no Qt, the C runtime linked in, so its
// timings are the capture paths' own and not a framework's.
//
//   capture-probe bitblt       BitBlt of the virtual desktop and each monitor
//   capture-probe wgc          Windows.Graphics.Capture, one frame per monitor
//   capture-probe border       borderless capture access and the yellow border
//   capture-probe hdr          advanced colour, SDR white level, scRGB mapping
//   capture-probe compare      BitBlt against WGC, pixel by pixel (SDR)
//   capture-probe start-menu   hotkey and capture while Start is open (input!)
//   capture-probe prtscn       taking over PrtScn from the Snipping Tool (input!)
//   capture-probe all          the five above without input, then the two with
//
// Every line it prints is a result; a summary table follows at the end.
// Whatever it changes (hotkeys, the PrtScn registry value, windows, Start
// being open) is put back before it exits, also when a step fails.

// C++/WinRT first, as in src/app/platform/TextRecognizer.cpp.
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <windows.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <wincodec.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

// ---------------------------------------------------------------------------
// Output: every result line goes to stdout at once, and the important ones
// also into the summary printed at the end.

struct SummaryRow {
    std::string item;
    std::string value;
};
std::vector<SummaryRow> g_summary;

std::string format(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    return buf;
}

std::wstring wformat(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    return buf;
}

void line(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void summary(const std::string& item, const std::string& value)
{
    g_summary.push_back({item, value});
}

std::string narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// Timing.

double nowMs()
{
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

// Runs the thread's messages while waiting, so the windows and the thread
// hotkeys the probe owns keep working; returns once done() is true or the
// time is up.
bool pumpUntil(double timeoutMs, const std::function<bool()>& done, const std::function<void(const MSG&)>& onMessage = {})
{
    const double end = nowMs() + timeoutMs;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (onMessage)
                onMessage(msg);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done())
            return true;
        const double left = end - nowMs();
        if (left <= 0)
            return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(std::min(left, 2.0)) + 1, QS_ALLINPUT);
    }
}

void pumpFor(double ms)
{
    pumpUntil(ms, [] { return false; });
}

// ---------------------------------------------------------------------------
// Monitors, in physical pixels: the process is per-monitor aware (v2) from
// the first line of main, so nothing here is virtualised.

struct Monitor {
    HMONITOR handle = nullptr;
    RECT rect{};
    std::wstring device; // \\.\DISPLAY1, matches DISPLAYCONFIG's GDI name
    bool primary = false;
    UINT dpi = 96;
};

std::vector<Monitor> monitors()
{
    std::vector<Monitor> list;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM p) -> BOOL {
        MONITORINFOEXW info{};
        info.cbSize = sizeof info;
        GetMonitorInfoW(h, &info);
        Monitor m;
        m.handle = h;
        m.rect = info.rcMonitor;
        m.device = info.szDevice;
        m.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        UINT dx = 96, dy = 96;
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        static auto getDpi = reinterpret_cast<GetDpiForMonitorFn>(
            reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor")));
        if (getDpi)
            getDpi(h, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy);
        m.dpi = dx;
        reinterpret_cast<std::vector<Monitor>*>(p)->push_back(m);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&list));
    return list;
}

int width(const RECT& r) { return r.right - r.left; }
int height(const RECT& r) { return r.bottom - r.top; }

// ---------------------------------------------------------------------------
// Pictures: 32-bit BGRA rows, top-down, no padding.

struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> px;
    uint32_t at(int x, int y) const { return px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)]; }
};

int channel(uint32_t p, int shift) { return static_cast<int>((p >> shift) & 0xFF); }

int rgbDiff(uint32_t a, uint32_t b)
{
    return std::max({std::abs(channel(a, 0) - channel(b, 0)), std::abs(channel(a, 8) - channel(b, 8)),
        std::abs(channel(a, 16) - channel(b, 16))});
}

// Saves through WIC; the alpha BitBlt leaves at 0 is forced opaque.
bool savePng(const Image& img, const std::wstring& path)
{
    winrt::com_ptr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))))
        return false;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(stream.put())) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))
        || FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()))
        || FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))
        || FAILED(encoder->CreateNewFrame(frame.put(), nullptr)) || FAILED(frame->Initialize(nullptr)))
        return false;
    frame->SetSize(static_cast<UINT>(img.w), static_cast<UINT>(img.h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    std::vector<uint32_t> opaque(img.px);
    for (auto& p : opaque)
        p |= 0xFF000000u;
    if (FAILED(frame->WritePixels(static_cast<UINT>(img.h), static_cast<UINT>(img.w) * 4,
            static_cast<UINT>(opaque.size() * 4), reinterpret_cast<BYTE*>(opaque.data()))))
        return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

std::wstring g_outDir;

std::wstring outPath(const std::wstring& name)
{
    return g_outDir + L"\\" + name;
}

// ---------------------------------------------------------------------------
// BitBlt: the screen DC into a top-down DIB section, the whole sequence a
// capture would run (DCs, DIB, blit, copy out), CAPTUREBLT so layered windows
// are in it.

Image bitblt(const RECT& r)
{
    Image img;
    img.w = width(r);
    img.h = height(r);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = img.w;
    bi.bmiHeader.biHeight = -img.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, img.w, img.h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    img.px.assign(static_cast<const uint32_t*>(bits), static_cast<const uint32_t*>(bits) + static_cast<size_t>(img.w) * img.h);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}

RECT virtualDesktop()
{
    RECT r;
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

// Cold (the first in the process) and the median of ten warm runs.
void timeBitblt(const std::string& name, const RECT& r)
{
    const double t0 = nowMs();
    Image first = bitblt(r);
    const double cold = nowMs() - t0;
    std::vector<double> warm;
    for (int i = 0; i < 10; ++i) {
        const double t = nowMs();
        Image again = bitblt(r);
        warm.push_back(nowMs() - t);
    }
    const auto mm = std::minmax_element(warm.begin(), warm.end());
    line("[bitblt] %s %dx%d: cold %.1f ms, warm median %.1f ms (min %.1f, max %.1f)", name.c_str(), first.w, first.h,
        cold, median(warm), *mm.first, *mm.second);
    summary("bitblt " + name, format("%dx%d cold %.1f ms, warm %.1f ms", first.w, first.h, cold, median(warm)));
}

int cmdBitblt()
{
    // The first GDI call in a process pays for loading gdi32full and the
    // like; that is part of "cold" only if nothing else ran before, so the
    // virtual desktop goes first.
    timeBitblt("virtual desktop", virtualDesktop());
    const auto list = monitors();
    for (size_t i = 0; i < list.size(); ++i)
        timeBitblt(format("monitor %zu", i), list[i].rect);
    return 0;
}

// ---------------------------------------------------------------------------
// Windows.Graphics.Capture.

struct Device {
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID3D11DeviceContext> context;
    wd3d::IDirect3DDevice winrt{nullptr};
};

Device createDevice()
{
    Device dev;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, dev.d3d.put(), nullptr, dev.context.put()));
    auto dxgi = dev.d3d.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    dev.winrt = inspectable.as<wd3d::IDirect3DDevice>();
    return dev;
}

wgc::GraphicsCaptureItem itemForMonitor(HMONITOR monitor)
{
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

struct Grab {
    bool ok = false;
    std::string error;
    double itemMs = 0; // CreateForMonitor
    double firstFrameMs = 0; // frame pool + session + StartCapture until the first frame
    double copyMs = 0; // texture to CPU memory
    bool borderSet = false;
    std::string borderError;
    wdx::DirectXPixelFormat format = wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    int w = 0;
    int h = 0;
    std::vector<uint8_t> bytes; // tightly packed rows
    Image bgra() const
    {
        Image img;
        img.w = w;
        img.h = h;
        img.px.resize(static_cast<size_t>(w) * h);
        std::memcpy(img.px.data(), bytes.data(), img.px.size() * 4);
        return img;
    }
};

// One frame of a monitor. border: leave the session's default, or ask for
// IsBorderRequired(value). whileLive runs while the session is still
// capturing (the yellow border is drawn on the screen only then). reuse: a
// capture item made earlier, which saves CreateForMonitor's time.
Grab grabMonitor(const Device& dev, HMONITOR monitor, wdx::DirectXPixelFormat pixelFormat, int border = -1,
    const std::function<void()>& whileLive = {}, wgc::GraphicsCaptureItem reuse = nullptr)
{
    Grab g;
    g.format = pixelFormat;
    try {
        double t = nowMs();
        auto item = reuse ? reuse : itemForMonitor(monitor);
        g.itemMs = nowMs() - t;

        t = nowMs();
        auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(dev.winrt, pixelFormat, 1, item.Size());
        auto session = pool.CreateCaptureSession(item);
        // BitBlt has no cursor in it: leave it out here too, or every
        // compare shows a cursor-shaped difference.
        session.IsCursorCaptureEnabled(false);
        if (border >= 0) {
            try {
                session.IsBorderRequired(border != 0);
                g.borderSet = true;
            } catch (const winrt::hresult_error& e) {
                g.borderError = format("0x%08X %s", static_cast<unsigned>(e.code().value), narrow(std::wstring(e.message())).c_str());
            }
        }
        HANDLE arrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<bool> taken{false};
        wgc::Direct3D11CaptureFrame frame{nullptr};
        auto token = pool.FrameArrived([&](const wgc::Direct3D11CaptureFramePool& sender, const auto&) {
            if (taken.exchange(true))
                return;
            frame = sender.TryGetNextFrame();
            SetEvent(arrived);
        });
        session.StartCapture();
        const DWORD wait = WaitForSingleObject(arrived, 3000);
        g.firstFrameMs = nowMs() - t;
        if (wait != WAIT_OBJECT_0 || !frame) {
            g.error = "no frame within 3 s";
        } else {
            t = nowMs();
            winrt::com_ptr<ID3D11Texture2D> texture;
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
            D3D11_TEXTURE2D_DESC desc;
            texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            winrt::com_ptr<ID3D11Texture2D> staging;
            winrt::check_hresult(dev.d3d->CreateTexture2D(&desc, nullptr, staging.put()));
            dev.context->CopyResource(staging.get(), texture.get());
            D3D11_MAPPED_SUBRESOURCE mapped;
            winrt::check_hresult(dev.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
            const size_t pixelBytes = pixelFormat == wdx::DirectXPixelFormat::R16G16B16A16Float ? 8 : 4;
            g.w = static_cast<int>(desc.Width);
            g.h = static_cast<int>(desc.Height);
            const size_t row = pixelBytes * desc.Width;
            g.bytes.resize(row * desc.Height);
            for (UINT y = 0; y < desc.Height; ++y)
                std::memcpy(g.bytes.data() + row * y, static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(mapped.RowPitch) * y, row);
            dev.context->Unmap(staging.get(), 0);
            g.copyMs = nowMs() - t;
            g.ok = true;
        }
        if (whileLive)
            whileLive();
        pool.FrameArrived(token);
        if (frame)
            frame.Close();
        session.Close();
        pool.Close();
        CloseHandle(arrived);
    } catch (const winrt::hresult_error& e) {
        g.error = format("0x%08X %s", static_cast<unsigned>(e.code().value), narrow(std::wstring(e.message())).c_str());
    }
    return g;
}

// ---------------------------------------------------------------------------
// Advanced colour per monitor: DISPLAYCONFIG, matched to the monitor by its
// GDI device name, and DXGI's view of the same output.

struct ColorInfo {
    bool found = false;
    bool acSupported = false;
    bool acEnabled = false;
    bool wideForced = false;
    int bitsPerChannel = 0;
    int encoding = -1;
    // _2 (Windows 11 24H2 and later)
    bool have2 = false;
    std::string mode2;
    bool hdrSupported = false;
    bool hdrUserEnabled = false;
    // SDR white level
    bool haveWhite = false;
    double sdrWhiteNits = 80;
    // DXGI
    bool haveDxgi = false;
    std::string colorSpace;
    double maxNits = 0;
    double minNits = 0;
    int dxgiBits = 0;
    bool hdr() const { return have2 ? mode2 == "HDR" : (acEnabled && colorSpace == "G2084 (HDR10)"); }
};

ColorInfo colorInfo(const Monitor& m)
{
    ColorInfo ci;
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) == ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < nPaths; ++i) {
                const auto& p = paths[i];
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                source.header.size = sizeof source;
                source.header.adapterId = p.sourceInfo.adapterId;
                source.header.id = p.sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || m.device != source.viewGdiDeviceName)
                    continue;
                ci.found = true;
                DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ac{};
                ac.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
                ac.header.size = sizeof ac;
                ac.header.adapterId = p.targetInfo.adapterId;
                ac.header.id = p.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&ac.header) == ERROR_SUCCESS) {
                    ci.acSupported = ac.advancedColorSupported;
                    ci.acEnabled = ac.advancedColorEnabled;
                    ci.wideForced = ac.wideColorEnforced;
                    ci.bitsPerChannel = static_cast<int>(ac.bitsPerColorChannel);
                    ci.encoding = static_cast<int>(ac.colorEncoding);
                }
                DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 ac2{};
                ac2.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
                ac2.header.size = sizeof ac2;
                ac2.header.adapterId = p.targetInfo.adapterId;
                ac2.header.id = p.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&ac2.header) == ERROR_SUCCESS) {
                    ci.have2 = true;
                    ci.mode2 = ac2.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR ? "HDR"
                        : ac2.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG     ? "WCG"
                                                                                           : "SDR";
                    ci.hdrSupported = ac2.highDynamicRangeSupported;
                    ci.hdrUserEnabled = ac2.highDynamicRangeUserEnabled;
                }
                // The SDR white level belongs to the target too, in
                // thousandths of 80 nits (scRGB 1.0).
                DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
                white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
                white.header.size = sizeof white;
                white.header.adapterId = p.targetInfo.adapterId;
                white.header.id = p.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) {
                    ci.haveWhite = true;
                    ci.sdrWhiteNits = white.SDRWhiteLevel / 1000.0 * 80.0;
                }
                break;
            }
        }
    }
    winrt::com_ptr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        for (UINT a = 0; !ci.haveDxgi && factory->EnumAdapters1(a, adapter.put()) == S_OK; ++a, adapter = nullptr) {
            winrt::com_ptr<IDXGIOutput> output;
            for (UINT o = 0; adapter->EnumOutputs(o, output.put()) == S_OK; ++o, output = nullptr) {
                auto output6 = output.try_as<IDXGIOutput6>();
                DXGI_OUTPUT_DESC1 d{};
                if (!output6 || FAILED(output6->GetDesc1(&d)) || d.Monitor != m.handle)
                    continue;
                ci.haveDxgi = true;
                ci.colorSpace = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? "G2084 (HDR10)"
                    : d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709             ? "G22 P709 (SDR)"
                                                                                           : format("%d", static_cast<int>(d.ColorSpace));
                ci.maxNits = d.MaxLuminance;
                ci.minNits = d.MinLuminance;
                ci.dxgiBits = static_cast<int>(d.BitsPerColor);
                break;
            }
        }
    }
    return ci;
}

// ---------------------------------------------------------------------------
// scRGB (linear, 1.0 = 80 nits, sRGB primaries) to 8-bit sRGB. Written for
// WinShun from the definitions (IEC 61966-2-1 for the sRGB curve, Reinhard
// et al. 2002 for the extended operator); no code taken from elsewhere.
//
// Windows composes SDR content at the monitor's SDR white level, so a white
// SDR pixel arrives as sdrWhiteNits / 80. Dividing by that puts SDR content
// back at 0..1 exactly. When nothing in the frame is brighter than that, the
// result is the SDR picture untouched; otherwise everything above the knee is
// rolled off with the extended Reinhard curve, whose white point is the
// frame's brightest value, so the brightest highlight just reaches 1.0 and
// none clips. Scaling all three channels by the same factor keeps the hue.

float halfToFloat(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else { // subnormal half: normalise it
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint8_t encodeSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    const float v = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(v * 255.0f));
}

float decodeSrgb(uint8_t b)
{
    const float v = b / 255.0f;
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

// rgb: linear scRGB triples, w*h of them.
Image mapScRgbToSdr(const std::vector<float>& rgb, int w, int h, double sdrWhiteNits, float* brightestOut = nullptr)
{
    const float scale = static_cast<float>(80.0 / sdrWhiteNits);
    const size_t n = static_cast<size_t>(w) * h;
    float brightest = 0;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c)
            brightest = std::max(brightest, rgb[i * 3 + c] * scale);
    if (brightestOut)
        *brightestOut = brightest;
    const float knee = 0.8f;
    const bool roll = brightest > 1.0f + 1e-4f;
    // The white point of the roll-off, in units of the room above the knee.
    const float wp = (brightest - knee) / (1.0f - knee);
    Image out;
    out.w = w;
    out.h = h;
    out.px.resize(n);
    for (size_t i = 0; i < n; ++i) {
        // Out-of-gamut (negative) components have no sRGB value: clip them.
        float r = std::max(0.0f, rgb[i * 3] * scale);
        float g = std::max(0.0f, rgb[i * 3 + 1] * scale);
        float b = std::max(0.0f, rgb[i * 3 + 2] * scale);
        const float m = std::max({r, g, b});
        if (roll && m > knee) {
            const float t = (m - knee) / (1.0f - knee);
            const float mapped = knee + (1.0f - knee) * (t * (1.0f + t / (wp * wp)) / (1.0f + t));
            const float f = mapped / m;
            r *= f;
            g *= f;
            b *= f;
        }
        out.px[i] = 0xFF000000u | (uint32_t(encodeSrgb(r)) << 16) | (uint32_t(encodeSrgb(g)) << 8) | encodeSrgb(b);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Comparing pictures.

struct DiffStats {
    long long differing = 0;
    int maxDiff = 0;
    RECT box{INT_MAX, INT_MAX, INT_MIN, INT_MIN};
};

DiffStats diff(const Image& a, const Image& b, int threshold = 0, const RECT* only = nullptr)
{
    DiffStats s;
    const int w = std::min(a.w, b.w), h = std::min(a.h, b.h);
    RECT r = only ? *only : RECT{0, 0, w, h};
    r.left = std::max<LONG>(r.left, 0);
    r.top = std::max<LONG>(r.top, 0);
    r.right = std::min<LONG>(r.right, w);
    r.bottom = std::min<LONG>(r.bottom, h);
    for (int y = r.top; y < r.bottom; ++y) {
        for (int x = r.left; x < r.right; ++x) {
            const int d = rgbDiff(a.at(x, y), b.at(x, y));
            s.maxDiff = std::max(s.maxDiff, d);
            if (d > threshold) {
                ++s.differing;
                s.box.left = std::min<LONG>(s.box.left, x);
                s.box.top = std::min<LONG>(s.box.top, y);
                s.box.right = std::max<LONG>(s.box.right, x + 1);
                s.box.bottom = std::max<LONG>(s.box.bottom, y + 1);
            }
        }
    }
    return s;
}

std::string boxText(const DiffStats& s)
{
    if (!s.differing)
        return "none";
    return format("x %ld..%ld, y %ld..%ld", s.box.left, s.box.right - 1, s.box.top, s.box.bottom - 1);
}

// ---------------------------------------------------------------------------
// wgc

int cmdWgc()
{
    if (!wgc::GraphicsCaptureSession::IsSupported()) {
        line("[wgc] skipped: GraphicsCaptureSession::IsSupported() is false");
        summary("wgc", "skipped: not supported");
        return 1;
    }
    double t = nowMs();
    Device dev;
    try {
        dev = createDevice();
    } catch (const winrt::hresult_error& e) {
        line("[wgc] D3D11 device failed: 0x%08X", static_cast<unsigned>(e.code().value));
        summary("wgc", "D3D11 device failed");
        return 1;
    }
    const double deviceMs = nowMs() - t;
    line("[wgc] D3D11 device + IDirect3DDevice (cold): %.1f ms", deviceMs);
    summary("wgc device creation (cold)", format("%.1f ms", deviceMs));
    // A second device in the same process: the DLLs are loaded by now, so
    // this is what creating one costs in an app that already uses D3D.
    t = nowMs();
    Device second = createDevice();
    const double device2Ms = nowMs() - t;
    line("[wgc] D3D11 device + IDirect3DDevice (second in the process): %.1f ms", device2Ms);
    summary("wgc device creation (second)", format("%.1f ms", device2Ms));

    const auto list = monitors();
    for (size_t i = 0; i < list.size(); ++i) {
        const ColorInfo ci = colorInfo(list[i]);
        for (int run = 0; run < 2; ++run) {
            const char* which = run == 0 ? "first" : "again, same device";
            Grab g = grabMonitor(dev, list[i].handle, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized);
            if (!g.ok) {
                line("[wgc] monitor %zu BGRA8 %s: FAILED %s", i, which, g.error.c_str());
                summary(format("wgc monitor %zu BGRA8 %s", i, which), "failed: " + g.error);
                continue;
            }
            line("[wgc] monitor %zu BGRA8 %s: %dx%d, item %.1f ms, session to first frame %.1f ms, copy to CPU %.1f ms, total %.1f ms",
                i, which, g.w, g.h, g.itemMs, g.firstFrameMs, g.copyMs, g.itemMs + g.firstFrameMs + g.copyMs);
            summary(format("wgc monitor %zu BGRA8 %s", i, which),
                format("item %.1f + first frame %.1f + copy %.1f = %.1f ms", g.itemMs, g.firstFrameMs, g.copyMs,
                    g.itemMs + g.firstFrameMs + g.copyMs));
        }
        // Warm median over ten more, the number a pre-created device would
        // see on every hotkey press.
        std::vector<double> totals, frames;
        for (int k = 0; k < 10; ++k) {
            Grab g = grabMonitor(dev, list[i].handle, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized);
            if (g.ok) {
                totals.push_back(g.itemMs + g.firstFrameMs + g.copyMs);
                frames.push_back(g.firstFrameMs);
            }
        }
        line("[wgc] monitor %zu BGRA8 warm, 10 runs: total median %.1f ms, first frame median %.1f ms", i, median(totals), median(frames));
        summary(format("wgc monitor %zu BGRA8 warm median", i), format("%.1f ms (first frame %.1f)", median(totals), median(frames)));
        // The same with the capture item kept from one press to the next
        // (CreateForMonitor is a large share of the above).
        {
            auto kept = itemForMonitor(list[i].handle);
            std::vector<double> keptTotals, keptFrames, keptCopies;
            for (int k = 0; k < 10; ++k) {
                Grab g = grabMonitor(second, list[i].handle, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, -1, {}, kept);
                if (g.ok) {
                    keptTotals.push_back(g.itemMs + g.firstFrameMs + g.copyMs);
                    keptFrames.push_back(g.firstFrameMs);
                    keptCopies.push_back(g.copyMs);
                }
            }
            line("[wgc] monitor %zu BGRA8 warm, item kept, 10 runs: total median %.1f ms (first frame %.1f, copy %.1f)", i,
                median(keptTotals), median(keptFrames), median(keptCopies));
            summary(format("wgc monitor %zu BGRA8 warm, item kept", i),
                format("%.1f ms (first frame %.1f, copy %.1f)", median(keptTotals), median(keptFrames), median(keptCopies)));
        }
        if (ci.hdr()) {
            Grab g = grabMonitor(dev, list[i].handle, wdx::DirectXPixelFormat::R16G16B16A16Float);
            line("[wgc] monitor %zu FP16 (HDR): %s item %.1f, first frame %.1f, copy %.1f ms", i, g.ok ? "ok" : g.error.c_str(),
                g.itemMs, g.firstFrameMs, g.copyMs);
            summary(format("wgc monitor %zu FP16", i), g.ok ? format("%.1f ms total", g.itemMs + g.firstFrameMs + g.copyMs) : g.error);
        } else {
            line("[wgc] monitor %zu FP16: skipped: the monitor is not in HDR mode", i);
            summary(format("wgc monitor %zu FP16", i), "skipped: not HDR");
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// border

// Pixels in the outer band of a monitor that look like the capture border
// (Windows 11 draws it in a saturated yellow-orange).
long long yellowInBand(const Image& img, int band)
{
    long long n = 0;
    for (int y = 0; y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            if (x >= band && x < img.w - band && y >= band && y < img.h - band)
                continue;
            const uint32_t p = img.at(x, y);
            const int r = channel(p, 16), g = channel(p, 8), b = channel(p, 0);
            if (r > 200 && g > 120 && g < 230 && b < 90)
                ++n;
        }
    }
    return n;
}

long long bandDiff(const Image& a, const Image& b, int band)
{
    long long n = 0;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) {
            if (x >= band && x < a.w - band && y >= band && y < a.h - band)
                continue;
            if (rgbDiff(a.at(x, y), b.at(x, y)) > 40)
                ++n;
        }
    return n;
}

int cmdBorder()
{
    using winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus;
    std::string access;
    try {
        const auto status = wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
        switch (status) {
        case AppCapabilityAccessStatus::Allowed: access = "Allowed"; break;
        case AppCapabilityAccessStatus::DeniedByUser: access = "DeniedByUser"; break;
        case AppCapabilityAccessStatus::DeniedBySystem: access = "DeniedBySystem"; break;
        case AppCapabilityAccessStatus::NotDeclaredByApp: access = "NotDeclaredByApp"; break;
        case AppCapabilityAccessStatus::UserPromptRequired: access = "UserPromptRequired"; break;
        default: access = format("%d", static_cast<int>(status)); break;
        }
    } catch (const winrt::hresult_error& e) {
        access = format("threw 0x%08X %s", static_cast<unsigned>(e.code().value), narrow(std::wstring(e.message())).c_str());
    }
    line("[border] RequestAccessAsync(Borderless) from an unpackaged medium-integrity exe: %s", access.c_str());
    summary("border RequestAccessAsync(Borderless)", access);

    Device dev = createDevice();
    const auto list = monitors();
    const Monitor& m = list.front();
    const int band = 6; // physical px; the border is a few px wide
    // The control: the screen with no capture running.
    const Image before = bitblt(m.rect);
    for (int required : {1, 0}) {
        Image during;
        Grab g = grabMonitor(dev, m.handle, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, required, [&] {
            // Give DWM time to draw the border, then look at the screen.
            Sleep(400);
            during = bitblt(m.rect);
        });
        const char* name = required ? "IsBorderRequired(true)" : "IsBorderRequired(false)";
        if (!g.ok) {
            line("[border] %s: capture failed: %s", name, g.error.c_str());
            summary(format("border %s", name), "capture failed: " + g.error);
            continue;
        }
        const long long screenChanged = bandDiff(before, during, band);
        const long long screenYellow = yellowInBand(during, band);
        const long long frameYellow = yellowInBand(g.bgra(), band);
        line("[border] %s: setter %s; on screen during capture: %lld edge px changed vs before, %lld yellow edge px (before: %lld);"
             " in the WGC frame: %lld yellow edge px",
            name, g.borderSet ? "accepted" : ("threw " + g.borderError).c_str(), screenChanged, screenYellow,
            yellowInBand(before, band), frameYellow);
        summary(format("border %s", name),
            format("setter %s, screen edge changed %lld px, yellow %lld px, frame yellow %lld px",
                g.borderSet ? "ok" : "threw", screenChanged, screenYellow, frameYellow));
        if (required)
            savePng(during, outPath(L"border-required-screen.png"));
        else
            savePng(during, outPath(L"border-not-required-screen.png"));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// hdr

int cmdHdr()
{
    const auto list = monitors();
    bool anyHdr = false;
    for (size_t i = 0; i < list.size(); ++i) {
        const ColorInfo ci = colorInfo(list[i]);
        if (!ci.found) {
            line("[hdr] monitor %zu (%s): no DISPLAYCONFIG path matched", i, narrow(list[i].device).c_str());
            continue;
        }
        line("[hdr] monitor %zu (%s): advanced colour supported %d, enabled %d, wide colour enforced %d, %d bits/channel, encoding %d",
            i, narrow(list[i].device).c_str(), ci.acSupported, ci.acEnabled, ci.wideForced, ci.bitsPerChannel, ci.encoding);
        if (ci.have2)
            line("[hdr] monitor %zu ADVANCED_COLOR_INFO_2: active mode %s, HDR supported %d, HDR user-enabled %d", i, ci.mode2.c_str(),
                ci.hdrSupported, ci.hdrUserEnabled);
        else
            line("[hdr] monitor %zu ADVANCED_COLOR_INFO_2: not available (older Windows)", i);
        line("[hdr] monitor %zu SDR white level: %s", i, ci.haveWhite ? format("%.0f nits", ci.sdrWhiteNits).c_str() : "not available");
        if (ci.haveDxgi)
            line("[hdr] monitor %zu DXGI: colour space %s, max %.0f nits, min %.3f nits, %d bits", i, ci.colorSpace.c_str(), ci.maxNits,
                ci.minNits, ci.dxgiBits);
        summary(format("hdr monitor %zu", i),
            format("mode %s, AC supported %d enabled %d, HDR supported %d, SDR white %.0f nits, DXGI %s",
                ci.have2 ? ci.mode2.c_str() : (ci.acEnabled ? "advanced" : "SDR"), ci.acSupported, ci.acEnabled, ci.hdrSupported,
                ci.sdrWhiteNits, ci.colorSpace.c_str()));
        if (!ci.hdr())
            continue;
        anyHdr = true;
        Device dev = createDevice();
        const Image blt = bitblt(list[i].rect);
        savePng(blt, outPath(wformat(L"hdr-monitor%zu-bitblt.png", i)));
        Grab g = grabMonitor(dev, list[i].handle, wdx::DirectXPixelFormat::R16G16B16A16Float);
        if (!g.ok) {
            line("[hdr] monitor %zu FP16 capture failed: %s", i, g.error.c_str());
            continue;
        }
        std::vector<float> rgb(static_cast<size_t>(g.w) * g.h * 3);
        const auto* half = reinterpret_cast<const uint16_t*>(g.bytes.data());
        for (size_t p = 0; p < static_cast<size_t>(g.w) * g.h; ++p)
            for (int c = 0; c < 3; ++c)
                rgb[p * 3 + c] = halfToFloat(half[p * 4 + c]);
        float brightest = 0;
        const double t = nowMs();
        const Image mapped = mapScRgbToSdr(rgb, g.w, g.h, ci.sdrWhiteNits, &brightest);
        const double mapMs = nowMs() - t;
        savePng(mapped, outPath(wformat(L"hdr-monitor%zu-wgc-mapped.png", i)));
        const DiffStats d = diff(blt, mapped, 8);
        line("[hdr] monitor %zu: brightest %.2f x SDR white, mapping %.1f ms; mapped vs BitBlt: %lld px differ by > 8, max %d; PNGs written",
            i, brightest, mapMs, d.differing, d.maxDiff);
        summary(format("hdr monitor %zu mapped vs BitBlt", i), format("%lld px > 8, max %d", d.differing, d.maxDiff));
    }
    if (!anyHdr) {
        line("[hdr] FP16 capture and mapping: skipped: no monitor is in HDR mode");
        summary("hdr capture + mapping", "skipped: no HDR monitor");
    }

    // The mapping's own check, which needs no HDR monitor: an SDR capture
    // lifted to scRGB at a 240-nit white level (as Windows would compose it)
    // must come back unchanged, and a frame with highlights must stay in
    // range, monotonic, and leave the SDR part almost unchanged.
    try {
        Device dev = createDevice();
        Grab g = grabMonitor(dev, list.front().handle, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized);
        if (g.ok) {
            const Image src = g.bgra();
            const double white = 240;
            std::vector<float> rgb(static_cast<size_t>(src.w) * src.h * 3);
            for (size_t p = 0; p < src.px.size(); ++p) {
                rgb[p * 3] = decodeSrgb(static_cast<uint8_t>(channel(src.px[p], 16))) * static_cast<float>(white / 80);
                rgb[p * 3 + 1] = decodeSrgb(static_cast<uint8_t>(channel(src.px[p], 8))) * static_cast<float>(white / 80);
                rgb[p * 3 + 2] = decodeSrgb(static_cast<uint8_t>(channel(src.px[p], 0))) * static_cast<float>(white / 80);
            }
            const double t = nowMs();
            const Image back = mapScRgbToSdr(rgb, src.w, src.h, white);
            const double ms = nowMs() - t;
            const DiffStats d = diff(src, back, 0);
            // Now a highlight 4x SDR white in the top-left 200x200.
            for (int y = 0; y < std::min(200, src.h); ++y)
                for (int x = 0; x < std::min(200, src.w); ++x)
                    for (int c = 0; c < 3; ++c)
                        rgb[(static_cast<size_t>(y) * src.w + x) * 3 + c] = static_cast<float>(4 * white / 80) * (x + 1) / 200.0f;
            const Image rolled = mapScRgbToSdr(rgb, src.w, src.h, white);
            RECT rest{0, 200, src.w, src.h};
            const DiffStats dr = diff(src, rolled, 0, &rest);
            bool monotonic = true;
            for (int x = 1; x < std::min(200, src.w); ++x)
                if (channel(rolled.at(x, 0), 16) < channel(rolled.at(x - 1, 0), 16))
                    monotonic = false;
            line("[hdr] mapping self-test (SDR capture lifted to 240-nit scRGB): round trip %lld px differ, max %d, %.1f ms for %dx%d",
                d.differing, d.maxDiff, ms, src.w, src.h);
            line("[hdr] mapping self-test with a 4x highlight: highlight ramp monotonic %d, ends at %d; SDR part max change %d",
                monotonic, channel(rolled.at(std::min(199, src.w - 1), 0), 16), dr.maxDiff);
            summary("hdr mapping self-test", format("round trip max diff %d (%.1f ms); with 4x highlight SDR part max change %d, monotonic %d",
                d.maxDiff, ms, dr.maxDiff, monotonic));
        }
    } catch (const winrt::hresult_error& e) {
        line("[hdr] mapping self-test failed: 0x%08X", static_cast<unsigned>(e.code().value));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// compare

int cmdCompare()
{
    Device dev = createDevice();
    const auto list = monitors();
    for (size_t i = 0; i < list.size(); ++i) {
        const ColorInfo ci = colorInfo(list[i]);
        if (ci.hdr()) {
            line("[compare] monitor %zu: skipped: HDR mode (BitBlt and BGRA8 WGC differ by design there)", i);
            summary(format("compare monitor %zu", i), "skipped: HDR");
            continue;
        }
        // Back to back with nothing printed in between: the console is on
        // the screen too.
        Grab g = grabMonitor(dev, list[i].handle, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized);
        const Image blt = bitblt(list[i].rect);
        Grab g2 = grabMonitor(dev, list[i].handle, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized);
        if (!g.ok || !g2.ok) {
            line("[compare] monitor %zu: WGC failed: %s", i, (g.ok ? g2.error : g.error).c_str());
            continue;
        }
        const Image w1 = g.bgra(), w2 = g2.bgra();
        if (w1.w != blt.w || w1.h != blt.h) {
            line("[compare] monitor %zu: sizes differ: WGC %dx%d, BitBlt %dx%d", i, w1.w, w1.h, blt.w, blt.h);
            summary(format("compare monitor %zu", i), "sizes differ");
            continue;
        }
        const DiffStats d = diff(blt, w1, 0);
        const DiffStats d2 = diff(blt, w2, 0);
        const DiffStats dw = diff(w1, w2, 0); // what changed on the screen meanwhile
        savePng(blt, outPath(wformat(L"compare-monitor%zu-bitblt.png", i)));
        savePng(w1, outPath(wformat(L"compare-monitor%zu-wgc.png", i)));
        line("[compare] monitor %zu %dx%d: BitBlt vs WGC before it: %lld px differ (%.4f%%), max %d, box %s", i, blt.w, blt.h, d.differing,
            100.0 * d.differing / (double(blt.w) * blt.h), d.maxDiff, boxText(d).c_str());
        line("[compare] monitor %zu: BitBlt vs WGC after it: %lld px differ, max %d, box %s", i, d2.differing, d2.maxDiff, boxText(d2).c_str());
        line("[compare] monitor %zu: WGC before vs WGC after (screen changes meanwhile): %lld px, box %s", i, dw.differing, boxText(dw).c_str());
        summary(format("compare monitor %zu BitBlt vs WGC", i),
            format("%lld / %lld px differ, max %d (screen itself changed %lld px)", std::min(d.differing, d2.differing),
                static_cast<long long>(blt.w) * blt.h, std::max(d.maxDiff, d2.maxDiff), dw.differing));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Input and windows, for the two interactive probes.

std::wstring processName(DWORD pid)
{
    std::wstring name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
            name = buf;
            name = name.substr(name.find_last_of(L'\\') + 1);
        }
        CloseHandle(h);
    }
    return name;
}

struct WindowFacts {
    HWND hwnd = nullptr;
    std::wstring cls;
    std::wstring process;
    bool elevated = false;
    bool queryable = true;
};

WindowFacts foreground()
{
    WindowFacts f;
    f.hwnd = GetForegroundWindow();
    if (!f.hwnd)
        return f;
    wchar_t cls[256] = {};
    GetClassNameW(f.hwnd, cls, 256);
    f.cls = cls;
    DWORD pid = 0;
    GetWindowThreadProcessId(f.hwnd, &pid);
    f.process = processName(pid);
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        HANDLE token;
        if (OpenProcessToken(h, TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION e{};
            DWORD n;
            if (GetTokenInformation(token, TokenElevation, &e, sizeof e, &n))
                f.elevated = e.TokenIsElevated != 0;
            CloseHandle(token);
        } else {
            f.queryable = false; // a higher-integrity or protected process
        }
        CloseHandle(h);
    } else {
        f.queryable = false;
    }
    return f;
}

bool isShellFlyout(const WindowFacts& f)
{
    return _wcsicmp(f.process.c_str(), L"StartMenuExperienceHost.exe") == 0 || _wcsicmp(f.process.c_str(), L"SearchHost.exe") == 0;
}

std::string describe(const WindowFacts& f)
{
    return narrow(f.cls) + " / " + narrow(f.process);
}

void sendKeys(std::initializer_list<std::pair<WORD, bool>> keys)
{
    std::vector<INPUT> in;
    for (const auto& [vk, up] : keys) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_SNAPSHOT)
            i.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        in.push_back(i);
    }
    SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
}

// The interactive probes press keys on the user's desktop: they wait for a
// quiet moment so the keys do not land in what the user is typing.
bool waitForIdleUser(const char* tag)
{
    const double end = nowMs() + 20000;
    while (nowMs() < end) {
        LASTINPUTINFO li{sizeof li};
        GetLastInputInfo(&li);
        if (GetTickCount() - li.dwTime >= 1500)
            return true;
        Sleep(100);
    }
    line("[%s] skipped: the keyboard or mouse was in use for 20 s", tag);
    return false;
}

// Thread hotkeys (no window): WM_HOTKEY arrives in the thread's queue.
struct HotkeyGuard {
    int id = 0;
    bool registered = false;
    HotkeyGuard(int i, UINT mods, UINT vk) : id(i)
    {
        registered = RegisterHotKey(nullptr, id, mods | MOD_NOREPEAT, vk) != 0;
    }
    ~HotkeyGuard()
    {
        if (registered)
            UnregisterHotKey(nullptr, id);
    }
    HotkeyGuard(const HotkeyGuard&) = delete;
    HotkeyGuard& operator=(const HotkeyGuard&) = delete;
};

LRESULT CALLBACK probeWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HBRUSH brush = CreateSolidBrush(RGB(40, 120, 200));
        FillRect(dc, &ps.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

struct WindowGuard {
    HWND hwnd = nullptr;
    ~WindowGuard()
    {
        if (hwnd)
            DestroyWindow(hwnd);
    }
};

HWND createTopmostWindow(const RECT& monitor)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = probeWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"CaptureProbeTopmost";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }
    // Small and in the corner: it stands in for the overlay, which only
    // needs to be a topmost window of ours that asks for the foreground.
    return CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"CaptureProbeTopmost", L"capture-probe", WS_POPUP,
        monitor.right - 240, monitor.top + 40, 200, 80, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

RECT frameBounds(HWND h)
{
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)))
        GetWindowRect(h, &r);
    return r;
}

// Fraction of pixels in r (screen coordinates) that differ by more than
// threshold between two pictures of the monitor m.
double regionChange(const Image& a, const Image& b, const RECT& m, RECT r, int threshold = 24)
{
    OffsetRect(&r, -m.left, -m.top);
    const DiffStats d = diff(a, b, threshold, &r);
    const double area = std::max(1.0, double(std::min<LONG>(r.right, a.w) - std::max<LONG>(r.left, 0))
            * double(std::min<LONG>(r.bottom, a.h) - std::max<LONG>(r.top, 0)));
    return d.differing / area;
}

// ---------------------------------------------------------------------------
// start-menu

int cmdStartMenu()
{
    const char* tag = "start-menu";
    // A free chord with modifiers (like a real capture hotkey). Never
    // Ctrl+1/2/3 (PixPin) and never anything with Win.
    struct Chord {
        UINT mods;
        UINT vk;
        const char* name;
    };
    const Chord chords[] = {{MOD_CONTROL | MOD_SHIFT, VK_F9, "Ctrl+Shift+F9"}, {MOD_CONTROL | MOD_SHIFT, VK_F10, "Ctrl+Shift+F10"},
        {MOD_CONTROL | MOD_ALT, VK_F9, "Ctrl+Alt+F9"}};
    std::unique_ptr<HotkeyGuard> hotkey;
    const Chord* chord = nullptr;
    for (const auto& c : chords) {
        hotkey = std::make_unique<HotkeyGuard>(0xC0DE, c.mods, c.vk);
        if (hotkey->registered) {
            chord = &c;
            break;
        }
    }
    if (!chord) {
        line("[%s] skipped: none of the test hotkeys could be registered", tag);
        summary("start-menu", "skipped: no free test hotkey");
        return 1;
    }
    line("[%s] test hotkey %s registered", tag, chord->name);

    const auto list = monitors();
    const Monitor& m = list.front();
    Device dev = createDevice();
    // Warm the capture path once, so the times below are a prepared app's.
    grabMonitor(dev, m.handle, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized);

    WindowGuard window;
    // Whatever happens below, Start is not left open.
    struct CloseStartAtExit {
        ~CloseStartAtExit()
        {
            for (int i = 0; i < 3 && isShellFlyout(foreground()); ++i) {
                sendKeys({{VK_ESCAPE, false}, {VK_ESCAPE, true}});
                pumpUntil(800, [] { return !isShellFlyout(foreground()); });
            }
            if (isShellFlyout(foreground()))
                line("[start-menu] WARNING: Start is still in front after three Esc presses");
        }
    } closeAtExit;

    const char* methods[] = {"Esc", "own topmost window + SetForegroundWindow", "Esc", "own topmost window + SetForegroundWindow"};
    for (int round = 0; round < 4; ++round) {
        const bool viaWindow = round % 2 == 1;
        if (!waitForIdleUser(tag))
            break;
        const WindowFacts before = foreground();
        if (before.elevated || !before.queryable) {
            line("[%s] skipped: the foreground window (%s) runs elevated; input to it would be dropped", tag, describe(before).c_str());
            summary("start-menu", "skipped: elevated foreground");
            break;
        }
        if (isShellFlyout(before)) {
            line("[%s] skipped: Start or search is already open", tag);
            break;
        }
        double t = nowMs();
        sendKeys({{VK_LWIN, false}, {VK_LWIN, true}});
        if (!pumpUntil(1500, [] { return isShellFlyout(foreground()); })) {
            const WindowFacts f = foreground();
            line("[%s] round %d: Start did not come to the front within 1.5 s (front: %s)", tag, round + 1, describe(f).c_str());
            summary(format("start-menu round %d", round + 1), "Start did not open");
            continue;
        }
        const double openMs = nowMs() - t;
        // Let the opening animation finish, so the picture has the menu.
        pumpFor(400);
        const WindowFacts menu = foreground();
        const RECT menuRect = frameBounds(menu.hwnd);
        line("[%s] round %d: Start in front after %.0f ms: %s, rect %ld,%ld %ldx%ld", tag, round + 1, openMs, describe(menu).c_str(),
            menuRect.left, menuRect.top, width(menuRect), height(menuRect));

        // The hotkey, only while Start is what receives the keys.
        if (!isShellFlyout(foreground()))
            continue;
        bool gotHotkey = false;
        double hotkeyMs = 0;
        Image withMenuBlt;
        Grab withMenuWgc;
        double bltMs = 0;
        t = nowMs();
        sendKeys({{VK_CONTROL, false}, {static_cast<WORD>(chord->mods & MOD_ALT ? VK_MENU : VK_SHIFT), false},
            {static_cast<WORD>(chord->vk), false}, {static_cast<WORD>(chord->vk), true},
            {static_cast<WORD>(chord->mods & MOD_ALT ? VK_MENU : VK_SHIFT), true}, {VK_CONTROL, true}});
        pumpUntil(1000, [&] { return gotHotkey; }, [&](const MSG& msg) {
            if (msg.message == WM_HOTKEY && msg.wParam == 0xC0DE && !gotHotkey) {
                gotHotkey = true;
                hotkeyMs = nowMs() - t;
                const double b = nowMs();
                withMenuBlt = bitblt(m.rect);
                bltMs = nowMs() - b;
                withMenuWgc = grabMonitor(dev, m.handle, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized);
            }
        });
        const WindowFacts afterHotkey = foreground();
        line("[%s] round %d: WM_HOTKEY %s%s; Start still in front after it: %d", tag, round + 1, gotHotkey ? "received" : "NOT received",
            gotHotkey ? format(" after %.0f ms", hotkeyMs).c_str() : "", isShellFlyout(afterHotkey));
        summary(format("start-menu round %d hotkey", round + 1), gotHotkey ? format("received, %.0f ms", hotkeyMs) : "not received");

        // Close it.
        double closeMs = -1;
        std::string how = methods[round];
        bool setFgResult = false;
        if (isShellFlyout(foreground())) {
            t = nowMs();
            if (viaWindow) {
                window.hwnd = createTopmostWindow(m.rect);
                ShowWindow(window.hwnd, SW_SHOWNA);
                setFgResult = SetForegroundWindow(window.hwnd) != 0;
                if (pumpUntil(1000, [] { return !isShellFlyout(foreground()); }))
                    closeMs = nowMs() - t;
            } else {
                sendKeys({{VK_ESCAPE, false}, {VK_ESCAPE, true}});
                if (pumpUntil(1000, [] { return !isShellFlyout(foreground()); }))
                    closeMs = nowMs() - t;
            }
        }
        const WindowFacts afterClose = foreground();
        // What the screen shows half a second after the close action (the
        // menu may leave the front before it leaves the screen, or not).
        pumpFor(500);
        const Image afterAction = bitblt(m.rect);
        if (viaWindow)
            line("[%s] round %d: SetForegroundWindow returned %d", tag, round + 1, setFgResult);
        line("[%s] round %d: close by %s: %s; front afterwards: %s", tag, round + 1, how.c_str(),
            closeMs >= 0 ? format("Start left the front after %.0f ms", closeMs).c_str() : "Start stayed in front for 1 s",
            describe(afterClose).c_str());
        summary(format("start-menu round %d close (%s)", round + 1, how.c_str()),
            closeMs >= 0 ? format("%.0f ms", closeMs) : "did not close in 1 s");
        // Make sure it is closed before the next round, by Esc if needed.
        for (int i = 0; i < 2 && isShellFlyout(foreground()); ++i) {
            sendKeys({{VK_ESCAPE, false}, {VK_ESCAPE, true}});
            pumpUntil(800, [] { return !isShellFlyout(foreground()); });
        }
        if (window.hwnd) {
            DestroyWindow(window.hwnd);
            window.hwnd = nullptr;
        }
        pumpFor(600);
        // The reference: the screen with Start surely closed. The menu is
        // dark on a dark editor here, so the share of the whole monitor that
        // changes (threshold 12) is the measure; the open menu covers about
        // half of it.
        const Image closed = bitblt(m.rect);
        const RECT whole{m.rect.left, m.rect.top, m.rect.right, m.rect.bottom};
        const double onScreen = regionChange(afterAction, closed, m.rect, whole, 12);
        line("[%s] round %d: half a second after the close action, %.0f%% of the screen still differs from the closed screen: menu %s",
            tag, round + 1, onScreen * 100, onScreen > 0.1 ? "STILL ON SCREEN" : "gone from the screen");
        if (gotHotkey) {
            const double inBlt = regionChange(withMenuBlt, closed, m.rect, whole, 12);
            const double inWgc = withMenuWgc.ok ? regionChange(withMenuWgc.bgra(), closed, m.rect, whole, 12) : -1;
            line("[%s] round %d: pictures taken on WM_HOTKEY (BitBlt %.1f ms, WGC %.1f ms) differ from the closed screen in"
                 " %.0f%% (BitBlt), %.0f%% (WGC) of the monitor: Start %s in the BitBlt picture, %s in the WGC picture",
                tag, round + 1, bltMs, withMenuWgc.itemMs + withMenuWgc.firstFrameMs + withMenuWgc.copyMs, inBlt * 100, inWgc * 100,
                inBlt > 0.1 ? "IS" : "is NOT", inWgc > 0.1 ? "IS" : "is NOT");
            summary(format("start-menu round %d menu in picture", round + 1),
                format("BitBlt %s (%.0f%%), WGC %s (%.0f%%)", inBlt > 0.1 ? "yes" : "no", inBlt * 100, inWgc > 0.1 ? "yes" : "no", inWgc * 100));
            if (round == 0) {
                savePng(withMenuBlt, outPath(L"start-menu-bitblt.png"));
                if (withMenuWgc.ok)
                    savePng(withMenuWgc.bgra(), outPath(L"start-menu-wgc.png"));
                savePng(closed, outPath(L"start-menu-closed.png"));
            }
        }
        pumpFor(600);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// prtscn

const wchar_t* kKeyboardKey = L"Control Panel\\Keyboard";
const wchar_t* kSnipValue = L"PrintScreenKeyForSnippingEnabled";

// What the value was before the probe, kept on disk too until it is put
// back: a crash or a closed console must not leave the user's setting
// changed. "absent" or the DWORD.
std::wstring backupFile()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + L"capture-probe-prtscn-backup.txt";
}

bool readSnipValue(DWORD& value)
{
    DWORD size = sizeof value;
    return RegGetValueW(HKEY_CURRENT_USER, kKeyboardKey, kSnipValue, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
}

void writeSnipValue(bool present, DWORD value)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKeyboardKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (present)
        RegSetValueExW(key, kSnipValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
    else
        RegDeleteValueW(key, kSnipValue);
    RegCloseKey(key);
}

void broadcastKeyboardChange()
{
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(kKeyboardKey), SMTO_ABORTIFHUNG, 1000, &r);
}

// Puts back a value left behind by an earlier run that did not finish.
void restoreLeftoverBackup()
{
    FILE* f = _wfopen(backupFile().c_str(), L"r");
    if (!f)
        return;
    char buf[64] = {};
    fgets(buf, sizeof buf, f);
    fclose(f);
    if (std::strncmp(buf, "absent", 6) == 0)
        writeSnipValue(false, 0);
    else
        writeSnipValue(true, static_cast<DWORD>(std::strtoul(buf, nullptr, 10)));
    broadcastKeyboardChange();
    DeleteFileW(backupFile().c_str());
    line("[prtscn] restored PrintScreenKeyForSnippingEnabled from an unfinished earlier run (%s)", buf);
}

struct SnipValueGuard {
    bool present = false;
    DWORD value = 0;
    bool armed = false;
    void arm()
    {
        present = readSnipValue(value);
        FILE* f = _wfopen(backupFile().c_str(), L"w");
        if (f) {
            if (present)
                std::fprintf(f, "%lu\n", value);
            else
                std::fputs("absent\n", f);
            fclose(f);
        }
        armed = true;
    }
    void restore()
    {
        if (!armed)
            return;
        writeSnipValue(present, value);
        broadcastKeyboardChange();
        DeleteFileW(backupFile().c_str());
        armed = false;
    }
    ~SnipValueGuard() { restore(); }
};
SnipValueGuard* g_snipGuard = nullptr;

BOOL WINAPI consoleHandler(DWORD)
{
    if (g_snipGuard)
        g_snipGuard->restore();
    return FALSE;
}

std::set<DWORD> snippingPids()
{
    std::set<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof pe};
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"SnippingTool.exe") == 0 || _wcsicmp(pe.szExeFile, L"ScreenClippingHost.exe") == 0
                || _wcsicmp(pe.szExeFile, L"OpenWith.exe") == 0)
                pids.insert(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

// What PrtScn opens when Windows has it: the Snipping Tool's overlay, or,
// when another app (PixPin here) has also registered ms-screenclip:, the
// "choose an app" dialog, OpenWith.exe, which takes a few seconds to come.
bool isSnipping(const WindowFacts& f)
{
    return _wcsicmp(f.process.c_str(), L"SnippingTool.exe") == 0 || _wcsicmp(f.process.c_str(), L"ScreenClippingHost.exe") == 0
        || _wcsicmp(f.process.c_str(), L"OpenWith.exe") == 0;
}

// Removes picture entries Windows' own clipboard history took after `since`
// (a PrtScn nobody consumed copies the screen). Only pictures, only newer.
int dropNewClipboardPictures(winrt::Windows::Foundation::DateTime since)
{
    using namespace winrt::Windows::ApplicationModel::DataTransfer;
    int removed = 0;
    try {
        auto result = Clipboard::GetHistoryItemsAsync().get();
        if (result.Status() != ClipboardHistoryItemsResultStatus::Success)
            return -1;
        for (const auto& item : result.Items())
            if (item.Timestamp() >= since && item.Content().Contains(StandardDataFormats::Bitmap()))
                removed += Clipboard::DeleteItemFromHistory(item) ? 1 : 0;
    } catch (const winrt::hresult_error&) {
        return -1;
    }
    return removed;
}

struct PressResult {
    bool hotkey = false;
    double hotkeyMs = 0;
    bool snipping = false;
    double snippingMs = 0;
    bool clipboardChanged = false;
    std::string closed;
    std::string who;
};

// Presses PrtScn once (the probe holds it as a hotkey) and watches who
// answers: our WM_HOTKEY, the Snipping Tool, or the clipboard.
PressResult pressPrintScreen(int hotkeyId)
{
    PressResult r;
    const std::set<DWORD> before = snippingPids();
    const DWORD seq = GetClipboardSequenceNumber();
    const auto since = winrt::clock::now();
    const double t = nowMs();
    sendKeys({{VK_SNAPSHOT, false}, {VK_SNAPSHOT, true}});
    std::set<DWORD> fresh;
    pumpUntil(6000, [&] {
        if (!r.snipping) {
            const WindowFacts f = foreground();
            if (isSnipping(f))
                r.who = narrow(f.process);
            for (DWORD pid : snippingPids())
                if (!before.count(pid)) {
                    fresh.insert(pid);
                    if (r.who.empty())
                        r.who = narrow(processName(pid));
                }
            if (!r.who.empty()) {
                r.snipping = true;
                r.snippingMs = nowMs() - t;
            }
        }
        // Stop early once our hotkey came and nothing else did for 3 s.
        return r.snipping || (r.hotkey && nowMs() - t > 3000);
    }, [&](const MSG& msg) {
        if (msg.message == WM_HOTKEY && static_cast<int>(msg.wParam) == hotkeyId && !r.hotkey) {
            r.hotkey = true;
            r.hotkeyMs = nowMs() - t;
        }
    });
    if (r.snipping) {
        // Close its overlay with Esc, once it is the front window.
        pumpUntil(2000, [] { return isSnipping(foreground()); });
        for (int i = 0; i < 3 && isSnipping(foreground()); ++i) {
            sendKeys({{VK_ESCAPE, false}, {VK_ESCAPE, true}});
            pumpUntil(1000, [] { return !isSnipping(foreground()); });
        }
        r.closed = isSnipping(foreground()) ? "still in front after Esc" : "closed by Esc";
        // Whatever the press started and Esc did not close goes: the probe
        // must not leave a dialog or an overlay on the user's screen.
        pumpFor(500);
        for (DWORD pid : snippingPids())
            if (!before.count(pid))
                if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
                    TerminateProcess(h, 0);
                    CloseHandle(h);
                    r.closed += ", leftover process ended";
                }
    }
    pumpFor(300);
    r.clipboardChanged = GetClipboardSequenceNumber() != seq;
    if (r.clipboardChanged) {
        const int removed = dropNewClipboardPictures(since);
        line("[prtscn] WARNING: the clipboard changed during the press; removed %d new picture(s) from Windows' clipboard history", removed);
    }
    return r;
}

std::string pressText(const PressResult& r)
{
    std::string s = r.hotkey ? format("WM_HOTKEY after %.0f ms", r.hotkeyMs) : "no WM_HOTKEY";
    s += r.snipping ? format(", %s came up after %.0f ms (%s)", r.who.c_str(), r.snippingMs, r.closed.c_str())
                    : ", no Snipping Tool or app picker within 6 s";
    if (r.clipboardChanged)
        s += ", clipboard changed";
    return s;
}

int cmdPrtScn()
{
    const char* tag = "prtscn";
    restoreLeftoverBackup();
    SnipValueGuard guard;
    g_snipGuard = &guard;
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    guard.arm();
    const std::string original = guard.present ? format("%lu", guard.value) : "absent";
    line("[%s] HKCU\\Control Panel\\Keyboard\\PrintScreenKeyForSnippingEnabled: %s", tag, original.c_str());
    summary("prtscn registry value at start", original);

    auto attempt = [&](const char* state) {
        HotkeyGuard hk(0xB0B, 0, VK_SNAPSHOT);
        const DWORD err = hk.registered ? 0 : GetLastError();
        line("[%s] %s: RegisterHotKey(PrtScn) %s", tag, state, hk.registered ? "succeeded" : format("failed, error %lu", err).c_str());
        std::string result = hk.registered ? "register ok" : format("register failed (%lu)", err);
        // Only press when we hold the key: a PrtScn nobody consumes copies
        // the screen into the clipboard (and the user's histories).
        if (hk.registered) {
            if (!waitForIdleUser(tag))
                return;
            const WindowFacts f = foreground();
            if (f.elevated || !f.queryable || isShellFlyout(f)) {
                line("[%s] %s: not pressed: the front window is %s", tag, state, describe(f).c_str());
                summary(format("prtscn %s", state), result + ", not pressed");
                return;
            }
            const PressResult r = pressPrintScreen(0xB0B);
            line("[%s] %s: PrtScn pressed: %s", tag, state, pressText(r).c_str());
            result += "; " + pressText(r);
        } else {
            line("[%s] %s: PrtScn not pressed (it would reach the default handler)", tag, state);
        }
        summary(format("prtscn %s", state), result);
    };

    attempt(format("value %s (as found)", original.c_str()).c_str());
    if (!guard.present || guard.value != 0) {
        writeSnipValue(true, 0);
        line("[%s] value set to 0 (no broadcast)", tag);
        attempt("value 0, no broadcast");
        broadcastKeyboardChange();
        line("[%s] WM_SETTINGCHANGE(\"Control Panel\\Keyboard\") broadcast", tag);
        attempt("value 0, after WM_SETTINGCHANGE");
    }
    if (guard.present && guard.value == 0) {
        writeSnipValue(true, 1);
        broadcastKeyboardChange();
        attempt("value 1, after WM_SETTINGCHANGE");
    }
    guard.restore();
    DWORD now = 0;
    const bool presentNow = readSnipValue(now);
    const std::string restored = presentNow ? format("%lu", now) : "absent";
    line("[%s] value restored: %s (was %s)", tag, restored.c_str(), original.c_str());
    summary("prtscn registry value restored", restored + (restored == original ? " (same as before)" : " (DIFFERS!)"));
    // After restoring: does the original state answer the same way again?
    attempt(format("value %s again (restored)", original.c_str()).c_str());
    g_snipGuard = nullptr;
    return 0;
}

// ---------------------------------------------------------------------------

void printSummary()
{
    size_t w = 4;
    for (const auto& r : g_summary)
        w = std::max(w, r.item.size());
    line("");
    line("==== summary ====");
    for (const auto& r : g_summary)
        line("%-*s | %s", static_cast<int>(w), r.item.c_str(), r.value.c_str());
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // Physical pixels everywhere: without this, BitBlt on a 150% monitor
    // gives a virtualised 1707x1067 and every comparison is meaningless.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // RequestAccessAsync(...).get() and the clipboard history calls block,
    // which C++/WinRT allows only off an STA.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_outDir = exe;
    g_outDir = g_outDir.substr(0, g_outDir.find_last_of(L'\\')) + L"\\capture-out";
    CreateDirectoryW(g_outDir.c_str(), nullptr);

    const std::wstring cmd = argc > 1 ? argv[1] : L"";
    {
        OSVERSIONINFOW v{sizeof v};
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        if (auto rtl = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))))
            rtl(&v);
        line("capture-probe: Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
        const auto list = monitors();
        for (size_t i = 0; i < list.size(); ++i)
            line("monitor %zu: %s %ld,%ld %dx%d physical px, %u dpi (%.0f%%)%s", i, narrow(list[i].device).c_str(), list[i].rect.left,
                list[i].rect.top, width(list[i].rect), height(list[i].rect), list[i].dpi, list[i].dpi / 96.0 * 100,
                list[i].primary ? ", primary" : "");
        if (list.size() < 2)
            line("multi-monitor cases: skipped: only one monitor is attached");
    }

    int rc = 0;
    try {
        if (cmd == L"bitblt")
            rc = cmdBitblt();
        else if (cmd == L"wgc")
            rc = cmdWgc();
        else if (cmd == L"border")
            rc = cmdBorder();
        else if (cmd == L"hdr")
            rc = cmdHdr();
        else if (cmd == L"compare")
            rc = cmdCompare();
        else if (cmd == L"start-menu")
            rc = cmdStartMenu();
        else if (cmd == L"prtscn")
            rc = cmdPrtScn();
        else if (cmd == L"all") {
            cmdBitblt();
            cmdWgc();
            cmdBorder();
            cmdHdr();
            cmdCompare();
            cmdStartMenu();
            cmdPrtScn();
        } else {
            line("usage: capture-probe bitblt|wgc|border|hdr|compare|start-menu|prtscn|all");
            return 2;
        }
    } catch (const winrt::hresult_error& e) {
        line("FAILED: 0x%08X %s", static_cast<unsigned>(e.code().value), narrow(std::wstring(e.message())).c_str());
        rc = 1;
    }
    printSummary();
    return rc;
}
