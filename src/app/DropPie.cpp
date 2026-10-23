#include "DropPie.h"

#include "DropPieText.h"
#include "Favorites.h"
#include "PieLabels.h"
#include "SystemCatalog.h"
#include "platform/Integrity.h"

#include <QDebug>
#include <QDir>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>

#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;
using Microsoft::WRL::ComPtr;

namespace ws {

namespace {

// The screen a physical point is on, as Qt knows it; the primary when none is.
QScreen* screenAt(POINT pt)
{
    const HMONITOR monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    QScreen* screen = QGuiApplication::primaryScreen();
    for (QScreen* candidate : QGuiApplication::screens()) {
        const auto* native = candidate->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor)
            screen = candidate;
    }
    return screen;
}

// A physical point in Qt's pixels on `screen` (the same origin in both).
QPointF logicalOf(POINT pt, const QScreen* screen)
{
    const QPoint origin = screen->geometry().topLeft();
    const qreal scale = screen->devicePixelRatio();
    return {origin.x() + (pt.x - origin.x()) / scale, origin.y() + (pt.y - origin.y()) / scale};
}

// "D:\Work\" -> "D:\Work"; a drive keeps its backslash.
QString normalized(QString path)
{
    path = QDir::toNativeSeparators(path);
    while (path.size() > 3 && path.endsWith(u'\\'))
        path.chop(1);
    return path;
}

bool sameOrInside(const QString& folder, const QString& item)
{
    if (folder.compare(item, Qt::CaseInsensitive) == 0)
        return true;
    const QString prefix = item.endsWith(u'\\') ? item : item + u'\\';
    return folder.startsWith(prefix, Qt::CaseInsensitive);
}

QString parentOf(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(u'\\');
    if (slash < 0)
        return {};
    return path.left(slash == 2 && path.size() > 1 && path.at(1) == u':' ? 3 : slash);
}

bool isNetworkPath(const std::wstring& path)
{
    if (path.starts_with(L"\\\\"))
        return true;
    if (path.size() < 2 || path[1] != L':')
        return false;
    const std::wstring root = path.substr(0, 2) + L"\\";
    return ::GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
}

// On the worker: a place's command that names a folder of the file system
// ("shell:Downloads", "%USERPROFILE%\Desktop") as its path; empty for the
// rest (Control Panel, a page of Settings, a program).
QString placeFolder(const QString& command)
{
    const auto [program, arguments] = splitCommand(command);
    if (program.isEmpty() || !arguments.isEmpty())
        return {};
    PIDLIST_ABSOLUTE item = nullptr;
    if (FAILED(::SHParseDisplayName(reinterpret_cast<const wchar_t*>(program.utf16()), nullptr, &item, 0, nullptr)))
        return {};
    wchar_t path[MAX_PATH * 4] {};
    const bool found = ::SHGetPathFromIDListEx(item, path, static_cast<DWORD>(std::size(path)), GPFIDL_DEFAULT);
    ::CoTaskMemFree(item);
    if (!found)
        return {};
    const DWORD attributes = ::GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? QString::fromWCharArray(path)
                                                                                         : QString();
}

} // namespace

DropPie::DropPie(Favorites* favorites, SystemCatalog* places, QObject* parent)
    : QObject(parent)
    , m_favorites(favorites)
    , m_places(places)
{
    m_revealWait.setSingleShot(true);
    connect(&m_revealWait, &QTimer::timeout, this, [this] {
        if (!m_open || m_entered)
            return;
        // The drag never came over the ring. While the pointer is still,
        // OLE asks no window (it looks again at the next move): wait on.
        POINT now {};
        ::GetCursorPos(&now);
        const bool moved = std::abs(now.x - m_shownAt.x) > 4 || std::abs(now.y - m_shownAt.y) > 4;
        if (moved || m_shownClock.elapsed() > 1500)
            hide("the drag did not come over it");
        else
            m_revealWait.start(100ms);
    });
    m_releaseWait.setSingleShot(true);
    connect(&m_releaseWait, &QTimer::timeout, this, [this] {
        if (m_open && !m_confirming)
            hide("the button was let go, but no drop or leave came");
    });
    m_confirmTimer.setSingleShot(true);
    connect(&m_confirmTimer, &QTimer::timeout, this, [this] { hide("dropped"); });
    m_hintTimer.setSingleShot(true);
    connect(&m_hintTimer, &QTimer::timeout, this, [this] { hide("note shown"); });
    if (m_favorites)
        connect(m_favorites, &Favorites::changed, this, &DropPie::gatherFolders);
    if (m_places)
        connect(m_places, &SystemCatalog::changed, this, &DropPie::gatherFolders);
    gatherFolders();
}

