#include "ScrollGrabber.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace ws::capture {

namespace {

constexpr int kPollMs = 16; // BitBlt's frames: about the screen's

} // namespace

bool samePixels(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.depth() != 32 || b.depth() != 32)
        return false;
    const size_t bytes = size_t(a.width()) * 4;
    for (int y = 0; y < a.height(); ++y)
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y), bytes) != 0)
            return false;
    return true;
}

ScrollGrabber::ScrollGrabber(Grabber& grabber, const QRect& region)
    : m_region(region)
{
    m_clock.start();
    m_stream = grabber.stream(region);
}

ScrollGrabber::~ScrollGrabber() = default;

QImage ScrollGrabber::bitblt() const
{
    const QRect& r = m_region;
    QImage image(r.width(), r.height(), QImage::Format_RGB32);
    const HDC screen = ::GetDC(nullptr);
    const HDC memory = ::CreateCompatibleDC(screen);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = r.width();
    info.bmiHeader.biHeight = -r.height(); // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP section = ::CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (section && bits) {
        const HGDIOBJ old = ::SelectObject(memory, section);
        ok = ::BitBlt(memory, 0, 0, r.width(), r.height(), screen, r.x(), r.y(), SRCCOPY | CAPTUREBLT);
        ::GdiFlush();
        ::SelectObject(memory, old);
        if (ok) {
            const size_t row = size_t(r.width()) * 4;
            for (int y = 0; y < r.height(); ++y) {
                auto* dst = reinterpret_cast<uint32_t*>(image.scanLine(y));
                std::memcpy(dst, static_cast<const uint8_t*>(bits) + row * size_t(y), row);
                for (int x = 0; x < r.width(); ++x)
                    dst[x] |= 0xFF000000u; // GDI leaves the alpha byte 0
            }
        }
    }
    if (section)
        ::DeleteObject(section);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return ok ? image : QImage();
}

QImage ScrollGrabber::changed(int timeoutMs)
{
    const qint64 until = m_clock.elapsed() + std::max(0, timeoutMs);
    for (;;) {
        const qint64 now = m_clock.elapsed();
        QImage frame;
        if (m_stream) {
            const Grabber::Stream::Frame f = m_stream->next(m_number, int(std::max<qint64>(0, until - now)));
            if (f.number <= m_number)
                return {}; // none came in time (or the stream ended)
            m_number = f.number;
            m_lastFrameMs = m_clock.elapsed();
            frame = f.image;
        } else {
            if (now < m_nextPollMs) {
                if (m_nextPollMs > until)
                    return {};
                ::Sleep(DWORD(m_nextPollMs - now));
            }
            m_nextPollMs = m_clock.elapsed() + kPollMs;
            frame = bitblt();
            if (!frame.isNull())
                m_lastFrameMs = m_clock.elapsed();
        }
        if (!frame.isNull() && !samePixels(frame, m_last)) {
            m_last = frame;
            return frame;
        }
        if (m_clock.elapsed() >= until)
            return {};
    }
}

} // namespace ws::capture
