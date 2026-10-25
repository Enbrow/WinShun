#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QRgb>
#include <QString>

#include <windows.h>

#include <optional>
#include <unordered_map>
#include <vector>

// The small features of stage 7 that need no window of their own: the colour
// formats of the magnifier, the delayed capture's countdown, the last
// selection (Shift+R), the mouse cursor drawn into a picture, and the rules
// for asking UI Automation about a control. Physical pixels throughout.
namespace ws::capture {

// ---- colour formats (the magnifier; C copies, Shift alone changes) --------------

enum class ColorFormat { Hex, Rgb, Hsl };
// "#1E90FF", "rgb(30, 144, 255)", "hsl(210, 100%, 56%)".
QString colorText(QRgb color, ColorFormat format);
ColorFormat nextColorFormat(ColorFormat format);
// "hex", "rgb", "hsl" (as the settings keep it); anything else is hex.
ColorFormat colorFormatFromName(const QString& name);
QString colorFormatName(ColorFormat format);

// ---- the delayed capture --------------------------------------------------------

// 3, 5 or 10 seconds: the nearest of them to `seconds`, 5 for none.
int normalizedDelay(int seconds);
// The number the countdown shows `elapsedMs` after it began: whole seconds
// left, rounded up; 0 once the time is up.
int countdownSeconds(qint64 elapsedMs, int totalSeconds);
// Milliseconds still to wait (never negative).
qint64 countdownRemaining(qint64 elapsedMs, int totalSeconds);

// ---- Shift+R: the last selection ------------------------------------------------

struct RememberedSelection {
    int monitor = -1; // index into the monitors given
    QRect rect; // in that monitor's own pixels
};
// The selection `remembered` (virtual desktop, physical) on the monitor it
// was on: the one containing most of it, the rectangle in that monitor's own
// pixels and kept inside it. None when it touches no monitor (`monitors` are
// their rectangles, virtual desktop).
std::optional<RememberedSelection> lastSelectionOn(const QRect& remembered, const std::vector<QRect>& monitors);

// ---- the mouse cursor in a picture ----------------------------------------------

// `cursor` drawn into `image` with its hot spot at `hotspot` (image pixels),
// the way Windows blends it over a 32-bit surface. False when the cursor has
// no picture to draw or lies wholly outside.
bool drawCursor(QImage& image, QPoint hotspot, HCURSOR cursor);

// ---- UI Automation for controls that are not child windows ----------------------

// Whether to ask UI Automation about the point in a window: its class is one
// whose controls are drawn by the program itself (XAML and WinUI, Firefox), or
// it has no child windows to pick. Never for a Chromium class (Chrome, Edge,
// Electron, CEF: WindowSnapshot::isChromiumClass); a window with a Chromium
// child window is ruled out by the caller (WindowSnapshot::hostsChromium).
bool wantsUia(const QString& windowClass, int childWindows);
// The controls a chain of elements gives (from the one under the point out to
// its ancestors): each cut to `window`, left out when under 8 pixels either
// way, empty, or the window's own rectangle; the same one only once.
std::vector<QRect> uiaControls(const std::vector<QRect>& chain, const QRect& window);
// Overlapping UIA branches: prefer the smallest usable target, then the
// deeper chain. Wrapper panes must not outweigh a small toolbar button.
bool preferUiaChain(const std::vector<QRect>& candidate, const std::vector<QRect>& current);

// Which windows may still be asked: a window whose answer took longer than
// the budget is not asked again in this capture (its controls stay the child
// windows'), nor one that answered nothing `kEmptyLimit` times running.
class UiaGate {
public:
    static constexpr int kBudgetMs = 150;
    static constexpr int kEmptyLimit = 6;

    bool allowed(quintptr window) const;
    // How the last question to `window` went: its time, and how many controls
    // it gave.
    void record(quintptr window, qint64 elapsedMs, int controls);
    bool slow(quintptr window) const;

private:
    struct State {
        bool slow = false;
        int empty = 0;
    };
    std::unordered_map<quintptr, State> m_states;
};

} // namespace ws::capture
