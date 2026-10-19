// C++/WinRT before Qt's headers: Qt's "signals" and "slots" macros would
// rewrite what is in it (as in platform/TextRecognizer.cpp).
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include "Grabber.h"

#include "Hdr.h"

#include <QDebug>
#include <QElapsedTimer>

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

using namespace Qt::StringLiterals;

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace ws::capture {

namespace {

constexpr DWORD kFrameWaitMs = 1000; // a frame comes in 6–11 ms (stage 0); a second means WGC is stuck

QRect toRect(const RECT& r)
{
    return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

struct ColorMode {
    bool hdr = false;
    double sdrWhiteNits = 80;
};

// Whether the monitor (by its GDI name, "\\.\DISPLAY1") shows HDR, and at
// which SDR white level Windows composes SDR content there.
ColorMode colorModeOf(const wchar_t* device)
{
    ColorMode mode;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return mode;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return mode;
    for (UINT32 i = 0; i < pathCount; ++i) {
        const DISPLAYCONFIG_PATH_INFO& path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || std::wcscmp(source.viewGdiDeviceName, device) != 0)
            continue;
        // Windows 11 24H2 tells HDR from wide colour gamut; before it, the
        // older call's "advanced colour" on a non-WCG monitor means HDR.
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 info2 {};
        info2.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
        info2.header.size = sizeof info2;
        info2.header.adapterId = path.targetInfo.adapterId;
        info2.header.id = path.targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) {
            mode.hdr = info2.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
        } else {
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info {};
            info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
            info.header.size = sizeof info;
            info.header.adapterId = path.targetInfo.adapterId;
            info.header.id = path.targetInfo.id;
            if (::DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS)
                mode.hdr = info.advancedColorEnabled && !info.wideColorEnforced;
        }
        // In thousandths of 80 nits (scRGB 1.0).
        DISPLAYCONFIG_SDR_WHITE_LEVEL white {};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof white;
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
            mode.sdrWhiteNits = white.SDRWhiteLevel / 1000.0 * 80.0;
        break;
    }
    return mode;
}

// The screen's pixels in `r` through GDI: CAPTUREBLT takes layered windows
// in too. SDR only (HDR monitors come out washed out).
QImage bitblt(const QRect& r)
{
    QImage image(r.width(), r.height(), QImage::Format_RGB32);
    const HDC screen = ::GetDC(nullptr);
    const HDC memory = ::CreateCompatibleDC(screen);
    const HBITMAP bitmap = ::CreateCompatibleBitmap(screen, r.width(), r.height());
    const HGDIOBJ old = ::SelectObject(memory, bitmap);
    const bool ok = ::BitBlt(memory, 0, 0, r.width(), r.height(), screen, r.x(), r.y(), SRCCOPY | CAPTUREBLT);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = r.width();
    info.bmiHeader.biHeight = -r.height(); // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const int lines = ok ? ::GetDIBits(memory, bitmap, 0, static_cast<UINT>(r.height()), image.bits(), &info, DIB_RGB_COLORS) : 0;
    ::SelectObject(memory, old);
    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return lines == r.height() ? image : QImage();
}

} // namespace

struct Grabber::Worker {
    struct Monitor {
        HMONITOR handle = nullptr;
        QRect rect;
        ColorMode color;
        wgc::GraphicsCaptureItem item {nullptr};
    };

    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool stopping = false;

    // Only touched on the thread:
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID3D11DeviceContext> context;
    wd3d::IDirect3DDevice device {nullptr};
    bool borderless = false;
    std::vector<Monitor> monitors;

    Worker()
    {
        thread = std::thread([this] { run(); });
        post([this] { prepare(); });
    }

    ~Worker()
    {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        thread.join();
    }

    void post(std::function<void()> job)
    {
        {
            const std::lock_guard lock(mutex);
            jobs.push_back(std::move(job));
        }
        wake.notify_all();
    }

