#pragma once

#include "AnnotationModel.h"

#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QtQml/qqmlregistration.h>

#include <vector>

class QSGImageNode;

namespace ws::capture {

// The annotations as the overlay shows them (docs/design-capture-roadmap.md,
// 3.3): the compositor's picture in 256 x 256 image px tiles, each a texture
// of its own, so a change draws and uploads again only the tiles it reaches.
// The tiles are drawn on the GUI thread (updatePolish), where the model and
// its compositor live; the render thread only makes textures of the finished
// pictures. The model never hands a picture through a property: the canvas
// reads model->base() and the document itself.
class AnnotationCanvas : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(ws::capture::AnnotationModel* model READ model WRITE setModel NOTIFY modelChanged FINAL)
    // Item px per image px, as the CaptureImage beneath maps the same picture.
    Q_PROPERTY(qreal scaleX READ scaleX WRITE setScaleX NOTIFY scaleChanged FINAL)
    Q_PROPERTY(qreal scaleY READ scaleY WRITE setScaleY NOTIFY scaleChanged FINAL)
    // Only the tiles some object reaches have a texture; elsewhere what is
    // beneath (the CaptureImage of the same picture) shows the same pixels.
    // The overlay's way, which keeps a mostly empty screen nearly free.
    Q_PROPERTY(bool sparse READ sparse WRITE setSparse NOTIFY sparseChanged FINAL)

public:
    static constexpr int kTile = 256;

    explicit AnnotationCanvas(QQuickItem* parent = nullptr);
    ~AnnotationCanvas() override;

    AnnotationModel* model() const { return m_model; }
    void setModel(AnnotationModel* model);
    qreal scaleX() const { return m_scaleX; }
    void setScaleX(qreal scale);
    qreal scaleY() const { return m_scaleY; }
    void setScaleY(qreal scale);
    bool sparse() const { return m_sparse; }
    void setSparse(bool sparse);
    // The tiles that have a picture now (what the tests see of sparse mode).
    int tilesShown() const;

signals:
    void modelChanged();
    void scaleChanged();
    void sparseChanged();

protected:
    void updatePolish() override;
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void releaseResources() override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    struct Tile {
        // The compositor's pixels, rendered into again and again (a new
        // picture while a texture still holds the last one); kept, so a new
        // window needs no new render.
        QImage image;
        bool shown = false; // has a picture (sparse: some object reaches it)
        bool dirty = false; // to render at the next polish
        bool upload = false; // its texture to make, or to remove, at the next sync
    };

    void onChanged(const QRectF& dirty);
    void markAll();
    void markTiles(const QRect& area);
    void dropTiles();
    QRect tileRect(int index) const;
    QRectF nodeRect(int index) const;

    QPointer<AnnotationModel> m_model;
    qreal m_scaleX = 1;
    qreal m_scaleY = 1;
    bool m_sparse = false;
    QSize m_size; // the picture's, image px
    int m_columns = 0;
    int m_rows = 0;
    std::vector<Tile> m_tiles; // row by row
    // The render thread's, touched only in updatePaintNode() (while the GUI
    // thread waits): the node of each tile, null where there is none.
    std::vector<QSGImageNode*> m_nodes;
    bool m_dropNodes = false; // the tiles were let go of: so are their nodes, at the next sync
    bool m_rectsStale = true; // the scale changed: every node's rectangle

    // What a gesture cost here, logged when it ends (info): each polish's
    // tiles and drawing time (GUI thread), each sync's textures and time
    // (render thread, the GUI thread waiting: no lock needed).
    struct Meter {
        bool on = false;
        std::vector<qint64> polishNs;
        std::vector<int> polishTiles;
        std::vector<qint64> syncNs;
        int textures = 0;
        qint64 patchesBefore = 0;
    };
    Meter m_meter;
    void reportMeter(qint64 patchesNow);
};

} // namespace ws::capture
