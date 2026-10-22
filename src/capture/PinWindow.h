#pragma once

#include "Geometry.h"

#include <QImage>
#include <QPoint>
#include <QRect>

#include <windows.h>

#include <functional>

namespace ws::capture {

// A picture left on the screen ("贴图", docs/design-capture.md 4.3): a
// native layered window (WS_EX_LAYERED, one UpdateLayeredWindow of a 32-bit
// premultiplied DIB), not a QQuickWindow. Stage 0 measured a Qt Quick window
// per pin at +13 MB of video memory and +22–37 MB of commit each, the commit
// not given back when it closed (D3D's heaps); a DIB costs the picture's
// pixels and goes away with the window.
//
// It shows the picture where it was taken, at its size in screen pixels:
// 100% is the picture's pixels copied as they are, so a pin over the place
// it came from matches the screen pixel for pixel. Around it a border of one
// logical pixel (neutral grey, the accent while it has the focus) and a soft
// shadow drawn into the DIB (DWM draws none round a layered window). On a
// monitor at another scale the picture keeps its physical size; only the
// border and shadow follow the DPI (WM_DPICHANGED, per-monitor v2, the
// suggested rectangle ignored).
//
// The mouse drags it; the wheel zooms about the cursor through zoomSteps()
// (no larger than kZoomBudget pixels of window); Ctrl+wheel changes the
// opacity; a double click goes back to 100%. Esc closes it at once,
// Ctrl+C copies, Ctrl+S saves; the right button or the menu key asks the
// owner for the menu. Not in the taskbar or Alt+Tab (WS_EX_TOOLWINDOW).
class PinWindow {
public:
    struct Host {
        std::function<void(PinWindow*)> closed; // the window is gone: the owner deletes this later
        std::function<void(PinWindow*, POINT)> menu; // at that point (physical)
        std::function<void(PinWindow*)> copy; // Ctrl+C
        std::function<void(PinWindow*)> save; // Ctrl+S
        std::function<bool()> dark; // the border's grey
    };

    // The largest window a zoom may make, in pixels (~96 MB of DIB).
    static constexpr qint64 kZoomBudget = 24'000'000;

    // Shown at once, its picture's top left at `origin` (physical, virtual
    // desktop), with the focus.
    PinWindow(const QImage& image, QPoint origin, Host host);
    ~PinWindow(); // destroys the window if it is still there
    PinWindow(const PinWindow&) = delete;
    PinWindow& operator=(const PinWindow&) = delete;

    HWND hwnd() const { return m_hwnd; }
    const QImage& image() const { return m_image; }
    QRect pictureRect() const; // physical, virtual desktop, at the zoom
    double dpr() const { return m_dpi / 96.0; }
    double zoom() const { return m_zoom; }
    double opacity() const { return m_opacity; }
    bool topmost() const { return m_topmost; }
    bool clickThrough() const { return m_clickThrough; }
    // Taken with the screen capture (copied, it is a screenshot in the
    // history), or a picture pinned from a file or the clipboard history.
    bool screenshot() const { return m_screenshot; }
    void setScreenshot(bool on) { m_screenshot = on; }
    // The annotated picture's editable form (opaque bytes, empty: none),
    // kept for the editing window of stage 4b.
    const QByteArray& project() const { return m_project; }
    void setProject(QByteArray project) { m_project = std::move(project); }

    // A new picture where this one is (the editing window's 完成): the top
    // left and the zoom stay, the window follows the new size. A null picture
    // changes nothing.
    void setPicture(const QImage& image, QByteArray project);
    // Out of sight while it is edited, and back (it keeps its place).
    void setShown(bool shown);

    void setTopmost(bool on);
    // The mouse goes through it to what is under it (WS_EX_TRANSPARENT);
    // only the tray menu brings it back then.
    void setClickThrough(bool on);
    void close(); // the window destroyed, its pixels let go of at once

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void render(); // the whole window: shadow, border, picture
    void paintBorder(); // the border alone (the focus came or went)
    void present(); // the DIB onto the screen, at the opacity
    void setZoom(double zoom, QPoint cursor);
    void releasePixels();
    QRect windowRect() const;

    HWND m_hwnd = nullptr;
    Host m_host;
    QImage m_image; // Format_RGB32, alpha forced opaque when drawn
    QImage m_scaled; // the picture at m_zoom when that is not 1
    QPoint m_origin; // the picture's top left, physical
    double m_zoom = 1.0;
    double m_opacity = 1.0;
    UINT m_dpi = 96;
    PinFrame m_frame;
    bool m_topmost = true;
    bool m_clickThrough = false;
    bool m_screenshot = true;
    QByteArray m_project;
    bool m_dragging = false;
    QPoint m_grab; // the cursor less m_origin at the press
    int m_wheel = 0; // wheel delta not yet a whole notch (touchpads)
    // The DIB the window is drawn from, selected into m_dc.
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_oldBitmap = nullptr;
    uchar* m_bits = nullptr;
    QSize m_dibSize;
};

} // namespace ws::capture