    void run()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (jobs.empty())
                    break; // stopping, nothing left
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            job();
        }
        // The WinRT and D3D objects go on the thread that made them.
        monitors.clear();
        device = nullptr;
        context = nullptr;
        d3d = nullptr;
        winrt::uninit_apartment();
    }

    // The device, borderless access, an item per monitor. Each part failing
    // leaves its BitBlt fallback.
    void prepare()
    {
        QElapsedTimer clock;
        clock.start();
        try {
            if (!wgc::GraphicsCaptureSession::IsSupported()) {
                qWarning() << "Screen capture: Windows.Graphics.Capture is not supported here; BitBlt is used";
                return;
            }
            if (!device) {
                winrt::check_hresult(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, context.put()));
                const auto dxgi = d3d.as<IDXGIDevice>();
                winrt::com_ptr<::IInspectable> inspectable;
                winrt::check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
                device = inspectable.as<wd3d::IDirect3DDevice>();
            }
            if (!borderless) {
                // Allowed for an unpackaged program without asking (stage 0);
                // without it the session draws a yellow border on the screen.
                const auto access = wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
                borderless = access == winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed;
            }
        } catch (const winrt::hresult_error& e) {
            qWarning().noquote() << u"Screen capture: Windows.Graphics.Capture not ready (0x%1 %2); BitBlt is used"_s
                                        .arg(static_cast<quint32>(e.code().value), 8, 16, QChar(u'0'))
                                        .arg(QString::fromWCharArray(e.message().c_str()));
            device = nullptr;
            context = nullptr;
            d3d = nullptr;
        }
        enumerate();
        qInfo().noquote() << u"Screen capture: prepared in %1 ms (%2 monitor(s), WGC %3, borderless %4)"_s
                                 .arg(clock.elapsed())
                                 .arg(monitors.size())
                                 .arg(device ? u"yes"_s : u"no"_s, borderless ? u"yes"_s : u"no"_s);
    }

    void enumerate()
    {
        monitors.clear();
        ::EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR handle, HDC, LPRECT, LPARAM param) -> BOOL {
                auto* self = reinterpret_cast<Worker*>(param);
                MONITORINFOEXW info {};
                info.cbSize = sizeof info;
                if (!::GetMonitorInfoW(handle, &info))
                    return TRUE;
                Monitor m;
                m.handle = handle;
                m.rect = toRect(info.rcMonitor);
                m.color = colorModeOf(info.szDevice);
                self->monitors.push_back(std::move(m));
                return TRUE;
            },
            reinterpret_cast<LPARAM>(this));
        if (!device)
            return;
        const auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        for (Monitor& m : monitors) {
            try {
                winrt::check_hresult(interop->CreateForMonitor(m.handle, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                    winrt::put_abi(m.item)));
            } catch (const winrt::hresult_error& e) {
                qWarning().noquote() << u"Screen capture: no capture item for a monitor (0x%1)"_s.arg(
                    static_cast<quint32>(e.code().value), 8, 16, QChar(u'0'));
                m.item = nullptr;
            }
        }
    }

    // One frame of every monitor that has an item, all sessions started
    // together; a monitor whose frame does not come is left with a null image.
    struct Pending {
        wgc::Direct3D11CaptureFramePool pool {nullptr};
        wgc::GraphicsCaptureSession session {nullptr};
        winrt::event_token token;
        HANDLE arrived = nullptr;
        std::atomic<bool> taken {false};
        wgc::Direct3D11CaptureFrame frame {nullptr};
    };

    QImage readFrame(const wgc::Direct3D11CaptureFrame& frame, const Monitor& m)
    {
        winrt::com_ptr<ID3D11Texture2D> texture;
        const auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        if (static_cast<int>(desc.Width) != m.rect.width() || static_cast<int>(desc.Height) != m.rect.height())
            return {}; // the monitor changed under its item: BitBlt this time
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(d3d->CreateTexture2D(&desc, nullptr, staging.put()));
        context->CopyResource(staging.get(), texture.get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        QImage image;
        if (m.color.hdr) {
            image = mapScRgbToSdr(static_cast<const std::uint16_t*>(mapped.pData), static_cast<int>(desc.Width),
                static_cast<int>(desc.Height), static_cast<int>(mapped.RowPitch), m.color.sdrWhiteNits);
        } else {
            image = QImage(static_cast<int>(desc.Width), static_cast<int>(desc.Height), QImage::Format_RGB32);
            const std::size_t row = std::size_t(desc.Width) * 4;
            for (UINT y = 0; y < desc.Height; ++y)
                std::memcpy(image.scanLine(static_cast<int>(y)),
                    static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(mapped.RowPitch) * y, row);
        }
        context->Unmap(staging.get(), 0);
        return image;
    }

    std::vector<Shot> grabAll(QString* report)
    {
        QElapsedTimer clock;
        clock.start();
        std::vector<Shot> shots(monitors.size());
        std::vector<std::unique_ptr<Pending>> pending(monitors.size());
        std::vector<HANDLE> events;
        for (std::size_t i = 0; i < monitors.size(); ++i) {
            const Monitor& m = monitors[i];
            shots[i].monitor = m.handle;
            shots[i].rect = m.rect;
            shots[i].hdr = m.color.hdr;
            if (!device || !m.item)
                continue;
            auto p = std::make_unique<Pending>();
            try {
                const auto format = m.color.hdr ? wdx::DirectXPixelFormat::R16G16B16A16Float
                                                : wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
                p->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device, format, 1, m.item.Size());
                p->session = p->pool.CreateCaptureSession(m.item);
                p->session.IsCursorCaptureEnabled(false); // the overlay shows the real one
                if (borderless)
                    p->session.IsBorderRequired(false);
                p->arrived = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                Pending* raw = p.get();
                p->token = p->pool.FrameArrived([raw](const wgc::Direct3D11CaptureFramePool& sender, const auto&) {
                    if (raw->taken.exchange(true))
                        return;
                    raw->frame = sender.TryGetNextFrame();
                    ::SetEvent(raw->arrived);
                });
                p->session.StartCapture();
                events.push_back(p->arrived);
                pending[i] = std::move(p);
            } catch (const winrt::hresult_error& e) {
                qWarning().noquote() << u"Screen capture: could not start capturing a monitor (0x%1)"_s.arg(
                    static_cast<quint32>(e.code().value), 8, 16, QChar(u'0'));
                if (p && p->arrived)
                    ::CloseHandle(p->arrived);
            }
        }
        if (!events.empty())
            ::WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), TRUE, kFrameWaitMs);
        const qint64 framesMs = clock.elapsed();
        int viaWgc = 0;
        for (std::size_t i = 0; i < monitors.size(); ++i) {
            Pending* p = pending[i].get();
            if (p) {
                try {
                    if (::WaitForSingleObject(p->arrived, 0) == WAIT_OBJECT_0 && p->frame)
                        shots[i].image = readFrame(p->frame, monitors[i]);
                } catch (const winrt::hresult_error& e) {
                    qWarning().noquote() << u"Screen capture: could not read a frame (0x%1)"_s.arg(
                        static_cast<quint32>(e.code().value), 8, 16, QChar(u'0'));
                }
                try {
                    p->pool.FrameArrived(p->token);
                    if (p->frame)
                        p->frame.Close();
                    p->session.Close();
                    p->pool.Close();
                } catch (const winrt::hresult_error&) {
                }
                ::CloseHandle(p->arrived);
            }
            if (!shots[i].image.isNull()) {
                shots[i].wgc = true;
                ++viaWgc;
                continue;
            }
            shots[i].hdr = false;
            shots[i].image = bitblt(shots[i].rect);
        }
        if (report) {
            *report = u"%1 monitor(s), %2 by WGC (frames in %3 ms), the rest by BitBlt, %4 ms in all"_s.arg(monitors.size())
                          .arg(viaWgc)
                          .arg(framesMs)
                          .arg(clock.elapsed());
        }
        // Every monitor failing through WGC: the device may be lost (a driver
        // update, a GPU reset). Made again for the next time.
        if (viaWgc == 0 && device && !monitors.empty()) {
            device = nullptr;
            context = nullptr;
            d3d = nullptr;
            post([this] { prepare(); });
        }
        return shots;
    }
};

Grabber::Grabber()
    : m_worker(std::make_unique<Worker>())
{
}

Grabber::~Grabber() = default;

void Grabber::refresh()
{
    Worker* worker = m_worker.get();
    worker->post([worker] {
        if (worker->device)
            worker->enumerate();
        else
            worker->prepare();
    });
}

std::vector<Grabber::Shot> Grabber::grab(QString* report)
{
    std::promise<std::vector<Shot>> promise;
    auto future = promise.get_future();
    Worker* worker = m_worker.get();
    worker->post([worker, &promise, report] { promise.set_value(worker->grabAll(report)); });
    return future.get();
}

} // namespace ws::capture
