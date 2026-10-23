#pragma once

#include "PieGeometry.h"
#include "platform/ComWorker.h"
#include "platform/PieDrop.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

class QQuickWindow;
class QScreen;

namespace ws {

class Favorites;
class SystemCatalog;

// 拖放圆盘: while files are dragged (File Explorer, the desktop, any file
// manager that drags with OLE), a shake of the mouse brings up a ring around
// the pointer: the middle cancels, the inner ring chooses 移动 (left half) or
// 复制 (right half), the outer ring holds the first eight favorite folders.
// Let go over a folder and the files go there; leave the ring and it goes
// away, the drag carrying on as before.
//
// KeyListener sees the shake (Raw Input, ShakeDetector) and that an OLE
// drag is under way; this shows the window (DropPieWindow.qml, made once
// and kept) without taking the focus, and is its OLE drop target in place
// of Qt's (piedrop::DropTarget). It shows itself only once the drag enters
// it with files or shell items: a drag of text (Word, VS Code) passes the
// shake's test too. Files are copied or moved by IFileOperation on threads
// of their own and the source is told nothing was dropped, so it never
// deletes anything itself; shell items that are not files (a phone's, a zip
// file's contents) go to the folder's own drop target, as in File Explorer.
//
// Its window is see-through but still takes the pointer where nothing is
// drawn (a layered D3D11 window): it goes the moment the pointer leaves the
// ring, so it never stands between a drag and what is under it.
class DropPie : public QObject, private piedrop::Handler {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    // The folders: {name, line1, line2, sub, path, disabled, reason, x, y}
    // (x, y: the label's centre from the ring's centre).
    Q_PROPERTY(QVariantList sectors READ sectors NOTIFY sectorsChanged FINAL)
    Q_PROPERTY(bool noFolders READ noFolders NOTIFY sectorsChanged FINAL)
    Q_PROPERTY(QPointF centre READ centre NOTIFY layoutChanged FINAL) // in the window
    Q_PROPERTY(qreal ringScale READ ringScale NOTIFY layoutChanged FINAL)
    Q_PROPERTY(int labelPixelSize READ labelPixelSize NOTIFY layoutChanged FINAL)
    Q_PROPERTY(bool revealed READ revealed NOTIFY revealedChanged FINAL)
    Q_PROPERTY(QPointF pointer READ pointer NOTIFY pointerChanged FINAL) // in the window
    Q_PROPERTY(int hovered READ hovered NOTIFY stateChanged FINAL) // the outer sector under the pointer, -1
    Q_PROPERTY(int picked READ picked NOTIFY stateChanged FINAL) // ... if it can be dropped on, else -1
    Q_PROPERTY(QString mode READ mode NOTIFY stateChanged FINAL) // "move", "copy" or ""
    Q_PROPERTY(QString innerHover READ innerHover NOTIFY stateChanged FINAL) // the inner half under the pointer
    Q_PROPERTY(bool moveAllowed READ moveAllowed NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool copyAllowed READ copyAllowed NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool confirming READ confirming NOTIFY stateChanged FINAL) // dropped: the sector flashes
    // Beside the pointer: "移动到" and the folder's whole name, or why it
    // cannot be dropped on (tipVerb empty); tip empty: none.
    Q_PROPERTY(QString tipVerb READ tipVerb NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString tip READ tip NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString statusVerb READ statusVerb NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString statusName READ statusName NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString statusDetail READ statusDetail NOTIFY stateChanged FINAL) // the path, shortened
    Q_PROPERTY(qreal statusWidth READ statusWidth NOTIFY layoutChanged FINAL)

public:
    DropPie(Favorites* favorites, SystemCatalog* places, QObject* parent = nullptr);
    ~DropPie() override;

    // DropPieWindow: made into a see-through window and taken over as a
    // drop target here, then shown once out of sight so that its first
    // real showing is not slowed by the graphics' setting up.
    void setWindow(QQuickWindow* window);

    // From the listener thread (KeyListener::Callbacks): a shake during an
    // OLE drag at `pt` (physical pixels), whose source has captured the
    // mouse with `captureWindow`; the primary button let go.
    void shaken(POINT pt, HWND captureWindow, DWORD sourcePid);
    void buttonReleased();

