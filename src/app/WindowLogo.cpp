#include "WindowLogo.h"

#include "Placement.h"
#include "SystemTheme.h"

#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>

namespace ws {

struct WindowLogo::Hook {
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<WindowLogo*>(data);
        if (message == WM_WINDOWPOSCHANGED) {
            // Moved (dragged, gliding) or resized: the logo too, now.
            const auto* pos = reinterpret_cast<const WINDOWPOS*>(lParam);
            if (self->m_shown && (!(pos->flags & SWP_NOMOVE) || !(pos->flags & SWP_NOSIZE)))
                self->place();
        } else if (message == WM_NCDESTROY) {
            ::RemoveWindowSubclass(hwnd, &Hook::proc, 0);
            self->m_hwnd = nullptr;
        }
        return ::DefSubclassProc(hwnd, message, wParam, lParam);
    }
};

WindowLogo::WindowLogo(QWindow* window)
    : QObject(window)
    , m_window(window)
    , m_hwnd(reinterpret_cast<HWND>(window->winId()))
{
    ::SetWindowSubclass(m_hwnd, &Hook::proc, 0, reinterpret_cast<DWORD_PTR>(this));
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible) {
            m_shown = false;
            m_logo.hide();
        }
    });
}

WindowLogo::~WindowLogo()
{
    if (m_hwnd)
        ::RemoveWindowSubclass(m_hwnd, &Hook::proc, 0);
}

void WindowLogo::follow(Placement* placement)
{
    m_placement = placement;
    // Out from behind a taskbar at the bottom: all of it, the logo too.
    placement->setOverTop(win::PeekingLogo::kSize - win::PeekingLogo::kSize / 4);
    connect(placement, &Placement::slideStepped, this, [this] {
        if (m_shown)
            place(); // faded on, or back over the taskbar
    });
}

void WindowLogo::reveal()
{
    if (!m_window || !m_window->isVisible())
        return;
    m_shown = true;
    place();
}

void WindowLogo::conceal()
{
    m_shown = false;
    m_logo.hide();
}

void WindowLogo::place()
{
    if (!m_hwnd)
        return;
    RECT host {};
    if (FAILED(::DwmGetWindowAttribute(m_hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &host, sizeof host)))
        ::GetWindowRect(m_hwnd, &host);
    MONITORINFO info {sizeof info};
    if (!::GetMonitorInfoW(::MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &info))
        return;
    // The window's own DPI: right in the middle of moving to another screen,
    // where Qt's devicePixelRatio is not yet.
    const qreal scale = ::GetDpiForWindow(m_hwnd) / 96.0;
    // Where the window rests: the edge is chosen there.
    const QPoint offset = m_placement ? m_placement->slideOffset() : QPoint();
    ::OffsetRect(&host, -offset.x(), -offset.y());
    const qreal opacity = m_placement ? std::clamp(m_placement->slideOpacity(), 0.0, 1.0) : 1.0;
    const auto under = reinterpret_cast<HWND>(m_placement ? m_placement->slideUnder() : 0);
    const RECT row {host.left, host.top, host.right, host.top + qRound(SystemTheme::headerHeight() * scale)};
    using Edge = win::PeekingLogo::Edge;
    m_logo.show(host, row, info.rcWork, m_avoid ? &*m_avoid : nullptr, scale,
        {Edge::Top, Edge::Left, Edge::Right, Edge::Bottom}, {offset.x(), offset.y()}, BYTE(qRound(opacity * 255)),
        under);
}

} // namespace ws
