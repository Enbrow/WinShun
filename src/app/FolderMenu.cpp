#include "FolderMenu.h"

#include "Favorites.h"
#include "History.h"
#include "RecentFolders.h"
#include "platform/FileManagers.h"
#include "platform/KeyRouter.h"
#include "platform/Shell.h"
#include "platform/WindowEffects.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QStyleHints>
#include <QTimer>
#include <QWindow>

#include <tlhelp32.h>

#include <chrono>
#include <cwchar>
#include <utility>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr int kMaxFavorites = 12;
constexpr int kMaxRecent = 5;
constexpr int kMaxWindows = 5;

// Listary's own process (not its service): it brings up its menu on the
// same double click (docs/pitfalls.md). Asked at most every few seconds.
bool listaryRuns()
{
    static std::atomic<qint64> askedAt {0};
    static std::atomic<bool> runs {false};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - askedAt.load() < 5000)
        return runs.load();
    bool found = false;
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry {sizeof entry};
        for (BOOL more = ::Process32FirstW(snapshot, &entry); more && !found; more = ::Process32NextW(snapshot, &entry))
            found = ::_wcsicmp(entry.szExeFile, L"Listary.exe") == 0;
        ::CloseHandle(snapshot);
    }
    runs = found;
    askedAt = now;
    return found;
}

// "D:\Projects\报告" -> "报告"; "D:\" -> "D:\".
QString nameOf(const QString& folder)
{
    const qsizetype slash = folder.lastIndexOf(u'\\');
    return slash < 0 || slash == folder.size() - 1 ? folder : folder.mid(slash + 1);
}

// Where it is: its parent folder, shortened from the left.
QString detailOf(const QString& folder)
{
    const qsizetype slash = folder.lastIndexOf(u'\\');
    if (slash < 0 || slash == folder.size() - 1)
        return {};
    QString parent = folder.left(slash == 2 ? 3 : slash);
    if (parent.size() > 40)
        parent = u"…"_s + parent.right(39);
    return parent;
}

bool samePath(const QString& a, const QString& b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

} // namespace

struct FolderMenu::Found {
    QString folder; // the tab's
    QStringList recent;
    QStringList windows;
};

FolderMenu::FolderMenu(History* history, Favorites* favorites, QObject* parent)
    : QObject(parent)
    , m_history(history)
    , m_favorites(favorites)
{
}

FolderMenu::~FolderMenu()
{
    if (m_router)
        m_router->drain();
}

void FolderMenu::setWindow(QWindow* window)
{
    m_window = window;
}

void FolderMenu::setRecordHistory(bool on)
{
    m_recordHistory = on;
    if (!on || !m_history)
        return;
    // The recent folders gathered now, in the background: the first menu comes with them.
    m_worker.post([this, history = m_history->items()] {
        if (!m_recentKnown) {
            m_recent = recentfolders::recent(history, kMaxRecent + 1);
            m_recentKnown = true;
        }
    });
}

void FolderMenu::setOptions(bool yieldToListary, bool explorer, bool desktop)
{
    m_yieldToListary = yieldToListary;
    m_explorer = explorer;
    m_desktop = desktop;
}

void FolderMenu::doubleClicked(HWND window, POINT pt)
{
    // On the listener thread: class names only, nothing that waits.
    const folderview::Hit hit = folderview::classify(window, pt);
    if ((hit.surface == folderview::Surface::Explorer && !m_explorer) || (hit.surface == folderview::Surface::Desktop && !m_desktop)
        || hit.surface == folderview::Surface::None)
        return;
    QMetaObject::invokeMethod(this, [this, hit] {
        if (m_blocked && m_blocked())
            return;
        const int ask = ++m_asks;
        const bool yield = m_yieldToListary;
        const bool recordHistory = m_recordHistory;
        const QStringList history = recordHistory && m_history ? m_history->items() : QStringList();
        m_worker.post([this, hit, ask, yield, recordHistory, history] {
            if (ask != m_asks || (yield && listaryRuns()))
                return;
            QElapsedTimer clock;
            clock.start();
            if (!folderview::isBlank(hit))
                return;
            const qint64 blankMs = clock.elapsed();
            const QString folder = hit.surface == folderview::Surface::Explorer
                ? QString::fromStdWString(filemanager::explorerFolder(hit.tab))
                : QString();
            qInfo().noquote() << u"Folder menu: blank %1, folder %2 ms"_s.arg(blankMs).arg(clock.elapsed());
            gatherHere(hit, ask, folder, recordHistory, history);
        });
    }, Qt::QueuedConnection);
}

