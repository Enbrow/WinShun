// Long screenshot, stage 6 (docs/design-capture-roadmap.md, 2.4): the three
// questions to answer before the long capture is written, asked of the real
// screen and the real programs.
//
//   scroll-probe exclude          WDA_EXCLUDEFROMCAPTURE on layered windows
//                                 (UpdateLayeredWindow, click-through, as the
//                                 long capture's frame) and on Qt Quick windows,
//                                 in a WGC capture of the whole monitor and in
//                                 BitBlt's
//   scroll-probe wheel T…         a WM_MOUSEWHEEL posted to the deepest child
//                                 window under the middle of each target: did it
//                                 scroll, how far a notch goes
//   scroll-probe uia T…           UI Automation's ScrollPattern on the element
//                                 under the middle of each target (the third
//                                 rung of the scroll ladder)
//   scroll-probe fps T [seconds]  a free-threaded frame pool on the target's
//                                 monitor while it scrolls: frames a second,
//                                 time to cut the region out and read it, and
//                                 whether the frames are whole-pixel shifts of
//                                 each other
//
// A target T is a window handle (decimal or 0x…), optionally with the region
// to watch: 0x1234@x,y,w,h (physical pixels, virtual desktop); without one,
// the middle of the window (15 % in from each side). Nothing is pressed: the
// wheel goes in as posted messages (scrolled back afterwards), and windows are
// made topmost only while a frame rate is measured. The process is per-monitor
// aware (Qt sets it), so every coordinate is physical.

// C++/WinRT before Qt's headers (as in src/capture/Grabber.cpp).
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>

#include <windows.h>

#include <ole2.h> // before uiautomation.h: WIN32_LEAN_AND_MEAN leaves it out of windows.h
#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Qt::StringLiterals;

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

// ---------------------------------------------------------------------------
// Output.

struct Row {
    std::string item;
    std::string value;
};
std::vector<Row> g_summary;

std::string format(const char* fmt, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    return buf;
}

void line(const char* fmt, ...)
{
    char buf[4096];
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
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

double nowMs()
{
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / freq;
}

double percentile(std::vector<double> v, double q)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[size_t(q * double(v.size() - 1))];
}

// Runs Qt's and the thread's messages for `ms`.
void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        Sleep(1);
    }
}

// ---------------------------------------------------------------------------
// Windows.

std::string className(HWND h)
{
    wchar_t buf[256] {};
    GetClassNameW(h, buf, 256);
    return narrow(buf);
}

std::string processName(HWND h)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p)
        return format("pid %lu (not opened, error %lu)", pid, GetLastError());
    wchar_t path[MAX_PATH] {};
    DWORD n = MAX_PATH;
    QueryFullProcessImageNameW(p, 0, path, &n);
    CloseHandle(p);
    std::wstring s(path);
    const size_t slash = s.find_last_of(L'\\');
    return narrow(slash == std::wstring::npos ? s : s.substr(slash + 1));
}

// The process's mandatory integrity level, or -1 when it cannot be read.
long integrityOf(HWND h)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p)
        return -1;
    HANDLE token = nullptr;
    long level = -1;
    if (OpenProcessToken(p, TOKEN_QUERY, &token)) {
        alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[64];
        DWORD size = 0;
        if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof buffer, &size)) {
            const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
            level = long(*GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1));
        }
        CloseHandle(token);
    }
    CloseHandle(p);
    return level;
}

// The deepest visible, enabled, not transparent child window under `screen`,
// from `top` down (the long capture's way of finding where a wheel goes).
HWND deepestChild(HWND top, POINT screen)
{
    HWND at = top;
    for (int depth = 0; depth < 32; ++depth) {
        POINT client = screen;
        ScreenToClient(at, &client);
        HWND child = ChildWindowFromPointEx(at, client, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!child || child == at)
            break;
        at = child;
    }
    return at;
}

RECT extendedBounds(HWND h)
{
    RECT r {};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)))
        GetWindowRect(h, &r);
    return r;
}

struct Target {
    HWND hwnd = nullptr;
    RECT region {}; // physical, virtual desktop
};

bool parseTarget(const QString& arg, Target* t)
{
    const QStringList parts = arg.split(u'@');
    bool ok = false;
    const qulonglong h = parts[0].toULongLong(&ok, 0);
    if (!ok || !IsWindow(reinterpret_cast<HWND>(h)))
        return false;
    t->hwnd = reinterpret_cast<HWND>(h);
    if (parts.size() > 1) {
        const QStringList n = parts[1].split(u',');
        if (n.size() != 4)
            return false;
        t->region = {n[0].toInt(), n[1].toInt(), n[0].toInt() + n[2].toInt(), n[1].toInt() + n[3].toInt()};
    } else {
        RECT w = extendedBounds(t->hwnd);
        const int dx = (w.right - w.left) * 15 / 100;
        const int dy = (w.bottom - w.top) * 15 / 100;
        t->region = {w.left + dx, w.top + dy, w.right - dx, w.bottom - dy};
    }
    return true;
}

// ---------------------------------------------------------------------------
// Pictures: 32-bit BGRA rows, top-down, no padding.

struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> px;
    const uint32_t* row(int y) const { return px.data() + size_t(y) * size_t(w); }
    bool empty() const { return px.empty(); }
};

uint64_t rowHash(const uint32_t* p, int n)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; ++i) {
        h ^= p[i] & 0x00FFFFFFu;
        h *= 1099511628211ull;
    }
    return h;
}

