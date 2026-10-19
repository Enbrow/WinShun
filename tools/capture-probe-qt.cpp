// Screen capture, stage 0, the Qt side (docs/design-capture.md, sections 4.2
// and 4.3): what WinShun's own Qt Quick setup costs for the capture overlay
// and for pinned pictures, measured before either is written.
//
//   capture-probe-qt show    a prepared, hidden full-screen overlay: show() to
//                            the first frameSwapped, cold and warm, and whether
//                            the frozen picture it shows matches the screen
//   capture-probe-qt pins    memory of 1, 5, 10, 20 pin windows, and after
//                            they are all destroyed
//   capture-probe-qt pixel   1:1 display of a noise picture at the screen's DPR
//   capture-probe-qt all     the three in order
//   --software               Qt Quick's software renderer instead of D3D11
//
// The same Qt Quick settings as WinShun.exe (src/app/main.cpp): D3D11,
// FreeType, no vblank thread. Windows here never take the focus, so the
// probe does not steal the keyboard from whoever is using the PC.

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QScreen>
#include <QThread>

#include <windows.h>

#include <dwmapi.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

struct Row {
    QString item;
    QString value;
};
std::vector<Row> g_summary;

void line(const QString& s)
{
    std::fputs(s.toUtf8().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void summary(const QString& item, const QString& value)
{
    g_summary.push_back({item, value});
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

bool waitUntil(int timeoutMs, const std::function<bool()>& done)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::usleep(200);
    }
    return true;
}

void waitFor(int ms)
{
    waitUntil(ms, [] { return false; });
}

// ---------------------------------------------------------------------------
// Pictures.

QImage noise(int w, int h, quint32 seed)
{
    QImage img(w, h, QImage::Format_ARGB32);
    QRandomGenerator rng(seed);
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<quint32*>(img.scanLine(y));
        for (int x = 0; x < w; ++x)
            row[x] = 0xFF000000u | (rng.generate() & 0xFFFFFFu);
    }
    return img;
}

// The screen as the user sees it, in physical pixels (the process is
// per-monitor aware, Qt's default).
QImage bitblt(const RECT& r)
{
    const int w = r.right - r.left, h = r.bottom - r.top;
    QImage img(w, h, QImage::Format_RGB32);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(mem, bmp, 0, static_cast<UINT>(h), img.bits(), &bi, DIB_RGB_COLORS);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}

struct Diff {
    qint64 differing = 0;
    int maxDiff = 0;
    QRect box;
};

// a and b compared over a's size; b is read at (ox, oy).
Diff compare(const QImage& a, const QImage& b, int ox = 0, int oy = 0)
{
    Diff d;
    for (int y = 0; y < a.height(); ++y) {
        if (y + oy >= b.height())
            break;
        const auto* ra = reinterpret_cast<const quint32*>(a.constScanLine(y));
        const auto* rb = reinterpret_cast<const quint32*>(b.constScanLine(y + oy));
        for (int x = 0; x < a.width() && x + ox < b.width(); ++x) {
            const quint32 pa = ra[x], pb = rb[x + ox];
            const int m = std::max({std::abs(int(pa & 0xFF) - int(pb & 0xFF)), std::abs(int((pa >> 8) & 0xFF) - int((pb >> 8) & 0xFF)),
                std::abs(int((pa >> 16) & 0xFF) - int((pb >> 16) & 0xFF))});
            d.maxDiff = std::max(d.maxDiff, m);
            if (m) {
                ++d.differing;
                d.box = d.box.united(QRect(x, y, 1, 1));
            }
        }
    }
    return d;
}

QString boxText(const Diff& d)
{
    return d.differing ? u"x %1..%2, y %3..%4"_s.arg(d.box.left()).arg(d.box.right()).arg(d.box.top()).arg(d.box.bottom()) : u"none"_s;
}

