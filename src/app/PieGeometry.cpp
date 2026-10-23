#include "PieGeometry.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ws::pie {

namespace {

constexpr double kDegree = std::numbers::pi / 180;

} // namespace

Radii Radii::scaled(double k) const noexcept
{
    return {dead * k, innerIn * k, innerOut * k, outerIn * k, outerOut * k, gap * k, leave * k};
}

double scaleForStep(int step) noexcept
{
    return labelPixelSize(step) / 13.0;
}

int labelPixelSize(int step) noexcept
{
    return step <= 0 ? 12 : step >= 2 ? 14 : 13;
}

double angleOf(QPointF offset) noexcept
{
    double angle = std::atan2(offset.x(), -offset.y()) / kDegree; // y grows downwards on screen
    if (angle < 0)
        angle += 360;
    return angle >= 360 ? 0 : angle;
}

int sectorAt(double angle, int sectors) noexcept
{
    if (sectors <= 1)
        return sectors == 1 ? 0 : -1;
    const double step = 360.0 / sectors;
    const double shifted = std::fmod(angle + step / 2 + 360, 360);
    return std::clamp(static_cast<int>(shifted / step), 0, sectors - 1);
}

double sectorCentre(int index, int sectors) noexcept
{
    return sectors > 0 ? index * 360.0 / sectors : 0;
}

QPointF polar(double radius, double angle) noexcept
{
    return {radius * std::sin(angle * kDegree), -radius * std::cos(angle * kDegree)};
}

Hit hitTest(QPointF offset, int sectors, const Radii& radii) noexcept
{
    const double r = std::hypot(offset.x(), offset.y());
    if (r < (radii.dead + radii.innerIn) / 2)
        return {Zone::Dead};
    if (r < (radii.innerOut + radii.outerIn) / 2)
        return {Zone::Inner, offset.x() < 0 ? Mode::Move : Mode::Copy};
    if (r <= radii.outerOut + radii.leave && sectors > 0)
        return {Zone::Outer, Mode::None, sectorAt(angleOf(offset), sectors)};
    return {Zone::Outside};
}

void Latch::update(const Hit& hit, Allowed allowed) noexcept
{
    if (hit.zone != Zone::Inner)
        return;
    if ((hit.half == Mode::Move && allowed.move) || (hit.half == Mode::Copy && allowed.copy))
        m_latched = hit.half;
}

Mode Latch::effective(Keys keys, Allowed allowed, Purpose purpose) const noexcept
{
    // Both held is Windows' "create a shortcut": neither.
    if (keys.ctrl && !keys.shift && allowed.copy)
        return Mode::Copy;
    if (keys.shift && !keys.ctrl && allowed.move)
        return Mode::Move;
    Mode mode = m_latched;
    if ((mode == Mode::Move && !allowed.move) || (mode == Mode::Copy && !allowed.copy))
        mode = Mode::None;
    if (mode == Mode::None && purpose == Purpose::Drop)
        mode = allowed.move ? Mode::Move : allowed.copy ? Mode::Copy : Mode::None;
    return mode;
}

Placement placeCentre(QPointF cursor, const QRectF& workArea, double ringRadius, double safeShift) noexcept
{
    // Each axis into the band where the ring fits; a work area too small for
    // it: its middle.
    const auto fit = [ringRadius](double v, double low, double high) {
        return high - low < 2 * ringRadius ? (low + high) / 2 : std::clamp(v, low + ringRadius, high - ringRadius);
    };
    const QPointF centre(fit(cursor.x(), workArea.left(), workArea.right()),
        fit(cursor.y(), workArea.top(), workArea.bottom()));
    const QPointF shift = centre - cursor;
    return {centre, std::hypot(shift.x(), shift.y()) > safeShift};
}

QRect windowRect(QPointF centre, double ringRadius, const QRect& monitor, const Margins& margins) noexcept
{
    const QRectF box(centre.x() - ringRadius - margins.side, centre.y() - ringRadius - margins.top,
        2 * (ringRadius + margins.side), 2 * ringRadius + margins.top + margins.bottom);
    // Outwards to whole pixels, so the ring's box is all inside.
    const QRect whole(QPoint(static_cast<int>(std::floor(box.left())), static_cast<int>(std::floor(box.top()))),
        QPoint(static_cast<int>(std::ceil(box.right())) - 1, static_cast<int>(std::ceil(box.bottom())) - 1));
    return whole.intersected(monitor);
}

double labelWidth(int sectors, const Radii& radii, double cap) noexcept
{
    if (sectors <= 2)
        return cap;
    const double chord = 2 * radii.labelRadius() * std::sin(std::numbers::pi / sectors);
    return std::min(cap, chord - 2 * radii.gap - 24);
}

} // namespace ws::pie