bool rowPlain(const uint32_t* p, int n)
{
    for (int i = 1; i < n; ++i)
        if ((p[i] ^ p[0]) & 0x00FFFFFFu)
            return false;
    return true;
}

// How far the content moved up from `a` to `b` (b's row y shows a's row
// y + dy), by exact row matches; only rows with more than one colour count.
struct Shift {
    int dy = 0;
    int matched = 0; // b's rows equal to a's at y + dy
    int compared = 0; // b's rows with content, where y + dy is inside a
    int sameRows = 0; // rows equal at dy = 0
    double fraction() const { return compared ? double(matched) / compared : 0; }
};

int g_ignoreRight = 0; // the right edge left out of the row comparison (a scroll bar): --ignore-right N

Shift bestShift(const Image& a, const Image& b)
{
    const int w = std::max(1, a.w - g_ignoreRight);
    Shift s;
    if (a.w != b.w || a.h != b.h || a.empty())
        return s;
    const int h = a.h;
    std::vector<uint64_t> ha(static_cast<size_t>(h)), hb(static_cast<size_t>(h));
    std::vector<char> plainA(static_cast<size_t>(h)), plainB(static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        ha[size_t(y)] = rowHash(a.row(y), w);
        hb[size_t(y)] = rowHash(b.row(y), w);
        plainA[size_t(y)] = rowPlain(a.row(y), w);
        plainB[size_t(y)] = rowPlain(b.row(y), w);
        if (ha[size_t(y)] == hb[size_t(y)])
            ++s.sameRows;
    }
    std::unordered_map<uint64_t, std::vector<int>> where;
    for (int y = 0; y < h; ++y)
        if (!plainA[size_t(y)])
            where[ha[size_t(y)]].push_back(y);
    std::map<int, int> votes;
    for (int y = 0; y < h; ++y) {
        if (plainB[size_t(y)])
            continue;
        const auto it = where.find(hb[size_t(y)]);
        if (it == where.end() || it->second.size() > 8)
            continue;
        for (int ya : it->second)
            ++votes[ya - y];
    }
    int best = 0;
    int bestVotes = -1;
    for (const auto& [dy, n] : votes) {
        if (n > bestVotes || (n == bestVotes && std::abs(dy) < std::abs(best))) {
            best = dy;
            bestVotes = n;
        }
    }
    if (bestVotes < 0)
        return s;
    s.dy = best;
    for (int y = 0; y < h; ++y) {
        const int ya = y + best;
        if (ya < 0 || ya >= h || plainB[size_t(y)])
            continue;
        ++s.compared;
        if (hb[size_t(y)] == ha[size_t(ya)])
            ++s.matched;
    }
    return s;
}

// The shift with the smallest mean luminance difference (not only whole equal
// rows: text drawn at another sub-pixel offset is not equal), and how many of
// the pixels are then within 8 of each other.
struct Nearest {
    int dy = 0;
    double meanDiff = 1e9;
    double within8 = 0; // fraction of compared pixels
};

Nearest nearestShift(const Image& a, const Image& b)
{
    Nearest best;
    if (a.w != b.w || a.h != b.h || a.empty())
        return best;
    const int w = std::max(1, a.w - g_ignoreRight);
    const auto luma = [](uint32_t p) { return int(((p >> 16) & 0xFF) + 2 * ((p >> 8) & 0xFF) + (p & 0xFF)) / 4; };
    std::vector<uint8_t> la(size_t(a.w) * size_t(a.h)), lb(la.size());
    for (size_t i = 0; i < la.size(); ++i) {
        la[i] = uint8_t(luma(a.px[i]));
        lb[i] = uint8_t(luma(b.px[i]));
    }
    for (int dy = -16; dy <= a.h * 3 / 4; ++dy) {
        long long sum = 0;
        long long n = 0;
        for (int y = 0; y < a.h; y += 3) {
            const int ya = y + dy;
            if (ya < 0 || ya >= a.h)
                continue;
            const uint8_t* ra = la.data() + size_t(ya) * size_t(a.w);
            const uint8_t* rb = lb.data() + size_t(y) * size_t(a.w);
            for (int x = 0; x < w; x += 2) {
                sum += std::abs(int(ra[x]) - int(rb[x]));
                ++n;
            }
        }
        if (n < 64LL * (w / 2)) // under 192 rows overlap
            continue;
        const double mean = double(sum) / double(n);
        if (mean < best.meanDiff) {
            best.meanDiff = mean;
            best.dy = dy;
        }
    }
    long long within = 0;
    long long n = 0;
    for (int y = 0; y < a.h; ++y) {
        const int ya = y + best.dy;
        if (ya < 0 || ya >= a.h)
            continue;
        for (int x = 0; x < w; ++x) {
            ++n;
            if (std::abs(int(la[size_t(ya) * size_t(a.w) + size_t(x)]) - int(lb[size_t(y) * size_t(a.w) + size_t(x)])) <= 8)
                ++within;
        }
    }
    best.within8 = n ? double(within) / double(n) : 0;
    return best;
}

// A picture as a 32-bit BMP, for looking at what a probe saw (--save DIR).
std::string g_saveDir;