// The image provider every window reads from. "screen/<n>" grabs the
// primary screen anew (a new frozen picture per press, as the overlay will
// get); "noise/<w>x<h>/<seed>" and "noisedpr/..." are test patterns, the
// latter tagged with the screen's device pixel ratio.
class Provider : public QQuickImageProvider {
public:
    Provider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage lastScreen;
    std::atomic<int> calls{0};
    QImage requestImage(const QString& id, QSize* size, const QSize&) override
    {
        ++calls;
        QImage img;
        const QStringList parts = id.split(u'/');
        if (parts.value(0) == u"screen") {
            QScreen* s = QGuiApplication::primaryScreen();
            const QRect g = s->geometry();
            const qreal dpr = s->devicePixelRatio();
            RECT r{LONG(std::lround(g.x() * dpr)), LONG(std::lround(g.y() * dpr)), 0, 0};
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromPoint(POINT{r.left, r.top}, MONITOR_DEFAULTTOPRIMARY), &mi);
            img = bitblt(mi.rcMonitor);
            lastScreen = img;
        } else if (parts.value(0) == u"noise" || parts.value(0) == u"noisedpr") {
            const QStringList wh = parts.value(1).split(u'x');
            img = noise(wh.value(0).toInt(), wh.value(1).toInt(), parts.value(2).toUInt());
            if (parts.value(0) == u"noisedpr")
                img.setDevicePixelRatio(QGuiApplication::primaryScreen()->devicePixelRatio());
        }
        if (size)
            *size = img.size();
        return img;
    }
};

Provider* g_provider = nullptr;
QQmlEngine* g_engine = nullptr;

QQuickItem* makeItem(const QString& qml, QQuickWindow* window)
{
    QQmlComponent component(g_engine);
    component.setData(qml.toUtf8(), QUrl(u"probe.qml"_s));
    QObject* obj = component.create();
    if (!obj) {
        line(u"QML error: "_s + component.errorString());
        return nullptr;
    }
    auto* item = qobject_cast<QQuickItem*>(obj);
    item->setParentItem(window->contentItem());
    item->setParent(window->contentItem());
    return item;
}

// The window flags of the overlay and of pins: frameless, topmost, no
// taskbar button, and never the focus (this is a probe on a PC in use).
QQuickWindow* makeWindow()
{
    auto* w = new QQuickWindow;
    w->setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus);
    w->setColor(QColor(255, 0, 255));
    return w;
}

// Times show() to the first frameSwapped (render thread, direct) and to
// the following DwmFlush (the frame has been composed by then).
struct ShowTiming {
    double swapMs = -1;
    double composedMs = -1;
};

ShowTiming timeShow(QQuickWindow* w)
{
    std::atomic<qint64> swappedAt{-1};
    QElapsedTimer t;
    auto c = QObject::connect(w, &QQuickWindow::frameSwapped, w, [&] {
        qint64 expected = -1;
        swappedAt.compare_exchange_strong(expected, t.nsecsElapsed());
    }, Qt::DirectConnection);
    t.start();
    w->show();
    ShowTiming st;
    if (waitUntil(3000, [&] { return swappedAt.load() >= 0; })) {
        st.swapMs = swappedAt.load() / 1e6;
        DwmFlush();
        st.composedMs = t.nsecsElapsed() / 1e6;
    }
    QObject::disconnect(c);
    return st;
}

RECT clientRect(QWindow* w)
{
    const HWND h = reinterpret_cast<HWND>(w->winId());
    RECT r;
    GetClientRect(h, &r);
    POINT p{0, 0};
    ClientToScreen(h, &p);
    OffsetRect(&r, p.x, p.y);
    return r;
}

// ---------------------------------------------------------------------------
// Memory.

struct Mem {
    double privateWsMB = 0;
    double commitMB = 0;
    double gpuLocalMB = -1;
    double gpuSharedMB = -1;
};

