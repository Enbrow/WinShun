#pragma once

#include <QRect>

#include <windows.h>

#include <optional>
#include <vector>

namespace ws::capture {

// The windows on the screen as the picture was frozen, to pick one (or one
// of its controls) with a click: top-level windows from the top of the Z
// order down, with their bounds in physical pixels of the virtual desktop
// (DWMWA_EXTENDED_FRAME_BOUNDS: without the shadow). Left out: hidden,
// minimised, cloaked (other virtual desktops, suspended UWP windows),
// empty, click-through (WS_EX_TRANSPARENT, invisible layered) windows, the
// text cursor indicator (CursorVisualClass, drawn over everything), and
// Win顺's own.
//
// The child windows are read only for a window the mouse goes over, and
// once: a window like Explorer has dozens, and nobody points at most windows.
class WindowSnapshot {
public:
    static WindowSnapshot take();

    const std::vector<QRect>& windows() const { return m_rects; }
    // Window i's visible child windows, cut to it.
    const std::vector<QRect>& children(std::size_t i) const;
    void clear();

private:
    struct Entry {
        HWND hwnd = nullptr;
        mutable std::optional<std::vector<QRect>> children;
    };
    std::vector<Entry> m_entries;
    std::vector<QRect> m_rects;
};

} // namespace ws::capture
