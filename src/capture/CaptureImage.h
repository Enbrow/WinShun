#pragma once

#include <QImage>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

namespace ws::capture {

// A picture drawn unsmoothed over the item's whole size: the frozen screen
// (1:1, the item sized by CaptureScreen's scale) and the magnifier (a whole
// number of pixels per picture pixel). Not Image with a provider: the
// texture here exists exactly as long as the picture is set. Setting a null
// picture removes it with the next frame, which is how the capture gives
// the memory back (Capture::finish renders that frame cloaked).
class CaptureImage : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QImage image READ image WRITE setImage NOTIFY imageChanged FINAL)

public:
    explicit CaptureImage(QQuickItem* parent = nullptr);

    QImage image() const { return m_image; }
    void setImage(const QImage& image);

signals:
    void imageChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;

private:
    QImage m_image;
    bool m_dirty = false;
};

} // namespace ws::capture