Mem memory()
{
    Mem m;
    PROCESS_MEMORY_COUNTERS_EX2 pmc{};
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
        m.privateWsMB = pmc.PrivateWorkingSetSize / 1048576.0;
        m.commitMB = pmc.PrivateUsage / 1048576.0;
    }
    // Swap chains and textures mostly live in video memory, which the two
    // above do not show: DXGI reports this process's share on the adapter.
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(0, &adapter) == S_OK) {
            IDXGIAdapter3* a3 = nullptr;
            if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&a3)))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
                if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
                    m.gpuLocalMB = local.CurrentUsage / 1048576.0;
                if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)))
                    m.gpuSharedMB = shared.CurrentUsage / 1048576.0;
                a3->Release();
            }
            adapter->Release();
        }
        factory->Release();
    }
    return m;
}

QString memText(const Mem& m)
{
    return u"private WS %1 MB, commit %2 MB, GPU local %3 MB, GPU shared %4 MB"_s.arg(m.privateWsMB, 0, 'f', 1)
        .arg(m.commitMB, 0, 'f', 1)
        .arg(m.gpuLocalMB, 0, 'f', 1)
        .arg(m.gpuSharedMB, 0, 'f', 1);
}

// ---------------------------------------------------------------------------
// show

void cmdShow()
{
    QScreen* screen = QGuiApplication::primaryScreen();
    const QRect g = screen->geometry();
    const qreal dpr = screen->devicePixelRatio();
    line(u"[show] primary screen: logical %1,%2 %3x%4, DPR %5"_s.arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height()).arg(dpr));

    QQuickWindow* w = makeWindow();
    w->setGeometry(g);
    // The frozen picture: physical pixels, shown at physical / DPR logical
    // pixels and not smoothed, so each pixel lands on one screen pixel.
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int pw = mi.rcMonitor.right - mi.rcMonitor.left, ph = mi.rcMonitor.bottom - mi.rcMonitor.top;
    QQuickItem* img = makeItem(u"import QtQuick\nImage { cache: false; smooth: false; width: %1; height: %2 }"_s.arg(pw / dpr, 0, 'g', 17)
                                  .arg(ph / dpr, 0, 'g', 17),
        w);
    // Prepared: native window created, picture loaded, never shown yet.
    w->create();
    int n = 0;
    QElapsedTimer t;
    t.start();
    img->setProperty("source", QUrl(u"image://probe/screen/%1"_s.arg(n++)));
    const double firstLoad = t.nsecsElapsed() / 1e6;
    const int callsLoaded = g_provider->calls;
    const ShowTiming cold = timeShow(w);
    // Qt reloads a cache:false Image when its window first gets a screen
    // (QQuickImageBase on a device pixel ratio change): then the picture
    // shown is a second grab, not the one prepared before show().
    line(u"[show] provider calls: %1 before show(), %2 after the first frame"_s.arg(callsLoaded).arg(g_provider->calls.load()));
    waitFor(150);
    // The acceptance test of 4.2: the overlay, showing the frozen screen,
    // must look exactly like the screen did.
    const QImage shown = bitblt(mi.rcMonitor);
    const Diff d = compare(g_provider->lastScreen, shown);
    const RECT cr = clientRect(w);
    line(u"[show] overlay window client rect (physical): %1,%2 %3x%4 for a %5x%6 monitor"_s.arg(cr.left).arg(cr.top)
             .arg(cr.right - cr.left).arg(cr.bottom - cr.top).arg(pw).arg(ph));
    line(u"[show] cold: load picture %1 ms, show() to first frameSwapped %2 ms, to DwmFlush %3 ms"_s.arg(firstLoad, 0, 'f', 1)
             .arg(cold.swapMs, 0, 'f', 1).arg(cold.composedMs, 0, 'f', 1));
    line(u"[show] overlay vs the screen it froze: %1 px differ, max %2, box %3"_s.arg(d.differing).arg(d.maxDiff).arg(boxText(d)));
    summary(u"overlay show cold"_s, u"%1 ms to frameSwapped, %2 ms composed"_s.arg(cold.swapMs, 0, 'f', 1).arg(cold.composedMs, 0, 'f', 1));
    summary(u"overlay 1:1 vs frozen screen"_s, u"%1 px differ, max %2"_s.arg(d.differing).arg(d.maxDiff));
    w->hide();
    waitFor(300);
    if (d.differing) {
        // Where do they differ: a window above the overlay, or the screen
        // itself changing? The screen without the overlay tells.
        const QImage after = bitblt(mi.rcMonitor);
        const Diff live = compare(g_provider->lastScreen, after);
        line(u"[show] for reference, the screen itself before vs after (no overlay): %1 px differ, box %2"_s.arg(live.differing).arg(boxText(live)));
        g_provider->lastScreen.save(u"capture-out/show-frozen.png"_s);
        shown.save(u"capture-out/show-overlay.png"_s);
        after.save(u"capture-out/show-after.png"_s);
    }

    for (bool newPicture : {false, true}) {
        std::vector<double> swaps, composed, loads;
        for (int i = 0; i < 10; ++i) {
            if (newPicture) {
                t.restart();
                img->setProperty("source", QUrl(u"image://probe/screen/%1"_s.arg(n++)));
                loads.push_back(t.nsecsElapsed() / 1e6);
            }
            const ShowTiming st = timeShow(w);
            swaps.push_back(st.swapMs);
            composed.push_back(st.composedMs);
            waitFor(100);
            w->hide();
            waitFor(300);
        }
        const QString name = newPicture ? u"warm, new picture each time"_s : u"warm, same picture"_s;
        line(u"[show] %1: show() to frameSwapped median %2 ms (min %3, max %4), composed median %5 ms%6"_s.arg(name)
                 .arg(median(swaps), 0, 'f', 1)
                 .arg(*std::min_element(swaps.begin(), swaps.end()), 0, 'f', 1)
                 .arg(*std::max_element(swaps.begin(), swaps.end()), 0, 'f', 1)
                 .arg(median(composed), 0, 'f', 1)
                 .arg(newPicture ? u", grab + load median %1 ms"_s.arg(median(loads), 0, 'f', 1) : QString()));
        summary(u"overlay show "_s + name, u"%1 ms to frameSwapped, %2 ms composed"_s.arg(median(swaps), 0, 'f', 1).arg(median(composed), 0, 'f', 1));
    }
    // The same overlay showing noise of the monitor's physical size: unlike
    // a screen grab, every pixel differs from its neighbours, so any
    // resampling or a picture off by one pixel shows up, and nothing on the
    // screen can change under it.
    const QImage src = noise(pw, ph, 5).convertToFormat(QImage::Format_RGB32);
    img->setProperty("source", QUrl(u"image://probe/noise/%1x%2/5"_s.arg(pw).arg(ph)));
    // Two ways to size the overlay: Qt's logical screen geometry (at 150%
    // 1707x1067 logical is 2560.5 physical, which Qt rounds up), and the
    // monitor's exact physical rectangle set on the native window.
    // Qt keeps window sizes in whole logical pixels, and the buffer is that
    // size times the DPR, rounded: 1707 logical gives 2561 pixels, and the
    // scene is stretched onto it at 2561 / 1707 = 1.50029, not 1.5. Three
    // ways to size the picture: physical / DPR; filling the window; and
    // physical / (buffer pixels / logical size), the scale Qt really uses.
    for (int variant = 0; variant < 6; ++variant) {
        const bool native = variant % 2 == 1;
        const bool fill = variant == 2 || variant == 3;
        const bool effective = variant >= 4;
        if (fill) {
            img->setWidth(w->width());
            img->setHeight(w->height());
        } else if (effective) {
            img->setWidth(pw * w->width() / std::round(w->width() * dpr));
            img->setHeight(ph * w->height() / std::round(w->height() * dpr));
        } else {
            img->setWidth(pw / dpr);
            img->setHeight(ph / dpr);
        }
        if (!native)
            w->setGeometry(g);
        timeShow(w);
        if (native) {
            SetWindowPos(reinterpret_cast<HWND>(w->winId()), HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, pw, ph,
                SWP_NOACTIVATE);
            // Let Qt take the new size and render a frame at it.
            waitFor(500);
        }
        waitFor(300);
        DwmFlush();
        const RECT ncr = clientRect(w);
        const QImage shot = bitblt(mi.rcMonitor);
        const Diff dn = compare(src, shot);
        const QString name = (native ? u"native SetWindowPos to the monitor rect"_s : u"Qt geometry = screen geometry"_s)
            + (fill ? u", picture fills the window"_s : effective ? u", picture physical / effective scale"_s : u", picture physical / DPR"_s);
        line(u"[show] overlay with full-screen noise, %1 (client %2x%3, Qt size %4x%5): %6 of %7 px differ, max %8, box %9"_s.arg(name)
                 .arg(ncr.right - ncr.left).arg(ncr.bottom - ncr.top).arg(w->width()).arg(w->height())
                 .arg(dn.differing).arg(qint64(pw) * ph).arg(dn.maxDiff).arg(boxText(dn)));
        summary(u"overlay 1:1 noise, "_s + name, u"%1 px differ, max %2, box %3"_s.arg(dn.differing).arg(dn.maxDiff).arg(boxText(dn)));
        if (dn.differing) {
            shot.save(u"capture-out/show-noise-variant%1.png"_s.arg(variant));
            src.save(u"capture-out/show-noise-src.png"_s);
        }
        w->hide();
        waitFor(200);
    }
    delete w;
}