DropPie::~DropPie()
{
    if (m_window && m_hwnd && ::IsWindow(m_hwnd))
        ::RevokeDragDrop(m_hwnd);
    if (m_target)
        m_target->detach();
    m_target.Reset();
}

void DropPie::setWindow(QQuickWindow* window)
{
    m_window = window;
    if (!m_window)
        return;
    // See-through around the ring: a window colour that is all transparent
    // (Theme: never half), and an alpha channel from the start, before the
    // swap chain is made.
    QSurfaceFormat format = m_window->requestedFormat();
    format.setAlphaBufferSize(8);
    m_window->setFormat(format);
    m_window->setColor(Qt::transparent);
    m_window->setProperty("_q_showWithoutActivating", true);
    m_window->create();
    takeDropSite();
    QTimer::singleShot(2s, this, &DropPie::prewarm); // after the launcher's own (App::prewarmLauncher)
}

// Qt registered its own drop target when it made the window: ours in its
// place. Qt never registers again unless the window is made anew (then its
// handle is another), so this is looked at before every showing.
void DropPie::takeDropSite()
{
    if (!m_window)
        return;
    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (hwnd == m_hwnd && m_target)
        return;
    if (m_target)
        m_target->detach();
    ::RevokeDragDrop(hwnd);
    m_target = Microsoft::WRL::Make<piedrop::DropTarget>(static_cast<piedrop::Handler*>(this), hwnd);
    const HRESULT hr = m_target ? ::RegisterDragDrop(hwnd, m_target.Get()) : E_OUTOFMEMORY;
    if (FAILED(hr))
        qWarning().noquote() << u"Drop pie: could not take the window's drops (0x%1)"_s.arg(static_cast<quint32>(hr), 8, 16, u'0');
    m_hwnd = hwnd;
}

// Shown once, a pixel big, until its first frame: the graphics are set up
// now, and the first shake shows the ring at once.
void DropPie::prewarm()
{
    const QScreen* screen = QGuiApplication::primaryScreen();
    if (!m_window || !screen)
        return;
    const QRect area = screen->availableGeometry();
    m_window->setGeometry(QRect(area.bottomRight(), QSize(1, 1)));
    connect(m_window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_window && !m_open)
            m_window->hide();
    }, static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::SingleShotConnection));
    m_window->show();
    QTimer::singleShot(1s, this, [this] {
        if (m_window && !m_open)
            m_window->hide(); // no frame came (a renderer that never draws hidden windows)
    });
}

void DropPie::shaken(POINT pt, HWND captureWindow, DWORD sourcePid)
{
    QMetaObject::invokeMethod(this, [this, pt, captureWindow, sourcePid] { onShaken(pt, captureWindow, sourcePid); },
        Qt::QueuedConnection);
}

void DropPie::buttonReleased()
{
    QMetaObject::invokeMethod(this, [this] {
        if (!m_hint.isEmpty())
            hide("the button was let go");
        else if (m_open && !m_confirming && !m_releaseWait.isActive())
            m_releaseWait.start(300ms); // the Drop or DragLeave is on its way
    }, Qt::QueuedConnection);
}

