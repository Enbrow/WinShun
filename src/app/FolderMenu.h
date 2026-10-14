#pragma once

#include "platform/ComWorker.h"
#include "platform/FolderViewHit.h"

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>

class QWindow;

namespace ws {

class Favorites;
class History;
class KeyRouter;

// A double click on the blank space of a File Explorer folder or of the
// desktop brings up a menu of folders to go to, as Listary's does (GitHub
// #22): the favorite folders, the recent ones, those open in other windows;
// then "search here" and adding this folder to the favorites. Chosen in a
// File Explorer window, the same tab goes there; on the desktop, the file
// manager opens it.
//
// The menu never takes the focus: File Explorer stays in front, its
// selection and caret as they were, and the keys the menu uses are taken on
// their way to it (KeyRouter, as the clipboard does over other programs). A
// click anywhere else closes it and goes on to where it was made; so does a
// double click there (it does not open the menu again).
class FolderMenu : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    // {text, detail, glyph} or {separator: true}
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged FINAL)

public:
    FolderMenu(History* history, Favorites* favorites, QObject* parent = nullptr);
    ~FolderMenu() override;

    void setWindow(QWindow* window); // FolderMenuWindow
    void setRecordHistory(bool on);
    // auto | on (Settings::folderMenu); Explorer and the desktop each on or off.
    void setOptions(bool yieldToListary, bool explorer, bool desktop);
    // Whether something else has the keys now (the clipboard over File
    // Explorer): the menu does not come, the double click only closes that.
    void setBlocked(std::function<bool()> blocked) { m_blocked = std::move(blocked); }

    // From the listener thread (KeyListener): a double click there. Window
    // classes are looked at at once; what may be a folder's blank space is
    // asked about on a thread of its own, and the menu comes if it is.
    void doubleClicked(HWND window, POINT pt);
    bool isOpen() const noexcept { return m_open.load(); }

    QVariantList entries() const { return m_entries; }

    Q_INVOKABLE QRectF screenArea(QPointF globalPos) const;
    Q_INVOKABLE void prepareMenuWindow(QWindow* menu) const;
    Q_INVOKABLE void trigger(int index);
    Q_INVOKABLE void dismiss();

signals:
    void entriesChanged();
    void popupRequested(QPointF pos); // logical pixels: the window's top-left corner, or where it flips from
    void hideRequested();
    void keyPressed(int key, const QString& text); // taken on its way to File Explorer
    void searchRequested(const QString& folder); // 在此搜索
    void openChanged(bool open);

private:
    enum class Kind { Folder, Search, AddFavorite, RemoveFavorite };
    struct Entry {
        Kind kind = Kind::Folder;
        QString path;
    };
    struct Found; // what the worker gathered

    void show(const folderview::Hit& hit, const Found& found);
    void startKeys(HWND target);
    void stopKeys();
    void goTo(const QString& folder);

    History* m_history;
    Favorites* m_favorites;
    QPointer<QWindow> m_window;
    std::function<bool()> m_blocked;
    bool m_recordHistory = true;
    std::atomic<bool> m_yieldToListary {true};
    std::atomic<bool> m_explorer {true};
    std::atomic<bool> m_desktop {true};
    std::atomic<bool> m_open {false};
    std::atomic<int> m_asks {0}; // the latest double click's number: older answers are dropped
    folderview::Hit m_hit; // where the menu is up
    QString m_folder; // the folder that tab shows; empty on the desktop, Home, This PC
    QVariantList m_entries;
    std::vector<Entry> m_actions; // by entry
    std::unique_ptr<KeyRouter> m_router;
    std::unique_ptr<KeyRouter> m_draining; // done, until the keys it took are let go
    // The recent folders as last gathered (worker thread only): reading
    // Windows' list takes a good part of a second the first time, so the
    // menu comes with these, and they are gathered again after it.
    QStringList m_recent;
    bool m_recentKnown = false;
    ComWorker m_worker; // last: its tasks use the members above
};

} // namespace ws
