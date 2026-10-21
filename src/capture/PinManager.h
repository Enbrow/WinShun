#pragma once

#include "PinWindow.h"

#include <QImage>
#include <QObject>
#include <QRect>

#include <functional>
#include <memory>
#include <vector>

namespace ws::capture {

// The pictures pinned on the screen (PinWindow): the screen capture's, and
// pictures pinned from a file's row or the clipboard history (贴到屏幕),
// which need no capture turned on. The app makes it when first wanted;
// turning the capture off closes them all (the stage 2 rule). Each one's
// menu (the native popup menu, as the tray's): 复制, 保存, 另存为…,
// 识别文字, 置顶, 鼠标穿透, 关闭, 关闭全部贴图. What leaves (the
// clipboard, a file, the OCR window) goes through the callbacks.
class PinManager : public QObject {
    Q_OBJECT

public:
    struct Callbacks {
        std::function<void(const QImage&, bool screenshot)> copy; // screenshot: the pin came from a capture
        std::function<void(const QImage&, const QString& path)> save; // empty path: the Screenshots folder
        std::function<void(const QImage&, double dpr, const QRect& picture)> ocr;
        std::function<void()> closed; // a pin closed: its memory can go back to Windows
        std::function<bool()> dark;
        bool ocrAvailable = false; // the models are there (PaddleOcr::installed)
    };

    explicit PinManager(Callbacks callbacks, QObject* parent = nullptr);
    ~PinManager() override;

    // A pin of `image` with its top left at `origin` (physical, virtual
    // desktop); `screenshot`: taken with the capture (PinWindow::screenshot);
    // `project`: its annotations, to edit again (empty: none).
    void pin(const QImage& image, QPoint origin, bool screenshot = true, QByteArray project = {});
    int count() const { return static_cast<int>(m_pins.size()); }
    bool anyClickThrough() const;
    void clearClickThrough(); // the tray menu: every pin takes the mouse again
    void closeAll();

private:
    void showMenu(PinWindow* pin, POINT at);
    void closed(PinWindow* pin);

    Callbacks m_callbacks;
    std::vector<std::unique_ptr<PinWindow>> m_pins;
    std::vector<std::unique_ptr<PinWindow>> m_closing; // windows gone, deleted once their procedure returned
};

} // namespace ws::capture