void DropPie::onShaken(POINT pt, HWND captureWindow, DWORD sourcePid)
{
    if (!m_window || m_open)
        return;
    if (sourcePid == ::GetCurrentProcessId()) {
        // Results dragged out of the launcher: DoDragDrop runs on this very
        // thread, and the ring would be its own drop target from inside it.
        qInfo() << "Drop pie: shaken while dragging out of Win顺 itself; not shown";
        return;
    }
    if (integrity::higherThanOurs(captureWindow)) {
        // UIPI: its drops never reach a window of ours.
        qInfo() << "Drop pie: shaken, but the drag comes from an administrator's program";
        showHint(pt, tr("这个程序以管理员身份运行，拖出来的文件 Win顺 接不到"));
        return;
    }
    qInfo().noquote() << u"Drop pie: shaken at %1,%2"_s.arg(pt.x).arg(pt.y);
    open(pt);
}

void DropPie::open(POINT pt)
{
    QScreen* screen = screenAt(pt);
    if (!screen || !m_window)
        return;
    updateSectors(); // the size may have changed (界面大小)
    const QPointF cursor = logicalOf(pt, screen);
    const double ringRadius = m_radii.outerOut + 2; // with its edge
    const QPointF centre = pie::placeCentre(cursor, QRectF(screen->availableGeometry()), ringRadius,
        m_radii.innerOut - 6 * m_scale);
    const pie::Margins margins {150 * m_scale, 48 * m_scale, 84 * m_scale};
    const QRect rect = pie::windowRect(centre, ringRadius, screen->geometry(), margins);
    m_centre = centre - rect.topLeft();
    m_pointer = cursor - rect.topLeft();
    m_statusWidth = std::min<qreal>(2 * m_radii.outerOut + 80 * m_scale, rect.width() - 16);

    m_open = true;
    m_entered = false;
    m_confirming = false;
    m_allowed = DROPEFFECT_MOVE | DROPEFFECT_COPY;
    m_items.clear();
    m_blocks.assign(m_folders.size(), Block::None);
    for (std::size_t i = 0; i < m_folders.size(); ++i)
        m_blocks[i] = blockOf(m_folders[i]);
    m_hit = {};
    m_hovered = m_picked = -1;
    m_latch.reset();
    updateSectors();
    updateTexts();
    emit layoutChanged();
    emit pointerChanged();
    emit stateChanged();

    m_window->setGeometry(rect);
    takeDropSite();
    m_window->show();
    m_window->raise();
    m_shownAt = pt;
    m_shownClock.start();
    m_revealWait.start(300ms);
    checkFolders();
}

void DropPie::showHint(POINT pt, const QString& text)
{
    QScreen* screen = screenAt(pt);
    if (!screen || !m_window)
        return;
    m_hint = text;
    emit hintChanged();
    // Beside the pointer, never under it: the window would stand in the
    // drag's way there.
    const QPointF cursor = logicalOf(pt, screen);
    const QRect area = screen->availableGeometry();
    QRect rect(QPoint(qRound(cursor.x()) + 16, qRound(cursor.y()) + 20), QSize(440, 48));
    if (rect.right() > area.right())
        rect.moveRight(qRound(cursor.x()) - 16);
    if (rect.bottom() > area.bottom())
        rect.moveBottom(qRound(cursor.y()) - 20);
    m_open = true;
    m_window->setGeometry(rect.intersected(screen->geometry()));
    takeDropSite();
    m_window->show();
    m_window->raise();
    m_hintTimer.start(3s);
}

void DropPie::hide(const char* why)
{
    const bool shown = m_open || (m_window && m_window->isVisible());
    if (m_target)
        m_target->leaveEarly();
    if (m_window)
        m_window->hide();
    m_revealWait.stop();
    m_releaseWait.stop();
    m_confirmTimer.stop();
    m_hintTimer.stop();
    m_open = false;
    m_entered = false;
    m_confirming = false;
    m_items.clear();
    m_hovered = m_picked = -1;
    m_hit = {};
    setRevealed(false);
    if (!m_hint.isEmpty()) {
        m_hint.clear();
        emit hintChanged();
    }
    emit stateChanged();
    if (shown)
        qInfo() << "Drop pie: hidden," << why;
}

void DropPie::setRevealed(bool revealed)
{
    if (m_revealed == revealed)
        return;
    m_revealed = revealed;
    emit revealedChanged();
}

// ---- the drag over it ------------------------------------------------------