void saveBmp(const Image& img, const std::string& name)
{
    if (g_saveDir.empty() || img.empty())
        return;
    const std::string path = g_saveDir + "\\" + name + ".bmp";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") || !f)
        return;
    BITMAPFILEHEADER fh {};
    BITMAPINFOHEADER ih {};
    ih.biSize = sizeof ih;
    ih.biWidth = img.w;
    ih.biHeight = -img.h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + DWORD(img.px.size() * 4);
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(img.px.data(), 4, img.px.size(), f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Windows.Graphics.Capture.

struct Device {
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID3D11DeviceContext> context;
    wd3d::IDirect3DDevice winrt {nullptr};
    bool borderless = false;
};

Device g_device;
std::mutex g_contextLock; // the immediate context is shared by the frame threads

void prepareDevice()
{
    if (g_device.d3d)
        return;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, g_device.d3d.put(), nullptr, g_device.context.put()));
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(g_device.d3d.as<IDXGIDevice>().get(), inspectable.put()));
    g_device.winrt = inspectable.as<wd3d::IDirect3DDevice>();
    try {
        const auto access = wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
        g_device.borderless = access == winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed;
    } catch (const winrt::hresult_error&) {
    }
}

wgc::GraphicsCaptureItem itemForMonitor(HMONITOR monitor)
{
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item {nullptr};
    winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

[[maybe_unused]] wgc::GraphicsCaptureItem itemForWindow(HWND window)
{
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item {nullptr};
    winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

// The frame's pixels in `box` (the frame's own coordinates; empty: all of it).
Image readFrame(const wgc::Direct3D11CaptureFrame& frame, RECT box, double* ms = nullptr)
{
    const double t0 = nowMs();
    winrt::com_ptr<ID3D11Texture2D> texture;
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    if (IsRectEmpty(&box))
        box = {0, 0, LONG(desc.Width), LONG(desc.Height)};
    box.left = std::clamp<LONG>(box.left, 0, LONG(desc.Width));
    box.right = std::clamp<LONG>(box.right, box.left, LONG(desc.Width));
    box.top = std::clamp<LONG>(box.top, 0, LONG(desc.Height));
    box.bottom = std::clamp<LONG>(box.bottom, box.top, LONG(desc.Height));
    Image img;
    img.w = box.right - box.left;
    img.h = box.bottom - box.top;
    if (img.w <= 0 || img.h <= 0)
        return {};
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Width = UINT(img.w);
    stagingDesc.Height = UINT(img.h);
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(g_device.d3d->CreateTexture2D(&stagingDesc, nullptr, staging.put()));
    const std::lock_guard lock(g_contextLock);
    const D3D11_BOX source {UINT(box.left), UINT(box.top), 0, UINT(box.right), UINT(box.bottom), 1};
    g_device.context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture.get(), 0, &source);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    winrt::check_hresult(g_device.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    img.px.resize(size_t(img.w) * size_t(img.h));
    for (int y = 0; y < img.h; ++y)
        std::memcpy(img.px.data() + size_t(y) * size_t(img.w), static_cast<const uint8_t*>(mapped.pData) + size_t(mapped.RowPitch) * size_t(y), size_t(img.w) * 4);
    g_device.context->Unmap(staging.get(), 0);
    if (ms)
        *ms = nowMs() - t0;
    return img;
}

// One frame of `item` cut to `box`.
Image grabOnce(const wgc::GraphicsCaptureItem& item, RECT box, std::string* error = nullptr)
{
    Image img;
    try {
        auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(g_device.winrt, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, item.Size());
        auto session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false);
        if (g_device.borderless)
            session.IsBorderRequired(false);
        HANDLE arrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<bool> taken {false};
        wgc::Direct3D11CaptureFrame frame {nullptr};
        auto token = pool.FrameArrived([&](const wgc::Direct3D11CaptureFramePool& sender, const auto&) {
            if (taken.exchange(true))
                return;
            frame = sender.TryGetNextFrame();
            SetEvent(arrived);
        });
        session.StartCapture();
        if (WaitForSingleObject(arrived, 3000) == WAIT_OBJECT_0 && frame)
            img = readFrame(frame, box);
        else if (error)
            *error = "no frame within 3 s";
        pool.FrameArrived(token);
        if (frame)
            frame.Close();
        session.Close();
        pool.Close();
        CloseHandle(arrived);
    } catch (const winrt::hresult_error& e) {
        if (error)
            *error = format("0x%08X %s", unsigned(e.code().value), narrow(std::wstring(e.message())).c_str());
    }
    return img;
}

Image bitblt(const RECT& r)
{
    Image img;
    img.w = r.right - r.left;
    img.h = r.bottom - r.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi {};
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
    img.px.assign(static_cast<const uint32_t*>(bits), static_cast<const uint32_t*>(bits) + size_t(img.w) * size_t(img.h));
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}

// ---------------------------------------------------------------------------
// exclude

const wchar_t* kLayeredClass = L"WinShun.ScrollProbe";

// A layered window over `r` (physical): filled with `color`, or with
// `ring` > 0 only a ring that wide round the inside of r, the rest fully
// transparent (as the long capture's frame).
HWND makeLayered(const RECT& r, uint32_t color, bool clickThrough, int ring)
{
    static const bool registered = [] {
        WNDCLASSEXW wc {sizeof wc};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kLayeredClass;
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered)
        return nullptr;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | (clickThrough ? WS_EX_TRANSPARENT : 0),
        kLayeredClass, L"scroll-probe", WS_POPUP, r.left, r.top, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd)
        return nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool on = ring <= 0 || x < ring || y < ring || x >= w - ring || y >= h - ring;
            px[size_t(y) * size_t(w) + size_t(x)] = on ? (0xFF000000u | (color & 0x00FFFFFFu)) : 0u; // premultiplied
        }
    }
    HGDIOBJ old = SelectObject(mem, dib);
    POINT to {r.left, r.top};
    SIZE size {w, h};
    POINT from {0, 0};
    BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screen, &to, &size, mem, &from, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

