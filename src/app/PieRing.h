#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace ws {

// The drop pie's rings (DropPieWindow.qml), drawn with QPainter: the
// centre, 移动 and 复制, and the folders' sectors, with gaps between them
// and a soft shadow outside. Antialiased arcs at the window's scale, which
// Qt Quick's own items cannot draw (QtQuick.Shapes is not deployed: see
// docs/pitfalls.md). The item is the ring's box plus `shadow` on each side;
// the labels are QML Text on top.
class PieRing : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal ringScale MEMBER m_scale NOTIFY changed FINAL)
    Q_PROPERTY(int count MEMBER m_count NOTIFY changed FINAL)
    Q_PROPERTY(int hovered MEMBER m_hovered NOTIFY changed FINAL)
    Q_PROPERTY(int picked MEMBER m_picked NOTIFY changed FINAL)
    Q_PROPERTY(QString mode MEMBER m_mode NOTIFY changed FINAL) // "move", "copy", ""
    Q_PROPERTY(QString innerHover MEMBER m_innerHover NOTIFY changed FINAL)
    Q_PROPERTY(bool moveAllowed MEMBER m_moveAllowed NOTIFY changed FINAL)
    Q_PROPERTY(bool copyAllowed MEMBER m_copyAllowed NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList disabled MEMBER m_disabled NOTIFY changed FINAL) // bools by sector
    Q_PROPERTY(qreal edgeWidth MEMBER m_edgeWidth NOTIFY changed FINAL) // Theme.edge(dpr)
    Q_PROPERTY(qreal shadow MEMBER m_shadow NOTIFY changed FINAL) // logical pixels around the ring
    Q_PROPERTY(QColor fill MEMBER m_fill NOTIFY changed FINAL)
    Q_PROPERTY(QColor innerFill MEMBER m_innerFill NOTIFY changed FINAL)
    Q_PROPERTY(QColor hoverFill MEMBER m_hoverFill NOTIFY changed FINAL)
    Q_PROPERTY(QColor accentFill MEMBER m_accentFill NOTIFY changed FINAL)
    Q_PROPERTY(QColor edge MEMBER m_edge NOTIFY changed FINAL)
    Q_PROPERTY(QColor shadowColor MEMBER m_shadowColor NOTIFY changed FINAL)

public:
    explicit PieRing(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

signals:
    void changed(); // any of them: painted again

private:
    qreal m_scale = 1;
    int m_count = 1;
    int m_hovered = -1;
    int m_picked = -1;
    QString m_mode;
    QString m_innerHover;
    bool m_moveAllowed = true;
    bool m_copyAllowed = true;
    QVariantList m_disabled;
    qreal m_edgeWidth = 1;
    qreal m_shadow = 16;
    QColor m_fill;
    QColor m_innerFill;
    QColor m_hoverFill;
    QColor m_accentFill;
    QColor m_edge;
    QColor m_shadowColor;
};

} // namespace ws
