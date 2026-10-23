#include "PieRing.h"

#include "PieGeometry.h"

#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ws {

namespace {

constexpr double kDegree = std::numbers::pi / 180;

QRectF circle(double radius)
{
    return {-radius, -radius, 2 * radius, 2 * radius};
}

// A piece of ring between radii r0 and r1 and angles a0 and a1 (degrees
// clockwise from the top), its sides `gap` in from the angles' lines at
// every radius, so neighbours are the same distance apart all along.
// Qt's arcs go counter-clockwise from three o'clock.
QPainterPath band(double r0, double r1, double a0, double a1, double gap)
{
    QPainterPath path;
    if (a1 - a0 >= 360) {
        path.addEllipse(circle(r1));
        path.addEllipse(circle(r0));
        path.setFillRule(Qt::OddEvenFill);
        return path;
    }
    const double h1 = gap / 2 / r1 / kDegree;
    const double h0 = gap / 2 / r0 / kDegree;
    const double b1 = a0 + h1;
    const double e1 = a1 - h1;
    const double b0 = a0 + h0;
    const double e0 = a1 - h0;
    path.moveTo(pie::polar(r1, b1));
    path.arcTo(circle(r1), 90 - b1, -(e1 - b1));
    path.lineTo(pie::polar(r0, e0));
    path.arcTo(circle(r0), 90 - e0, e0 - b0);
    path.closeSubpath();
    return path;
}

} // namespace

PieRing::PieRing(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    connect(this, &PieRing::changed, this, [this] { update(); });
}

void PieRing::paint(QPainter* painter)
{
    const pie::Radii radii = pie::Radii {}.scaled(m_scale);
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate(width() / 2, height() / 2);

    // A soft shadow outside the ring only: the gaps inside stay see-through.
    if (m_shadow > 0 && m_shadowColor.alpha() > 0) {
        const double outer = radii.outerOut + m_shadow;
        QRadialGradient gradient(QPointF(0, m_shadow / 4), outer);
        QColor clear = m_shadowColor;
        clear.setAlpha(0);
        const double start = (radii.outerOut - 4) / outer;
        gradient.setColorAt(start, m_shadowColor);
        gradient.setColorAt(start + (1 - start) * 0.45, QColor(m_shadowColor.red(), m_shadowColor.green(),
                                                             m_shadowColor.blue(), m_shadowColor.alpha() / 3));
        gradient.setColorAt(1, clear);
        QPainterPath ring;
        ring.addEllipse(circle(outer));
        ring.addEllipse(circle(radii.outerOut - 1));
        ring.setFillRule(Qt::OddEvenFill);
        painter->fillPath(ring, gradient);
    }

    // The disk under it all: the gaps read as grey lines.
    painter->setPen(Qt::NoPen);
    painter->setBrush(m_gapFill);
    painter->drawEllipse(circle(radii.outerOut));

    const auto draw = [&](const QPainterPath& path, const QColor& fill) {
        painter->setBrush(fill);
        painter->drawPath(path);
    };

    // The folders
    const int count = std::max(1, m_count);
    const double step = 360.0 / count;
    for (int i = 0; i < count; ++i) {
        const bool disabled = i < m_disabled.size() && m_disabled[i].toBool();
        const bool picked = i == m_picked && !disabled;
        const bool hovered = i == m_hovered;
        const QColor fill = picked ? m_accentFill : hovered ? m_hoverFill : m_fill;
        // To the disk's edge: no grey rim outside the folders.
        draw(band(radii.outerIn, radii.outerOut, i * step - step / 2, i * step + step / 2, count > 1 ? radii.gap : 0), fill);
    }

    // 移动 on the left, 复制 on the right
    const auto half = [&](const QString& name, bool allowed, double from) {
        const bool active = allowed && m_mode == name;
        const QColor fill = active ? m_accentFill : m_innerHover == name ? m_hoverFill : m_innerFill;
        draw(band(radii.innerIn, radii.innerOut, from, from + 180, radii.gap), fill);
    };
    half(QStringLiteral("copy"), m_copyAllowed, 0);
    half(QStringLiteral("move"), m_moveAllowed, 180);

    // The middle
    QPainterPath middle;
    middle.addEllipse(circle(radii.dead));
    draw(middle, m_fill);

    // The edge round it, its 1 px inside the disk
    painter->setPen(QPen(m_edge, m_edgeWidth));
    painter->setBrush(Qt::NoBrush);
    painter->drawEllipse(circle(radii.outerOut - m_edgeWidth / 2));
}

} // namespace ws
