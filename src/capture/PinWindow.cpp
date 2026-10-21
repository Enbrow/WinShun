#include "PinWindow.h"

#include <QDebug>
#include <QPainter>

#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ws::capture {

namespace {

const wchar_t* const kClassName = L"WinShun.Pin";

void registerClass()
{
    static const bool registered = [] {
        WNDCLASSEXW wc {sizeof wc};
        wc.style = CS_DBLCLKS; // a double click: back to 100%
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) { return ::DefWindowProcW(h, m, w, l); };
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_SIZEALL); // it moves where it is dragged
        wc.lpszClassName = kClassName;
        return ::RegisterClassExW(&wc) != 0;
    }();
    Q_UNUSED(registered);
}

// A frame `thickness` pixels wide just inside `outer`.
void ring(QPainter& painter, const QRect& outer, int thickness, const QColor& color)
{
    const int t = std::min({thickness, outer.width(), outer.height()});
    painter.fillRect(QRect(outer.left(), outer.top(), outer.width(), t), color);
    painter.fillRect(QRect(outer.left(), outer.bottom() - t + 1, outer.width(), t), color);
    painter.fillRect(QRect(outer.left(), outer.top() + t, t, outer.height() - 2 * t), color);
    painter.fillRect(QRect(outer.right() - t + 1, outer.top() + t, t, outer.height() - 2 * t), color);
}

// The picture's rows into the canvas at `at`, opaque: grabbed pixels may
// carry alpha 0 (BitBlt), which a layered window would show as nothing.
void copyOpaque(const QImage& picture, QImage& canvas, QPoint at)
{
    const int w = std::min(picture.width(), canvas.width() - at.x());
    for (int y = 0; y < picture.height() && at.y() + y < canvas.height(); ++y) {
        const auto* in = reinterpret_cast<const QRgb*>(picture.constScanLine(y));
        auto* out = reinterpret_cast<QRgb*>(canvas.scanLine(at.y() + y)) + at.x();
        for (int x = 0; x < w; ++x)
            out[x] = in[x] | 0xFF000000u;
    }
}

} // namespace

PinWindow::PinWindow(const QImage& image, QPoint origin, Host host)
    : m_host(std::move(host))
    , m_image(image.convertToFormat(QImage::Format_RGB32))
    , m_origin(origin)
{
    registerClass();
    // Per-monitor v2 whatever the thread's default: the picture must stay
    // in physical pixels, and WM_DPICHANGED must come.
    const DPI_AWARENESS_CONTEXT previous = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    m_hwnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"Win顺 贴图", WS_POPUP,
        origin.x(), origin.y(), 1, 1, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (previous)
        ::SetThreadDpiAwarenessContext(previous);
    if (!m_hwnd) {
        qWarning() << "Screen capture: no pin window, error" << ::GetLastError();
        return;
    }
    ::SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ::SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PinWindow::proc));
    // No input method: a pin takes no text, and with the thread's default
    // context a letter typed at it opened the IME's candidate window. Qt
    // does the same for its windows whose focus takes no text
    // (QWindowsInputContext::setWindowsImeEnabled); the overlay's text
    // box keeps its own.
    ::ImmAssociateContextEx(m_hwnd, nullptr, 0);
    m_dpi = std::max<UINT>(96, ::GetDpiForWindow(m_hwnd));
    m_frame = pinFrame(dpr());
    render();
    ::ShowWindow(m_hwnd, SW_SHOW);
    ::SetForegroundWindow(m_hwnd); // Esc closes it at once; Win顺 is in front (the overlay was)
}

