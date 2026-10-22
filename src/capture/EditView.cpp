#include "EditView.h"

#include <QList>

#include <algorithm>
#include <cmath>

namespace ws::capture::editview {

double clampZoom(double zoom)
{
    if (!std::isfinite(zoom))
        return 1.0;
    return std::clamp(zoom, kMinZoom, kMaxZoom);
}

double fitZoom(QSize image, QSize viewport)
{
    if (image.isEmpty() || viewport.isEmpty())
        return 1.0;
    const double across = double(viewport.width()) / image.width();
    const double down = double(viewport.height()) / image.height();
    double zoom = std::min({1.0, across, down});
    if (zoom < kLeastFit)
        zoom = std::min(1.0, std::max(across, down));
    return clampZoom(zoom);
}

QSize shownSize(QSize image, double zoom)
{
    return QSize(std::max(1, int(std::lround(image.width() * zoom))), std::max(1, int(std::lround(image.height() * zoom))));
}

namespace {

int clampAxis(int origin, int shown, int viewport)
{
    if (shown <= viewport)
        return (viewport - shown) / 2;
    return std::clamp(origin, viewport - shown, 0);
}

} // namespace

QPoint clampOrigin(QPoint origin, QSize shown, QSize viewport)
{
    return QPoint(clampAxis(origin.x(), shown.width(), viewport.width()),
        clampAxis(origin.y(), shown.height(), viewport.height()));
}

QPoint imagePixelAt(QPoint at, QPoint origin, double zoom)
{
    if (zoom <= 0)
        zoom = 1.0;
    return QPoint(int(std::floor((at.x() - origin.x()) / zoom)), int(std::floor((at.y() - origin.y()) / zoom)));
}

QRectF visibleImage(QPoint origin, double zoom, QSize viewport)
{
    if (zoom <= 0)
        zoom = 1.0;
    return QRectF(-origin.x() / zoom, -origin.y() / zoom, viewport.width() / zoom, viewport.height() / zoom);
}

QRect tileRange(const QRectF& visible, QSize image, int tile, int ring)
{
    if (image.isEmpty() || tile <= 0)
        return {};
    const QRectF inside = visible & QRectF(QPointF(0, 0), QSizeF(image));
    if (inside.isEmpty())
        return {};
    const int columns = (image.width() + tile - 1) / tile;
    const int rows = (image.height() + tile - 1) / tile;
    // The tile holding the last visible pixel: a view ending exactly on a
    // tile's edge does not need the next one.
    const int left = int(std::floor(inside.left() / tile));
    const int top = int(std::floor(inside.top() / tile));
    const int right = int(std::ceil(inside.right() / tile)) - 1;
    const int bottom = int(std::ceil(inside.bottom() / tile)) - 1;
    const int c0 = std::max(0, left - ring);
    const int r0 = std::max(0, top - ring);
    const int c1 = std::min(columns - 1, std::max(left, right) + ring);
    const int r1 = std::min(rows - 1, std::max(top, bottom) + ring);
    return QRect(QPoint(c0, r0), QPoint(c1, r1));
}

QList<int> nearestTiles(const QRect& range, int columns, int tile, QPointF centre, int limit)
{
    QList<int> out;
    if (range.isEmpty() || columns <= 0 || limit <= 0)
        return out;
    struct Entry {
        double distance;
        int index;
    };
    QList<Entry> all;
    all.reserve(range.width() * range.height());
    for (int row = range.top(); row <= range.bottom(); ++row) {
        for (int column = range.left(); column <= range.right(); ++column) {
            const QPointF middle((column + 0.5) * tile, (row + 0.5) * tile);
            const QPointF d = middle - centre;
            all.append({d.x() * d.x() + d.y() * d.y(), row * columns + column});
        }
    }
    std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) {
        return a.distance < b.distance || (a.distance == b.distance && a.index < b.index);
    });
    const int n = std::min<int>(limit, int(all.size()));
    out.reserve(n);
    for (int i = 0; i < n; ++i)
        out.append(all[i].index);
    return out;
}

int deviceLength(double logical, double bufferScale)
{
    return int(std::lround(logical * bufferScale));
}

ExitPlan planDone(Origin origin, bool modified, bool originalThere)
{
    switch (origin) {
    case Origin::History:
        return {modified || !originalThere ? Exit::NewEntry : Exit::TouchOriginal, false};
    case Origin::Pin:
        if (!originalThere)
            return {Exit::NewEntry, false};
        return modified ? ExitPlan {Exit::ReplacePin, true} : ExitPlan {Exit::Close, false};
    case Origin::Fresh:
        break;
    }
    return {Exit::NewEntry, false};
}

WindowPlace placeForPin(const QRect& picture, QPoint imageInWindow, QSize window, const QRect& work)
{
    WindowPlace place;
    place.topLeft = picture.topLeft() - imageInWindow;
    const int maxX = std::max(work.left(), work.left() + work.width() - window.width());
    const int maxY = std::max(work.top(), work.top() + work.height() - window.height());
    const QPoint moved(std::clamp(place.topLeft.x(), work.left(), maxX), std::clamp(place.topLeft.y(), work.top(), maxY));
    place.exact = moved == place.topLeft;
    place.topLeft = moved;
    return place;
}

} // namespace ws::capture::editview
