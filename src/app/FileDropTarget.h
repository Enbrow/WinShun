#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

namespace ws {

// Files and folders dropped on a window from File Explorer. Win顺 runs as
// administrator and Explorer does not: Windows (UIPI) lets no OLE drag from a
// lower process reach it, so Qt's own drop target (and a QML DropArea) never
// hears of them. Windows' older way still works when let through: the window
// takes WM_DROPFILES (DragAcceptFiles, with no OLE drop target registered,
// which would be asked instead) and lets that message and the two that carry
// its data in from below (ChangeWindowMessageFilterEx). No feedback while
// dragging: only the drop is told.
class FileDropTarget : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QWindow* window READ window WRITE setWindow NOTIFY windowChanged FINAL)

public:
    explicit FileDropTarget(QObject* parent = nullptr);
    ~FileDropTarget() override;

    QWindow* window() const { return m_window; }
    void setWindow(QWindow* window);

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void windowChanged();
    // "file:///…" for each, and where in the window (logical pixels).
    void dropped(const QVariantList& urls, qreal x, qreal y);

private:
    void accept(); // once the window has its HWND, and again each time it is shown

    QPointer<QWindow> m_window;
};

} // namespace ws
