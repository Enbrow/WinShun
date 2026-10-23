#pragma once

#include <QPointF>
#include <QRect>
#include <QRectF>

namespace ws::pie {

// The drop pie's shape (DropPie, DropPieWindow.qml): a dead centre, an inner
// ring split into 移动 (left half) and 复制 (right half), an outer ring of up
// to eight folders, sector 0 centred at the top and the rest clockwise. Pure
// geometry in logical pixels, unit-tested (tst_piegeometry); angles in
// degrees clockwise from the top, as a clock's.
struct Radii {
    double dead = 50; // the centre: let go here and nothing happens
    double innerIn = 58;
    double innerOut = 112;
    double outerIn = 120;
    double outerOut = 226;
    double gap = 4; // between neighbouring sectors, along the ring
    double leave = 12; // past outerOut before the pointer has left the ring

    // All of it times `k` (界面大小: the label type's size over 13 px).
    Radii scaled(double k) const noexcept;
    double labelRadius() const noexcept { return (outerIn + outerOut) / 2; }
    double innerLabelRadius() const noexcept { return (innerIn + innerOut) / 2; }
};

// The scale for a 界面大小 step (Theme.step: 0 compact, 1 standard, 2 roomy):
// the labels' type is 12, 13 or 14 px, the ring grows with it.
double scaleForStep(int step) noexcept;
int labelPixelSize(int step) noexcept;

enum class Mode { None, Move, Copy };
enum class Zone { Dead, Inner, Outer, Outside };

struct Hit {
    Zone zone = Zone::Outside;
    Mode half = Mode::None; // Inner: the half the pointer is over
    int sector = -1; // Outer: which one
};

// Where `offset` (from the centre) falls on a ring of `sectors` folders.
// The gaps between the rings belong to the nearer one; the outer ring
// reaches `leave` beyond its edge before the pointer is Outside.
Hit hitTest(QPointF offset, int sectors, const Radii& radii) noexcept;

double angleOf(QPointF offset) noexcept; // 0 up, 90 right, in [0, 360)
int sectorAt(double angle, int sectors) noexcept;
double sectorCentre(int index, int sectors) noexcept;
QPointF polar(double radius, double angle) noexcept; // the offset from the centre

// The inner half the pointer last crossed wins and stays while it slides
// along the outer ring; a half the drag's source does not allow (no
// DROPEFFECT_MOVE, say) is passed over. Ctrl (copy) and Shift (move)
// override it while held, as in File Explorer.
class Latch {
public:
    void reset(Mode start = Mode::None) noexcept { m_latched = start; }
    void update(const Hit& hit, bool moveAllowed, bool copyAllowed) noexcept;
    Mode latched() const noexcept { return m_latched; }
    // What a drop would do now: None until a half was crossed (or a key
    // held), unless `forDrop`, which falls back to what the source allows.
    Mode effective(bool ctrl, bool shift, bool moveAllowed, bool copyAllowed, bool forDrop = false) const noexcept;

private:
    Mode m_latched = Mode::None;
};

// The ring's centre for a pointer at `cursor`: the whole ring of radius
// `ringRadius` inside `workArea` if it can be, but never more than
// `maxShift` from the pointer (so that it starts inside the inner ring, never
// on a folder).
QPointF placeCentre(QPointF cursor, const QRectF& workArea, double ringRadius, double maxShift) noexcept;

// Room around the ring in the window: for the tip beside the pointer and
// the line under the ring.
struct Margins {
    double side = 150;
    double top = 48;
    double bottom = 84;
};

// The window: the ring's box with the margins, within the one monitor it is
// on (never across two: they may differ in scale). Whole logical pixels.
QRect windowRect(QPointF centre, double ringRadius, const QRect& monitor, const Margins& margins = {}) noexcept;

// How wide a folder's label may be: the sector's chord at the label's
// radius less some padding, at most `cap`.
double labelWidth(int sectors, const Radii& radii, double cap) noexcept;

} // namespace ws::pie