DWORD DropPie::dragEnter(IDataObject* data, DWORD keys, POINT pt, DWORD allowed)
{
    if (!m_open || !m_hint.isEmpty()) {
        hide("the drag came over the note");
        return DROPEFFECT_NONE;
    }
    m_entered = true;
    m_revealWait.stop();
    const bool files = piedrop::hasFiles(data);
    const bool shellItems = piedrop::hasShellItems(data);
    if (!files && !shellItems) {
        hide("not files (text, a picture…)");
        return DROPEFFECT_NONE;
    }
    if (!(allowed & (DROPEFFECT_MOVE | DROPEFFECT_COPY))) {
        hide("the source allows neither moving nor copying");
        return DROPEFFECT_NONE;
    }
    m_allowed = allowed;
    // Read once, here: every call to the source waits on another process.
    m_items = files ? piedrop::filePaths(data) : std::vector<std::wstring> {};
    if (m_items.empty() && !shellItems) {
        hide("its file names could not be read");
        return DROPEFFECT_NONE;
    }
    for (std::size_t i = 0; i < m_folders.size(); ++i)
        m_blocks[i] = blockOf(m_folders[i]);
    // Starting on the inner ring (the ring moved off a screen edge): that half.
    const pie::Hit start = hitAt(pt);
    m_latch.reset();
    m_latch.update(start, moveAllowed(), copyAllowed());
    updateSectors();
    qInfo().noquote() << u"Drop pie: shown for %1 %2"_s.arg(m_items.empty() ? u"shell items"_s : QString::number(m_items.size()))
                             .arg(m_items.empty() ? QString() : u"files"_s);
    setRevealed(true);
    return track(keys, pt);
}

DWORD DropPie::dragOver(DWORD keys, POINT pt, DWORD /*allowed: as at DragEnter*/)
{
    if (!m_open || !m_entered)
        return DROPEFFECT_NONE;
    return track(keys, pt);
}

void DropPie::dragLeave()
{
    if (m_open && !m_confirming)
        hide("the drag left it (Esc, or it went elsewhere)");
}

DWORD DropPie::drop(IDataObject* data, DWORD keys, POINT pt, DWORD /*allowed*/)
{
    if (!m_open || !m_entered)
        return DROPEFFECT_NONE;
    track(keys, pt);
    if (!m_open)
        return DROPEFFECT_NONE;
    if (m_picked < 0) {
        hide(m_hit.zone == pie::Zone::Dead ? "let go in the middle: nothing done" : "let go off the folders");
        return DROPEFFECT_NONE;
    }
    const Folder folder = m_folders[static_cast<std::size_t>(m_picked)];
    const bool move = m_latch.effective(m_ctrl, m_shift, moveAllowed(), copyAllowed(), true) == pie::Mode::Move;
    DWORD effect = DROPEFFECT_NONE;
    if (!m_items.empty()) {
        // Files: copied or moved here, by Windows' own file operation, on a
        // thread of its own. The source is told nothing was dropped, so it
        // deletes nothing itself (a "move" it would finish by deleting).
        const auto count = m_items.size();
        qInfo().noquote() << u"Drop pie: dropped, %1 %2 files"_s.arg(move ? u"moving"_s : u"copying"_s).arg(count);
        m_operations.start(m_items, folder.path.toStdWString(), move, [this, count](HRESULT hr, bool aborted) {
            QMetaObject::invokeMethod(this, [hr, aborted, count] {
                if (FAILED(hr))
                    qWarning().noquote() << u"Drop pie: the file operation for %1 files failed (0x%2)"_s.arg(count).arg(
                        static_cast<quint32>(hr), 8, 16, u'0');
                else
                    qInfo().noquote() << u"Drop pie: file operation done%1"_s.arg(aborted ? u", partly cancelled"_s : QString());
            }, Qt::QueuedConnection);
        });
    } else {
        // Shell items that are not files: the folder's own drop target,
        // as File Explorer hands them on. It starts the copy and returns.
        HRESULT hr = S_OK;
        effect = piedrop::dropOnFolder(folder.path.toStdWString(), data, move, pt, m_allowed, &hr);
        if (FAILED(hr))
            qWarning().noquote() << u"Drop pie: the folder did not take the shell items (0x%1)"_s.arg(static_cast<quint32>(hr), 8, 16, u'0');
        else
            qInfo().noquote() << u"Drop pie: dropped shell items, effect %1"_s.arg(effect);
    }
    // The folder shown taken a moment, then gone.
    m_confirming = true;
    m_releaseWait.stop();
    emit stateChanged();
    m_confirmTimer.start(150ms);
    return effect;
}