PinWindow::~PinWindow()
{
    if (m_hwnd) {
        ::SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    releasePixels();
}

QRect PinWindow::pictureRect() const
{
    return QRect(m_origin, zoomedSize(m_image.size(), m_zoom));
}

QRect PinWindow::windowRect() const
{
    return pinWindowRect(pictureRect(), m_frame);
}

void PinWindow::releasePixels()
{
    if (m_dc) {
        ::SelectObject(m_dc, m_oldBitmap);
        ::DeleteDC(m_dc);
        m_dc = nullptr;
    }
    if (m_bitmap) {
        ::DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }
    m_bits = nullptr;
    m_dibSize = {};
    m_image = {};
    m_scaled = {};
}

void PinWindow::render()
{
    if (!m_hwnd || m_image.isNull())
        return;
    const QRect window = windowRect();
    if (m_dibSize != window.size()) {
        if (m_dc) {
            ::SelectObject(m_dc, m_oldBitmap);
            ::DeleteObject(m_bitmap);
        } else {
            m_dc = ::CreateCompatibleDC(nullptr);
        }
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = window.width();
        info.bmiHeader.biHeight = -window.height(); // top-down, as QImage
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!m_bitmap) {
            qWarning() << "Screen capture: no pin bitmap of" << window.size();
            m_bits = nullptr;
            m_dibSize = {};
            return;
        }
        m_oldBitmap = ::SelectObject(m_dc, m_bitmap);
        m_bits = static_cast<uchar*>(bits);
        m_dibSize = window.size();
    }
    // The DIB as a QImage (BGRA in memory = ARGB32 on little endian), no copy.
    QImage canvas(m_bits, m_dibSize.width(), m_dibSize.height(), m_dibSize.width() * 4, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    const int m = m_frame.margin();
    const QRect picture(m, m, window.width() - 2 * m, window.height() - 2 * m);
    {
        QPainter painter(&canvas);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        // The shadow: fading out from the border, quadratically.
        const int s = m_frame.shadow;
        const QRect bordered = picture.adjusted(-m_frame.border, -m_frame.border, m_frame.border, m_frame.border);
        for (int i = 0; i < s; ++i) {
            const double f = static_cast<double>(s - i) / s;
            ring(painter, bordered.adjusted(-i - 1, -i - 1, i + 1, i + 1), 1, QColor(0, 0, 0, static_cast<int>(std::lround(70 * f * f))));
        }
    }
    if (std::abs(m_zoom - 1.0) < 1e-9) {
        m_scaled = {};
        copyOpaque(m_image, canvas, picture.topLeft()); // its pixels as they are
    } else {
        if (m_scaled.size() != picture.size()) {
            // Whole multiples as squares of pixels; other zooms smoothed
            // (QImage::scaled filters properly when shrinking).
            const bool whole = m_zoom > 1 && std::abs(m_zoom - std::round(m_zoom)) < 1e-9;
            m_scaled = m_image.scaled(picture.size(), Qt::IgnoreAspectRatio, whole ? Qt::FastTransformation : Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_RGB32);
        }
        copyOpaque(m_scaled, canvas, picture.topLeft());
    }
    paintBorder();
}

void PinWindow::paintBorder()
{
    if (!m_bits)
        return;
    QImage canvas(m_bits, m_dibSize.width(), m_dibSize.height(), m_dibSize.width() * 4, QImage::Format_ARGB32_Premultiplied);
    const bool dark = m_host.dark && m_host.dark();
    // Theme.windowEdge whether active or not: Win顺's edges are one grey,
    // never the accent (docs/colors.md); the shadow tells the pin apart.
    const QColor color = dark ? QColor(0x6A, 0x6A, 0x6A) : QColor(0xB4, 0xB4, 0xB4);
    const int s = m_frame.shadow;
    QPainter painter(&canvas);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    ring(painter, QRect(s, s, m_dibSize.width() - 2 * s, m_dibSize.height() - 2 * s), m_frame.border, color);
    painter.end();
    present();
}

void PinWindow::present()
{
    if (!m_hwnd || !m_dc)
        return;
    const QRect window = windowRect();
    POINT to {window.x(), window.y()};
    SIZE size {window.width(), window.height()};
    POINT from {0, 0};
    BLENDFUNCTION blend {AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(255 * m_opacity)), AC_SRC_ALPHA};
    if (!::UpdateLayeredWindow(m_hwnd, nullptr, &to, &size, m_dc, &from, 0, &blend, ULW_ALPHA))
        qWarning() << "Screen capture: UpdateLayeredWindow failed, error" << ::GetLastError();
}