// ---------------------------------------------------------------------------
// pins

void cmdPins()
{
    QScreen* screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    const QRect avail = screen->availableGeometry();
    waitFor(500);
    const Mem base = memory();
    line(u"[pins] before any pin: "_s + memText(base));
    summary(u"pins 0 (baseline)"_s, memText(base));
    std::vector<QQuickWindow*> pins;
    int seed = 1;
    for (int target : {1, 5, 10, 20}) {
        while (int(pins.size()) < target) {
            QQuickWindow* w = makeWindow();
            const int k = int(pins.size());
            const QSizeF logical(800 / dpr, 600 / dpr);
            w->setGeometry(QRect(QPoint(avail.x() + 20 + (k % 10) * 24, avail.y() + 20 + (k % 10) * 24), logical.toSize()));
            makeItem(u"import QtQuick\nImage { cache: false; smooth: false; width: %1; height: %2; source: \"image://probe/noise/800x600/%3\" }"_s
                         .arg(800 / dpr, 0, 'g', 17).arg(600 / dpr, 0, 'g', 17).arg(seed++),
                w);
            const ShowTiming st = timeShow(w);
            if (st.swapMs < 0)
                line(u"[pins] pin %1 did not render within 3 s"_s.arg(k + 1));
            pins.push_back(w);
        }
        waitFor(800);
        const Mem m = memory();
        line(u"[pins] %1 pin(s): %2 (+%3 MB private WS, +%4 MB commit, +%5 MB GPU local over the baseline)"_s.arg(target).arg(memText(m))
                 .arg(m.privateWsMB - base.privateWsMB, 0, 'f', 1)
                 .arg(m.commitMB - base.commitMB, 0, 'f', 1)
                 .arg(m.gpuLocalMB - base.gpuLocalMB, 0, 'f', 1));
        summary(u"pins %1"_s.arg(target), u"WS +%1 MB, commit +%2 MB, GPU local +%3 MB, GPU shared +%4 MB"_s
                                             .arg(m.privateWsMB - base.privateWsMB, 0, 'f', 1)
                                             .arg(m.commitMB - base.commitMB, 0, 'f', 1)
                                             .arg(m.gpuLocalMB - base.gpuLocalMB, 0, 'f', 1)
                                             .arg(m.gpuSharedMB - base.gpuSharedMB, 0, 'f', 1));
    }
    // Deleted outright: deleteLater() outside a running event loop would
    // leave them alive and the numbers below would mean nothing.
    for (QQuickWindow* w : pins)
        delete w;
    pins.clear();
    g_engine->trimComponentCache();
    waitFor(2000);
    const Mem after = memory();
    line(u"[pins] all destroyed (%1 Qt windows left), 2 s later: "_s.arg(QGuiApplication::allWindows().size()) + memText(after));
    summary(u"pins destroyed, 2 s later"_s, u"WS +%1 MB, commit +%2 MB, GPU local +%3 MB over the baseline"_s
                                                 .arg(after.privateWsMB - base.privateWsMB, 0, 'f', 1)
                                                 .arg(after.commitMB - base.commitMB, 0, 'f', 1)
                                                 .arg(after.gpuLocalMB - base.gpuLocalMB, 0, 'f', 1));
    // What WinShun's IdleTrim would add on top: the working set emptied.
    SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
    waitFor(300);
    const Mem trimmed = memory();
    line(u"[pins] after emptying the working set (like IdleTrim): "_s + memText(trimmed));
    summary(u"pins destroyed + working set emptied"_s, u"WS +%1 MB, commit +%2 MB over the baseline"_s
                                                         .arg(trimmed.privateWsMB - base.privateWsMB, 0, 'f', 1)
                                                         .arg(trimmed.commitMB - base.commitMB, 0, 'f', 1));
    // The commit left behind: a cache the next pins reuse, or a leak that
    // grows with every round? Twenty more, then gone again.
    for (int k = 0; k < 20; ++k) {
        QQuickWindow* w = makeWindow();
        w->setGeometry(QRect(QPoint(avail.x() + 20 + (k % 10) * 24, avail.y() + 20 + (k % 10) * 24), QSizeF(800 / dpr, 600 / dpr).toSize()));
        makeItem(u"import QtQuick\nImage { cache: false; smooth: false; width: %1; height: %2; source: \"image://probe/noise/800x600/%3\" }"_s
                     .arg(800 / dpr, 0, 'g', 17).arg(600 / dpr, 0, 'g', 17).arg(seed++),
            w);
        timeShow(w);
        pins.push_back(w);
    }
    waitFor(800);
    const Mem again = memory();
    for (QQuickWindow* w : pins)
        delete w;
    pins.clear();
    waitFor(2000);
    const Mem again2 = memory();
    line(u"[pins] second round: 20 pins %1; destroyed, 2 s later: %2"_s.arg(memText(again), memText(again2)));
    summary(u"pins second round, destroyed"_s, u"commit +%1 MB over the baseline (20 open: +%2 MB)"_s
                                                  .arg(again2.commitMB - base.commitMB, 0, 'f', 1)
                                                  .arg(again.commitMB - base.commitMB, 0, 'f', 1));
}

