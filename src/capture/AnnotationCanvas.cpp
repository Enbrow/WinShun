#include "AnnotationCanvas.h"

#include "EditView.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGNode>
#include <QSGTexture>

#include <algorithm>
#include <numeric>

using namespace Qt::StringLiterals;

namespace ws::capture {

AnnotationCanvas::AnnotationCanvas(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_viewMoving.setSingleShot(true);
    m_viewMoving.setInterval(400);
    connect(&m_viewMoving, &QTimer::timeout, this, [this] {
        if (m_meter.on && m_model)
            reportMeter(m_model->compositor().patchesMade());
    });
}

// The nodes are the scene graph's (it deletes them with their textures);
// m_nodes only points at them.
AnnotationCanvas::~AnnotationCanvas() = default;

void AnnotationCanvas::setModel(AnnotationModel* model)
{
    if (m_model == model)
        return;
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    m_model = model;
    if (model) {
        connect(model, &AnnotationModel::changed, this, &AnnotationCanvas::onChanged);
        // The QPointer is null by then: the next polish lets the tiles go.
        connect(model, &QObject::destroyed, this, &AnnotationCanvas::markAll);
        // A gesture's cost is logged when it ends; the next one starts afresh.
        connect(model, &AnnotationModel::gestureChanged, this, [this] {
            if (m_meter.on && m_model && !m_model->gestureActive())
                reportMeter(m_model->compositor().patchesMade());
        });
    }
    markAll();
    emit modelChanged();
}

void AnnotationCanvas::setScaleX(qreal scale)
{
    if (scale == m_scaleX)
        return;
    m_scaleX = scale;
    m_rectsStale = true;
    update();
    emit scaleChanged();
}

void AnnotationCanvas::setScaleY(qreal scale)
{
    if (scale == m_scaleY)
        return;
    m_scaleY = scale;
    m_rectsStale = true;
    update();
    emit scaleChanged();
}

void AnnotationCanvas::setSparse(bool sparse)
{
    if (sparse == m_sparse)
        return;
    m_sparse = sparse;
    markAll(); // which tiles have a picture is decided again
    emit sparseChanged();
}

void AnnotationCanvas::setLimited(bool limited)
{
    if (limited == m_limited)
        return;
    m_limited = limited;
    markAll();
    emit limitedChanged();
}

void AnnotationCanvas::setViewport(const QRectF& viewport)
{
    if (viewport == m_viewport)
        return;
    m_viewport = viewport;
    if (m_limited) {
        m_viewMoving.start();
        polish(); // tiles come into view or go out of it
    }
    emit viewportChanged();
}

void AnnotationCanvas::setLinear(bool linear)
{
    if (linear == m_linear)
        return;
    m_linear = linear;
    m_filterStale = true;
    update();
    emit linearChanged();
}

int AnnotationCanvas::tilesShown() const
{
    return int(std::count_if(m_tiles.cbegin(), m_tiles.cend(), [](const Tile& t) { return t.shown; }));
}

// An empty region is all of it (a new picture, the capture's end).
void AnnotationCanvas::onChanged(const QRectF& dirty)
{
    if (dirty.isEmpty())
        markAll();
    else
        markTiles(dirty.toAlignedRect());
}

void AnnotationCanvas::markAll()
{
    for (Tile& tile : m_tiles)
        tile.dirty = true;
    polish(); // a new size makes new tiles there, all of them dirty
}

void AnnotationCanvas::markTiles(const QRect& area)
{
    const QRect r = area & QRect(QPoint(0, 0), m_size);
    if (r.isEmpty() || m_columns <= 0)
        return;
    for (int row = r.top() / kTile; row <= r.bottom() / kTile; ++row) {
        for (int column = r.left() / kTile; column <= r.right() / kTile; ++column)
            m_tiles[std::size_t(row * m_columns + column)].dirty = true;
    }
    polish();
}

void AnnotationCanvas::dropTiles()
{
    if (m_tiles.empty() && m_size.isEmpty())
        return;
    m_tiles.clear();
    m_tiles.shrink_to_fit();
    m_size = QSize();
    m_columns = 0;
    m_rows = 0;
    m_dropNodes = true;
}

QRect AnnotationCanvas::tileRect(int index) const
{
    const int x = (index % m_columns) * kTile;
    const int y = (index / m_columns) * kTile;
    return QRect(x, y, std::min(kTile, m_size.width() - x), std::min(kTile, m_size.height() - y));
}

QRectF AnnotationCanvas::nodeRect(int index) const
{
    const QRect r = tileRect(index);
    // Each edge from its own image px, so neighbours meet without a seam.
    const qreal x0 = r.left() * m_scaleX;
    const qreal y0 = r.top() * m_scaleY;
    const qreal x1 = (r.left() + r.width()) * m_scaleX;
    const qreal y1 = (r.top() + r.height()) * m_scaleY;
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

// The GUI thread, before the frame is synchronized: the dirty tiles drawn
// by the model's compositor, which is not thread safe and lives here.
void AnnotationCanvas::updatePolish()
{
    AnnotationModel* model = m_model;
    if (!model || !model->active()) {
        if (m_limited && m_mostShown > 0) {
            qInfo().noquote() << u"Annotation canvas: a view of a %1 x %2 picture kept at most %3 tiles (the view and a ring need %4; the picture has %5)"_s
                                     .arg(m_size.width())
                                     .arg(m_size.height())
                                     .arg(m_mostShown)
                                     .arg(m_mostWanted)
                                     .arg(m_tiles.size());
        }
        m_mostShown = m_mostWanted = 0;
        dropTiles();
        update();
        return;
    }
    QElapsedTimer clock;
    clock.start();
    const bool gesture = model->gestureActive() || (m_limited && m_viewMoving.isActive());
    if (gesture && !m_meter.on) {
        m_meter = Meter {};
        m_meter.on = true;
        m_meter.patchesBefore = model->compositor().patchesMade();
    }
    const QSize size = model->base().size();
    if (size != m_size) {
        dropTiles();
        m_size = size;
        m_columns = (size.width() + kTile - 1) / kTile;
        m_rows = (size.height() + kTile - 1) / kTile;
        m_tiles.assign(std::size_t(m_columns) * std::size_t(m_rows), Tile {});
        for (Tile& tile : m_tiles)
            tile.dirty = true;
    }
    const annot::Document& doc = model->document();
    const std::optional<quint32> skip = model->skipped();
    // Sparse: where the objects reach, measured once for all the tiles. The
    // text being typed is not drawn, so it does not count.
    std::vector<QRect> reach;
    if (m_sparse) {
        const QHash<quint32, int> numbers = annot::numbers(doc);
        reach.reserve(std::size_t(doc.items.size()));
        for (const annot::Item& item : doc.items) {
            if (skip && item.id == *skip)
                continue;
            const int number = item.kind == annot::Kind::Number ? numbers.value(item.id, 1) : 1;
            const QRectF r = annot::bounds(item, doc.dpr, number);
            if (!r.isEmpty())
                reach.push_back(r.toAlignedRect());
        }
    }
    const QRect sample = model->sampleBounds();
    bool any = false;
    int drawn = 0;
    bool pending = false;
    // The tiles to look at: all of them, or the view's nearest the middle
    // first (the others let go of at once).
    std::vector<int> order;
    if (m_limited) {
        const QRect range = editview::tileRange(m_viewport, m_size, kTile, kRing);
        const QList<int> near = editview::nearestTiles(range, m_columns, kTile, m_viewport.center(), kMaxTiles);
        std::vector<bool> wanted(m_tiles.size(), false);
        order.reserve(std::size_t(near.size()));
        for (const int i : near) {
            wanted[std::size_t(i)] = true;
            order.push_back(i);
        }
        for (std::size_t i = 0; i < m_tiles.size(); ++i) {
            Tile& tile = m_tiles[i];
            if (wanted[i] || (!tile.shown && tile.image.isNull()))
                continue;
            tile.image = QImage();
            tile.shown = false;
            tile.dirty = true; // drawn again when it comes back
            tile.upload = true; // its node goes
            any = true;
        }
    } else {
        order.resize(m_tiles.size());
        std::iota(order.begin(), order.end(), 0);
    }
    for (const int i : order) {
        Tile& tile = m_tiles[std::size_t(i)];
        if (!tile.dirty && (!m_limited || tile.shown))
            continue;
        if (m_limited && drawn > 0 && clock.elapsed() >= kPolishBudgetMs) {
            pending = true; // the rest at the next frame
            break;
        }
        tile.dirty = false;
        const QRect area = tileRect(i);
        const bool needed = !m_sparse
            || std::any_of(reach.cbegin(), reach.cend(), [&area](const QRect& r) { return r.intersects(area); });
        if (!needed) {
            if (tile.shown) {
                tile.image = QImage();
                tile.shown = false;
                tile.upload = true; // its node goes
                any = true;
            }
            continue;
        }
        // The texture made of the tile's last picture may still hold it:
        // drawing into it would first copy all of it (QImage detaches), only
        // for the compositor to overwrite every pixel. A new picture costs
        // just the allocation (dragging a big rectangle: 99% of the tiles).
        if (!tile.image.isDetached())
            tile.image = QImage();
        model->compositor().render(doc, model->base(), sample, area, &tile.image, skip);
        ++drawn;
        tile.shown = true;
        tile.upload = true;
        any = true;
    }
    if (pending)
        polish();
    if (m_limited)
        m_order.assign(order.begin(), order.end());
    else
        m_order.clear();
    if (m_limited) {
        m_mostShown = std::max<int>(m_mostShown, tilesShown());
        m_mostWanted = std::max<int>(m_mostWanted, int(order.size()));
    }
    if (gesture) {
        m_meter.polishNs.push_back(clock.nsecsElapsed());
        m_meter.polishTiles.push_back(drawn);
    } else if (m_meter.on) {
        reportMeter(model->compositor().patchesMade());
    }
    if (any || m_dropNodes)
        update();
}

void AnnotationCanvas::reportMeter(qint64 patchesNow)
{
    using namespace Qt::StringLiterals;
    Meter m = std::exchange(m_meter, Meter {});
    if (m.polishNs.size() < 8)
        return;
    const auto at = [](std::vector<qint64> v, double q) {
        if (v.empty())
            return 0.0;
        std::ranges::sort(v);
        return v[static_cast<std::size_t>(q * double(v.size() - 1))] / 1e6;
    };
    const int tiles = std::accumulate(m.polishTiles.cbegin(), m.polishTiles.cend(), 0);
    qInfo().noquote() << u"Annotation canvas: %1 polishes, %2 tiles drawn (most %3 at once), drawing p50 %4 ms, p95 %5 ms, max %6 ms; "
                         u"%7 syncs, %8 textures made, sync p50 %9 ms, p95 %10 ms; %11 mosaic patches made"_s
                             .arg(m.polishNs.size())
                             .arg(tiles)
                             .arg(*std::ranges::max_element(m.polishTiles))
                             .arg(at(m.polishNs, 0.5), 0, 'f', 2)
                             .arg(at(m.polishNs, 0.95), 0, 'f', 2)
                             .arg(at(m.polishNs, 1.0), 0, 'f', 2)
                             .arg(m.syncNs.size())
                             .arg(m.textures)
                             .arg(at(m.syncNs, 0.5), 0, 'f', 2)
                             .arg(at(m.syncNs, 0.95), 0, 'f', 2)
                             .arg(patchesNow - m.patchesBefore);
}

// The render thread, the GUI thread waiting: textures of the tiles that
// were drawn again; nothing is drawn here.
QSGNode* AnnotationCanvas::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    QElapsedTimer clock;
    clock.start();
    struct Measured {
        Meter& meter;
        QElapsedTimer& clock;
        ~Measured()
        {
            if (meter.on)
                meter.syncNs.push_back(clock.nsecsElapsed());
        }
    } measured {m_meter, clock};
    if (!old) {
        // The first frame, or the scene graph deleted the nodes (another
        // window): what m_nodes points at is gone, every picture needs its
        // texture again.
        m_nodes.clear();
        for (Tile& tile : m_tiles)
            tile.upload = tile.upload || tile.shown;
    }
    if (m_dropNodes) {
        m_dropNodes = false;
        delete old; // its children, and their textures, with it
        old = nullptr;
        m_nodes.clear();
    }
    QQuickWindow* win = window();
    if (m_tiles.empty() || !win) {
        delete old;
        m_nodes.clear();
        return nullptr;
    }
    QSGNode* root = old ? old : new QSGNode;
    m_nodes.resize(m_tiles.size(), nullptr);
    // Zoomed out on a long picture hundreds of tiles come into view at once, and all their
    // textures made in one frame took 50 ms: a limited canvas makes a few dozen a frame, those
    // nearest the middle first, and asks for the next frame.
    // (The software renderer has no textures to upload, and shows nodes added in a later frame wrongly.)
    const bool software = win->rendererInterface() && win->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    int made = 0;
    bool deferred = false;
    const auto step = [&](std::size_t i) {
        Tile& tile = m_tiles[i];
        QSGImageNode*& node = m_nodes[i];
        if (tile.upload) {
            const bool creating = tile.shown && !tile.image.isNull();
            if (creating && m_limited && !software && made >= kUploadsPerFrame) {
                deferred = true; // the node it has now (an older picture) stays meanwhile
                if (node && m_rectsStale)
                    node->setRect(nodeRect(int(i)));
                if (node && m_filterStale)
                    node->setFiltering(m_linear ? QSGTexture::Linear : QSGTexture::Nearest);
                return;
            }
            tile.upload = false;
            // A new picture gets a new node, as CaptureImage does: the old
            // one goes with its texture (setOwnsTexture).
            if (node) {
                root->removeChildNode(node);
                delete node;
                node = nullptr;
            }
            if (creating) {
                ++made;
                node = win->createImageNode();
                node->setOwnsTexture(true);
                node->setFiltering(m_linear ? QSGTexture::Linear : QSGTexture::Nearest);
                node->setMipmapFiltering(QSGTexture::None);
                node->setTexture(win->createTextureFromImage(tile.image, QQuickWindow::TextureIsOpaque));
                if (m_meter.on)
                    ++m_meter.textures;
                node->setSourceRect(QRectF(QPointF(0, 0), tile.image.size()));
                node->setRect(nodeRect(int(i)));
                root->appendChildNode(node);
            }
        } else if (node) {
            if (m_rectsStale)
                node->setRect(nodeRect(int(i)));
            if (m_filterStale)
                node->setFiltering(m_linear ? QSGTexture::Linear : QSGTexture::Nearest);
        }
    };
    if (m_limited && !m_order.empty()) {
        std::vector<bool> seen(m_tiles.size(), false);
        for (const int i : m_order) {
            if (i >= 0 && std::size_t(i) < m_tiles.size()) {
                seen[std::size_t(i)] = true;
                step(std::size_t(i));
            }
        }
        for (std::size_t i = 0; i < m_tiles.size(); ++i) {
            if (!seen[i])
                step(i);
        }
    } else {
        for (std::size_t i = 0; i < m_tiles.size(); ++i)
            step(i);
    }
    if (deferred)
        QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
    m_rectsStale = false;
    m_filterStale = false;
    return root;
}

// The item leaves its window (or the window its scene graph): the nodes go
// with the scene graph; the pictures go here, drawn again if it comes back.
void AnnotationCanvas::releaseResources()
{
    dropTiles();
}

void AnnotationCanvas::itemChange(ItemChange change, const ItemChangeData& value)
{
    if (change == ItemSceneChange && value.window)
        markAll();
    QQuickItem::itemChange(change, value);
}

} // namespace ws::capture