std::string setExclude(HWND h)
{
    const BOOL ok = SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE);
    const DWORD error = ok ? 0 : GetLastError();
    DWORD now = 0;
    GetWindowDisplayAffinity(h, &now);
    return format("SetWindowDisplayAffinity %s (error %lu), reads back 0x%lX", ok ? "ok" : "FAILED", error, now);
}

// Pixels of `rect` (physical, monitor-relative to `img`) within 8 of `color`.
int countColor(const Image& img, const RECT& rect, uint32_t color)
{
    int n = 0;
    for (int y = std::max<LONG>(0, rect.top); y < std::min<LONG>(img.h, rect.bottom); ++y) {
        for (int x = std::max<LONG>(0, rect.left); x < std::min<LONG>(img.w, rect.right); ++x) {
            const uint32_t p = img.row(y)[x];
            if (std::abs(int((p >> 16) & 0xFF) - int((color >> 16) & 0xFF)) <= 8 && std::abs(int((p >> 8) & 0xFF) - int((color >> 8) & 0xFF)) <= 8
                && std::abs(int(p & 0xFF) - int(color & 0xFF)) <= 8)
                ++n;
        }
    }
    return n;
}

int cmdExclude()
{
    prepareDevice();
    const HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi {sizeof mi};
    GetMonitorInfoW(monitor, &mi);
    const RECT mr = mi.rcMonitor;
    const uint32_t magenta = 0xFF00FF; // excluded
    const uint32_t green = 0x00FF00; // control: not excluded
    const int base = 60;
    const int side = 160;
    struct Probe {
        std::string name;
        RECT rect;
        uint32_t color;
        int ring;
        std::string affinity;
    };
    std::vector<Probe> probes;
    std::vector<HWND> natives;
    // Layered: plain, click-through, a click-through ring; each excluded and not.
    const auto addLayered = [&](const char* name, int column, bool through, int ring, bool exclude) {
        const RECT r {mr.left + base + column * (side + 20), mr.top + base + (exclude ? 0 : side + 20),
            mr.left + base + column * (side + 20) + side, mr.top + base + (exclude ? 0 : side + 20) + side};
        HWND h = makeLayered(r, exclude ? magenta : green, through, ring);
        natives.push_back(h);
        probes.push_back({format("%s, %s", name, exclude ? "excluded" : "not excluded"), r, exclude ? magenta : green, ring,
            exclude ? setExclude(h) : std::string("-")});
    };
    addLayered("layered (UpdateLayeredWindow)", 0, false, 0, true);
    addLayered("layered (UpdateLayeredWindow)", 0, false, 0, false);
    addLayered("layered + WS_EX_TRANSPARENT", 1, true, 0, true);
    addLayered("layered + WS_EX_TRANSPARENT", 1, true, 0, false);
    addLayered("layered ring 3 px, inside alpha 0", 2, true, 3, true);
    addLayered("layered ring 3 px, inside alpha 0", 2, true, 3, false);
    // Qt Quick windows (D3D11), as the long capture's panel: excluded before
    // and after the first show, and one not excluded.
    std::vector<std::unique_ptr<QQuickWindow>> quick;
    QScreen* screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    const auto addQuick = [&](const char* name, int column, int when, bool exclude) {
        auto w = std::make_unique<QQuickWindow>();
        w->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
        w->setColor(exclude ? QColor(255, 0, 255) : QColor(0, 255, 0));
        const QPoint at((base + column * (side + 20)) / dpr, (base + 2 * (side + 20) + (exclude ? 0 : side + 20)) / dpr);
        w->setGeometry(QRect(screen->geometry().topLeft() + at, QSize(int(side / dpr), int(side / dpr))));
        w->create();
        const HWND h = reinterpret_cast<HWND>(w->winId());
        std::string affinity = "-";
        if (exclude && when == 0)
            affinity = setExclude(h);
        w->setProperty("_q_showWithoutActivating", true);
        w->show();
        pump(300);
        if (exclude && when == 1)
            affinity = setExclude(h) + " (after the first frame)";
        RECT r {};
        GetWindowRect(h, &r);
        probes.push_back({format("%s, %s", name, exclude ? "excluded" : "not excluded"), r, exclude ? magenta : green, 0, affinity});
        quick.push_back(std::move(w));
    };
    addQuick("Qt Quick, affinity before show", 0, 0, true);
    addQuick("Qt Quick", 0, 0, false);
    addQuick("Qt Quick, affinity after show", 1, 1, true);
    pump(500);
    std::string error;
    const Image wgcShot = grabOnce(itemForMonitor(monitor), {}, &error);
    const Image gdiShot = bitblt(mr);
    if (wgcShot.empty())
        line("[exclude] WGC: no picture: %s", error.c_str());
    for (const Probe& p : probes) {
        RECT local = p.rect;
        OffsetRect(&local, -mr.left, -mr.top);
        const int area = p.ring > 0 ? (side * side - (side - 2 * p.ring) * (side - 2 * p.ring)) : side * side;
        const int inWgc = wgcShot.empty() ? -1 : countColor(wgcShot, local, p.color);
        const int inGdi = countColor(gdiShot, local, p.color);
        line("[exclude] %-55s WGC %6d of %6d px, BitBlt %6d px; %s", p.name.c_str(), inWgc, area, inGdi, p.affinity.c_str());
        summary("exclude " + p.name, format("WGC %d / %d px, BitBlt %d px", inWgc, area, inGdi));
    }
    for (HWND h : natives)
        DestroyWindow(h);
    quick.clear();
    return 0;
}