// ---------------------------------------------------------------------------
// pixel

void pixelCase(int pw, int ph, bool dprTagged, const QPoint& logicalPos)
{
    QScreen* screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    const quint32 seed = 77;
    const QImage src = noise(pw, ph, seed).convertToFormat(QImage::Format_RGB32);
    QQuickWindow* w = makeWindow();
    // A magenta margin around the picture shows a picture too small or too
    // large by a pixel.
    w->setGeometry(QRect(logicalPos, QSize(int(std::ceil(pw / dpr)) + 20, int(std::ceil(ph / dpr)) + 20)));
    const QString qml = dprTagged
        ? u"import QtQuick\nImage { cache: false; smooth: false; width: implicitWidth; height: implicitHeight; source: \"image://probe/noisedpr/%1x%2/%3\" }"_s
              .arg(pw).arg(ph).arg(seed)
        : u"import QtQuick\nImage { cache: false; smooth: false; width: %1; height: %2; source: \"image://probe/noise/%3x%4/%5\" }"_s
              .arg(pw / dpr, 0, 'g', 17).arg(ph / dpr, 0, 'g', 17).arg(pw).arg(ph).arg(seed);
    QQuickItem* item = makeItem(qml, w);
    timeShow(w);
    waitFor(200);
    DwmFlush();
    const RECT cr = clientRect(w);
    const QImage shot = bitblt(cr);
    const Diff d = compare(src, shot);
    // The column right of and the row below the picture should be margin.
    int marginWrong = 0;
    for (int y = 0; y < ph && pw < shot.width(); ++y)
        marginWrong += (shot.pixel(pw, y) & 0xFFFFFF) != 0xFF00FF;
    for (int x = 0; x < pw && ph < shot.height(); ++x)
        marginWrong += (shot.pixel(x, ph) & 0xFFFFFF) != 0xFF00FF;
    const QString name = u"%1x%2 %3, window at logical %4,%5 (physical %6,%7)"_s.arg(pw).arg(ph)
                             .arg(dprTagged ? u"image tagged with DPR, size = implicit"_s : u"size = physical / DPR"_s)
                             .arg(logicalPos.x()).arg(logicalPos.y()).arg(cr.left).arg(cr.top);
    line(u"[pixel] %1: item %2x%3 logical; %4 of %5 px differ, max %6, box %7; margin pixels not magenta: %8"_s.arg(name)
             .arg(item ? item->width() : -1).arg(item ? item->height() : -1)
             .arg(d.differing).arg(qint64(pw) * ph).arg(d.maxDiff).arg(boxText(d)).arg(marginWrong));
    summary(u"pixel "_s + name, u"%1 px differ, max %2, margin wrong %3"_s.arg(d.differing).arg(d.maxDiff).arg(marginWrong));
    if (d.differing)
        shot.save(u"capture-out/pixel-%1x%2-%3.png"_s.arg(pw).arg(ph).arg(dprTagged ? u"dpr"_s : u"div"_s));
    delete w;
    waitFor(200);
}