pie::Hit DropPie::hitAt(POINT pt) const
{
    if (!m_hwnd)
        return {};
    // From the window's own corner in physical pixels: Qt's rectangle times
    // the scale is off by a pixel at fractional scales.
    RECT rect {};
    ::GetWindowRect(m_hwnd, &rect);
    const qreal dpr = m_window ? m_window->devicePixelRatio() : 1;
    const QPointF local((pt.x - rect.left) / dpr, (pt.y - rect.top) / dpr);
    return pie::hitTest(local - m_centre, std::max<int>(1, static_cast<int>(m_folders.size())), m_radii);
}

DWORD DropPie::track(DWORD keys, POINT pt)
{
    const pie::Hit hit = hitAt(pt);
    if (hit.zone == pie::Zone::Outside) {
        hide("the pointer left the ring");
        return DROPEFFECT_NONE;
    }
    RECT rect {};
    ::GetWindowRect(m_hwnd, &rect);
    const qreal dpr = m_window ? m_window->devicePixelRatio() : 1;
    m_pointer = QPointF((pt.x - rect.left) / dpr, (pt.y - rect.top) / dpr);
    emit pointerChanged();

    const bool ctrl = keys & MK_CONTROL;
    const bool shift = keys & MK_SHIFT;
    m_latch.update(hit, moveAllowed(), copyAllowed());
    const int hovered = hit.zone == pie::Zone::Outer && !m_folders.empty() ? hit.sector : -1;
    const int picked = hovered >= 0 && m_blocks[static_cast<std::size_t>(hovered)] == Block::None ? hovered : -1;
    const bool changed = hit.zone != m_hit.zone || hit.half != m_hit.half || hovered != m_hovered || picked != m_picked
        || ctrl != m_ctrl || shift != m_shift;
    m_hit = hit;
    m_hovered = hovered;
    m_picked = picked;
    m_ctrl = ctrl;
    m_shift = shift;
    if (changed) {
        updateTexts();
        emit stateChanged();
    }
    if (picked < 0)
        return DROPEFFECT_NONE;
    switch (m_latch.effective(ctrl, shift, moveAllowed(), copyAllowed(), true)) {
    case pie::Mode::Move:
        return DROPEFFECT_MOVE & m_allowed;
    case pie::Mode::Copy:
        return DROPEFFECT_COPY & m_allowed;
    case pie::Mode::None:
        break;
    }
    return DROPEFFECT_NONE;
}

QString DropPie::mode() const
{
    const pie::Mode mode = m_latch.effective(m_ctrl, m_shift, moveAllowed(), copyAllowed(), m_picked >= 0);
    return mode == pie::Mode::Move ? u"move"_s : mode == pie::Mode::Copy ? u"copy"_s : QString();
}

QString DropPie::innerHover() const
{
    if (m_hit.zone != pie::Zone::Inner)
        return {};
    return m_hit.half == pie::Mode::Move ? u"move"_s : u"copy"_s;
}