void PinWindow::setZoom(double zoom, QPoint cursor)
{
    zoom = std::clamp(zoom, zoomSteps().front(), maxZoom(m_image.size(), kZoomBudget));
    if (std::abs(zoom - m_zoom) < 1e-9)
        return;
    m_origin = zoomAbout(m_origin, cursor, m_zoom, zoom);
    m_zoom = zoom;
    render();
}

void PinWindow::setTopmost(bool on)
{
    m_topmost = on;
    if (m_hwnd)
        ::SetWindowPos(m_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void PinWindow::setClickThrough(bool on)
{
    m_clickThrough = on;
    if (!m_hwnd)
        return;
    LONG_PTR style = ::GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    style = on ? style | WS_EX_TRANSPARENT : style & ~LONG_PTR(WS_EX_TRANSPARENT);
    ::SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, style);
    ::SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void PinWindow::close()
{
    if (m_hwnd)
        ::DestroyWindow(m_hwnd); // WM_NCDESTROY lets go of the pixels and tells the owner
}

LRESULT CALLBACK PinWindow::proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<PinWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self)
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    return self->handle(message, wParam, lParam);
}

LRESULT PinWindow::handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    const HWND hwnd = m_hwnd;
    switch (message) {
    case WM_ACTIVATE:
        return 0;
    case WM_LBUTTONDOWN: {
        POINT cursor {};
        ::GetCursorPos(&cursor);
        m_grab = QPoint(cursor.x, cursor.y) - m_origin;
        m_dragging = true;
        ::SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (m_dragging && ::GetCapture() == hwnd) {
            POINT cursor {};
            ::GetCursorPos(&cursor);
            const QPoint origin = QPoint(cursor.x, cursor.y) - m_grab;
            if (origin != m_origin) {
                m_origin = origin; // first: a WM_DPICHANGED inside the move draws it there
                const QRect window = windowRect();
                ::SetWindowPos(hwnd, nullptr, window.x(), window.y(), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        m_dragging = false;
        if (::GetCapture() == hwnd)
            ::ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        m_dragging = false;
        return 0;
    case WM_LBUTTONDBLCLK: {
        POINT cursor {};
        ::GetCursorPos(&cursor);
        setZoom(1.0, QPoint(cursor.x, cursor.y));
        return 0;
    }
    case WM_MOUSEWHEEL: {
        m_wheel += GET_WHEEL_DELTA_WPARAM(wParam);
        const int steps = m_wheel / WHEEL_DELTA;
        m_wheel -= steps * WHEEL_DELTA;
        if (steps == 0)
            return 0;
        if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
            m_opacity = nextOpacity(m_opacity, steps);
            present();
        } else {
            const QPoint cursor(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); // screen, physical
            setZoom(nextZoom(m_zoom, steps, maxZoom(m_image.size(), kZoomBudget)), cursor);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        const bool ctrl = ::GetKeyState(VK_CONTROL) < 0;
        if (wParam == VK_ESCAPE) {
            close();
            return 0;
        }
        if (ctrl && wParam == 'C' && m_host.copy) {
            m_host.copy(this);
            return 0;
        }
        if (ctrl && wParam == 'S' && m_host.save) {
            m_host.save(this);
            return 0;
        }
        break;
    }
    case WM_CONTEXTMENU: {
        POINT at {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (at.x == -1 && at.y == -1) { // the menu key: by the picture's top left
            const QRect picture = pictureRect();
            at = {picture.x() + 8, picture.y() + 8};
        }
        if (m_host.menu)
            m_host.menu(this, at);
        return 0;
    }
    case WM_DPICHANGED:
        // Onto a monitor at another scale: the picture keeps its pixels and
        // its place, the border and shadow follow; not the system's rectangle.
        m_dpi = std::max<UINT>(96, HIWORD(wParam));
        m_frame = pinFrame(dpr());
        render();
        return 0;
    case WM_NCDESTROY: {
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        m_hwnd = nullptr;
        releasePixels(); // at once: the memory goes back now
        if (m_host.closed)
            m_host.closed(this);
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace ws::capture
