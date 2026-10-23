#include "WindowSnapshot.h"

#include <dwmapi.h>

#include <algorithm>
#include <cwchar>

namespace ws::capture {

namespace {

QRect toRect(const RECT& r)
{
    return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

bool skipped(HWND hwnd, const std::vector<HWND>& overlays)
{
    if (!::IsWindowVisible(hwnd) || ::IsIconic(hwnd))
        return true;
    // Only the capture's own surfaces are left out, not every window of the
    // process: Win顺's settings window is picked like any other app's.
    if (std::ranges::find(overlays, hwnd) != overlays.end())
        return true;
    DWORD cloaked = 0;
    if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked != 0)
        return true;
    const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TRANSPARENT) // the mouse goes through it: an overlay, not something to pick
        return true;
    if (exStyle & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (::GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0)
            return true;
    }
    wchar_t className[64] {};
    ::GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    // Windows' text cursor indicator (EoAExperiences.exe) sits over every
    // window, ours included; it is not a window anyone means to capture.
    if (std::wcscmp(className, L"CursorVisualClass") == 0)
        return true;
    return WindowSnapshot::isCaptureSurfaceClass(className);
}

QRect boundsOf(HWND hwnd)
{
    RECT r {};
    if (FAILED(::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) || ::IsRectEmpty(&r))
        ::GetWindowRect(hwnd, &r);
    return toRect(r);
}

struct ChildWalk {
    QRect parent;
    std::vector<QRect>* out = nullptr;
};

BOOL CALLBACK addChild(HWND hwnd, LPARAM param)
{
    auto* walk = reinterpret_cast<ChildWalk*>(param);
    if (!::IsWindowVisible(hwnd)) // the window or one of its parents is hidden
        return TRUE;
    RECT r {};
    if (!::GetWindowRect(hwnd, &r))
        return TRUE;
    const QRect rect = toRect(r).intersected(walk->parent);
    if (!rect.isEmpty() && rect != walk->parent)
        walk->out->push_back(rect);
    return TRUE;
}

} // namespace

bool WindowSnapshot::isCaptureSurfaceClass(std::wstring_view className)
{
    // The pins and the delayed capture's countdown (the overlays are passed by handle).
    return className == L"WinShun.Pin" || className == L"WinShun.Countdown";
}

WindowSnapshot WindowSnapshot::take(const std::vector<HWND>& overlays)
{
    WindowSnapshot snapshot;
    struct Walk {
        WindowSnapshot* snapshot;
        const std::vector<HWND>* overlays;
    } walk {&snapshot, &overlays};
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* w = reinterpret_cast<Walk*>(param);
            if (skipped(hwnd, *w->overlays))
                return TRUE;
            const QRect rect = boundsOf(hwnd);
            if (rect.isEmpty())
                return TRUE;
            w->snapshot->m_entries.push_back({hwnd, std::nullopt});
            w->snapshot->m_rects.push_back(rect);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&walk));
    return snapshot;
}

const std::vector<QRect>& WindowSnapshot::children(std::size_t i) const
{
    static const std::vector<QRect> none;
    if (i >= m_entries.size())
        return none;
    const Entry& entry = m_entries[i];
    if (!entry.children) {
        entry.children.emplace();
        ChildWalk walk {m_rects[i], &*entry.children};
        ::EnumChildWindows(entry.hwnd, addChild, reinterpret_cast<LPARAM>(&walk));
    }
    return *entry.children;
}

HWND WindowSnapshot::windowAt(const QPoint& point) const
{
    for (std::size_t i = 0; i < m_rects.size(); ++i)
        if (m_rects[i].contains(point))
            return m_entries[i].hwnd;
    return nullptr;
}

HWND WindowSnapshot::handle(std::size_t i) const
{
    return i < m_entries.size() ? m_entries[i].hwnd : nullptr;
}

QString WindowSnapshot::windowClass(std::size_t i) const
{
    wchar_t name[256] {};
    if (i < m_entries.size())
        ::GetClassNameW(m_entries[i].hwnd, name, static_cast<int>(std::size(name)));
    return QString::fromWCharArray(name);
}

int WindowSnapshot::childWindows(std::size_t i) const
{
    return static_cast<int>(children(i).size()) - (i < m_entries.size() ? m_entries[i].controls : 0);
}

bool WindowSnapshot::addControls(std::size_t i, const std::vector<QRect>& controls)
{
    if (i >= m_entries.size())
        return false;
    children(i); // the child windows first
    Entry& entry = m_entries[i];
    bool added = false;
    for (const QRect& r : controls) {
        if (std::ranges::find(*entry.children, r) == entry.children->end()) {
            entry.children->push_back(r);
            ++entry.controls;
            added = true;
        }
    }
    return added;
}

int WindowSnapshot::indexOf(HWND hwnd) const
{
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].hwnd == hwnd)
            return static_cast<int>(i);
    }
    return -1;
}

void WindowSnapshot::clear()
{
    m_entries = {};
    m_rects = {};
}

} // namespace ws::capture