void DropPie::updateTexts()
{
    const pie::Mode mode = m_latch.effective(m_ctrl, m_shift, moveAllowed(), copyAllowed(), true);
    const QString verb = mode == pie::Mode::Copy ? tr("复制到") : tr("移动到");
    QFont font = QGuiApplication::font();
    font.setPixelSize(std::max(12, m_labelPixelSize - 1));
    const QFontMetricsF metrics(font);
    m_tipVerb.clear();
    m_tip.clear();
    if (m_folders.empty()) {
        m_statusVerb.clear();
        m_statusName = tr("还没有收藏文件夹");
        m_statusDetail = tr("在 设置 → 收藏夹 里添加");
        return;
    }
    if (m_hovered >= 0) {
        const Folder& folder = m_folders[static_cast<std::size_t>(m_hovered)];
        const Block block = m_blocks[static_cast<std::size_t>(m_hovered)];
        if (block == Block::None) {
            m_tipVerb = verb;
            m_tip = folder.name;
            m_statusVerb = verb;
        } else {
            m_tip = reasonOf(block);
            m_statusVerb = reasonOf(block) + u" ·"_s;
        }
        m_statusName = folder.name;
        m_statusDetail = pielabels::elidePath(folder.path, metrics, m_statusWidth - 32);
        return;
    }
    if (m_hit.zone == pie::Zone::Inner) {
        if (m_hit.half == pie::Mode::Move && !moveAllowed())
            m_tip = tr("这里只能复制");
        else if (m_hit.half == pie::Mode::Copy && !copyAllowed())
            m_tip = tr("这里只能移动");
    }
    m_statusVerb.clear();
    m_statusName = tr("穿过内圈选移动或复制 · 松开在中间或按 Esc 取消");
    m_statusDetail.clear();
}

// ---- the folders -------------------------------------------------------------

void DropPie::gatherFolders()
{
    struct Candidate {
        QString path; // a folder's, or a place's command
        QString name;
        bool place = false;
    };
    std::vector<Candidate> candidates;
    const std::shared_ptr<const PlaceList> places = m_places ? m_places->places() : nullptr;
    for (const Favorite& f : m_favorites ? m_favorites->items() : FavoriteList()) {
        if (f.kind == Favorite::Kind::Folder) {
            candidates.push_back({normalized(f.path), f.name.isEmpty() ? pielabels::lastName(normalized(f.path)) : f.name});
        } else if (f.kind == Favorite::Kind::Place && places) {
            const auto place = std::ranges::find_if(*places, [&](const PlaceInfo& p) {
                return p.path().compare(f.path, Qt::CaseInsensitive) == 0;
            });
            if (place != places->end())
                candidates.push_back({place->command, f.name.isEmpty() ? place->name : f.name, true});
        }
    }
    // Places are looked up through the shell, and drives asked whether they
    // are on the network: on the worker.
    m_worker.post([this, candidates = std::move(candidates)] {
        std::vector<Folder> folders;
        for (const Candidate& c : candidates) {
            const QString path = c.place ? normalized(placeFolder(c.path)) : c.path;
            if (path.isEmpty())
                continue;
            folders.push_back({path, c.name, isNetworkPath(path.toStdWString())});
        }
        QMetaObject::invokeMethod(this, [this, folders = std::move(folders)]() mutable {
            DropPieText::setFolderCount(static_cast<int>(folders.size()));
            if (folders.size() > DropPieText::kMaxFolders)
                folders.resize(DropPieText::kMaxFolders);
            if (m_open)
                hide("the favorites changed");
            m_folders = std::move(folders);
            m_blocks.assign(m_folders.size(), Block::None);
            updateSectors();
        }, Qt::QueuedConnection);
    });
}

// Which local folders are gone, asked again at every shake: a folder may be
// deleted any time. Network ones never (a server that does not answer would
// hold this up for half a minute; its folder is taken as there).
void DropPie::checkFolders()
{
    std::vector<std::wstring> local;
    for (const Folder& f : m_folders) {
        if (!f.network)
            local.push_back(f.path.toStdWString());
    }
    const int check = ++m_checks;
    m_worker.post([this, check, local = std::move(local)] {
        QSet<QString> missing;
        for (const std::wstring& path : local) {
            const DWORD attributes = ::GetFileAttributesW(path.c_str());
            const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : ERROR_SUCCESS;
            const bool gone = attributes == INVALID_FILE_ATTRIBUTES
                ? error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_NOT_READY
                : !(attributes & FILE_ATTRIBUTE_DIRECTORY);
            if (gone)
                missing.insert(QString::fromStdWString(path).toCaseFolded());
        }
        QMetaObject::invokeMethod(this, [this, check, missing = std::move(missing)] {
            if (check != m_checks || missing == m_missing)
                return;
            m_missing = missing;
            if (!m_open || m_blocks.size() != m_folders.size())
                return;
            for (std::size_t i = 0; i < m_folders.size(); ++i)
                m_blocks[i] = blockOf(m_folders[i]);
            updateSectors();
            if (m_hovered >= 0 && m_blocks[static_cast<std::size_t>(m_hovered)] != Block::None)
                m_picked = -1;
            updateTexts();
            emit stateChanged();
        }, Qt::QueuedConnection);
    });
}