    QVariantList sectors() const { return m_sectorItems; }
    bool noFolders() const noexcept { return m_folders.empty(); }
    QPointF centre() const noexcept { return m_centre; }
    qreal ringScale() const noexcept { return m_scale; }
    int labelPixelSize() const noexcept { return m_labelPixelSize; }
    bool revealed() const noexcept { return m_revealed; }
    QPointF pointer() const noexcept { return m_pointer; }
    int hovered() const noexcept { return m_hovered; }
    int picked() const noexcept { return m_picked; }
    QString mode() const;
    QString innerHover() const;
    bool moveAllowed() const noexcept { return m_allowed & DROPEFFECT_MOVE; }
    bool copyAllowed() const noexcept { return m_allowed & DROPEFFECT_COPY; }
    bool confirming() const noexcept { return m_confirming; }
    QString tipVerb() const { return m_tipVerb; }
    QString tip() const { return m_tip; }
    QString statusVerb() const { return m_statusVerb; }
    QString statusName() const { return m_statusName; }
    QString statusDetail() const { return m_statusDetail; }
    qreal statusWidth() const noexcept { return m_statusWidth; }

signals:
    void sectorsChanged();
    void layoutChanged();
    void revealedChanged();
    void pointerChanged();
    void stateChanged();

private:
    struct Folder {
        QString path; // file-system path, no trailing backslash but a drive's
        QString name; // as the favorite is named
        bool network = false;
    };
    enum class Block { None, Missing, Source, Inside, Network };

    // piedrop::Handler, on the GUI thread
    DWORD dragEnter(IDataObject* data, DWORD keys, POINT pt, DWORD allowed) override;
    DWORD dragOver(DWORD keys, POINT pt, DWORD allowed) override;
    void dragLeave() override;
    DWORD drop(IDataObject* data, DWORD keys, POINT pt, DWORD allowed) override;

    void onShaken(POINT pt, HWND captureWindow, DWORD sourcePid);
    void open(POINT pt);
    void hide(const char* why);
    void takeDropSite();
    void prewarm();
    void gatherFolders(); // favorites -> m_folders, places resolved on the worker
    void checkFolders(); // which local ones are missing, on the worker
    void updateSectors();
    Block blockOf(const Folder& folder) const;
    QString reasonOf(Block block) const;
    pie::Hit hitAt(POINT pt) const;
    DWORD track(DWORD keys, POINT pt); // DragOver's work: the state and the effect
    void updateTexts();
    void setRevealed(bool revealed);

    Favorites* m_favorites;
    SystemCatalog* m_places;
    QPointer<QQuickWindow> m_window;
    HWND m_hwnd = nullptr; // the window's, as registered for drops
    Microsoft::WRL::ComPtr<piedrop::DropTarget> m_target;

    std::vector<Folder> m_folders; // the sectors, at most eight
    QSet<QString> m_missing; // case-folded paths of local folders found missing
    QVariantList m_sectorItems;
    pie::Radii m_radii;
    qreal m_scale = 1;
    int m_labelPixelSize = 13;
    QPointF m_centre; // in the window
    QPointF m_pointer;
    qreal m_statusWidth = 480;

    // The drag over the pie
    bool m_open = false; // shown (revealed or not)
    bool m_entered = false; // DragEnter came
    bool m_revealed = false;
    bool m_confirming = false;
    DWORD m_allowed = 0; // what the source allows
    std::vector<std::wstring> m_items; // its files; empty: shell items that are not files
    std::vector<Block> m_blocks; // by sector, for this drag
    pie::Latch m_latch;
    pie::Hit m_hit;
    bool m_ctrl = false;
    bool m_shift = false;
    int m_hovered = -1;
    int m_picked = -1;
    QString m_tipVerb;
    QString m_tip;
    QString m_statusVerb;
    QString m_statusName;
    QString m_statusDetail;
    POINT m_shownAt {};
    QElapsedTimer m_shownClock;
    QTimer m_revealWait; // no DragEnter yet: the drag is elsewhere, or the pointer is still
    QTimer m_releaseWait; // the button let go without a Drop or a DragLeave
    QTimer m_confirmTimer; // the dropped-on sector shown a moment
    int m_checks = 0; // the latest existence check: older answers are dropped

    piedrop::FileOperations m_operations; // waits for copies under way when it goes
    ComWorker m_worker; // last: its tasks use the members above
};

} // namespace ws