void cmdPixel()
{
    const qreal dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
    line(u"[pixel] DPR %1"_s.arg(dpr));
    // An even and an odd size: at 150% the odd one has no whole logical size.
    pixelCase(600, 400, false, QPoint(200, 200));
    pixelCase(601, 401, false, QPoint(200, 200));
    pixelCase(600, 400, true, QPoint(200, 200));
    pixelCase(601, 401, true, QPoint(200, 200));
    // A window whose logical position is not a whole physical pixel at 150%.
    pixelCase(600, 400, false, QPoint(201, 201));
}

} // namespace

int main(int argc, char** argv)
{
    // As WinShun.exe sets up Qt Quick (src/app/main.cpp).
    const bool software = std::any_of(argv + 1, argv + argc, [](const char* a) { return std::strcmp(a, "--software") == 0; });
    qputenv("QT_QPA_PLATFORM", software ? "windows:fontengine=gdi" : "windows:fontengine=freetype");
    qputenv("QT_D3D_NO_VBLANK_THREAD", "1");
    if (software)
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    else
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    g_engine = &engine;
    g_provider = new Provider;
    engine.addImageProvider(u"probe"_s, g_provider); // the engine owns it
    CreateDirectoryW(L"capture-out", nullptr);

    QString cmd;
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-')
            cmd = QString::fromLocal8Bit(argv[i]);
    line(u"capture-probe-qt: Qt %1, renderer %2, render loop %3"_s.arg(QString::fromLatin1(qVersion()), software ? u"software"_s : u"d3d11"_s,
        qEnvironmentVariableIsSet("QSG_RENDER_LOOP") ? QString::fromLocal8Bit(qgetenv("QSG_RENDER_LOOP")) : u"default"_s));
    if (cmd == u"show")
        cmdShow();
    else if (cmd == u"pins")
        cmdPins();
    else if (cmd == u"pixel")
        cmdPixel();
    else if (cmd == u"all") {
        cmdShow();
        cmdPixel();
        cmdPins(); // last: what it measures is easier to read with nothing else left
    } else {
        line(u"usage: capture-probe-qt show|pins|pixel|all [--software]"_s);
        return 2;
    }
    qsizetype w = 4;
    for (const auto& r : g_summary)
        w = std::max(w, r.item.size());
    line(QString());
    line(u"==== summary (%1) ===="_s.arg(software ? u"software"_s : u"d3d11"_s));
    for (const auto& r : g_summary)
        line(r.item.leftJustified(w) + u" | "_s + r.value);
    return 0;
}