DropPie::Block DropPie::blockOf(const Folder& folder) const
{
    if (m_missing.contains(folder.path.toCaseFolded()))
        return Block::Missing;
    for (const std::wstring& item : m_items) {
        const QString path = normalized(QString::fromStdWString(item));
        if (parentOf(path).compare(folder.path, Qt::CaseInsensitive) == 0)
            return Block::Source;
        if (sameOrInside(folder.path, path))
            return Block::Inside;
    }
    // Shell items go to the folder's own drop target on this thread, which
    // must not wait on a server.
    if (m_entered && m_items.empty() && folder.network)
        return Block::Network;
    return Block::None;
}

QString DropPie::reasonOf(Block block) const
{
    switch (block) {
    case Block::Missing:
        return tr("这个文件夹已不存在");
    case Block::Source:
        return tr("就是它们所在的文件夹");
    case Block::Inside:
        return tr("不能放进它自己里面");
    case Block::Network:
        return tr("这类项目只能放进本机的文件夹");
    case Block::None:
        break;
    }
    return {};
}

void DropPie::updateSectors()
{
    const int step = DropPieText::uiStep();
    const qreal scale = pie::scaleForStep(step);
    if (scale != m_scale) {
        m_scale = scale;
        m_labelPixelSize = pie::labelPixelSize(step);
        m_radii = pie::Radii {}.scaled(m_scale);
        emit layoutChanged();
    }
    const int count = std::max<int>(1, static_cast<int>(m_folders.size()));
    const QFontMetricsF metrics(DropPieText::labelFont());
    QFont subFont = DropPieText::labelFont();
    subFont.setPixelSize(std::max(12, m_labelPixelSize - 1));
    const QFontMetricsF subMetrics(subFont);
    const double width = DropPieText::labelWidth(count);
    const auto item = [&](int index, const QString& name, const QString& path, const QString& sub, Block block) {
        const pielabels::Fitted fitted = pielabels::fitTwoLines(name, metrics, width);
        const QPointF at = pie::polar(m_radii.labelRadius(), pie::sectorCentre(index, count));
        return QVariantMap {{u"name"_s, name}, {u"path"_s, path}, {u"line1"_s, fitted.line1}, {u"line2"_s, fitted.line2},
            {u"sub"_s, sub.isEmpty() ? QString() : pielabels::elideMiddle(sub, subMetrics, width)},
            {u"disabled"_s, block != Block::None}, {u"reason"_s, reasonOf(block)}, {u"x"_s, at.x()}, {u"y"_s, at.y()}};
    };
    QVariantList items;
    if (m_folders.empty()) {
        items.append(item(0, tr("还没有收藏文件夹"), QString(), QString(), Block::Missing));
    } else {
        QStringList names;
        QStringList paths;
        for (const Folder& f : m_folders) {
            names.append(f.name);
            paths.append(f.path);
        }
        const QStringList hints = pielabels::parentHints(names, paths);
        for (std::size_t i = 0; i < m_folders.size(); ++i) {
            const Block block = i < m_blocks.size() ? m_blocks[i] : Block::None;
            const QString hint = hints.value(static_cast<qsizetype>(i));
            items.append(item(static_cast<int>(i), m_folders[i].name, m_folders[i].path,
                hint.isEmpty() ? QString() : tr("在 %1").arg(hint), block));
        }
    }
    if (items == m_sectorItems)
        return;
    m_sectorItems = std::move(items);
    emit sectorsChanged();
}

} // namespace ws
