#pragma once

#include "platform/PeekingLogo.h"

#include <QObject>
#include <QPointer>
#include <QWindow>

#include <windows.h>

#include <optional>

namespace ws {

class Placement;

// The logo peeking out from behind one of our windows (the launcher; the bar
// by file dialogs places its own, see DialogBar), moving with it as one: it
// is moved on the window's own WM_WINDOWPOSCHANGED, in the same instant, not
// when Qt hears of the move later (a drag would leave it behind). Hidden with
// the window. Shown by reveal(), once the window is on the screen (not while
// it is still cloaked, waiting for a frame). Sliding in or out
// (Placement::slideIn), it goes along: as far off, as faded, and behind the
// taskbar the window comes out from.
class WindowLogo : public QObject {
public:
    // `rowHeight`: logical pixels, the box at the top of the window; at a
    // side, the logo is level with its middle.
    WindowLogo(QWindow* window, int rowHeight); // a child of `window`, its native handle created
    ~WindowLogo() override;

    void follow(Placement* placement); // the window's: its slides
    void reveal();
    void conceal(); // until revealed again
    // Kept off this (physical pixels): the line typed in, by the clipboard.
    void setAvoid(std::optional<RECT> avoid) { m_avoid = avoid; }

private:
    struct Hook;

    void place();

    QPointer<QWindow> m_window;
    QPointer<Placement> m_placement;
    HWND m_hwnd = nullptr; // m_window's, subclassed
    int m_rowHeight;
    bool m_shown = false;
    std::optional<RECT> m_avoid;
    win::PeekingLogo m_logo;
};

} // namespace ws