// ---------------------------------------------------------------------------
// wheel

void postWheel(HWND to, POINT at, int notches)
{
    const WPARAM w = MAKEWPARAM(0, WORD(short(-120 * notches)));
    const LPARAM l = MAKELPARAM(WORD(short(at.x)), WORD(short(at.y)));
    PostMessageW(to, WM_MOUSEWHEEL, w, l);
}

// The target's region as the screen shows it (the target is made topmost by
// the caller): a capture of its monitor, as the long capture takes them.
// The window's own capture (itemForWindow) gave blank pictures of Chrome,
// Settings and Word.
Image grabRegion(const Target& t, std::string* error)
{
    const HMONITOR monitor = MonitorFromRect(&t.region, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi {sizeof mi};
    GetMonitorInfoW(monitor, &mi);
    RECT box = t.region;
    OffsetRect(&box, -mi.rcMonitor.left, -mi.rcMonitor.top);
    return grabOnce(itemForMonitor(monitor), box, error);
}

int cmdWheel(const std::vector<Target>& targets, int notches)
{
    prepareDevice();
    for (const Target& t : targets) {
        const POINT mid {(t.region.left + t.region.right) / 2, (t.region.top + t.region.bottom) / 2};
        const HWND child = deepestChild(t.hwnd, mid);
        const std::string who = format("%s %s", processName(t.hwnd).c_str(), className(t.hwnd).c_str());
        const long level = integrityOf(t.hwnd);
        line("[wheel] %s: region %ld,%ld %ldx%ld, child under the middle %s (0x%llX), integrity 0x%lX", who.c_str(),
            t.region.left, t.region.top, t.region.right - t.region.left, t.region.bottom - t.region.top, className(child).c_str(),
            (unsigned long long)child, level);
        std::vector<HWND> tries {child};
        if (child != t.hwnd)
            tries.push_back(t.hwnd);
        for (HWND to : tries) {
            std::string error;
            const Image before = grabRegion(t, &error);
            if (before.empty()) {
                line("[wheel]   no picture: %s", error.c_str());
                break;
            }
            SetLastError(0);
            const WPARAM w = MAKEWPARAM(0, WORD(short(-120 * notches)));
            const LPARAM l = MAKELPARAM(WORD(short(mid.x)), WORD(short(mid.y)));
            const BOOL posted = PostMessageW(to, WM_MOUSEWHEEL, w, l);
            const DWORD postError = posted ? 0 : GetLastError();
            pump(900);
            const Image after = grabRegion(t, &error);
            const std::string name = format("wheel-%llX-%s", (unsigned long long)t.hwnd, to == child ? "child" : "top");
            saveBmp(before, name + "-before");
            saveBmp(after, name + "-after");
            const HWND under = WindowFromPoint(mid);
            line("[wheel]   window under the middle: %s %s (0x%llX), root 0x%llX", processName(under).c_str(), className(under).c_str(),
                (unsigned long long)under, (unsigned long long)GetAncestor(under, GA_ROOT));
            const Shift s = bestShift(before, after);
            const bool moved = s.dy != 0 && s.matched >= 8;
            const std::string result = posted ? (moved ? format("scrolled %d px (%.1f px a notch), %d of %d rows exact", s.dy,
                                                              double(s.dy) / notches, s.matched, s.compared)
                                                       : format("did not scroll (best dy %d, %d of %d rows; %d of %d rows unchanged)", s.dy,
                                                             s.matched, s.compared, s.sameRows, before.h))
                                              : format("PostMessage failed, error %lu", postError);
            line("[wheel]   posted %d notch(es) to %s: %s", notches, to == child ? "the deepest child" : "the top-level window", result.c_str());
            summary(format("wheel %s -> %s", who.c_str(), to == child ? className(child).c_str() : "top-level"), result);
            if (moved) {
                postWheel(to, mid, -notches); // back to where it was
                pump(900);
                break;
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// uia

std::string bstr(BSTR b)
{
    std::string s = b ? narrow(std::wstring(b, SysStringLen(b))) : std::string();
    SysFreeString(b);
    return s;
}

int cmdUia(const std::vector<Target>& targets, int notches)
{
    using Microsoft::WRL::ComPtr;
    prepareDevice();
    ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))
        && FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))) {
        line("[uia] no UI Automation");
        return 1;
    }
    ComPtr<IUIAutomation2> timed;
    if (SUCCEEDED(uia.As(&timed))) {
        timed->put_ConnectionTimeout(1000);
        timed->put_TransactionTimeout(1000);
    }
    ComPtr<IUIAutomationTreeWalker> walker;
    uia->get_ControlViewWalker(&walker);
    for (const Target& t : targets) {
        const POINT mid {(t.region.left + t.region.right) / 2, (t.region.top + t.region.bottom) / 2};
        const std::string who = format("%s %s", processName(t.hwnd).c_str(), className(t.hwnd).c_str());
        double t0 = nowMs();
        ComPtr<IUIAutomationElement> at;
        const HRESULT hr = uia->ElementFromPoint(mid, &at);
        const double fromPointMs = nowMs() - t0;
        if (FAILED(hr) || !at) {
            line("[uia] %s: ElementFromPoint failed 0x%08lX", who.c_str(), hr);
            summary("uia " + who, format("ElementFromPoint failed 0x%08lX", hr));
            continue;
        }
        DWORD targetPid = 0;
        GetWindowThreadProcessId(deepestChild(t.hwnd, mid), &targetPid);
        int atPid = 0;
        at->get_CurrentProcessId(&atPid);
        if (DWORD(atPid) != targetPid) {
            line("[uia] %s: the element under the middle is another program's (pid %d), left alone", who.c_str(), atPid);
            continue;
        }
        // Up from the element under the point to the first that scrolls up and down.
        ComPtr<IUIAutomationScrollPattern> scroll;
        ComPtr<IUIAutomationElement> owner;
        int depth = 0;
        t0 = nowMs();
        for (ComPtr<IUIAutomationElement> e = at; e && depth < 40; ++depth) {
            ComPtr<IUIAutomationScrollPattern> p;
            if (SUCCEEDED(e->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&p))) && p) {
                BOOL vertical = FALSE;
                p->get_CurrentVerticallyScrollable(&vertical);
                if (vertical) {
                    scroll = p;
                    owner = e;
                    break;
                }
            }
            ComPtr<IUIAutomationElement> parent;
            if (!walker || FAILED(walker->GetParentElement(e.Get(), &parent)))
                break;
            e = parent;
        }
        const double findMs = nowMs() - t0;
        if (!scroll) {
            line("[uia] %s: nothing scrolls up and down above the element under the middle (%d levels, %.1f ms)", who.c_str(), depth, findMs);
            summary("uia " + who, format("no ScrollPattern (%d levels up)", depth));
            continue;
        }
        BSTR cls = nullptr;
        owner->get_CurrentClassName(&cls);
        CONTROLTYPEID type = 0;
        owner->get_CurrentControlType(&type);
        double percent = 0;
        double view = 0;
        scroll->get_CurrentVerticalScrollPercent(&percent);
        scroll->get_CurrentVerticalViewSize(&view);
        const std::string ownerText = format("%s (control type %d, %d level(s) up), at %.1f %%, view %.1f %%", bstr(cls).c_str(), int(type), depth,
            percent, view);
        std::string error;
        const Image before = grabRegion(t, &error);
        t0 = nowMs();
        HRESULT scrolled = S_OK;
        for (int i = 0; i < notches && SUCCEEDED(scrolled); ++i)
            scrolled = scroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_SmallIncrement);
        const double scrollMs = nowMs() - t0;
        pump(900);
        const Image after = grabRegion(t, &error);
        saveBmp(before, format("uia-%llX-before", (unsigned long long)t.hwnd));
        saveBmp(after, format("uia-%llX-after", (unsigned long long)t.hwnd));
        const Shift s = bestShift(before, after);
        const bool moved = s.dy != 0 && s.matched >= 8;
        double percentAfter = 0;
        scroll->get_CurrentVerticalScrollPercent(&percentAfter);
        const std::string result = FAILED(scrolled) ? format("Scroll failed 0x%08lX", scrolled)
            : moved ? format("scrolled %d px (%.1f px a step), %d of %d rows exact; %.1f %% -> %.1f %%", s.dy, double(s.dy) / notches, s.matched,
                          s.compared, percent, percentAfter)
                    : format("did not scroll (best dy %d, %d of %d rows); %.1f %% -> %.1f %%", s.dy, s.matched, s.compared, percent, percentAfter);
        line("[uia] %s: %s; from point %.1f ms, finding it %.1f ms, %d step(s) %.1f ms: %s", who.c_str(), ownerText.c_str(), fromPointMs,
            findMs, notches, scrollMs, result.c_str());
        summary("uia " + who, result);
        for (int i = 0; i < notches; ++i)
            scroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_SmallDecrement);
        pump(600);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// fps

