#pragma once

#include <QRect>

#include <windows.h>

#include <optional>
#include <string_view>
#include <vector>

namespace ws::capture {

// The windows on the screen as the picture was frozen, to pick one (or one
// of its controls) with a click: top-level windows from the top of the Z
// order down, with their bounds in physical pixels of the virtual desktop
// (DWMWA_EXTENDED_FRAME_BOUNDS: without the shadow). Left out: hidden,
// minimised, cloaked (other virtual desktops, suspended UWP windows),
// empty, click-through (WS_EX_TRANSPARENT, invisible layered) windows, the
// text cursor indicator (CursorVisualClass, drawn over everything), and
// the capture's own overlays, pins and countdown (not Win顺's other windows).
//
// The child windows are read only for a window the mouse goes over, and
// once: a window like Explorer has dozens, and nobody points at most windows.
class WindowSnapshot {
public:
    // `overlays`: the capture's own overlay windows, never listed.
    static WindowSnapshot take(const std::vector<HWND>& overlays = {});
    // The class of a Win顺 window that is part of the capture (a pin, the
    // countdown) and never a target; other Win顺 windows are listed.
    static bool isCaptureSurfaceClass(std::wstring_view className);

    const std::vector<QRect>& windows() const { return m_rects; }
    // Window i's visible child windows, cut to it.
    const std::vector<QRect>& children(std::size_t i) const;
    // The window on top at `point` (physical, virtual desktop); null for none.
    HWND windowAt(const QPoint& point) const;
    HWND handle(std::size_t i) const;
    // The window's class name and how many child windows it has (its controls
    // read already, for UI Automation's sake: wantsUia).
    QString windowClass(std::size_t i) const;
    int childWindows(std::size_t i) const;
    // Controls UI Automation found in window i (UiaProbe): they join its
    // child windows. True when any was not there yet.
    bool addControls(std::size_t i, const std::vector<QRect>& controls);
    // The index of the window `hwnd` in the list; -1 for none.
    int indexOf(HWND hwnd) const;
    void clear();

private:
    struct Entry {
        HWND hwnd = nullptr;
        mutable std::optional<std::vector<QRect>> children;
        int controls = 0; // of children's last ones, from UI Automation
    };
    std::vector<Entry> m_entries;
    std::vector<QRect> m_rects;
};

} // namespace ws::capture