void FolderMenu::requested(const QString& folder)
{
    if (m_blocked && m_blocked())
        return;
    const int ask = ++m_asks;
    const bool recordHistory = m_recordHistory;
    const QStringList history = recordHistory && m_history ? m_history->items() : QStringList();
    m_worker.post([this, ask, folder, recordHistory, history] {
        if (ask != m_asks)
            return;
        // Where the pointer is: Opus ran WinShunSearch.exe on the double
        // click a moment ago, and the lister under it is the one. (Asked
        // here: WindowFromPoint may wait on the window it finds.)
        folderview::Hit hit;
        ::GetCursorPos(&hit.pt);
        hit.view = ::WindowFromPoint(hit.pt);
        hit.root = hit.view ? ::GetAncestor(hit.view, GA_ROOT) : nullptr;
        if (!hit.root || filemanager::kindOf(hit.root) != filemanager::Kind::DirectoryOpus) {
            qWarning() << "Folder menu: asked for by Directory Opus, but no lister under the pointer";
            return;
        }
        hit.surface = folderview::Surface::Opus;
        hit.tab = hit.root;
        gatherHere(hit, ask, folder, recordHistory, history);
    });
}

// On the worker: the rest of what the menu shows, then the menu.
void FolderMenu::gatherHere(const folderview::Hit& hit, int ask, const QString& folder, bool recordHistory,
    const QStringList& history)
{
    QElapsedTimer clock;
    clock.start();
    Found found;
    found.folder = folder;
    for (const filemanager::Folder& f : filemanager::openFolders()) {
        const QString path = QString::fromStdWString(f.path);
        if (!samePath(path, found.folder) && !found.windows.contains(path, Qt::CaseInsensitive))
            found.windows.append(path);
    }
    const qint64 windowsMs = clock.elapsed();
    if (recordHistory && !m_recentKnown) {
        m_recent = recentfolders::recent(history, kMaxRecent + 1);
        m_recentKnown = true;
    }
    if (recordHistory)
        found.recent = m_recent;
    qInfo().noquote() << u"Folder menu: windows %1, ready %2 ms"_s.arg(windowsMs).arg(clock.elapsed());
    QMetaObject::invokeMethod(this, [this, hit, ask, found = std::move(found)] {
        if (ask == m_asks)
            show(hit, found);
    }, Qt::QueuedConnection);
    if (recordHistory) // for the next time, now that this one is up
        m_recent = recentfolders::recent(history, kMaxRecent + 1);
}

void FolderMenu::show(const folderview::Hit& hit, const Found& found)
{
    if (!m_window || (m_blocked && m_blocked()))
        return;
    // Still where it was double-clicked: the menu goes with that window.
    const HWND foreground = ::GetForegroundWindow();
    if (!foreground || ::GetAncestor(foreground, GA_ROOT) != hit.root)
        return;
    m_hit = hit;
    m_folder = found.folder;
    m_entries.clear();
    m_actions.clear();
    const auto add = [this](Kind kind, const QString& path, const QString& text, const QString& detail,
                         const QString& glyph) {
        m_entries.append(QVariantMap {{u"text"_s, text}, {u"detail"_s, detail}, {u"glyph"_s, glyph}});
        m_actions.push_back({kind, path});
    };
    const auto separator = [this] {
        if (!m_entries.isEmpty() && !m_entries.last().toMap().value(u"separator"_s).toBool()) {
            m_entries.append(QVariantMap {{u"separator"_s, true}});
            m_actions.push_back({});
        }
    };
    QStringList listed;
    if (!m_folder.isEmpty())
        listed.append(m_folder); // where it is already
    int favorites = 0;
    for (const Favorite& f : m_favorites ? m_favorites->items() : FavoriteList()) {
        if (f.kind != Favorite::Kind::Folder || favorites >= kMaxFavorites || listed.contains(f.path, Qt::CaseInsensitive))
            continue;
        add(Kind::Folder, f.path, f.name.isEmpty() ? nameOf(f.path) : f.name, f.keyword.isEmpty() ? detailOf(f.path) : f.keyword,
            u"\uE734"_s); // FavoriteStar
        listed.append(f.path);
        ++favorites;
    }
    if (favorites == 0) {
        m_entries.append(QVariantMap {{u"text"_s, tr("还没有收藏的文件夹")}, {u"detail"_s, QString()},
            {u"glyph"_s, u"\uE734"_s}, {u"disabled"_s, true}});
        m_actions.push_back({});
    }
    const auto addList = [&](const QStringList& folders, int max, const QString& glyph) {
        bool first = true;
        int count = 0;
        for (const QString& folder : folders) {
            if (count >= max || listed.contains(folder, Qt::CaseInsensitive))
                continue;
            if (std::exchange(first, false))
                separator();
            add(Kind::Folder, folder, nameOf(folder), detailOf(folder), glyph);
            listed.append(folder);
            ++count;
        }
    };
    addList(found.recent, kMaxRecent, u"\uE81C"_s); // History
    addList(found.windows, kMaxWindows, u"\uE8A7"_s); // OpenInNewWindow
    if (!m_folder.isEmpty()) {
        separator();
        add(Kind::Search, m_folder, tr("在“%1”里搜索").arg(nameOf(m_folder)), QString(), u"\uE721"_s); // Search
        if (m_favorites && m_favorites->contains(m_folder))
            add(Kind::RemoveFavorite, m_folder, tr("从收藏夹移除这个文件夹"), QString(), u"\uE8D9"_s); // Unfavorite
        else
            add(Kind::AddFavorite, m_folder, tr("收藏这个文件夹"), QString(), u"\uE734"_s);
    }
    emit entriesChanged();

    // Where the second press was: its top-left corner there, as a context
    // menu's. In Qt's pixels on the screen it is on (the same origin in both).
    const HMONITOR monitor = ::MonitorFromPoint(hit.pt, MONITOR_DEFAULTTONEAREST);
    QScreen* screen = QGuiApplication::primaryScreen();
    for (QScreen* candidate : QGuiApplication::screens()) {
        const auto* native = candidate->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor)
            screen = candidate;
    }
    if (!screen)
        return;
    const QPoint origin = screen->geometry().topLeft();
    const qreal scale = screen->devicePixelRatio();
    const QPointF pos(origin.x() + (hit.pt.x - origin.x()) / scale, origin.y() + (hit.pt.y - origin.y()) / scale);
    emit popupRequested(pos);
    if (!m_window->isVisible())
        return;
    m_open = true;
    emit openChanged(true);
    startKeys(hit.root);
}