// The vertical ScrollPattern of the element under `at` or its nearest
// ancestor with one; none unless it is `owner`'s process's.
Microsoft::WRL::ComPtr<IUIAutomationScrollPattern> scrollPatternAt(IUIAutomation* uia, POINT at, HWND owner)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IUIAutomationElement> e;
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(uia->ElementFromPoint(at, &e)) || FAILED(uia->get_ControlViewWalker(&walker)))
        return {};
    DWORD ownerPid = 0; // the window under the point's (a UWP app's CoreWindow is not its frame's process)
    GetWindowThreadProcessId(deepestChild(owner, at), &ownerPid);
    int pid = 0;
    if (FAILED(e->get_CurrentProcessId(&pid)) || DWORD(pid) != ownerPid)
        return {};
    for (int depth = 0; e && depth < 40; ++depth) {
        ComPtr<IUIAutomationScrollPattern> p;
        BOOL vertical = FALSE;
        if (SUCCEEDED(e->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&p))) && p
            && SUCCEEDED(p->get_CurrentVerticallyScrollable(&vertical)) && vertical)
            return p;
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(e.Get(), &parent)))
            break;
        e = parent;
    }
    return {};
}

int cmdFps(const Target& t, int seconds, bool byUia, bool settle, bool byPercent)
{
    prepareDevice();
    Microsoft::WRL::ComPtr<IUIAutomation> uia;
    Microsoft::WRL::ComPtr<IUIAutomationScrollPattern> scroll;
    // On top first: the element under the middle must be the target's.
    SetWindowPos(t.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    pump(400);
    if (byUia) {
        CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
        if (uia)
            scroll = scrollPatternAt(uia.Get(), {(t.region.left + t.region.right) / 2, (t.region.top + t.region.bottom) / 2}, t.hwnd);
        if (!scroll) {
            line("[fps] no ScrollPattern of the target under the middle");
            SetWindowPos(t.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return 1;
        }
    }
    const HMONITOR monitor = MonitorFromRect(&t.region, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi {sizeof mi};
    GetMonitorInfoW(monitor, &mi);
    RECT box = t.region;
    OffsetRect(&box, -mi.rcMonitor.left, -mi.rcMonitor.top);
    const POINT mid {(t.region.left + t.region.right) / 2, (t.region.top + t.region.bottom) / 2};
    const HWND child = deepestChild(t.hwnd, mid);

    struct Stats {
        std::mutex lock;
        std::vector<double> arrivals; // ms since start
        std::vector<double> readMs;
        Image last;
        int pairs = 0; // consecutive frames compared
        int moved = 0; // of them, the content moved
        int exact = 0; // of those, every row with content equal at the shift found
        int nearly = 0; // 90 % or more
        int changedNoShift = 0;
        std::map<int, int> dys;
        int drained = 0; // frames skipped because a newer one was there
    } st;
    const double start = nowMs();
    auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(g_device.winrt, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
        itemForMonitor(monitor).Size());
    auto item = itemForMonitor(monitor);
    auto session = pool.CreateCaptureSession(item);
    session.IsCursorCaptureEnabled(false);
    if (g_device.borderless)
        session.IsBorderRequired(false);
    auto token = pool.FrameArrived([&](const wgc::Direct3D11CaptureFramePool& sender, const auto&) {
        const std::lock_guard lock(st.lock);
        wgc::Direct3D11CaptureFrame frame = sender.TryGetNextFrame();
        if (!frame)
            return;
        for (;;) { // the newest only
            wgc::Direct3D11CaptureFrame newer = sender.TryGetNextFrame();
            if (!newer)
                break;
            frame.Close();
            frame = newer;
            ++st.drained;
        }
        st.arrivals.push_back(nowMs() - start);
        double ms = 0;
        Image img = readFrame(frame, box, &ms);
        frame.Close();
        st.readMs.push_back(ms);
        if (!st.last.empty()) {
            ++st.pairs;
            const Shift s = bestShift(st.last, img);
            if (s.dy != 0 && s.matched >= 8) {
                ++st.moved;
                ++st.dys[s.dy];
                if (s.matched == s.compared)
                    ++st.exact;
                if (s.fraction() >= 0.9)
                    ++st.nearly;
            } else if (s.sameRows < img.h * 9 / 10) {
                ++st.changedNoShift;
            }
        }
        st.last = std::move(img);
    });
    session.StartCapture();
    // 1 s still, then a notch every 250 ms, then still again; then the same
    // notches back up, with 3 notches at a time.
    pump(1000);
    const double scrollFrom = nowMs() - start;
    int posted = 0;
    QElapsedTimer t0;
    t0.start();
    const auto step = [&] {
        if (scroll && byPercent) {
            // Half the view down at once: where to is known, not a step's size.
            double at = 0;
            double view = 0;
            scroll->get_CurrentVerticalScrollPercent(&at);
            scroll->get_CurrentVerticalViewSize(&view);
            const HRESULT hr = scroll->SetScrollPercent(UIA_ScrollPatternNoScroll, std::min(100.0, at + view * 0.5));
            line("[percent] %.2f %% + %.2f %% -> 0x%08lX", at, view * 0.5, hr);
        } else if (scroll) {
            scroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_SmallIncrement);
        } else {
            postWheel(child, mid, 1);
        }
        ++posted;
    };
    if (settle) {
        // One step at a time, each compared once it is still (500 ms on).
        const auto latest = [&] {
            const std::lock_guard lock(st.lock);
            return st.last;
        };
        Image prev = latest();
        for (int i = 0; i < seconds; ++i) {
            step();
            pump(500);
            Image now = latest();
            const Shift s = bestShift(prev, now);
            const Nearest n = nearestShift(prev, now);
            line("[settle] step %d: exact rows: dy %d, %d of %d rows equal; nearest: dy %d, mean difference %.2f, %.1f %% of pixels within 8", i + 1,
                s.dy, s.matched, s.compared, n.dy, n.meanDiff, n.within8 * 100);
            summary(format("settle %s step %d", processName(t.hwnd).c_str(), i + 1),
                format("exact dy %d (%d/%d rows); nearest dy %d, mean diff %.2f, %.1f%% within 8", s.dy, s.matched, s.compared, n.dy, n.meanDiff,
                    n.within8 * 100));
            prev = std::move(now);
        }
    }
    while (!settle && t0.elapsed() < seconds * 1000) {
        step();
        pump(250);
    }
    const double scrollTo = nowMs() - start;
    pump(1000);
    pool.FrameArrived(token);
    session.Close();
    pool.Close();
    for (int left = posted; left > 0; left -= 3) {
        for (int i = 0; scroll && !byPercent && i < std::min(3, left); ++i)
            scroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_SmallDecrement);
        if (!scroll)
            postWheel(child, mid, -std::min(3, left));
        pump(150);
    }
    if (scroll && byPercent)
        scroll->SetScrollPercent(UIA_ScrollPatternNoScroll, 0);
    pump(600);
    SetWindowPos(t.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    const std::lock_guard lock(st.lock);
    int during = 0;
    int still = 0;
    for (double a : st.arrivals)
        (a >= scrollFrom && a <= scrollTo ? during : still)++;
    std::vector<double> gaps;
    for (size_t i = 1; i < st.arrivals.size(); ++i)
        if (st.arrivals[i] >= scrollFrom && st.arrivals[i] <= scrollTo)
            gaps.push_back(st.arrivals[i] - st.arrivals[i - 1]);
    std::string dyText;
    int shown = 0;
    std::vector<std::pair<int, int>> byCount(st.dys.begin(), st.dys.end());
    std::sort(byCount.begin(), byCount.end(), [](auto a, auto b) { return a.second > b.second; });
    for (const auto& [dy, n] : byCount) {
        if (shown++ == 12)
            break;
        dyText += format("%d:%d ", dy, n);
    }
    const std::string who = format("%s %s, region %ldx%ld", processName(t.hwnd).c_str(), className(t.hwnd).c_str(),
        t.region.right - t.region.left, t.region.bottom - t.region.top);
    line("[fps] %s; %d %s, one per 250 ms for %d s", who.c_str(), posted, scroll ? "UI Automation small steps" : format("notch(es) posted to %s", className(child).c_str()).c_str(), seconds);
    line("[fps]   frames: %zu in all, %d while scrolling (%.1f a second), %d while still (2 s); %d older frames skipped for a newer one",
        st.arrivals.size(), during, during / std::max(0.001, (scrollTo - scrollFrom) / 1000.0), still, st.drained);
    line("[fps]   between frames while scrolling: p50 %.1f ms, p95 %.1f ms, max %.1f ms", percentile(gaps, 0.5), percentile(gaps, 0.95),
        percentile(gaps, 1.0));
    line("[fps]   cut + read back: p50 %.2f ms, p95 %.2f ms, max %.2f ms", percentile(st.readMs, 0.5), percentile(st.readMs, 0.95),
        percentile(st.readMs, 1.0));
    line("[fps]   consecutive pairs %d: moved %d (exact shift %d, 90 %%+ rows exact %d), changed without a clean shift %d", st.pairs, st.moved,
        st.exact, st.nearly, st.changedNoShift);
    line("[fps]   the shifts seen (dy:count): %s", dyText.c_str());
    summary("fps " + who, format("%.1f frames/s scrolling, %d still; read p50 %.2f ms; moved pairs %d, exact %d, 90%%+ %d, unclean %d",
                                    during / std::max(0.001, (scrollTo - scrollFrom) / 1000.0), still, percentile(st.readMs, 0.5), st.moved,
                                    st.exact, st.nearly, st.changedNoShift));
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    // The multithreaded apartment first: the blocking waits on WinRT's
    // asynchronous calls and UI Automation want it (Qt's OleInitialize then
    // fails, which only costs drag and drop). As WinShun.exe sets up Qt
    // Quick (src/app/main.cpp) after that.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    qputenv("QT_QPA_PLATFORM", "windows:fontengine=freetype");
    qputenv("QT_D3D_NO_VBLANK_THREAD", "1");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);

    const QStringList args = QCoreApplication::arguments().mid(1);
    if (args.isEmpty()) {
        line("usage: scroll-probe exclude | wheel T... [--notches N] | uia T... [--notches N] | fps T [seconds | steps] [--uia | --percent] [--settle] [--ignore-right N]");
        return 2;
    }
    const QString cmd = args[0];
    OSVERSIONINFOW os {sizeof os};
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (auto get = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))))
        get(&os);
    line("scroll-probe %s: Windows %lu.%lu.%lu, Qt %s", qPrintable(cmd), os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, qVersion());
    int result = 0;
    try {
        if (cmd == u"exclude") {
            result = cmdExclude();
        } else if (cmd == u"wheel" || cmd == u"uia") {
            int notches = 3;
            std::vector<Target> targets;
            for (int i = 1; i < args.size(); ++i) {
                if (args[i] == u"--notches" && i + 1 < args.size()) {
                    notches = args[++i].toInt();
                    continue;
                }
                if (args[i] == u"--save" && i + 1 < args.size()) {
                    g_saveDir = args[++i].toStdString();
                    continue;
                }
                Target t;
                if (parseTarget(args[i], &t))
                    targets.push_back(t);
                else
                    line("[wheel] not a window: %s", qPrintable(args[i]));
            }
            result = cmd == u"wheel" ? cmdWheel(targets, std::max(1, notches)) : cmdUia(targets, std::max(1, notches));
        } else if (cmd == u"fps" && args.size() >= 2) {
            Target t;
            if (!parseTarget(args[1], &t)) {
                line("[fps] not a window: %s", qPrintable(args[1]));
                return 2;
            }
            int seconds = 3;
            bool byUia = false;
            bool settle = false;
            bool byPercent = false;
            for (int i = 2; i < args.size(); ++i) {
                if (args[i] == u"--uia")
                    byUia = true;
                else if (args[i] == u"--settle")
                    settle = true;
                else if (args[i] == u"--percent")
                    byPercent = byUia = true;
                else if (args[i] == u"--ignore-right" && i + 1 < args.size())
                    g_ignoreRight = args[++i].toInt();
                else
                    seconds = std::max(1, args[i].toInt());
            }
            result = cmdFps(t, seconds, byUia, settle, byPercent);
        } else {
            line("unknown command");
            return 2;
        }
    } catch (const winrt::hresult_error& e) {
        line("error 0x%08X %s", unsigned(e.code().value), narrow(std::wstring(e.message())).c_str());
        result = 1;
    }
    size_t w = 4;
    for (const auto& r : g_summary)
        w = std::max(w, r.item.size());
    line("");
    line("==== summary ====");
    for (const auto& r : g_summary)
        line("%-*s | %s", int(w), r.item.c_str(), r.value.c_str());
    return result;
}