void FolderMenu::startKeys(HWND target)
{
    stopKeys();
    KeyRouter::Callbacks callbacks;
    callbacks.key = [this](const KeyRouter::Key& key) {
        if (!key.press)
            return;
        QMetaObject::invokeMethod(this, [this, key] {
            if (m_open)
                emit keyPressed(key.key, key.text);
        }, Qt::QueuedConnection);
    };
    callbacks.clickedAway = [this] { QMetaObject::invokeMethod(this, &FolderMenu::dismiss, Qt::QueuedConnection); };
    callbacks.foregroundChanged = [this](HWND window) {
        QMetaObject::invokeMethod(this, [this, window] {
            // File Explorer's own going to another folder says so too: by
            // what is in front now, not by the event.
            const HWND foreground = ::GetForegroundWindow();
            if (window != m_hit.root && (!foreground || ::GetAncestor(foreground, GA_ROOT) != m_hit.root))
                dismiss();
        }, Qt::QueuedConnection);
    };
    auto router = std::make_unique<KeyRouter>(target, reinterpret_cast<HWND>(m_window->winId()), std::move(callbacks),
        Qt::NoModifier, KeyRouter::Policy::Menu);
    if (!router->isActive()) {
        qWarning() << "Folder menu: no keyboard hook; the mouse only";
        return;
    }
    m_router = std::move(router);
}

void FolderMenu::stopKeys()
{
    if (!m_router)
        return;
    // No key is taken any more; the releases of those it took still are its
    // own, for a moment.
    m_router->drain();
    m_draining = std::move(m_router);
    QTimer::singleShot(2s, this, [this] { m_draining.reset(); });
}

void FolderMenu::dismiss()
{
    stopKeys();
    if (m_open.exchange(false))
        emit openChanged(false);
    emit hideRequested();
}

void FolderMenu::trigger(int index)
{
    if (index < 0 || index >= static_cast<int>(m_actions.size()))
        return;
    const Entry entry = m_actions[static_cast<size_t>(index)];
    if (entry.path.isEmpty())
        return;
    dismiss();
    switch (entry.kind) {
    case Kind::Folder:
        goTo(entry.path);
        break;
    case Kind::Search:
        emit searchRequested(entry.path);
        break;
    case Kind::AddFavorite:
        if (m_favorites)
            m_favorites->add({Favorite::Kind::Folder, entry.path, nameOf(entry.path), {}});
        break;
    case Kind::RemoveFavorite:
        if (m_favorites)
            m_favorites->remove(entry.path);
        break;
    }
}

void FolderMenu::goTo(const QString& folder)
{
    if (m_recordHistory && m_history)
        m_history->record(folder, QDateTime::currentMSecsSinceEpoch());
    if (m_hit.surface == folderview::Surface::Opus) {
        // The lister it came from (the latest one used) and its tab in front.
        m_worker.post([path = QDir::toNativeSeparators(folder)] {
            if (!filemanager::goToInOpus(path.toStdWString()))
                shell::open(path);
        });
        return;
    }
    if (m_hit.surface != folderview::Surface::Explorer) {
        shell::open(folder); // the desktop: in the file manager chosen
        return;
    }
    // The same tab goes there; one the shell's list lacks (it can miss
    // some): a new window.
    m_worker.post([tab = m_hit.tab, path = QDir::toNativeSeparators(folder)] {
        if (!filemanager::goTo(tab, path.toStdWString())) {
            qWarning() << "Folder menu: tab not found; a new window instead";
            shell::open(path);
        }
    });
}

QRectF FolderMenu::screenArea(QPointF globalPos) const
{
    QScreen* screen = QGuiApplication::screenAt(globalPos.toPoint());
    return screen ? QRectF(screen->availableGeometry()) : QRectF();
}

void FolderMenu::prepareMenuWindow(QWindow* menu) const
{
    if (!menu)
        return;
    // Shown without taking the focus from File Explorer (see the class).
    menu->setProperty("_q_showWithoutActivating", true);
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    win::styleFramelessWindow(menu);
    win::setDarkFrame(menu, dark);
}

} // namespace ws
