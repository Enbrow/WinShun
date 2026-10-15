#include "App.h"

#include "AiBridge.h"
#include "AppCatalog.h"
#include "Backup.h"
#include "ClipStore.h"
#include "Clipboard.h"
#include "DialogBar.h"
#include "Favorites.h"
#include "FavoritesSettings.h"
#include "FileIconProvider.h"
#include "ThumbnailProvider.h"
#include "FolderMenu.h"
#include "History.h"
#include "Host.h"
#include "IndexFolder.h"
#include "IndexService.h"
#include "Launcher.h"
#include "McpServer.h"
#include "McpTools.h"
#include "Pipe.h"
#include "Placement.h"
#include "PluginSettings.h"
#include "SearchEngine.h"
#include "Secrets.h"
#include "ServiceVolumeAccess.h"
#include "SettingsEditor.h"
#include "SystemCatalog.h"
#include "SystemTheme.h"
#include "TextField.h"
#include "Tools.h"
#include "Unc.h"
#include "Updater.h"
#include "WindowFrame.h"
#include "WindowLogo.h"
#include "platform/ClipboardWatcher.h"
#include "platform/Credentials.h"
#include "platform/DialogJump.h"
#include "platform/Foreground.h"
#include "platform/IdleTrim.h"
#include "platform/Integrity.h"
#include "platform/KeyListener.h"
#include "platform/MessageWindow.h"
#include "platform/NetworkNotifier.h"
#include "platform/NumberKeys.h"
#include "platform/Paster.h"
#include "platform/Shell.h"
#include "platform/ShellContextMenu.h"
#include "platform/ShellMenu.h"
#include "platform/SiteIcons.h"
#include "platform/StartMenuTyping.h"
#include "platform/TaskbarSearch.h"
#include "platform/TextCaret.h"
#include "platform/VolumeNotifier.h"
#include "platform/WinV.h"
#include "platform/WindowEffects.h"

#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputMethod>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QStorageInfo>
#include <QStyleHints>
#include <QSurfaceFormat>
#include <QWinEventNotifier>

#include <windows.h>

#include <thread>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr int kHotkeyId = 1;
constexpr int kDialogJumpHotkeyId = 2; // Ctrl+G, while a file dialog is in front
constexpr int kWinVHotkeyId = 3; // Win+V: the clipboard history, once Explorer gave it up (winv::)
constexpr int kClipboardHotkeyId = 4; // another shortcut for the clipboard history
constexpr int kWinSHotkeyId = 5; // Win+S: the launcher over the taskbar, once Explorer gave it up (winv::)
constexpr int kScreenClipHotkeyId = 6; // Win+Shift+S, which Explorer gives up with Win+S: the screen capture
constexpr int kFolderMenuHotkeyId = 7; // the folder menu in the window in front (GitHub #25)
// For another program to tell where its text caret is: longer, and the
// clipboard would open late; it opens by the mouse pointer then.
constexpr auto kCaretWait = 100ms;
constexpr DWORD kSlowMenuMs = 100; // a tray menu slower than this to appear is logged

// The favorites' keywords (keyword, name), which the plugins' give way to.
QList<std::pair<QString, QString>> favoriteKeywords(const Favorites& favorites)
{
    QList<std::pair<QString, QString>> keywords;
    for (const Favorite& f : favorites.items()) {
        if (!f.keyword.isEmpty())
            keywords.append({f.keyword, f.name});
    }
    return keywords;
}

enum TrayCommand {
    ShowCommand = 1,
    IndexInfo,
    AccessInfo, // the index service wanted and not reached: walking meanwhile
    ContentInfo,
    RebuildCommand,
    AutostartCommand,
    SettingsCommand,
    UpdateCommand,
    CheckUpdateCommand,
    RestartCommand,
    QuitCommand,
    ClipboardCommand,
    PauseClipboardCommand
};

IndexService::Options indexOptions(const Settings& settings)
{
    IndexService::Options options;
    options.rules = settings.crawlRules();
    options.includeRemovable = settings.includeRemovableDrives;
    options.rescanOnStartup = settings.rescanOnStartup;
    options.content.enabled = settings.contentSearch && settings.contentIndex; // off: the index is cleared
    options.content.extensions = settings.contentExtensions;
    options.content.sizeLimits = settings.contentSizeLimits();
    options.content.includeLowPriority = settings.contentInLowPriority;
    options.content.documents = settings.contentDocuments;
    options.shares = settings.shareOptions();
    return options;
}

// The index service (GitHub #21) comes up a moment after the user's Win顺 at
// login: the index waits this long for it (IndexService::Options::accessWaitMs)
// rather than walk a volume whose journal position it has, looking every
// kServiceRetry meanwhile, and every kServiceRecheck after that (the service
// installed later, stopped and started again).
constexpr auto kServiceWait = 30s;
constexpr auto kServiceRetry = 2s;
constexpr auto kServiceRecheck = 5min;

// Where the index reads NTFS volumes from (Settings::indexAccess, GitHub
// #21): the volumes themselves when running as administrator (unless told
// service or walk), else the index service (WinShunService.exe) when it
// answers, else nowhere: the volumes are walked and watched. `wantsService`:
// the service would be used, so it is looked for again while it is not
// (App::startServiceRetry); a portable copy too, for one installed from its
// settings. WINSHUN_PIPE names another pipe, for tests with
// `WinShunService.exe --console --pipe NAME`.
struct AccessChoice {
    std::shared_ptr<ntfs::VolumeAccess> access;
    bool wantsService = false;
    QString why; // the service wanted and not used: why (English, from the pipe client)
};

AccessChoice chooseVolumeAccess(const QString& mode)
{
    AccessChoice choice;
    const bool elevated = integrity::processElevated();
    if (elevated && mode != u"service" && mode != u"walk") {
        choice.access = std::make_shared<ntfs::DirectVolumeAccess>();
        return choice;
    }
    if (mode == u"walk") {
        choice.access = std::make_shared<ntfs::NoVolumeAccess>();
        return choice;
    }
    // auto and service; direct too while not running as administrator (the
    // settings offer the restart), rather than nothing.
    choice.wantsService = true;
    auto service = std::make_shared<ntfs::ServiceVolumeAccess>();
    if (service->probe()) {
        choice.access = std::move(service);
        return choice;
    }
    choice.why = QString::fromStdWString(service->lastError());
    qWarning("WinShun: the index service is not used: %ls (pipe %ls)", service->lastError().c_str(),
        service->pipeName().c_str());
    choice.access = std::make_shared<ntfs::NoVolumeAccess>();
    return choice;
}

// '/' separated and cleaned, as IndexService keeps it.
QString folderPath(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool sameFolder(const QString& a, const QString& b)
{
    return folderPath(a).compare(folderPath(b), Qt::CaseInsensitive) == 0;
}

// "D:\" for "D:/...", else nothing.
QString driveRoot(const QString& folder)
{
    if (folder.size() < 3 || !folder[0].isLetter() || folder[1] != u':' || folder[2] != u'/')
        return {};
    return QDir::toNativeSeparators(folder.left(3));
}

UINT driveType(const QString& folder)
{
    const QString root = driveRoot(folder);
    return root.isEmpty() ? DRIVE_UNKNOWN : ::GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
}

bool isDarkMode()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

// Before create(): see-through where the window's background is (Theme.qml),
// for the Mica behind it. And it stays so: the window's color turns opaque
// while it is inactive, QQuickWindow::setColor() then takes the alpha out of
// the format, though the swap chain made with it keeps its alpha, and Qt's
// D3D11 backend logged a warning about that on every resize after.
void prepareBackdrop(QQuickWindow* window)
{
    if (!SystemTheme::backdropAvailable()) // the setting can turn it on later
        return;
    const auto keepAlpha = [window] {
        QSurfaceFormat format = window->requestedFormat();
        if (format.alphaBufferSize() < 8) {
            format.setAlphaBufferSize(8);
            window->setFormat(format);
        }
    };
    keepAlpha();
    QObject::connect(window, &QQuickWindow::colorChanged, window, keepAlpha);
}

// Where the user dragged Win顺's box on the taskbar: in state.ini, with the
// windows' spots.
QString statePath()
{
    return Settings::dataDir() + u"\\state.ini"_s;
}

taskbar::SearchBox::Spot loadBoxSpot()
{
    using Spot = taskbar::SearchBox::Spot;
    const QSettings state(statePath(), QSettings::IniFormat);
    const QString side = state.value(u"SearchBox/Side"_s).toString();
    Spot spot;
    spot.side = side == u"before" ? Spot::Side::Before
        : side == u"after"        ? Spot::Side::After
        : side == u"start"        ? Spot::Side::AfterStart
                                  : Spot::Side::Auto;
    spot.fromIcons = state.value(u"SearchBox/From"_s).toString() != u"end";
    spot.offset = std::clamp(state.value(u"SearchBox/Offset"_s).toInt(), 0, 10000);
    return spot;
}

void saveBoxSpot(const taskbar::SearchBox::Spot& spot)
{
    using Spot = taskbar::SearchBox::Spot;
    QSettings state(statePath(), QSettings::IniFormat);
    state.setValue(u"SearchBox/Side"_s, spot.side == Spot::Side::Before ? u"before"_s
                                         : spot.side == Spot::Side::After ? u"after"_s
                                         : spot.side == Spot::Side::AfterStart ? u"start"_s
                                                                          : u"auto"_s);
    state.setValue(u"SearchBox/From"_s, spot.fromIcons ? u"icons"_s : u"end"_s);
    state.setValue(u"SearchBox/Offset"_s, spot.offset);
}

// Rounded corners and the shadow, for our windows without a system title bar
// (the launcher, the clipboard, the bar by file dialogs, the settings
// window); each draws its edge itself (WindowEdge.qml).
void styleWindow(QWindow* window, bool backdrop = false)
{
    win::styleFramelessWindow(window, backdrop);
}

} // namespace

App::App()
{
    m_settingsReload.setSingleShot(true);
    m_settingsReload.setInterval(400ms); // editors often write a file in several steps
    connect(&m_settingsReload, &QTimer::timeout, this, &App::reloadSettings);
    m_indexOptionsApply.setSingleShot(true);
    m_indexOptionsApply.setInterval(1s);
    connect(&m_indexOptionsApply, &QTimer::timeout, this, [this] { m_index->setOptions(indexOptions(m_settings)); });
    m_revealTimeout.setSingleShot(true);
    // Never waited for longer: the first frame is up by then, and fresh rows
    // coming in a moment later (while it slides in) beat a launcher that comes late.
    m_revealTimeout.setInterval(60ms);
    connect(&m_revealTimeout, &QTimer::timeout, this, &App::revealLauncher);
    m_clipUncloakTimeout.setSingleShot(true);
    m_clipUncloakTimeout.setInterval(150ms); // should no frame come
    connect(&m_clipUncloakTimeout, &QTimer::timeout, this, [this] {
        m_clipUncloak = false;
        uncloakClipboard();
    });
    m_darkFrameGuard.setInterval(10ms);
    connect(&m_darkFrameGuard, &QTimer::timeout, this, [this] {
        if (m_darkFrameWatch.elapsed() < 50) // Qt's own, after some 5 ms, first
            return;
        for (QWindow* window : {static_cast<QWindow*>(m_window), static_cast<QWindow*>(m_settingsWindow),
                 static_cast<QWindow*>(m_barWindow), static_cast<QWindow*>(m_clipWindow)}) {
            if (window && win::isDarkFrame(window) != m_darkFrame)
                win::setDarkFrame(window, m_darkFrame);
        }
        if (m_darkFrameWatch.elapsed() > 1000)
            m_darkFrameGuard.stop();
    });
    m_contentStatusTimer.setInterval(2s);
    connect(&m_contentStatusTimer, &QTimer::timeout, this, &App::refreshContentIndexStatus);
    connect(&m_contentStatusTimer, &QTimer::timeout, this, &App::refreshShareStatuses); // counts while reading
    m_serviceRetry.setSingleShot(true); // serviceProbed() starts the next
    connect(&m_serviceRetry, &QTimer::timeout, this, &App::probeService);
}

App::~App()
{
    siteicons::shutdown(); // an icon arriving now would be told to no one
    m_keyRouter.reset(); // no keys taken from here on
    m_drainingRouter.reset();
    m_keyListener.reset(); // no double click told to the folder menu
    delete m_folderMenuWindow; // before its view-model and the QML engine
    m_folderMenu.reset();
    delete m_settingsWindow; // before the QML engine it was created with
    delete m_barWindow;
    delete m_clipWindow;
}

bool App::start(const StartOptions& options)
{
    const bool firstRun = !Settings::exists();
    // Updated from a version without the clipboard history: tell about it once.
    const bool clipboardIsNew = !firstRun && !Settings::hasClipboardSettings();
    m_settings.load();
    applyAutostartLevel(); // may set the mode (an old elevated task): before the index is made
    applyAppearance(); // before any window: the first frame is already in the chosen theme and language
    m_renderer = Settings::resolveRenderer(m_settings.renderer);
    if (m_renderer == u"software")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    else if (m_renderer == u"d3d11")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    // The software renderer draws a window with an alpha channel as a layered
    // window, which DWM puts no backdrop behind.
    SystemTheme::setBackdropAvailable(m_renderer == u"d3d11" && win::backdropSupported());
    if (SystemTheme::backdropAvailable()) {
        // Grey-scale text, not ClearType: a window see-through to Mica has
        // alpha 0 under its text, and subpixel glyphs blended there come out
        // with colour fringes, blue and orange the wrong way round (GitHub
        // #12). Qt draws them only where the ClearType tuner was run
        // (HKLM Avalon.Graphics\DISPLAY1 PixelStructure). For every window
        // and from the start: the transparency can be turned on at any
        // time, and the text items copy the application font when made.
        QFont font = QGuiApplication::font();
        font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::NoSubpixelAntialias));
        QGuiApplication::setFont(font);
    }

    const QString dataDir = Settings::dataDir();
    QDir().mkpath(dataDir);
    m_history = std::make_unique<History>(dataDir + u"\\history.txt"_s);
    m_history->load();
    // The dialog bar's pinned folders of old become favorites, once.
    m_favorites = std::make_unique<Favorites>(dataDir + u"\\favorites.json"_s, dataDir + u"\\dialog-pins.txt"_s);
    m_favorites->load();
    connect(m_favorites.get(), &Favorites::changed, this, [this] {
        if (m_pluginHost)
            m_pluginHost->setFavoriteKeywords(favoriteKeywords(*m_favorites));
    });
    // Sites' icons: fetched when a row of theirs is first shown, not before.
    siteicons::setFolder(dataDir + u"\\icons\\web"_s);
    IndexService::Options indexStart = indexOptions(m_settings);
    {
        AccessChoice choice = chooseVolumeAccess(m_settings.indexAccess);
        m_serviceWanted = choice.wantsService;
        m_serviceWhy = choice.why;
        if (m_serviceWanted && choice.access->kind() == ntfs::VolumeAccess::Kind::None)
            indexStart.accessWaitMs = static_cast<int>(std::chrono::milliseconds(kServiceWait).count()); // on its way, maybe
        indexStart.volumeAccess = std::move(choice.access);
    }
    m_index = std::make_unique<IndexService>(startIndexFolder(), std::move(indexStart));
    connect(m_index.get(), &IndexService::moveProgress, this, [this](int percent) {
        m_indexMoveProgress = percent;
        refreshIndexFolder();
    });
    connect(m_index.get(), &IndexService::moveFinished, this, &App::indexMoved);
    // Files still in the default place go where the settings say, once read.
    connect(m_index.get(), &IndexService::stateChanged, this, &App::applyIndexFolder);
    connect(m_index.get(), &IndexService::sharesChanged, this, &App::refreshShareStatuses);
    // Shares that could not be reached are tried again when the network changes.
    m_network = std::make_unique<NetworkNotifier>([index = m_index.get()] { index->shares().networkChanged(); });
    m_idleTrim = std::make_unique<IdleTrim>([this]() -> const char* {
        if (!m_index || m_index->state() != IndexService::State::Ready)
            return "loading";
        if (m_index->isRefreshing())
            return "catching up";
        if (m_index->readingContent())
            return "reading content";
        if (m_index->moving())
            return "moving";
        return nullptr;
    });
    // Loaded, caught up, done reading: emptied now, not at the next recheck.
    connect(m_index.get(), &IndexService::stateChanged, m_idleTrim.get(), &IdleTrim::poke);
    m_apps = std::make_unique<AppCatalog>();
    m_places = std::make_unique<SystemCatalog>();
    m_engine = std::make_unique<SearchEngine>(m_index.get(), m_apps.get(), m_places.get());
    m_launcher
        = std::make_unique<Launcher>(m_index.get(), m_apps.get(), m_places.get(), m_engine.get(), m_history.get());
    m_launcher->setFavorites(m_favorites.get());
    connect(m_launcher.get(), &Launcher::dismissRequested, this, &App::hideLauncher);
    connect(m_launcher.get(), &Launcher::openFailed, this, [this](const QString& name) {
        m_messages->showNotification(tr("没有打开“%1”").arg(name),
            integrity::processElevated()
                ? tr("找不到资源管理器，没法用普通权限打开它。请稍后再试，或按 Ctrl+Shift+Enter 以管理员身份运行。")
                : tr("Windows 没能打开它。请稍后再试，或按 Ctrl+Shift+Enter 以管理员身份运行。"));
    });
    connect(m_launcher.get(), &Launcher::namesShown, this, [this] {
        if (m_revealing)
            armReveal();
    });
    connect(m_launcher.get(), &Launcher::webSettingsRequested, this, &App::showWebSettings);
    connect(m_launcher.get(), &Launcher::fileTypeSettingsRequested, this, &App::showFileTypeSettings);
    connect(m_launcher.get(), &Launcher::favoriteSettingsRequested, this, &App::showFavoriteSettings);
    connect(m_launcher.get(), &Launcher::pluginSettingsRequested, this, &App::showPluginSettings);
    connect(m_launcher.get(), &Launcher::fieldReported, this, &App::mirrorField);
    connect(m_launcher.get(), &Launcher::scopeChanged, this, &App::updateSearchBoxLook); // the placeholder
    m_boxPressGrace.setSingleShot(true);
    m_boxPressGrace.setInterval(400ms);
    connect(&m_boxPressGrace, &QTimer::timeout, this, [this] {
        if (m_window && m_window->isVisible() && !m_window->isActive())
            hideLauncher();
    });
    // The footer's switches (the order, the start list): remembered.
    const auto remember = [this](QString Settings::* field, QString value) {
        if (m_settings.*field == value)
            return;
        m_settings.*field = std::move(value);
        m_settings.save();
        if (m_settingsEditor)
            m_settingsEditor->setSettings(m_settings);
    };
    connect(m_launcher.get(), &Launcher::sortChanged, this, [this, remember] {
        static constexpr QStringView kSorts[] {u"match", u"newest", u"oldest"};
        remember(&Settings::sort, kSorts[m_launcher->sort()].toString());
    });
    connect(m_launcher.get(), &Launcher::startListChanged, this, [this, remember] {
        static constexpr QStringView kLists[] {u"opened", u"modified", u"none"};
        remember(&Settings::startList, kLists[m_launcher->startList()].toString());
    });
    createPluginHost();

    // Clipboard history (Win+V), in clipboard\ next to the index.
    m_clipStore = std::make_unique<ClipStore>(dataDir + u"\\clipboard"_s);
    if (!m_clipStore->open(QDateTime::currentMSecsSinceEpoch()))
        qWarning() << "Clipboard history: cannot be kept";
    ClipboardWatcher::Callbacks clipCallbacks;
    // On the watcher's thread: hop to the GUI thread.
    clipCallbacks.captured = [this](ClipCapture capture) {
        auto copied = std::make_shared<ClipCapture>(std::move(capture));
        QMetaObject::invokeMethod(this, [this, copied] { clipCaptured(*copied); }, Qt::QueuedConnection);
    };
    clipCallbacks.reused = [this](qint64 id) { // pasted from the history: it moves up
        QMetaObject::invokeMethod(this, [this, id] {
            m_clipStore->touch(id, QDateTime::currentMSecsSinceEpoch());
            m_clipboard->historyChanged();
        }, Qt::QueuedConnection);
    };
    m_clipWatcher = std::make_unique<ClipboardWatcher>(clipboardOptions(), std::move(clipCallbacks));
    m_clipboard = std::make_unique<Clipboard>(m_clipStore.get(), m_clipWatcher.get());
    // Pinned, it stays up through a paste or a file opened (hideClipboard);
    // Esc hands the focus back too.
    connect(m_clipboard.get(), &Clipboard::dismissRequested, this, [this] { hideClipboard(); });
    connect(m_clipboard.get(), &Clipboard::keysReleased, this, [this] {
        if (m_clipWindow && m_clipWindow->isVisible())
            releaseClipboard(true);
    });
    connect(m_clipboard.get(), &Clipboard::pinnedChanged, this, &App::clipboardPinnedChanged);
    connect(m_clipboard.get(), &Clipboard::keysWanted, this, &App::engageClipboard);
    connect(m_clipboard.get(), &Clipboard::focusNeeded, this, &App::activateClipboard);
    // A group's name is typed in the window itself, Chinese with the input
    // method, which only works in the window with the focus.
    connect(m_clipboard.get(), &Clipboard::groupNameRequested, this, &App::activateClipboard);
    connect(m_clipboard.get(), &Clipboard::fieldRequested, this, &App::returnToField);
    connect(m_clipboard.get(), &Clipboard::turnOnRequested, this, [this] {
        Settings settings = m_settings;
        settings.clipboard = true;
        settings.save();
        settingsEdited(settings);
        if (m_settingsEditor)
            m_settingsEditor->setSettings(m_settings);
    });

    m_updater = std::make_unique<Updater>();
    m_backup = std::make_unique<Backup>();
    connect(m_backup.get(), &Backup::restartRequested, this, [this] { restart({u"--settings"_s}); });
    m_updater->setChinese(m_language == u"zh");
    connect(m_updater.get(), &Updater::found, this, [this](bool manual) {
        // Asked for, or the settings window is open: show it there. Otherwise
        // a tray notification, which does not take the keyboard from anyone.
        if (manual || (m_settingsWindow && m_settingsWindow->isVisible())) {
            showUpdate();
            return;
        }
        m_messages->showNotification(tr("Win顺 %1 可以更新了").arg(m_updater->availableVersion()),
            tr("点这里查看更新内容"));
        m_updateNotified = true;
    });

    if (!createWindow())
        return false;

    MessageWindow::Callbacks callbacks;
    callbacks.trayActivated = [this] { toggleLauncher(); };
    callbacks.trayMenuRequested = [this] { showTrayMenu(); };
    callbacks.notificationClicked = [this] {
        if (std::exchange(m_updateNotified, false))
            showUpdate();
        else if (std::exchange(m_clipboardNotified, false))
            showClipboardSettings();
    };
    callbacks.hotkeyPressed = [this](int id) {
        if (id == kWinVHotkeyId || id == kClipboardHotkeyId)
            toggleClipboard();
        else if (id == kWinSHotkeyId)
            toggleAtTaskbar(false);
        else if (id == kScreenClipHotkeyId)
            startScreenClip();
        else if (id == kFolderMenuHotkeyId)
            folderMenuHotkey();
        else if (id != kDialogJumpHotkeyId)
            toggleLauncher();
        else if (m_dialogJump && m_settings.dialogJump)
            m_dialogJump->jump();
    };
    callbacks.shellRestarted = [this] { // Explorer let go of Win+V or Win+S, or took them back
        applyClipboardHotkeys();
        applyTaskbarHotkeys();
        if (m_searchBox)
            m_searchBox->reattach(); // onto the new taskbar
        if (m_startGap)
            m_startGap->reattach(); // the new Explorer has no gap yet
        QTimer::singleShot(3s, this, [this] { // once it has registered its own keys
            applyClipboardHotkeys();
            applyTaskbarHotkeys();
        });
    };
    callbacks.commandReceived = [this](const QString& command) { handleCommand(command); };
    // Explorer's "search here" (WinShunSearch.exe): the folder typed, words to come.
    callbacks.searchHere = [this](const QString& folder) {
        showLauncher(folder.endsWith(u'\\') ? folder + u' ' : folder + u"\\ "_s);
    };
    // Directory Opus's double click on blank space (WinShunSearch.exe, put
    // into Opus's settings by SettingsEditor::setOpusFolderMenu). Not with
    // the double click turned off: the menu may be there for its hotkey alone.
    callbacks.folderMenu = [this](const QString& folder) {
        if (m_folderMenu && m_settings.folderMenu != u"off")
            m_folderMenu->requested(folder);
    };
    // Windows ends us right after this returns, or a few seconds in: the
    // clipboard history and the index saved first. Past those seconds its
    // "preventing shutdown" screen says why rather than a bare name.
    callbacks.sessionEnding = [this] {
        const HWND window = m_messages ? m_messages->hwnd() : nullptr;
        if (window)
            ::ShutdownBlockReasonCreate(window, reinterpret_cast<LPCWSTR>(tr("正在保存文件索引").utf16()));
        if (m_clipStore)
            m_clipStore->flush(); // what was copied just now
        m_index->shutdown();
        if (window)
            ::ShutdownBlockReasonDestroy(window);
    };
    callbacks.deviceChange = [this](WPARAM event, LPARAM data) -> LRESULT {
        return m_volumeNotifier ? m_volumeNotifier->handle(event, data) : TRUE;
    };
    // Once the disks have settled: after hibernating, the file cache is gone.
    callbacks.resumed = [this] {
        m_index->prewarmSoon(30s);
        m_index->shares().networkChanged(); // the servers may have gone, or come back, meanwhile
    };
    m_messages = std::make_unique<MessageWindow>(std::move(callbacks));

    // Drives: let go of one being ejected or locked; index new ones, drop gone ones.
    VolumeNotifier::Callbacks volumeCallbacks;
    volumeCallbacks.releaseRequested = [this](const std::wstring& root) { m_index->suspendVolume(root); };
    volumeCallbacks.released = [this](const std::wstring& root) { m_index->resumeVolume(root); };
    volumeCallbacks.volumesChanged = [this] { m_index->refreshVolumes(); };
    m_volumeNotifier = std::make_unique<VolumeNotifier>(m_messages->hwnd(), std::move(volumeCallbacks));
    connect(m_index.get(), &IndexService::volumesChanged, this,
        [this] { m_volumeNotifier->track(m_index->volumeRoots()); });
    m_messages->showTrayIcon(trayTooltip());

    // The button on the taskbar (WinShunSearch.exe) sets it: over the taskbar,
    // or away again. Set before Win顺 was up (it started Win顺), it opens now.
    m_taskbarEvent = win32::UniqueHandle(taskbar::createEvent());
    if (m_taskbarEvent.valid()) {
        m_taskbarNotifier = std::make_unique<QWinEventNotifier>(m_taskbarEvent.get());
        connect(m_taskbarNotifier.get(), &QWinEventNotifier::activated, this, [this] { toggleAtTaskbar(true); });
    } else {
        qWarning() << "No event for the taskbar button:" << ::GetLastError();
    }

    applySettings(true);
    // The launcher's strip of kinds of file as its user left it (after
    // applySettings: the kinds are known by then).
    m_launcher->setFileChoice(QSettings(statePath(), QSettings::IniFormat).value(u"Search/FileType"_s).toString());
    connect(m_launcher.get(), &Launcher::fileChoiceChanged, this, [this] {
        QSettings(statePath(), QSettings::IniFormat).setValue(u"Search/FileType"_s, m_launcher->fileChoice());
    });
    applyTheme();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &App::applyTheme);

    m_settingsWatcher.addPath(Settings::filePath());
    connect(&m_settingsWatcher, &QFileSystemWatcher::fileChanged, this, [this] { m_settingsReload.start(); });

    m_index->start();
    if (m_serviceWanted) {
        if (m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None)
            startServiceRetry(); // the index waits for it meanwhile (accessWaitMs)
        else
            m_serviceRetry.start(kServiceRecheck); // in use: looked at now and then, in case it goes away
    }
    m_apps->refresh(true); // in the background; about half a second
    m_places->refresh(true); // likewise

    autostart::adoptIfOrphaned();
    if (firstRun) {
        // Autostart is on by default; the settings switch turns it off for good
        // (the settings file exists from now on, so this runs once).
        autostart::setEnabled(true);
        // A portable copy walks the folders (no UAC, like Everything's): the
        // settings offer the index service and running as administrator.
        const bool walking = !Settings::portableFolder().isEmpty()
            && m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None;
        m_messages->showNotification(tr("Win顺已在后台运行"),
            walking ? tr("双击 Ctrl 打开搜索。便携版按文件夹遍历建立索引；要快一些，可以在设置里安装索引服务，或以管理员身份运行。")
                    : tr("双击 Ctrl 打开搜索。首次运行需要一点时间建立文件索引。"));
    } else if (clipboardIsNew) {
        m_messages->showNotification(tr("Win顺 现在有剪贴板历史了"),
            m_settings.clipboard ? tr("复制过的文字、图片和文件都能找回来。点这里设置用 Win+V 打开它。")
                                 : tr("复制过的文字、图片和文件都能找回来。点这里打开它。"));
        m_updateNotified = false;
        m_clipboardNotified = true;
    } else if (!m_indexFolderProblem.isEmpty()) {
        m_messages->showNotification(tr("索引位置"), m_indexFolderProblem);
    }
    if (options.settings) {
        showSettings();
        if (m_settingsWindow && !options.settingsAt.isEmpty())
            QMetaObject::invokeMethod(m_settingsWindow, "showPlace", Q_ARG(QVariant, QVariant(options.settingsAt)));
    } else if (!options.background || !options.query.isEmpty())
        showLauncher(options.query);
    QTimer::singleShot(1s, this, &App::prewarmLauncher);
    QTimer::singleShot(1500ms, this, &App::prewarmClipboard);
    return true;
}

bool App::createWindow()
{
    m_qml = std::make_unique<QQmlApplicationEngine>();
    m_qml->addImageProvider(u"fileicon"_s, new FileIconProvider); // engine takes ownership
    m_qml->addImageProvider(u"thumbnail"_s, new ThumbnailProvider); // result rows' pictures, videos, PDFs
    m_placement = std::make_unique<Placement>(Settings::dataDir() + u"\\state.ini"_s, u"Launcher"_s);
    m_frame = std::make_unique<WindowFrame>(false);
    m_qml->setInitialProperties({
        {u"launcher"_s, QVariant::fromValue(m_launcher.get())},
        {u"clipboard"_s, QVariant::fromValue(m_clipboard.get())},
        {u"placement"_s, QVariant::fromValue(m_placement.get())},
        {u"frame"_s, QVariant::fromValue(m_frame.get())},
    });
    m_qml->loadFromModule(u"WinShun"_s, u"Main"_s);
    const auto roots = m_qml->rootObjects();
    if (roots.isEmpty())
        return false;
    m_window = qobject_cast<QQuickWindow*>(roots.constFirst());
    if (!m_window)
        return false;

    prepareBackdrop(m_window);
    m_window->create(); // native handle now, so DWM styling applies before the first show
    m_window->installEventFilter(this);
    shellcontext::watchKeys(reinterpret_cast<HWND>(m_window->winId())); // which key asks for a context menu (eventFilter)
    m_launcher->setWindow(m_window);
    m_placement->setWindow(m_window);
    m_frame->setWindow(m_window);
    m_launcherLogo = new WindowLogo(m_window);
    m_launcherLogo->follow(m_placement.get());
    m_searchField = m_window->findChild<QQuickItem*>(u"searchField"_s);
    if (m_searchField)
        m_searchField->installEventFilter(this); // the input method's questions, while it is in the box
    connect(m_window, &QWindow::activeChanged, this, [this] {
        // Clicking elsewhere or switching apps dismisses the launcher, like a
        // menu; the clipboard under its search box goes with it (see there).
        // Not a press on Win顺's box on the taskbar: that brings it back. Nor
        // the press that just brought it back: Qt may hear of the taskbar
        // taking the focus for it only after that (it slid back up while on
        // its way out). Those leave no window in front (or the taskbar, the
        // pointer on the box), nor does the Start menu closing as it opens
        // over it; another window in front (a click on it, on the desktop,
        // on the rest of the taskbar) dismisses it at once, however soon
        // after it opened.
        if (m_window && !m_window->isActive() && m_window->isVisible() && !clipboardUnderLauncher()) {
            // A command of the shell's menu that keeps the launcher (删除,
            // say) runs, or just did: its dialogs, owned by a hidden window
            // of ours, take the focus and hand it back (Launcher::
            // runSystemMenu); another program's window ends that.
            if (m_launcher->systemCommandRunning()) {
                DWORD pid = 0;
                ::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
                if (pid == ::GetCurrentProcessId())
                    return;
                m_launcher->systemCommandDone();
            }
            const bool justShown = m_launcherShown.isValid() && m_launcherShown.elapsed() < 500;
            const bool onBox = m_searchBox && m_searchBox->underPointer();
            const HWND front = ::GetForegroundWindow();
            const bool elsewhere = front && front != reinterpret_cast<HWND>(m_window->winId())
                && !(onBox && taskbar::isTaskbar(front)) && !win::isShellFlyout(front);
            if (m_searchBox && m_launcherOverBox && !elsewhere && (justShown || onBox))
                m_boxPressGrace.start();
            else
                hideLauncher();
        } else if (m_window && m_window->isActive()) {
            m_launcher->systemCommandDone();
            m_boxPressGrace.stop();
            deliverTyping(); // asked for from the Start menu
        }
        updateNumberKeys();
    });
    // Reveal (see showLauncher). Both run on the render thread: a frame
    // synchronised after arming shows the fresh results, and once it has been
    // handed to the screen the window can be uncloaked.
    connect(m_window, &QQuickWindow::afterSynchronizing, this, [this] {
        if (m_revealArmed.load())
            m_revealSynced.store(true);
    }, Qt::DirectConnection);
    connect(m_window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_revealSynced.exchange(false))
            QMetaObject::invokeMethod(this, &App::revealLauncher, Qt::QueuedConnection);
        if (m_prewarming.load())
            QMetaObject::invokeMethod(this, &App::finishPrewarm, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    return true;
}

// The clipboard's own window (ClipboardWindow.qml): not the launcher's, so
// that it can drop down under any search box of ours, and be as wide as it
// needs.
bool App::createClipWindow()
{
    m_clipPlacement = std::make_unique<Placement>(Settings::dataDir() + u"\\state.ini"_s, u"Clipboard"_s);
    m_textCaret = std::make_unique<win::TextCaret>();
    m_clipFrame = std::make_unique<WindowFrame>(false);
    QQmlComponent component(m_qml.get(), u"WinShun"_s, u"ClipboardWindow"_s);
    QObject* object = component.createWithInitialProperties({
        {u"clipboard"_s, QVariant::fromValue(m_clipboard.get())},
        {u"placement"_s, QVariant::fromValue(m_clipPlacement.get())},
        {u"frame"_s, QVariant::fromValue(m_clipFrame.get())},
    });
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        qWarning().noquote() << component.errorString();
        delete object;
        return false;
    }
    m_clipWindow = window;
    prepareBackdrop(window);
    window->create();
    window->installEventFilter(this); // the Menu key, pasting into its search box
    styleWindow(window, SystemTheme::backdropAvailable());
    win::setDarkFrame(window, isDarkMode());
    m_clipboard->setWindow(window);
    m_clipPlacement->setWindow(window);
    m_clipFrame->setWindow(window);
    m_clipLogo = new WindowLogo(window);
    connect(window, &QWindow::activeChanged, this, [this] {
        // Clicking elsewhere or switching apps hides it, like a menu. Not at
        // once: on the way to another window of ours (the field it goes back
        // to), the focus may first go to none.
        if (m_clipWindow && !m_clipWindow->isActive())
            QMetaObject::invokeMethod(this, [this] {
                if (m_clipWindow && !m_clipWindow->isActive())
                    hideClipboard();
            }, Qt::QueuedConnection);
    });
    connect(window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_clipUncloak.exchange(false))
            QMetaObject::invokeMethod(this, &App::uncloakClipboard, Qt::QueuedConnection);
        if (m_clipPrewarming.load())
            QMetaObject::invokeMethod(this, &App::finishClipPrewarm, Qt::QueuedConnection);
    }, Qt::DirectConnection); // on the render thread
    updateCompanions();
    return true;
}

bool App::eventFilter(QObject* watched, QEvent* event)
{
    // The launcher's field, drawn in the box on the taskbar: the input
    // method's window goes by the caret there.
    if (watched == m_searchField && event->type() == QEvent::InputMethodQuery && m_launcher->inTaskbarBox()) {
        auto* query = static_cast<QInputMethodQueryEvent*>(event);
        const Qt::InputMethodQueries rects = query->queries() & (Qt::ImCursorRectangle | Qt::ImAnchorRectangle);
        if (const std::optional<QRectF> caret = rects ? boxCaretInField() : std::nullopt) {
            static_cast<QObject*>(m_searchField)->event(event); // the field's own answers to the rest
            if (rects & Qt::ImCursorRectangle)
                query->setValue(Qt::ImCursorRectangle, *caret);
            if (rects & Qt::ImAnchorRectangle)
                query->setValue(Qt::ImAnchorRectangle, *caret);
            return true;
        }
    }
    // Pasted into a one-line field (the search boxes, the settings' fields):
    // several lines go in as one. Before the field sees the keys.
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->matches(QKeySequence::Paste)) {
        if (auto* window = qobject_cast<QQuickWindow*>(watched); window && textfield::pasteOneLine(window->activeFocusItem()))
            return true;
    }
    if (watched == m_window && event->type() == QEvent::ContextMenu
        && static_cast<QContextMenuEvent*>(event)->reason() == QContextMenuEvent::Keyboard) {
        // Shift+Menu and Ctrl+Shift+F10 ask for the shell's menu, the Menu
        // key and Shift+F10 for Win顺's. Main.qml takes the chords as keys
        // when Qt hands them on as such; an IME in its Chinese mode keeps
        // the press for itself, and only this event (from the release)
        // tells, so the key is looked up (shellcontext::watchKeys). Not
        // when the shell's menu is up or about to be (opened from the key):
        // that is the release's event, and the menu has the keyboard.
        const Qt::KeyboardModifiers modifiers = static_cast<QContextMenuEvent*>(event)->modifiers();
        const UINT key = shellcontext::lastKeyPressed();
        const bool system = (modifiers & Qt::ShiftModifier)
            && (key == VK_APPS || (key == VK_F10 && (modifiers & Qt::ControlModifier)));
        if (!m_launcher->systemMenuBusy())
            emit m_launcher->contextMenuKeyPressed(system);
        return true;
    }
    if (watched == m_clipWindow && event->type() == QEvent::ContextMenu
        && static_cast<QContextMenuEvent*>(event)->reason() == QContextMenuEvent::Keyboard) {
        emit m_clipboard->contextMenuKeyPressed();
        return true;
    }
    if (watched == m_barWindow && m_dialogBar && event->type() == QEvent::ContextMenu
        && static_cast<QContextMenuEvent*>(event)->reason() == QContextMenuEvent::Keyboard) {
        emit m_dialogBar->contextMenuKeyPressed();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

void App::applyTheme()
{
    const bool dark = isDarkMode();
    win::setMenuTheme(dark);
    if (m_pluginHost)
        m_pluginHost->setAppearance(m_language, dark ? u"dark"_s : u"light"_s);
    if (m_window)
        styleWindow(m_window, SystemTheme::backdropAvailable());
    if (m_clipWindow)
        styleWindow(m_clipWindow, SystemTheme::backdropAvailable());
    if (m_settingsWindow)
        styleWindow(m_settingsWindow, SystemTheme::backdropAvailable());
    if (m_barWindow)
        styleWindow(m_barWindow);
    updateSearchBoxLook(); // the accent
    // The frames' dark mode only once Qt has set its own (light) one, a few
    // milliseconds later; then watch them for a while (see win::setDarkFrame).
    m_darkFrame = dark;
    m_darkFrameWatch.start();
    m_darkFrameGuard.start();
}

// Theme and language take effect at once, in every window, without a restart.
void App::applyAppearance()
{
    // 界面大小: QML follows SystemTheme; the bar by file dialogs is placed by its heights.
    if (SystemTheme::uiSize() != m_settings.uiSize) {
        SystemTheme::setUiSize(m_settings.uiSize);
        DialogBar::setUiSize(m_settings.uiSize);
        if (m_clipPlacement)
            m_clipPlacement->resized();
    }
    SystemTheme::setBackdropOn(m_settings.transparency == u"on"); // Mica stays set up behind, covered when off
    // Unknown follows Windows. Everything else reads the scheme from the style
    // hints and listens to colorSchemeChanged (see applyTheme, SystemTheme).
    QGuiApplication::styleHints()->setColorScheme(m_settings.theme == u"dark" ? Qt::ColorScheme::Dark
            : m_settings.theme == u"light"                                     ? Qt::ColorScheme::Light
                                                                               : Qt::ColorScheme::Unknown);

    const QString language = Settings::resolveLanguage(m_settings.language);
    if (language == m_language)
        return;
    m_language = language;
    QCoreApplication::removeTranslator(&m_translator);
    if (language == u"en") {
        if (m_translator.load(u":/i18n/winshun_en.qm"_s))
            QCoreApplication::installTranslator(&m_translator);
        else
            qWarning() << "No English translation in the resources";
    }
    // Numbers and sizes ("1,234", "172 MB") the language's way.
    QLocale::setDefault(language == u"en" ? QLocale(QLocale::English, QLocale::UnitedStates)
                                          : QLocale(QLocale::Chinese, QLocale::China));
    QGuiApplication::setApplicationDisplayName(tr("Win顺"));
    if (m_updater)
        m_updater->setChinese(language == u"zh");

    if (m_pluginHost)
        m_pluginHost->setAppearance(language, isDarkMode() ? u"dark"_s : u"light"_s);

    if (!m_qml)
        return; // starting up: nothing shown yet
    m_qml->retranslate();
    m_launcher->retranslate();
    updateSearchBoxLook();
    m_clipboard->retranslate();
    if (m_settingsEditor)
        m_settingsEditor->plugins()->setLocale(language); // the cards' names and texts
    m_messages->setTrayTooltip(trayTooltip());
    applyHotkey(); // its error message
    applyFolderMenuHotkey();
    refreshContentIndexStatus();
}

QString App::trayTooltip() const
{
    return m_settings.doubleCtrl ? tr("Win顺 — %1 打开").arg(doubleCtrlName()) : tr("Win顺");
}

// "双击 Ctrl", "双击左 Ctrl": the keys as set.
QString App::doubleCtrlName() const
{
    if (m_settings.doubleCtrlKey == u"left")
        return tr("双击左 Ctrl");
    if (m_settings.doubleCtrlKey == u"right")
        return tr("双击右 Ctrl");
    return tr("双击 Ctrl");
}

void App::applySettings(bool initial)
{
    applyFolderMenu(); // before the listener, which tells it double clicks
    // Double clicks: for File Explorer and the desktop; Directory Opus says
    // so itself. Not when the menu is there for its hotkey alone.
    const bool doubleClicks = m_folderMenu && m_settings.folderMenu != u"off"
        && (m_settings.folderMenuExplorer || m_settings.folderMenuDesktop);
    const int listenTo = (m_settings.doubleCtrl ? 1 : 0) | (doubleClicks ? 2 : 0);
    if (listenTo != m_keyListenerMode) {
        m_keyListener.reset(); // the process's one mouse registration goes with it first
        m_keyListenerMode = listenTo;
        if (listenTo != 0) {
            // Called on the listener thread: hop to the GUI thread.
            KeyListener::Callbacks callbacks;
            if (m_settings.doubleCtrl)
                callbacks.doubleCtrl = [this](const std::string& sent) {
                    // A program made the taps: which keys came, for "it opens by itself".
                    // A few times a run (a key remapped to Ctrl makes every one).
                    static std::atomic<int> logged {0};
                    if (!sent.empty() && logged.fetch_add(1) < 10)
                        qWarning().noquote() << "Double Ctrl partly sent by a program:" << QString::fromStdString(sent);
                    QMetaObject::invokeMethod(this, &App::onDoubleCtrl, Qt::QueuedConnection);
                };
            if (FolderMenu* menu = doubleClicks ? m_folderMenu.get() : nullptr)
                callbacks.doubleClick = [menu](HWND window, POINT pt) { menu->doubleClicked(window, pt); };
            m_keyListener = std::make_unique<KeyListener>(std::move(callbacks));
        }
    }
    if (m_keyListener)
        m_keyListener->setSide(m_settings.doubleCtrlKey == u"left" ? DoubleTapDetector::Side::Left
                : m_settings.doubleCtrlKey == u"right"            ? DoubleTapDetector::Side::Right
                                                                  : DoubleTapDetector::Side::Both);
    applyDialogs();
    applyClipboard();
    applyTaskbar();
    // The tray's icon and the taskbar box's: Win顺's colours, or the taskbar's one.
    m_messages->setTrayIconMono(m_settings.taskbarIcon == u"mono");
    m_messages->setTrayTooltip(trayTooltip()); // which Ctrl
    updateSearchBoxLook();
    applyMcp();
    filemanager::setCustomCommands(m_settings.fileManagerOpenCommand, m_settings.fileManagerRevealCommand);
    for (const auto& [kind, name] : filemanager::kSettingNames) {
        if (m_settings.fileManager == QLatin1StringView(name))
            shell::setFileManager(kind);
    }

    if (!initial)
        applyAppearance();
    shellmenu::apply(m_settings.explorerMenu, tr("用 Win顺 搜索此处")); // after the language
    m_updater->setAutomatic(m_settings.autoUpdate);
    m_backup->setFolder(m_settings.backupFolder);
    m_launcher->setRecordHistory(m_settings.recordHistory);
    m_launcher->setResultOptions(m_settings.foldersFirst ? KindOrder::FoldersFirst : KindOrder::FilesFirst,
        m_settings.showModified, m_settings.thumbnails, m_settings.numberKeys);
    m_launcher->setSort(m_settings.sort == u"newest" ? Launcher::NewestFirst
            : m_settings.sort == u"oldest"           ? Launcher::OldestFirst
                                                     : Launcher::BestMatch);
    m_launcher->setStartList(m_settings.startList == u"modified" ? Launcher::RecentlyModified
            : m_settings.startList == u"none"                     ? Launcher::NoStartList
                                                                 : Launcher::RecentlyOpened);
    m_clipboard->setNumberKeys(m_settings.clipboardNumberKeys);
    updateNumberKeys();
    applyHotkey();
    applyFolderMenuHotkey(); // after applyFolderMenu: the menu it brings up
    m_launcher->setContentOptions(m_settings.contentSearch, m_settings.contentExtensions,
        m_settings.contentSizeLimits(), m_settings.contentInLowPriority, m_settings.contentDocuments);
    m_launcher->setWebShortcuts(m_settings.webShortcuts);
    m_launcher->setFileTypes(m_settings.namedFileTypes()); // after applyAppearance: the built-in ones' names
    applyPlugins();
    if (!initial) {
        m_indexOptionsApply.start();
        applyIndexFolder();
    }
}

void App::applyHotkey()
{
    m_messages->unregisterHotkey(kHotkeyId);
    if (m_settingsEditor && m_settingsEditor->recordingHotkey())
        return; // the keys being pressed must reach the settings window
    const QString hotkey = m_settings.hotkey.trimmed();
    QString hotkeyError;
    if (!hotkey.isEmpty() && !m_messages->registerHotkey(kHotkeyId, hotkey)) {
        UINT modifiers = 0;
        UINT vk = 0;
        hotkeyError = MessageWindow::parseHotkey(hotkey, &modifiers, &vk)
            ? tr("“%1” 已被其他程序或系统占用，请换一个。").arg(hotkey)
            : tr("无法识别“%1”，请重新设置。").arg(hotkey);
    }
    if (m_settingsEditor)
        m_settingsEditor->setHotkeyError(hotkeyError); // shown next to the shortcut
    else if (!hotkeyError.isEmpty())
        m_messages->showNotification(tr("快捷键不可用"), hotkeyError);
}

// The folder menu's hotkey (GitHub #25). Over the launcher itself, the
// launcher goes first (as on Esc) and the menu is for the window Windows
// brings to the front after it: a window of ours in front could not have
// the keys taken for the menu (keyboard Raw Input, see KeyListener), and
// the user pressed the menu's key, not the launcher's. The launcher slides
// out: the menu waits for it to be gone.
void App::folderMenuHotkey()
{
    if (!m_folderMenu)
        return;
    if (m_folderMenu->isOpen() || !launcherShown() || !m_window->isActive()) {
        m_folderMenu->hotkeyPressed();
        return;
    }
    hideLauncher();
    auto wait = std::make_shared<std::function<void(int)>>();
    *wait = [this, wait](int left) {
        if (!m_folderMenu)
            return;
        if (m_window && m_window->isVisible() && left > 0) {
            QTimer::singleShot(30ms, this, [wait, left] { (*wait)(left - 1); });
            return;
        }
        m_folderMenu->hotkeyPressed();
    };
    (*wait)(20);
}

void App::applyFolderMenuHotkey()
{
    m_messages->unregisterHotkey(kFolderMenuHotkeyId);
    if (m_settingsEditor && m_settingsEditor->recordingHotkey())
        return; // the keys being pressed must reach the settings window
    const QString hotkey = m_settings.folderMenuHotkey.trimmed();
    QString hotkeyError;
    if (!hotkey.isEmpty() && !m_messages->registerHotkey(kFolderMenuHotkeyId, hotkey)) {
        UINT modifiers = 0;
        UINT vk = 0;
        hotkeyError = MessageWindow::parseHotkey(hotkey, &modifiers, &vk)
            ? tr("“%1” 已被其他程序或系统占用，请换一个。").arg(hotkey)
            : tr("无法识别“%1”，请重新设置。").arg(hotkey);
    }
    if (m_settingsEditor)
        m_settingsEditor->setFolderMenuHotkeyError(hotkeyError); // shown next to the shortcut
    else if (!hotkeyError.isEmpty())
        m_messages->showNotification(tr("快捷键不可用"), hotkeyError);
}

ClipboardWatcher::Options App::clipboardOptions() const
{
    return {m_settings.clipboard && !m_clipboardPaused, m_settings.clipboardImages, m_settings.clipboardExcludedApps};
}

void App::applyClipboard()
{
    // Explorer gives Win+V up (or gets it back) when it next starts. Only
    // when the switch moved, or it is on at start: someone else may have put
    // a V there for another program.
    if (m_winVApplied ? *m_winVApplied != m_settings.clipboardWinV : m_settings.clipboardWinV)
        m_winVRegistryFailed = !winv::setReleasedByExplorer(L'V', m_settings.clipboardWinV);
    m_winVApplied = m_settings.clipboardWinV;
    m_clipWatcher->setOptions(clipboardOptions());
    m_clipStore->setLimits({m_settings.clipboardMaxItems, m_settings.clipboardMaxDays}, QDateTime::currentMSecsSinceEpoch());
    m_clipboard->historyChanged();
    applyClipboardHotkeys();
}

void App::applyClipboardHotkeys()
{
    m_messages->unregisterHotkey(kWinVHotkeyId);
    m_messages->unregisterHotkey(kClipboardHotkeyId);
    if (m_settingsEditor && m_settingsEditor->recordingHotkey())
        return; // the keys being pressed must reach the settings window
    const bool winV = m_settings.clipboardWinV && m_messages->registerHotkey(kWinVHotkeyId, u"Win+V"_s);
    QString state = u"off"_s;
    if (m_settings.clipboardWinV) {
        state = winV ? u"on"_s : m_winVRegistryFailed ? u"failed"_s : u"waiting"_s; // waiting: Explorer still has it
    } else if (!winv::releasedByExplorer(L'V') && m_messages->registerHotkey(kWinVHotkeyId, u"Win+V"_s)) {
        m_messages->unregisterHotkey(kWinVHotkeyId); // nobody has it: Explorer takes it back when it restarts
        state = u"releasing"_s;
    }
    const QString hotkey = m_settings.clipboardHotkey.trimmed();
    QString hotkeyError;
    const bool custom = !hotkey.isEmpty() && m_messages->registerHotkey(kClipboardHotkeyId, hotkey);
    if (!hotkey.isEmpty() && !custom) {
        UINT modifiers = 0;
        UINT vk = 0;
        hotkeyError = MessageWindow::parseHotkey(hotkey, &modifiers, &vk)
            ? tr("“%1” 已被其他程序或系统占用，请换一个。").arg(hotkey)
            : tr("无法识别“%1”，请重新设置。").arg(hotkey);
    }
    m_clipboardShortcut = winV ? u"Win+V"_s : custom ? hotkey : QString();
    m_clipboard->setState(m_settings.clipboard, m_clipboardPaused, m_clipboardShortcut);
    if (m_settingsEditor) {
        m_settingsEditor->setWinVState(state, winv::canRestartExplorer());
        m_settingsEditor->setClipboardHotkeyError(hotkeyError);
    } else if (!hotkeyError.isEmpty()) {
        m_messages->showNotification(tr("快捷键不可用"), hotkeyError);
    }
}

void App::applyTaskbar()
{
    // As Win+V (applyClipboard): Explorer gives Win+S up, or takes it back,
    // when it next starts.
    if (m_winSApplied ? *m_winSApplied != m_settings.taskbarWinS : m_settings.taskbarWinS)
        m_winSRegistryFailed = !winv::setReleasedByExplorer(L'S', m_settings.taskbarWinS);
    m_winSApplied = m_settings.taskbarWinS;
    applyTaskbarHotkeys();

    if (m_settings.taskbarStartTyping && !m_startTyping) {
        // Called on its thread: hop to the GUI thread.
        m_startTyping = std::make_unique<StartMenuTyping>(StartMenuTyping::Callbacks {
            [this] { QMetaObject::invokeMethod(this, &App::startMenuTyped, Qt::QueuedConnection); }});
    } else if (!m_settings.taskbarStartTyping) {
        m_startTyping.reset();
    }
    applySearchBox();
}

// For the settings page: empty while the box is on the taskbar.
static QString searchBoxHiddenName(taskbar::SearchBox::Hidden why)
{
    static const char* const names[] = {"", "pending", "notaskbar", "unreadable", "offscreen", "vertical", "noroom"};
    return QString::fromLatin1(names[static_cast<int>(why)]);
}

void App::applySearchBox()
{
    const bool wanted = m_settings.taskbarSearchBox && taskbar::SearchBox::supported();
    const bool boxWas = m_searchBox != nullptr;
    if (wanted && !m_searchBox) {
        // Called on the box's thread: over to the GUI thread.
        taskbar::SearchBox::Callbacks callbacks;
        callbacks.pressed = [this](const taskbar::SearchBox::Press& press) {
            QMetaObject::invokeMethod(this, [this, press] { searchBoxPressed(press); }, Qt::QueuedConnection);
        };
        callbacks.dragged = [this](int anchor, int at) {
            QMetaObject::invokeMethod(this, [this, anchor, at] {
                if (m_launcher->inTaskbarBox())
                    emit m_launcher->selectionRequested(anchor, at);
            }, Qt::QueuedConnection);
        };
        callbacks.shownChanged = [this](taskbar::SearchBox::Hidden why) {
            QMetaObject::invokeMethod(this, [this, why] { setSearchBoxShown(why); }, Qt::QueuedConnection);
        };
        callbacks.caretMoved = [this] {
            QMetaObject::invokeMethod(this, [this] {
                if (m_launcher->inTaskbarBox())
                    QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
            }, Qt::QueuedConnection);
        };
        callbacks.moved = [this](const taskbar::SearchBox::Spot& spot) {
            QMetaObject::invokeMethod(this, [spot] { saveBoxSpot(spot); }, Qt::QueuedConnection);
        };
        callbacks.wideChanged = [this](bool wide) {
            QMetaObject::invokeMethod(this, [this, wide] { setSearchBoxWide(wide); }, Qt::QueuedConnection);
        };
        callbacks.gapWanted = [this](double dips) {
            QMetaObject::invokeMethod(this, [this, dips] {
                if (m_startGap)
                    m_startGap->set(dips);
            }, Qt::QueuedConnection);
        };
        callbacks.gapMissing = [this] {
            QMetaObject::invokeMethod(this, [this] {
                if (m_startGap)
                    m_startGap->noGapSeen();
            }, Qt::QueuedConnection);
        };
        m_searchBox = std::make_unique<taskbar::SearchBox>(
            std::move(callbacks), taskbar::SearchBox::Look {}, loadBoxSpot());
        setSearchBoxShown(taskbar::SearchBox::Hidden::Pending);
        updateSearchBoxLook();
    } else if (!wanted && m_searchBox) {
        if (m_launcher->inTaskbarBox())
            hideLauncher();
        m_startGap.reset(); // the gap closes first
        setStartGapStatus({});
        m_searchBox.reset();
        setSearchBoxShown(taskbar::SearchBox::Hidden::Pending);
    }
    applyStartGap(boxWas);
}

// The room right of Start, made in Explorer (StartGap), for the box; the
// box says how much it needs (gapWanted). Made before the box wants it,
// gone before it stops wanting it.
void App::applyStartGap(bool boxWas)
{
    const bool wanted = m_searchBox && m_settings.taskbarStartGap;
    if (wanted && !m_startGap) {
        // Called on its thread: over to the GUI thread.
        m_startGap = std::make_unique<taskbar::StartGap>([this](const taskbar::StartGap::Status& status) {
            QMetaObject::invokeMethod(this, [this, status] { setStartGapStatus(status); }, Qt::QueuedConnection);
        });
        m_searchBox->setStartGapWanted(true, boxWas); // turned on with the box already there: by Start
    } else if (!wanted && m_startGap) {
        m_startGap.reset(); // asks for the margin back
        setStartGapStatus({});
        if (m_searchBox)
            m_searchBox->setStartGapWanted(false);
    }
}

void App::setStartGapStatus(const taskbar::StartGap::Status& status)
{
    using State = taskbar::StartGap::State;
    static const char* const names[] = {"off", "attaching", "ready", "applied", "nogap", "conflict", "incompatible", "failed"};
    if (!m_startGap && status.state != State::Off)
        return; // a late word from one gone since
    if (status.state != m_startGapStatus.state || status.reason != m_startGapStatus.reason)
        qWarning().noquote() << "Start gap:" << names[static_cast<int>(status.state)]
                             << QString::fromStdWString(status.reason) << status.dips << "DIPs";
    m_startGapStatus = status;
    if (m_searchBox)
        m_searchBox->setStartGap(status.state == State::Applied, status.dips,
            status.state == State::NoGapSeen || status.state == State::Conflict || status.state == State::Incompatible
                || status.state == State::Failed);
    if (m_settingsEditor) {
        m_settingsEditor->setStartGapState(QString::fromLatin1(names[static_cast<int>(status.state)]),
            QString::fromStdWString(status.reason));
    }
}

void App::setSearchBoxShown(taskbar::SearchBox::Hidden why)
{
    using Hidden = taskbar::SearchBox::Hidden;
    if (!m_searchBox)
        why = Hidden::Pending; // a late word from a box gone since
    const bool shown = why == Hidden::None;
    m_searchBoxShown = shown;
    m_searchBoxHidden = why;
    if (m_settingsEditor)
        m_settingsEditor->setSearchBoxShown(searchBoxHiddenName(why));
    if (!shown && m_launcher->inTaskbarBox())
        hideLauncher(); // what is typed there would be out of sight
}

void App::setSearchBoxWide(bool wide)
{
    // Narrowed under what is being typed (the taskbar filled up): on in the
    // launcher's own field, the box looking open. Not back the other way
    // while it is open, under the user's hands.
    if (wide || !m_searchBox || !m_launcher->inTaskbarBox() || m_launcherClosing)
        return;
    m_launcher->setInTaskbarBox(false);
    m_searchBox->setText({.active = true});
    updateSearchBoxLook();
}

void App::updateSearchBoxLook()
{
    if (!m_searchBox)
        return;
    const QColor accent = SystemTheme::accentColor();
    taskbar::SearchBox::Look look;
    look.name = tr("Win顺 搜索").toStdWString();
    // Typing there: what the launcher searches (its tabs say which).
    const bool typing = m_launcher->inTaskbarBox() && !m_launcherClosing;
    look.placeholder = (typing ? m_launcher->placeholder() : tr("搜索应用和文件")).toStdWString();
    look.label = tr("搜索").toStdWString();
    look.accent = RGB(accent.red(), accent.green(), accent.blue());
    look.monoIcon = m_settings.taskbarIcon == u"mono";
    m_searchBox->setLook(std::move(look));
}

void App::searchBoxPressed(const taskbar::SearchBox::Press& press)
{
    if (!m_window || !m_searchBox)
        return;
    if (!launcherShown() || !m_launcherOverBox) {
        showLauncherInBox();
        return;
    }
    if (!m_launcher->inTaskbarBox()) { // its label or icon alone: a button, pressed again
        hideLauncher();
        return;
    }
    // Open over it already: the press took the keyboard (see m_boxPressGrace).
    // Back to the launcher, the caret where the box was pressed.
    m_boxPressGrace.stop();
    win::bringToFront(m_window);
    m_window->requestActivate();
    if (press.at)
        emit m_launcher->caretRequested(*press.at, press.word);
}

void App::showLauncherInBox()
{
    const std::optional<RECT> box = m_searchBox ? m_searchBox->rect() : std::nullopt;
    if (!box) {
        showLauncher({}, taskbar::locate(false));
        return;
    }
    showLauncher({}, taskbar::locateAt({(box->left + box->right) / 2, box->top}), true);
}

void App::mirrorField(
    const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition)
{
    m_fieldShown.active = true;
    m_fieldShown.text = text.toStdWString();
    m_fieldShown.cursor = cursor;
    m_fieldShown.selectionStart = selectionStart;
    m_fieldShown.selectionEnd = selectionEnd;
    m_fieldShown.composition = composition.toStdWString();
    if (m_searchBox && m_launcher->inTaskbarBox() && !m_launcherClosing) // closing: the box is idle already
        m_searchBox->setText(m_fieldShown);
}

std::optional<QRectF> App::boxCaretInField() const
{
    const std::optional<RECT> caret = m_searchBox ? m_searchBox->caret() : std::nullopt;
    if (!caret || !m_window || !m_searchField)
        return std::nullopt;
    // Physical pixels on the screen to the window's logical ones, then the field's.
    POINT origin {};
    ::ClientToScreen(reinterpret_cast<HWND>(m_window->winId()), &origin);
    const qreal scale = m_window->devicePixelRatio();
    const QPointF inWindow((caret->left - origin.x) / scale, (caret->top - origin.y) / scale);
    return QRectF(m_searchField->mapFromScene(inWindow), QSizeF(1, (caret->bottom - caret->top) / scale));
}

void App::startMenuTyped()
{
    // Over the taskbar, where the Start menu was (showing it closes the
    // Start menu, see win::bringToFront), over Win顺's box if it is there;
    // the keys follow once it has the keyboard.
    m_deliverTyping = true;
    if (m_searchBoxShown)
        showLauncherInBox();
    else
        showLauncher({}, taskbar::locate(false));
    if (m_window && m_window->isActive())
        deliverTyping();
}

void App::deliverTyping()
{
    if (std::exchange(m_deliverTyping, false) && m_startTyping)
        m_startTyping->deliver();
}

void App::applyTaskbarHotkeys()
{
    m_messages->unregisterHotkey(kWinSHotkeyId);
    m_messages->unregisterHotkey(kScreenClipHotkeyId);
    if (m_settingsEditor && m_settingsEditor->recordingHotkey())
        return; // the keys being pressed must reach the settings window
    const bool winS = m_settings.taskbarWinS && m_messages->registerHotkey(kWinSHotkeyId, u"Win+S"_s);
    QString state = u"off"_s;
    if (m_settings.taskbarWinS) {
        state = winS ? u"on"_s : m_winSRegistryFailed ? u"failed"_s : u"waiting"_s; // waiting: Explorer still has it
    } else if (!winv::releasedByExplorer(L'S') && m_messages->registerHotkey(kWinSHotkeyId, u"Win+S"_s)) {
        m_messages->unregisterHotkey(kWinSHotkeyId); // nobody has it: Explorer takes it back when it restarts
        state = u"releasing"_s;
    }
    // Explorer let go of every Win+S key, the screen capture's too: started
    // from here while Win+S is ours. Not while it goes back (releasing):
    // held as the new Explorer starts, it would be gone for good, Explorer
    // registers its keys once.
    if (winS)
        m_messages->registerHotkey(kScreenClipHotkeyId, u"Win+Shift+S"_s);
    if (m_settingsEditor)
        m_settingsEditor->setWinSState(state, winv::canRestartExplorer());
}

void App::toggleAtTaskbar(bool clicked)
{
    if (launcherShown() && m_window->isActive()) {
        hideLauncher();
        return;
    }
    // Clicking the button took the keyboard from the launcher, which went
    // away for it: that click closes the launcher, it does not open it
    // again. The button's program starts on the release, 0.1–0.3 s later.
    if (clicked && m_launcherHidden.isValid() && m_launcherHidden.elapsed() < 500)
        return;
    if (!clicked && m_searchBoxShown) // Win+S: into Win顺's box, as Windows' own goes into its box
        showLauncherInBox();
    else
        showLauncher({}, taskbar::locate(clicked));
}

void App::startScreenClip()
{
    // Not started from here, where it would run elevated: the taskbar
    // button's program starts it with the user's rights, as Explorer does.
    const QString button = QCoreApplication::applicationDirPath() + u"/WinShunSearch.exe"_s;
    if (QFileInfo::exists(button))
        shell::run(u'"' + QDir::toNativeSeparators(button) + u"\" --screenclip"_s);
    else
        shell::openUrl(u"ms-screenclip:///?source=HotKey"_s);
}

void App::clipCaptured(const ClipCapture& capture)
{
    if (!m_settings.clipboard || m_clipboardPaused)
        return; // turned off since the watcher read it
    m_clipStore->add(capture, QDateTime::currentMSecsSinceEpoch());
    m_clipboard->historyChanged();
    if (m_settingsEditor)
        m_settingsEditor->setClipboardCount(static_cast<int>(m_clipStore->clips().size()));
}

void App::restartExplorer()
{
    if (m_restartingExplorer)
        return;
    m_restartingExplorer = true;
    // Seconds of waiting for Explorer to end and to start again: not on the
    // GUI thread.
    if (m_explorerRestart.joinable())
        m_explorerRestart.join(); // the last one, over already
    m_explorerRestart = std::jthread([self = QPointer<App>(this)] {
        const bool back = winv::restartExplorer();
        QMetaObject::invokeMethod(qApp, [self, back] {
            if (self)
                self->explorerRestarted(back);
        }, Qt::QueuedConnection);
    });
}

void App::explorerRestarted(bool back)
{
    m_restartingExplorer = false;
    if (!back && winv::canRestartExplorer()) {
        m_messages->showNotification(tr("没能重启资源管理器"), tr("注销后重新登录 Windows 也能生效。"));
    } else if (!back) {
        m_messages->showNotification(tr("资源管理器没有重新启动"),
            tr("按 Ctrl+Shift+Esc 打开任务管理器，选“运行新任务”，输入 explorer 启动它；注销后重新登录也行。"));
    }
    // Win+V and Win+S are free once the old Explorer is gone; the new one
    // also says when it is up (shellRestarted).
    applyClipboardHotkeys();
    applyTaskbarHotkeys();
}

void App::setClipboardPaused(bool paused)
{
    m_clipboardPaused = paused;
    m_clipWatcher->setOptions(clipboardOptions());
    m_clipboard->setState(m_settings.clipboard, m_clipboardPaused, m_clipboardShortcut);
}

void App::reloadSettings()
{
    const QString file = Settings::filePath();
    if (!m_settingsWatcher.files().contains(file))
        m_settingsWatcher.addPath(file); // editors that save by replacing the file drop the watch
    Settings fresh;
    fresh.load();
    if (fresh == m_settings)
        return; // our own save from the settings window
    const bool accessChanged = fresh.indexAccess != m_settings.indexAccess;
    m_settings = fresh;
    applySettings(false);
    if (m_settingsEditor)
        m_settingsEditor->setSettings(m_settings);
    if (accessChanged)
        applyIndexAccess();
}

void App::settingsEdited(const Settings& settings)
{
    const bool accessChanged = settings.indexAccess != m_settings.indexAccess;
    m_settings = settings;
    applySettings(false);
    m_backup->soon();
    if (accessChanged)
        applyIndexAccess();
}

void App::showSettings()
{
    if (!m_settingsWindow) {
        auto* editor = new SettingsEditor(m_settings, m_renderer);
        auto* frame = new WindowFrame(true, editor); // its own title bar; goes with the editor
        QQmlComponent component(m_qml.get(), u"WinShun"_s, u"SettingsWindow"_s);
        QObject* object = component.createWithInitialProperties({
            {u"editor"_s, QVariant::fromValue(editor)},
            {u"frame"_s, QVariant::fromValue(frame)},
            {u"updater"_s, QVariant::fromValue(m_updater.get())},
            {u"backup"_s, QVariant::fromValue(m_backup.get())},
        });
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning().noquote() << component.errorString();
            delete object;
            delete editor;
            return;
        }
        editor->setParent(window);
        editor->setWindow(window);
        editor->setPlaces(m_places.get()); // Windows' settings, when its search finds none of its own
        connect(editor, &SettingsEditor::edited, this, &App::settingsEdited);
        connect(editor, &SettingsEditor::recordingHotkeyChanged, this, &App::applyHotkey);
        connect(editor, &SettingsEditor::recordingHotkeyChanged, this, &App::applyClipboardHotkeys);
        connect(editor, &SettingsEditor::recordingHotkeyChanged, this, &App::applyTaskbarHotkeys);
        connect(editor, &SettingsEditor::recordingHotkeyChanged, this, &App::applyFolderMenuHotkey);
        connect(editor, &SettingsEditor::explorerRestartRequested, this, &App::restartExplorer);
        connect(editor, &SettingsEditor::clipboardClearRequested, this, [this, editor] {
            m_clipStore->clearHistory();
            m_clipboard->historyChanged();
            editor->setClipboardCount(static_cast<int>(m_clipStore->clips().size()));
        });
        connect(editor, &SettingsEditor::restartRequested, this, [this] { restart({u"--settings"_s}); });
        connect(editor, &SettingsEditor::historyClearRequested, this, [this, editor] {
            m_launcher->clearHistory();
            editor->setHistoryCount(0);
        });
        connect(editor, &SettingsEditor::shareVerifyRequested, this,
            [this](const QString& path) { m_index->shares().verifyNow(path.toStdWString()); });
        connect(editor, &SettingsEditor::shareAcceptRequested, this,
            [this](const QString& path) { m_index->shares().acceptMissing(path.toStdWString()); });
        connect(editor, &SettingsEditor::signInRequested, this, [this](const QString& path) { signInToServer(path); });
        connect(editor, &SettingsEditor::elevatedRestartRequested, this,
            [this] { restart({u"--settings"_s, u"--settings-at"_s, u"indexAccess"_s}, Elevation::Elevated); });
        connect(editor, &SettingsEditor::indexServiceInstallRequested, this, &App::installIndexService);
        connect(editor, &SettingsEditor::opusRestarted, this, [this] { // the settings window back over it
            if (m_settingsWindow && m_settingsWindow->isVisible()) {
                m_settingsWindow->raise();
                win::bringToFront(m_settingsWindow);
                m_settingsWindow->requestActivate();
            }
        });
        editor->plugins()->setLocale(m_language);
        editor->plugins()->setHost(m_pluginHost.get());
        editor->favorites()->setCatalogs(m_apps.get(), m_places.get());
        editor->favorites()->setStore(m_favorites.get());
        m_settingsWindow = window;
        m_settingsEditor = editor;
        applyHotkey(); // shows whether the current hotkey works
        applyClipboardHotkeys(); // ... and where Win+V stands
        applyTaskbarHotkeys(); // ... and Win+S
        applyFolderMenuHotkey(); // ... and the folder menu's
        editor->refreshTaskbarState();
        editor->setSearchBoxShown(searchBoxHiddenName(m_searchBoxHidden));
        setStartGapStatus(m_startGapStatus);
        editor->refreshFileManagers();
        editor->refreshMcpClients();
        refreshMcpStatus();
        refreshIndexAccess();
        if (m_serviceWanted && m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None)
            probeService(); // installed meanwhile? Looked for now, not in five minutes

        prepareBackdrop(window);
        window->create();
        window->installEventFilter(this); // pasting into its fields
        frame->setWindow(window);
        styleWindow(window, SystemTheme::backdropAvailable());
        win::setDarkFrame(window, isDarkMode());
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        const QRect area = screen->availableGeometry();
        window->resize(window->width(), std::min(window->height(), area.height() - 40));
        window->setPosition(area.center() - QPoint(window->width() / 2, window->height() / 2));
        connect(window, &QWindow::visibleChanged, window, [this, window](bool visible) {
            if (!visible) {
                m_contentStatusTimer.stop();
                window->deleteLater(); // free the window's memory until it is needed again
            }
        });
    }
    refreshContentIndexStatus();
    refreshShareStatuses();
    refreshIndexFolder(true);
    m_settingsEditor->setHistoryCount(static_cast<int>(m_history->items().size()));
    m_settingsEditor->setClipboardCount(static_cast<int>(m_clipStore->clips().size()));
    m_contentStatusTimer.start();
    hideLauncher();
    m_settingsWindow->show();
    m_settingsWindow->raise();
    win::bringToFront(m_settingsWindow);
    m_settingsWindow->requestActivate();
}

void App::showClipboardSettings()
{
    showSettings();
    if (m_settingsWindow)
        QMetaObject::invokeMethod(m_settingsWindow, "showClipboardPage");
}

void App::showWebSettings()
{
    showSettings();
    if (m_settingsWindow)
        QMetaObject::invokeMethod(m_settingsWindow, "showWebPage");
}

void App::showFileTypeSettings()
{
    showSettings();
    if (m_settingsWindow)
        QMetaObject::invokeMethod(m_settingsWindow, "showFileTypesPage");
}

void App::showFavoriteSettings(const QString& path)
{
    showSettings();
    if (m_settingsWindow)
        QMetaObject::invokeMethod(m_settingsWindow, "showFavoritesPage", Q_ARG(QVariant, QVariant(path)));
}

void App::showPluginSettings(const QString& pluginId)
{
    showSettings();
    if (m_settingsWindow)
        QMetaObject::invokeMethod(m_settingsWindow, "showPluginPage", Q_ARG(QVariant, QVariant(pluginId)));
}

void App::showUpdate()
{
    showSettings();
    if (m_settingsWindow)
        QMetaObject::invokeMethod(m_settingsWindow, "showUpdateDialog");
}

void App::refreshContentIndexStatus()
{
    if (!m_settingsEditor) {
        m_contentStatusTimer.stop();
        return;
    }
    QString status;
    if (m_settings.contentSearch && m_settings.contentIndex) {
        const ContentIndex::Stats stats = m_index->contentIndex()->stats();
        const int files = static_cast<int>(stats.documents);
        if (m_index->readingContent())
            status = tr("正在建立索引… 已收录 %Ln 个文件", nullptr, files)
                + tr("，这一轮读到 %1 / %2")
                      .arg(QLocale().toString(static_cast<qulonglong>(m_index->contentIndexer()->progress().done)),
                          QLocale().toString(static_cast<qulonglong>(m_index->contentIndexer()->progress().total)));
        else if (stats.documents == 0)
            status = tr("文件索引一建好，就开始建立");
        else
            status = tr("已收录 %Ln 个文件，占用 %1 磁盘空间", nullptr, files)
                         .arg(QLocale().formattedDataSize(static_cast<qint64>(stats.segmentBytes + stats.textBytes), 0,
                             QLocale::DataSizeTraditionalFormat));
        if (stats.pending > 0 && !m_index->readingContent())
            status += tr("；%Ln 个文件有改动，稍后更新", nullptr, static_cast<int>(stats.pending));
    }
    m_settingsEditor->setContentIndexStatus(status);
}

void App::refreshShareStatuses()
{
    using State = ShareIndexer::State;
    std::vector<ShareIndexer::Status> statuses = m_index->shares().statuses();
    // A share read for the first time: told once it is, unless the settings,
    // where it was added, still show it.
    for (const ShareIndexer::Status& s : statuses) {
        const auto building = std::ranges::find(m_sharesBuilding, s.path);
        if (s.state == State::Building && building == m_sharesBuilding.end()) {
            m_sharesBuilding.push_back(s.path);
        } else if (s.state != State::Building && building != m_sharesBuilding.end()) {
            m_sharesBuilding.erase(building);
            const bool shown = m_settingsWindow && m_settingsWindow->isVisible();
            if (s.state == State::Online && !shown)
                m_messages->showNotification(tr("%1 可以搜索了").arg(QString::fromStdWString(s.name.empty() ? s.path : s.name)),
                    tr("已收录 %L1 项，之后有变化会自动更新。").arg(static_cast<qulonglong>(s.items)));
        }
    }
    if (m_settingsEditor)
        m_settingsEditor->setShareStatuses(std::move(statuses));
}

// Windows' own dialog for the user name and password of the server of
// `path`, then connecting with them in the background; asked again while
// they are wrong. The share is read again once connected.
void App::signInToServer(const QString& path, unsigned long error, const QString& user)
{
    const std::wstring host(unc::host(path.toStdWString()));
    if (host.empty())
        return;
    const HWND owner = m_settingsWindow ? reinterpret_cast<HWND>(m_settingsWindow->winId()) : nullptr;
    auto entered = std::make_shared<credentials::Entered>();
    const QString server = QString::fromStdWString(host);
    if (!credentials::prompt(owner, tr("连接到 %1").arg(server).toStdWString(),
            tr("输入 %1 上的账户（例如 NAS 的用户名）和密码。勾选“记住我的凭据”，资源管理器以后也不用再输入。")
                .arg(server)
                .toStdWString(),
            error, user.toStdWString(), *entered))
        return;
    std::thread([this, guard = QPointer<QObject>(this), path, entered] {
        const unsigned long result = credentials::connect(path.toStdWString(), *entered);
        QMetaObject::invokeMethod(qApp, [this, guard, path, entered, result] {
            if (!guard)
                return;
            if (result == ERROR_LOGON_FAILURE || result == ERROR_INVALID_PASSWORD || result == ERROR_ACCOUNT_RESTRICTION) {
                signInToServer(path, result, QString::fromStdWString(entered->user)); // wrong: again, saying so
                return;
            }
            QString problem;
            if (result != NO_ERROR) {
                const share::Problem p = share::classify(result, true);
                problem = p == share::Problem::Offline ? tr("连不上 %1").arg(QString::fromStdWString(std::wstring(unc::host(path.toStdWString()))))
                    : p == share::Problem::GuestBlocked ? tr("这台服务器只允许来宾访问，Windows 不允许这样连接")
                    : p == share::Problem::NotFound     ? tr("服务器上没有这个共享文件夹")
                                                        : tr("没有连上（错误 %1）").arg(result);
            }
            if (m_settingsEditor)
                m_settingsEditor->setSignedIn(path, problem);
            if (result != NO_ERROR)
                return;
            // Every share on the server is worth trying again now.
            const std::wstring_view host = unc::host(path.toStdWString());
            for (const ShareIndexer::Status& s : m_index->shares().statuses()) {
                if (s.state == ShareIndexer::State::Problem && unc::host(s.path).size() == host.size()
                    && ::_wcsnicmp(unc::host(s.path).data(), host.data(), host.size()) == 0)
                    m_index->shares().verifyNow(s.path);
            }
        });
    }).detach();
}

QString App::startIndexFolder()
{
    const QString dataDir = folderPath(Settings::dataDir());
    const QString wanted = folderPath(m_settings.indexDir());
    m_indexFolder = m_settings.indexFolder;
    m_indexDir = dataDir;
    if (sameFolder(wanted, dataDir))
        return m_indexDir;
    const QString native = QDir::toNativeSeparators(wanted);
    // A disk that is not there today (not plugged in, not unlocked yet): the
    // default place for now, and the setting is kept for the next start.
    const auto later = [&] {
        m_indexFolderProblem
            = tr("%1 现在用不了，这次先把索引放在默认位置，下次启动 Win顺 时再试。").arg(native);
        return m_indexDir;
    };
    if (driveType(wanted) == DRIVE_NO_ROOT_DIR)
        return later();
    // A place that will not do (an external disk, say; set in the INI file):
    // back to the default one, setting and all.
    QString problem = indexFolderProblem(wanted);
    if (problem.isEmpty() && !indexfolder::onlyIndexFiles(wanted))
        problem = tr("那里已有别的文件");
    if (!problem.isEmpty()) {
        m_indexFolderProblem = tr("索引不能放在 %1：%2。已改回默认位置。").arg(native, problem);
        m_settings.indexFolder.clear();
        m_settings.save();
        m_indexFolder.clear();
        return m_indexDir;
    }
    if (!QDir().mkpath(wanted))
        return later();
    if (indexfolder::hasIndex(wanted) || !indexfolder::hasIndex(dataDir)) {
        indexfolder::remove(dataDir); // left from a start without that disk, or a move cut short
        m_indexDir = wanted;
    } else {
        // Still in the default place (the setting was changed while Win顺 was
        // not running): moved there once it runs.
        m_indexFolder.clear();
    }
    return m_indexDir;
}

void App::applyIndexFolder()
{
    // Not before the snapshot has been read (the worker would wait for the
    // move, and stopWorker() for the worker): stateChanged() comes then.
    // After a move, indexMoved() looks again.
    const IndexService::State state = m_index->state();
    if (state == IndexService::State::Idle || state == IndexService::State::Loading || m_index->moving()
        || m_settings.indexFolder == m_indexFolder)
        return;
    const QString to = folderPath(m_settings.indexDir());
    if (sameFolder(to, m_indexDir)) { // written another way, or back where the files stayed
        m_indexFolder = m_settings.indexFolder;
        m_indexFolderProblem.clear();
        refreshIndexFolder();
        return;
    }
    const QString native = QDir::toNativeSeparators(to);
    const bool toDefault = sameFolder(to, Settings::dataDir());
    QString problem = toDefault ? QString() : indexFolderProblem(to);
    if (problem.isEmpty() && !toDefault && !indexfolder::onlyIndexFiles(to))
        problem = tr("%1 里已有别的文件，请选择一个空文件夹").arg(native);
    if (problem.isEmpty()) {
        const auto needed = static_cast<qint64>(indexfolder::size(m_indexDir)) + (64ll << 20); // and room to grow
        const QStorageInfo disk(to.left(3));
        if (disk.isValid() && disk.bytesAvailable() < needed) {
            const auto size = [](qint64 bytes) {
                return QLocale().formattedDataSize(bytes, 0, QLocale::DataSizeTraditionalFormat);
            };
            problem = tr("%1 盘的剩余空间不够：索引需要 %2，只剩 %3")
                          .arg(to.left(1).toUpper(), size(needed), size(disk.bytesAvailable()));
        }
    }
    if (!problem.isEmpty()) {
        indexMoveFailed(m_settings.indexFolder, problem);
        return;
    }
    m_indexMoveFolder = m_settings.indexFolder;
    m_indexMoveDir = to;
    m_indexMoveMadeDir = !QFileInfo::exists(to);
    m_indexMoveProgress = 0;
    m_indexFolderProblem.clear();
    m_index->moveTo(to);
    refreshIndexFolder();
}

QString App::indexFolderProblem(const QString& folder) const
{
    // On a disk built into this computer: "D:/...".
    if (driveRoot(folder).isEmpty())
        return tr("%1 不在这台电脑的硬盘上").arg(QDir::toNativeSeparators(folder));
    const QString external = tr("U 盘、移动硬盘这类外接的磁盘拔下或弹出后就用不了索引了，请选择电脑内置的硬盘");
    switch (driveType(folder)) {
    case DRIVE_FIXED: // 移动硬盘 as well
        return indexfolder::onExternalDisk(folder) ? external : QString();
    case DRIVE_NO_ROOT_DIR:
        return tr("找不到 %1 盘").arg(folder.left(1).toUpper());
    case DRIVE_REMOVABLE:
        return external;
    case DRIVE_REMOTE:
        return tr("网络位置不能存放索引，请选择这台电脑硬盘上的文件夹");
    default:
        return tr("%1 盘不能存放索引，请选择这台电脑硬盘上的文件夹").arg(folder.left(1).toUpper());
    }
}

void App::indexMoved(int error)
{
    if (error == ERROR_CANCELLED)
        return; // Win顺 is closing
    if (error == 0) {
        const QString from = std::exchange(m_indexDir, m_indexMoveDir);
        m_indexFolder = m_indexMoveFolder;
        if (!sameFolder(from, Settings::dataDir()))
            QDir().rmdir(from); // a folder of its own, empty now
        refreshIndexFolder(true);
        if (!m_settingsWindow || !m_settingsWindow->isVisible()) {
            m_messages->showNotification(
                tr("索引已经移好"), tr("现在存放在 %1").arg(QDir::toNativeSeparators(m_indexDir)));
        }
    } else {
        if (m_indexMoveMadeDir)
            QDir().rmdir(m_indexMoveDir); // made for the move (and empty again)
        indexMoveFailed(m_indexMoveFolder,
            tr("没能把索引移到 %1：%2")
                .arg(QDir::toNativeSeparators(m_indexMoveDir), qt_error_string(error).trimmed()));
    }
    applyIndexFolder(); // the setting may have changed again meanwhile
}

// The files stay where they are, and so the setting goes back to that
// (unless it was changed again since `attempted`). Why is shown next to it
// in the settings window, or in a notification.
void App::indexMoveFailed(const QString& attempted, const QString& problem)
{
    m_indexFolderProblem = problem;
    if (m_settings.indexFolder == attempted) {
        m_settings.indexFolder = m_indexFolder;
        m_settings.save();
        m_indexOptionsApply.start(); // the folder left out of searches goes back too
        if (m_settingsEditor)
            m_settingsEditor->setSettings(m_settings);
    }
    if (m_settingsWindow && m_settingsWindow->isVisible())
        refreshIndexFolder();
    else
        m_messages->showNotification(tr("索引没有移动"), problem);
}

void App::refreshIndexFolder(bool measure)
{
    if (!m_settingsEditor)
        return;
    if (measure) {
        m_indexBytes = indexfolder::size(m_indexDir);
        m_indexOnHardDisk = indexfolder::onSpinningDisk(m_indexDir) == true;
    }
    SettingsEditor::IndexFolderState state;
    state.folder = QDir::toNativeSeparators(m_indexDir);
    state.isDefault = m_settings.indexFolder.isEmpty();
    if (m_indexBytes > 0)
        state.size = QLocale().formattedDataSize(
            static_cast<qint64>(m_indexBytes), 0, QLocale::DataSizeTraditionalFormat);
    state.moveProgress = m_index->moving() ? m_indexMoveProgress : -1;
    state.problem = m_indexFolderProblem;
    if (m_indexOnHardDisk) // nothing to choose: a hint, not a warning (IndexService::prewarmSoon)
        state.note = tr("这是机械硬盘：文件名搜索不受影响；内容索引会在后台预读进内存，硬盘休眠后第一次内容搜索可能要等它转起来");
    m_settingsEditor->setIndexFolderState(state);
}

void App::handleCommand(const QString& command)
{
    if (command == u"toggle")
        toggleLauncher();
    else if (command == u"quit")
        QGuiApplication::quit();
    else if (command == u"settings")
        showSettings();
    else if (command.startsWith(u"query:"))
        showLauncher(command.mid(6));
    else
        showLauncher();
}

void App::applyFolderMenu()
{
    // On with File Explorer and the desktop both off too: Directory Opus may
    // ask for it (requested()). And with the double click off, for its
    // hotkey (GitHub #25).
    const bool on = m_settings.folderMenu != u"off" || !m_settings.folderMenuHotkey.trimmed().isEmpty();
    if (!on && m_folderMenu) {
        m_keyListener.reset(); // it tells the menu double clicks: gone first
        m_keyListenerMode = 0;
        delete m_folderMenuWindow;
        m_folderMenu.reset();
    } else if (on && !m_folderMenu) {
        auto menu = std::make_unique<FolderMenu>(m_history.get(), m_favorites.get());
        QQmlComponent component(m_qml.get(), u"WinShun"_s, u"FolderMenuWindow"_s);
        QObject* object = component.createWithInitialProperties({{u"folderMenu"_s, QVariant::fromValue(menu.get())}});
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            qWarning().noquote() << component.errorString();
            delete object;
            return;
        }
        menu->setWindow(window);
        menu->setBlocked([this] { return clipboardInCharge(); }); // its keys over File Explorer
        menu->setDialogGo([this](HWND dialog, const QString& folder) { // the hotkey over a file dialog
            if (!m_dialogJump)
                return false;
            m_dialogJump->go(dialog, folder.toStdWString(), false);
            return true;
        });
        menu->setDialogOf([this](HWND window) -> HWND { // ... over the bar beside one: that dialog
            if (!m_dialogJump || !m_dialogJump->dialog() || !m_dialogJump->isCompanion(window))
                return nullptr;
            return m_dialogJump->dialog();
        });
        connect(menu.get(), &FolderMenu::openChanged, this, [this](bool open) {
            if (m_keyListener)
                m_keyListener->setMenuOpen(open);
        });
        connect(menu.get(), &FolderMenu::searchRequested, this, [this](const QString& folder) {
            showLauncher(folder.endsWith(u'\\') ? folder + u' ' : folder + u"\\ "_s);
        });
        m_folderMenuWindow = window;
        m_folderMenu = std::move(menu);
    }
    if (m_folderMenu) {
        m_folderMenu->setOptions(m_settings.folderMenu == u"auto", m_settings.folderMenuExplorer, m_settings.folderMenuDesktop);
        m_folderMenu->setRecordHistory(m_settings.recordHistory);
    }
}

void App::applyDialogs()
{
    if (m_settings.dialogBar && !m_dialogBar) {
        if (!m_dialogEngine)
            m_dialogEngine = std::make_unique<SearchEngine>(m_index.get(), m_apps.get(), m_places.get());
        m_dialogBar = std::make_unique<DialogBar>(m_dialogEngine.get(), m_history.get(),
            m_favorites.get(), [this](HWND dialog, std::wstring path, bool isFile, bool open) {
                if (m_dialogJump)
                    m_dialogJump->go(dialog, std::move(path), isFile, open);
            });
        connect(m_dialogBar.get(), &DialogBar::excludeAppRequested, this, &App::excludeFromDialogBar);
        connect(m_dialogBar.get(), &DialogBar::settingsRequested, this, [this] { showSettings(); });
        connect(m_dialogBar.get(), &DialogBar::autoJumpAnswered, this, &App::answerAutoJump);
    } else if (!m_settings.dialogBar && m_dialogBar) {
        delete m_barWindow;
        m_dialogBar.reset();
        updateCompanions();
    }
    if (m_dialogBar) {
        m_dialogBar->setRecordHistory(m_settings.recordHistory);
        m_dialogBar->setExcludedApps(m_settings.dialogBarExcludedApps);
        m_dialogBar->setPlace(m_settings.dialogBarPlace);
        m_dialogBar->setResultOptions(m_settings.showModified, m_settings.thumbnails, m_settings.numberKeys);
        m_dialogBar->setAskAutoJump(!m_settings.dialogAutoJump && !m_settings.dialogAutoJumpAsked);
    }

    const bool followDialogs = m_settings.dialogJump || m_settings.dialogBar || m_settings.dialogAutoJump;
    if (followDialogs && !m_dialogJump) {
        DialogJump::Callbacks callbacks;
        callbacks.setHotkey = [this](bool on) {
            if (on)
                return m_messages->registerHotkey(kDialogJumpHotkeyId, u"Ctrl+G"_s);
            m_messages->unregisterHotkey(kDialogJumpHotkeyId);
            return true;
        };
        callbacks.dialogChanged = [this](HWND dialog) {
            if (m_dialogBar && (!dialog || m_barWindow || createBarWindow()))
                m_dialogBar->setDialog(dialog);
        };
        callbacks.dialogMoved = [this] {
            if (m_dialogBar)
                m_dialogBar->dialogMoved();
        };
        callbacks.autoJumped = [this](HWND dialog, std::wstring from) { // on DialogJump's thread
            QMetaObject::invokeMethod(this, [this, dialog, from = QString::fromStdWString(from)] {
                if (m_dialogBar)
                    m_dialogBar->setOrigin(dialog, from);
            }, Qt::QueuedConnection);
        };
        m_dialogJump = std::make_unique<DialogJump>(std::move(callbacks));
        updateCompanions();
    } else if (!followDialogs) {
        m_dialogJump.reset();
    }
    if (m_dialogJump) {
        m_dialogJump->setHotkeyEnabled(m_settings.dialogJump);
        m_dialogJump->setAutoJump(m_settings.dialogAutoJump, m_settings.dialogAutoJumpWhen != u"back");
    }
    // Turned on with a file dialog in front: under it at once.
    if (m_dialogBar && !m_barWindow && m_dialogJump && m_dialogJump->dialog() && createBarWindow())
        m_dialogBar->setDialog(m_dialogJump->dialog());
}

void App::answerAutoJump(const QString& when)
{
    m_settings.dialogAutoJumpAsked = true;
    m_settings.dialogAutoJump = when == u"open" || when == u"back";
    if (m_settings.dialogAutoJump)
        m_settings.dialogAutoJumpWhen = when;
    m_settings.save();
    applySettings(false);
    if (m_settingsEditor)
        m_settingsEditor->setSettings(m_settings);
}

void App::excludeFromDialogBar(const QString& app)
{
    if (app.isEmpty() || m_settings.dialogBarExcludedApps.contains(app, Qt::CaseInsensitive))
        return;
    m_settings.dialogBarExcludedApps.append(app);
    m_settings.save();
    applySettings(false);
    if (m_settingsEditor)
        m_settingsEditor->setSettings(m_settings);
}

bool App::createBarWindow()
{
    QQmlComponent component(m_qml.get(), u"WinShun"_s, u"DialogBarWindow"_s);
    QObject* object = component.createWithInitialProperties({{u"bar"_s, QVariant::fromValue(m_dialogBar.get())}});
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        qWarning().noquote() << component.errorString();
        delete object;
        return false;
    }
    window->create();
    styleWindow(window);
    win::setDarkFrame(window, isDarkMode());
    window->installEventFilter(this); // the Menu key, pasting into the box
    // Gone back to from the clipboard (returnToField): into the box as
    // soon as the bar is up again with its dialog, once it is done showing.
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible || std::exchange(m_barFocusDeadline, {}).hasExpired())
            return;
        QMetaObject::invokeMethod(this, [this] {
            if (m_barWindow && m_barWindow->isVisible() && !m_barWindow->isActive()) {
                win::bringToFront(m_barWindow);
                m_barWindow->requestActivate();
            }
        }, Qt::QueuedConnection);
    });
    connect(window, &QWindow::activeChanged, this, &App::updateNumberKeys);
    m_barWindow = window;
    m_dialogBar->setWindow(window);
    // Made for DialogJump's first look at the window in front, before it is
    // there: then applyDialogs() does this.
    updateCompanions();
    return true;
}

void App::updateNumberKeys()
{
    const bool launcherInFront = m_window && m_window->isActive();
    const bool barInFront = m_barWindow && m_barWindow->isActive() && m_dialogBar;
    if (m_settings.numberKeys == u"off" || (!launcherInFront && !barInFront))
        return;
    // Asked again each time, as programs start and end: digits another
    // program holds as global hotkeys stay its (PixPin's Ctrl+1…), as do Win顺's own.
    const unsigned left = numberkeys::heldMask(
        m_settings.numberKeys == u"alt" ? numberkeys::Modifier::Alt : numberkeys::Modifier::Ctrl);
    m_launcher->setNumberKeysLeft(left);
    if (m_dialogBar)
        m_dialogBar->setNumberKeysLeft(left);
}

void App::updateCompanions()
{
    if (!m_dialogJump)
        return;
    std::vector<HWND> windows;
    for (QWindow* window : {static_cast<QWindow*>(m_barWindow), static_cast<QWindow*>(m_clipWindow)}) {
        if (window)
            windows.push_back(reinterpret_cast<HWND>(window->winId()));
    }
    m_dialogJump->setCompanions(std::move(windows));
}

void App::onDoubleCtrl()
{
    const ForegroundFacts facts = foreground::facts();
    const IgnoreReason reason = doubleCtrlIgnoreReason(facts,
        {m_settings.doubleCtrlPauseInGames, m_settings.doubleCtrlPauseInFullScreen, m_settings.doubleCtrlExcludedApps});
    if (reason == IgnoreReason::None) {
        toggleLauncher();
        return;
    }
    // Nothing shown over a game. The log says why, for "double Ctrl does nothing".
    static const char* const why[] {"", "it is in the list", "it is full screen", "it is in exclusive full screen",
        "it has taken the mouse (cursor hidden and confined, or full screen)"};
    const QString key = facts.program + u' ' + QString::number(static_cast<int>(reason));
    if (!m_doubleCtrlIgnoredLogged.contains(key)) {
        m_doubleCtrlIgnoredLogged.append(key);
        qWarning().noquote() << "Double Ctrl ignored over" << facts.program << "as"
                             << why[static_cast<int>(reason)];
    }
}

void App::toggleLauncher()
{
    // The clipboard in front: back into the box it came from, or over to searching.
    if (clipboardInCharge()) {
        if (m_clipboard->field()) {
            m_clipboard->dismiss();
            return;
        }
        hideClipboard();
    }
    // In front of a file dialog: into its search bar, and back.
    if (m_dialogBar && m_dialogBar->isShown()) {
        m_dialogBar->toggleFocus();
        return;
    }
    if (launcherShown() && m_window->isActive())
        hideLauncher();
    else
        showLauncher();
}

void App::toggleClipboard()
{
    // Again in front: back to the field it was opened from, or hidden.
    if (clipboardInCharge())
        m_clipboard->dismiss();
    else
        showClipboard();
}

void App::showLauncher(const QString& query, const std::optional<taskbar::Spot>& spot, bool inBox)
{
    if (!m_window)
        return;
    if (m_idleTrim->takeTrimmed() && m_index->index()) {
        // The index's pages back while the window comes up, before the first
        // search needs them (IdleTrim, FileIndex::warm).
        std::thread([index = m_index->index()] {
            const auto lock = index->longReadLock();
            index->warm();
        }).detach();
    }
    hideClipboard(false); // from under its search box: searching again
    m_boxPressGrace.stop();
    m_launcherShown.start();
    const bool wasClosing = std::exchange(m_launcherClosing, false); // back from on its way out
    // Opened anew (not back from on its way out, not up already): as at its
    // first opening, if so set. Still hidden or cloaked until the rows for
    // it are drawn, so the old words and rows are never seen.
    if (m_settings.clearOnOpen && !wasClosing && (!m_window->isVisible() || m_prewarming.load()))
        m_launcher->startOver();
    const bool wasInBox = m_launcher->inTaskbarBox();
    m_launcherOverBox = inBox && m_searchBox;
    m_launcher->setInTaskbarBox(m_launcherOverBox && m_searchBox->wide());
    updateSearchBoxLook();
    if (m_searchBox && !m_launcher->inTaskbarBox()) // opened elsewhere: the box is idle; over it, it looks open
        m_searchBox->setText({.active = m_launcherOverBox});
    else if (m_searchBox && wasClosing && wasInBox) // the field has not changed, nor said so again
        m_searchBox->setText(m_fieldShown);
    if (spot) {
        m_placement->attachTaskbar(*spot);
    } else {
        // On the monitor under the mouse, where the user last put it (see Placement).
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        m_placement->placeOn(screen ? screen : QGuiApplication::primaryScreen());
    }
    if (!query.isEmpty())
        m_launcher->setQuery(query);

    const bool prewarming = m_prewarming.exchange(false); // up already, cloaked
    if (wasClosing) {
        // Back from where it got to on its way out.
        m_placement->slideIn([this] {
            if (m_launcherLogo && launcherShown())
                m_launcherLogo->reveal();
        });
    } else if (!m_window->isVisible() || prewarming) {
        // Shown cloaked: Windows would first put up the window as it looked
        // when it was hidden (old selection, old rows), then our repaint.
        // Revealed once a frame with the refreshed results is drawn.
        win::setCloaked(m_window, true);
        m_revealing = true;
        m_revealTimeout.start();
    }
    m_window->show();
    win::bringToFront(m_window);
    m_window->requestActivate();
    m_launcher->handleShown(); // selects all that is typed
    if (!query.isEmpty()) // typed for the user (a folder, --query): words go after it
        emit m_launcher->caretRequested(static_cast<int>(query.size()), false);
    if (m_revealing && !m_launcher->waitingForNames())
        armReveal(); // nothing to wait for (内容 refreshes in the background)
}

void App::showClipboard()
{
    if (!m_clipWindow && !createClipWindow())
        return;
    // Digits another program holds as global hotkeys stay its (KeyRouter
    // passes them on): their rows carry no number.
    if (m_settings.clipboardNumberKeys != u"off") {
        m_clipboard->setNumberKeysLeft(numberkeys::heldMask(
            m_settings.clipboardNumberKeys == u"ctrl" ? numberkeys::Modifier::Ctrl : numberkeys::Modifier::Alt));
    }
    if (m_keyRouter) // up already, over the program that keeps the focus
        return;
    if (m_clipboard->pinned() && m_clipWindow->isVisible() && !m_clipPrewarming.load()) {
        engageClipboard(); // where it is, over whatever is in front now
        return;
    }
    const bool prewarming = m_clipPrewarming.exchange(false); // up already, cloaked
    bool keepFocus = false; // where it is, with the program it pastes into
    if (!m_clipWindow->isVisible() || prewarming) {
        // It pastes into what had the keyboard before it: a text field of
        // Win顺's own (read before the window takes the focus from it), or
        // else the window in front.
        QQuickItem* field = focusedField();
        m_clipboard->setField(field, field && field->window() == m_settingsWindow ? tr("输入框") : tr("搜索框"));
        const HWND foreground = ::GetForegroundWindow();
        m_clipboard->setTarget(foreground);
        m_clipboard->setPinned(false); // as it opens, whatever closed it last (a close of Qt's own)
        m_clipboard->setActive(true); // under the launcher's search box, it folds its rows away (Main.qml)
        m_clipboard->handleShown(); // the list alone again: its width as it opens
        if (field) {
            // Dropping down under the field's box (the launcher's search bar,
            // the bar by a file dialog, a box in the settings window).
            const QQuickItem* box = field->parentItem() ? field->parentItem() : field;
            m_clipPlacement->attach(QRectF(box->mapToGlobal(QPointF(0, 0)), box->size()).toAlignedRect());
        } else {
            // By the text caret in the program it pastes into, as Windows'
            // own clipboard opens; else by the mouse pointer, its top left
            // corner at the pointer's tip, as a menu.
            const HWND target = paste::usableTarget(foreground);
            const std::optional<QRect> caret = target ? m_textCaret->find(target, kCaretWait) : std::nullopt;
            if (caret)
                m_clipPlacement->attachNative(*caret);
            else
                m_clipPlacement->attach(QRect(QCursor::pos(), QSize(1, 0)));
            constexpr LONG reach = 1 << 20; // the logo stays off the line typed in, all along it
            m_clipLogo->setAvoid(caret ? std::optional(RECT {caret->x() - reach, caret->y(), caret->x() + reach,
                                             caret->y() + caret->height()})
                                       : std::nullopt);
            // As Windows' own: that program stays in front, whatever closes
            // without the focus stays open there (VS Code's command palette,
            // a browser's suggestions), the caret stays where the paste goes.
            // Not over the Start menu, search and the like, which stay above
            // every window: bringToFront closes them.
            keepFocus = target && !win::isShellFlyout(foreground) && startKeyRouting(target);
        }
        m_clipboard->handlePlaced(); // the preview, if left open: as wide as there is room for here
        // Cloaked until its first frame: Windows would first put up the
        // window as it looked when it was hidden, then our repaint.
        win::setCloaked(m_clipWindow, true);
        m_clipUncloak = true;
        m_clipUncloakTimeout.start();
    }
    if (keepFocus) {
        m_clipFrame->setNoActivate(true);
        m_clipWindow->setProperty("_q_showWithoutActivating", true);
        m_clipWindow->show();
        m_clipWindow->setProperty("_q_showWithoutActivating", QVariant());
        // On top of the other topmost windows, as an activated one would be.
        ::SetWindowPos(reinterpret_cast<HWND>(m_clipWindow->winId()), HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    m_clipWindow->show();
    win::bringToFront(m_clipWindow);
    m_clipWindow->requestActivate();
}

bool App::clipboardInCharge() const
{
    return m_clipWindow && m_clipWindow->isVisible() && !m_clipPrewarming.load()
        && (m_clipWindow->isActive() || m_keyRouter);
}

bool App::startKeyRouting(HWND target)
{
    // An elevated program while Win顺 is not: Windows keeps the keys typed
    // there from the hook (UIPI). The clipboard takes the focus instead.
    if (integrity::higherThanOurs(target))
        return false;
    const std::uint64_t run = ++m_keyRouterRun;
    // From the router's thread, in order; a key still on its way when the
    // routing has ended goes nowhere.
    const auto post = [this, run](auto call) {
        QMetaObject::invokeMethod(this, [this, run, call = std::move(call)] {
            if (m_keyRouter && m_keyRouterRun == run)
                call();
        }, Qt::QueuedConnection);
    };
    KeyRouter::Callbacks callbacks;
    callbacks.key = [this, post](const KeyRouter::Key& key) { post([this, key] { routedKey(key); }); };
    callbacks.clickedAway = [this, post] { post([this] { hideClipboard(); }); }; // as a menu
    callbacks.foregroundChanged = [this, post](HWND window) { post([this, window] { routedForeground(window); }); };
    callbacks.drained = [this, run] {
        QMetaObject::invokeMethod(this, [this, run] {
            if (m_drainingRun == run)
                m_drainingRouter.reset();
        }, Qt::QueuedConnection);
    };
    const Qt::KeyboardModifier numberKeys = m_settings.clipboardNumberKeys == u"ctrl" ? Qt::ControlModifier
        : m_settings.clipboardNumberKeys == u"off"                                    ? Qt::NoModifier
                                                                                      : Qt::AltModifier;
    auto router = std::make_unique<KeyRouter>(
        target, reinterpret_cast<HWND>(m_clipWindow->winId()), std::move(callbacks), numberKeys);
    if (!router->isActive()) {
        qWarning() << "Clipboard: no keyboard hook; it takes the focus instead";
        return false;
    }
    m_keyRouter = std::move(router);
    m_keyRouterTarget = target;
    m_clipboard->setKeysRouted(true);
    return true;
}

void App::stopKeyRouting()
{
    if (m_keyRouter) {
        // No key is taken any more; the releases of those it took still are
        // its own, for a moment.
        m_keyRouter->drain();
        m_drainingRouter = std::move(m_keyRouter);
        m_drainingRun = m_keyRouterRun;
        QTimer::singleShot(2s, this, [this, run = m_drainingRun] {
            if (m_drainingRun == run)
                m_drainingRouter.reset(); // a key held that long is the program's again
        });
    }
    m_keyRouterTarget = nullptr;
    if (!m_clipboard->keysRouted())
        return;
    m_clipFrame->setNoActivate(false);
    m_clipboard->setKeysRouted(false);
}

// What the window's own focus would do with the key: to the item that has
// the focus in it (not the active focus, the window has none), then up its
// parents until one takes it.
void App::routedKey(const KeyRouter::Key& key)
{
    if (!m_clipWindow || !m_clipWindow->isVisible())
        return;
    if (key.press && (key.vk == VK_APPS || (key.vk == VK_F10 && key.modifiers == Qt::ShiftModifier))) {
        emit m_clipboard->contextMenuKeyPressed(); // as the window's ContextMenu event (eventFilter)
        return;
    }
    if (key.press && key.vk == VK_F4 && key.modifiers.testFlag(Qt::AltModifier)) {
        m_clipboard->setPinned(false); // closed, pinned or not
        m_clipboard->dismiss(); // closes it, not the program behind
        return;
    }
    QQuickItem* item = m_clipWindow->contentItem();
    while (item->isFocusScope() && item->scopedFocusItem() && item->scopedFocusItem()->isEnabled())
        item = item->scopedFocusItem();
    QKeyEvent event(key.press ? QEvent::KeyPress : QEvent::KeyRelease, key.key, key.modifiers, key.scanCode, key.vk, 0,
                    key.text, key.autoRepeat);
    if (key.press && event.matches(QKeySequence::Paste) && textfield::pasteOneLine(item))
        return; // as eventFilter does for the window's own keys
    for (; item; item = item->parentItem()) {
        event.setAccepted(true);
        QCoreApplication::sendEvent(item, &event);
        if (event.isAccepted())
            break;
    }
}

void App::routedForeground(HWND window)
{
    if (window == m_keyRouterTarget)
        return;
    // Activated after all: from now on as any window with the focus, which
    // goes when it loses it. Only if it still is: the event may be old.
    const HWND clip = m_clipWindow ? reinterpret_cast<HWND>(m_clipWindow->winId()) : nullptr;
    if (clip && window == clip) {
        if (::GetForegroundWindow() == clip)
            stopKeyRouting();
        return;
    }
    hideClipboard(); // another program in front: as a menu
}

void App::activateClipboard()
{
    if (!m_clipWindow || (!m_keyRouter && !m_clipboard->pinned()))
        return;
    if (!m_keyRouter) // pinned, the keys elsewhere: it takes no focus until told
        m_clipFrame->setNoActivate(false);
    // Active first, then no longer drawn as active without being so: the
    // backdrop does not flicker. Keys pressed in between are its own already.
    win::bringToFront(m_clipWindow);
    m_clipWindow->requestActivate();
    stopKeyRouting();
}

void App::releaseClipboard(bool giveFocus)
{
    const bool hadFocus = m_clipWindow->isActive();
    stopKeyRouting();
    m_clipFrame->setNoActivate(true); // clicks paste into the program in front, which keeps the focus
    if (giveFocus && hadFocus) {
        if (const HWND target = paste::usableTarget(m_clipboard->target()))
            paste::activate(target);
    }
}

void App::engageClipboard()
{
    if (!m_clipWindow || !m_clipWindow->isVisible() || m_keyRouter)
        return;
    // As when it opens over a program: that one keeps the focus, the keys
    // come here. Its own window in front (nothing else to paste into): it
    // takes the focus, and pastes where it did last.
    const HWND foreground = ::GetForegroundWindow();
    if (const HWND target = paste::usableTarget(foreground)) {
        m_clipboard->setTarget(target);
        if (!win::isShellFlyout(foreground) && startKeyRouting(target)) {
            m_clipFrame->setNoActivate(true);
            return;
        }
    }
    m_clipFrame->setNoActivate(false);
    win::bringToFront(m_clipWindow);
    m_clipWindow->requestActivate();
}

void App::clipboardPinnedChanged()
{
    if (!m_clipWindow || !m_clipWindow->isVisible() || !m_clipboard->active() || m_clipPrewarming.load())
        return;
    if (m_clipboard->pinned())
        releaseClipboard(false); // the keys back to the program in front; the window stays
    else
        engageClipboard(); // a menu again: clicking elsewhere hides it
}

void App::uncloakClipboard()
{
    m_clipUncloakTimeout.stop();
    if (!m_clipWindow)
        return;
    win::setCloaked(m_clipWindow, false);
    // The logo when it is on its own: under a box, that box's window has one.
    if (m_clipWindow->isVisible() && !m_clipboard->field() && m_clipLogo)
        m_clipLogo->reveal();
}

void App::hideClipboard(bool launcherToo)
{
    if (m_clipboard->dragging())
        return;
    if (m_clipboard->pinned() && m_clipWindow && m_clipWindow->isVisible() && !m_clipPrewarming.load()) {
        releaseClipboard(false); // clicked elsewhere, another program in front, the launcher opened
        return;
    }
    if (m_keyRouter)
        m_keyRouter->drain(); // first: the keys go to the program from here on
    if (!m_clipWindow || !m_clipWindow->isVisible() || m_clipPrewarming.load()) {
        stopKeyRouting();
        return;
    }
    const bool underLauncher = clipboardUnderLauncher();
    m_clipWindow->hide();
    stopKeyRouting(); // hidden first: no inactive backdrop on the way out
    m_clipUncloak = false;
    m_clipUncloakTimeout.stop();
    win::setCloaked(m_clipWindow, false);
    m_clipboard->setActive(false); // the launcher's rows come back
    m_clipboard->handleHidden();
    // Gone without going back to the search box (pasted elsewhere, clicked
    // away): the launcher goes too.
    if (launcherToo && underLauncher && m_window && !m_window->isActive())
        hideLauncher();
}

bool App::clipboardUnderLauncher() const
{
    const QQuickItem* field = m_clipboard->field();
    return m_clipboard->active() && field && field->window() == m_window;
}

// The first frame a window draws sets up the graphics device, shaders and
// glyphs: drawn ahead, as for the launcher (see prewarmLauncher).
void App::prewarmClipboard()
{
    if (!m_settings.clipboard || (m_clipWindow && m_clipWindow->isVisible()))
        return;
    if (!m_clipWindow && !createClipWindow())
        return;
    m_clipPlacement->placeOn(QGuiApplication::primaryScreen()); // drawn at that scale
    m_clipPrewarming = true;
    win::setCloaked(m_clipWindow, true);
    m_clipWindow->setProperty("_q_showWithoutActivating", true);
    m_clipWindow->show();
    m_clipWindow->setProperty("_q_showWithoutActivating", QVariant());
    QTimer::singleShot(2s, this, &App::finishClipPrewarm); // should no frame come
}

void App::finishClipPrewarm()
{
    if (!m_clipPrewarming.exchange(false))
        return;
    m_clipWindow->hide();
    win::setCloaked(m_clipWindow, false);
}

QQuickItem* App::focusedField() const
{
    auto* window = qobject_cast<QQuickWindow*>(QGuiApplication::focusWindow());
    if (!window || window == m_clipWindow || reinterpret_cast<HWND>(window->winId()) != ::GetForegroundWindow())
        return nullptr; // the clipboard's own search box included
    QQuickItem* item = window->activeFocusItem();
    return textfield::takesTyping(item) ? item : nullptr;
}

// The field's window comes to the front again, which hides the clipboard as
// any window would (see createClipWindow); the field gets the keyboard back
// with it (Clipboard waits for that). The launcher under it stays.
void App::returnToField(QQuickItem* field)
{
    QQuickWindow* window = field->window();
    if (!window)
        return;
    // The dialog bar shows only while its dialog (or a window of ours) is in
    // front: should it be gone, the dialog first, then the bar as soon as it
    // is up again (see createBarWindow).
    if (window == m_barWindow && m_dialogBar && !m_barWindow->isVisible()) {
        m_barFocusDeadline.setRemainingTime(1s);
        m_dialogBar->back();
        return;
    }
    win::bringToFront(window);
    window->requestActivate();
}

void App::armReveal()
{
    m_revealArmed.store(true);
    m_window->update(); // a new frame even if the results did not change
}

void App::revealLauncher()
{
    if (!m_revealing)
        return;
    m_revealing = false;
    m_revealArmed.store(false);
    m_revealSynced.store(false);
    m_revealTimeout.stop();
    if (!m_window)
        return;
    // In from off its place first, then seen, the logo with it: not if this
    // was the launcher hiding before its first frame.
    if (m_window->isVisible())
        m_placement->slideIn();
    win::setCloaked(m_window, false);
    if (m_launcherLogo && launcherShown())
        m_launcherLogo->reveal();
}

bool App::launcherShown() const
{
    return m_window && m_window->isVisible() && !m_launcherClosing && !m_prewarming.load();
}

// The first frame a window draws sets up the graphics device, shaders and
// glyphs. Drawn once ahead, cloaked and without taking the focus, so the first
// open is as quick as later ones (which reuse all that: it is kept while the
// window is hidden).
void App::prewarmLauncher()
{
    win::prepareTextInput(); // also for the tray menu, which takes the focus
    if (!m_window || m_window->isVisible())
        return;
    m_prewarming = true;
    win::setCloaked(m_window, true);
    m_window->setProperty("_q_showWithoutActivating", true);
    m_window->show();
    m_window->setProperty("_q_showWithoutActivating", QVariant());
    QTimer::singleShot(2s, this, &App::finishPrewarm); // should no frame come
}

void App::finishPrewarm()
{
    if (!m_prewarming.exchange(false))
        return;
    m_window->hide();
    win::setCloaked(m_window, false);
}

void App::hideLauncher()
{
    if (!m_window || !m_window->isVisible() || m_launcherClosing)
        return;
    if (m_launcher->systemMenuOpen())
        ::EndMenu(); // the shell's menu over it (Shift+right-click) goes with it: double Ctrl, say
    m_launcherHidden.start();
    m_boxPressGrace.stop();
    m_deliverTyping = false;
    if (clipboardUnderLauncher())
        hideClipboard(false);
    // Slid out, unless it was never seen (still waiting for its first frame).
    if (m_revealing || m_prewarming.load()) {
        finishHidingLauncher();
        return;
    }
    m_launcherClosing = true;
    // The box idle at once; the launcher keeps its look (in the box: no
    // field) until it is gone.
    if (m_searchBox && m_launcherOverBox) {
        m_searchBox->setText({}); // the placeholder again
        updateSearchBoxLook();
    }
    m_placement->slideOut([this] { // the logo goes along (WindowLogo)
        if (m_launcherClosing)
            finishHidingLauncher();
    });
}

void App::finishHidingLauncher()
{
    m_launcherClosing = false;
    m_launcherOverBox = false;
    if (m_launcher->inTaskbarBox()) {
        m_launcher->setInTaskbarBox(false);
        if (m_searchBox) {
            m_searchBox->setText({});
            updateSearchBoxLook();
        }
    }
    if (!m_window || !m_window->isVisible())
        return;
    m_window->hide();
    revealLauncher(); // hidden before its first frame: drop the cloak
    m_launcher->handleHidden();
}

void App::showTrayMenu()
{
    // The click that asked for the menu was the last input (tick count, ms).
    LASTINPUTINFO input {sizeof input};
    ::GetLastInputInfo(&input);
    const DWORD asked = ::GetTickCount();
    const bool ready = m_index->state() == IndexService::State::Ready;
    const QString info = ready ? tr("已索引 %Ln 项", nullptr, static_cast<int>(m_index->itemCount()))
                               : tr("正在建立索引…");
    const QString show = tr("打开 Win顺");
    std::vector<shell::MenuItem> items {
        {ShowCommand, m_settings.doubleCtrl ? show + u'\t' + doubleCtrlName() : show, false, true, true},
        {ClipboardCommand, m_clipboardShortcut.isEmpty() ? tr("剪贴板历史") : tr("剪贴板历史") + u'\t' + m_clipboardShortcut},
        {PauseClipboardCommand, tr("暂停记录剪贴板"), m_clipboardPaused, m_settings.clipboard},
        shell::MenuItem::separator(),
        {IndexInfo, info, false, false},
        {RebuildCommand, tr("重建索引")},
        shell::MenuItem::separator(),
        {AutostartCommand, tr("开机自动启动"), autostart::isEnabled()},
        {SettingsCommand, tr("设置…")},
        {CheckUpdateCommand, tr("检查更新…") + u'\t' + QCoreApplication::applicationVersion()},
        shell::MenuItem::separator(),
        {RestartCommand, tr("重新启动")},
        {QuitCommand, tr("退出")},
    };
    // Files being read for the content index: how far (the disk is busy meanwhile).
    if (ready && m_index->readingContent()) {
        const ContentIndexer::Progress progress = m_index->contentIndexer()->progress();
        const auto at = std::ranges::find_if(items, [](const shell::MenuItem& i) { return i.id == IndexInfo; });
        items.insert(at + 1,
            {ContentInfo,
                tr("正在建立内容索引 %1 / %2")
                    .arg(QLocale().toString(static_cast<qulonglong>(progress.done)),
                        QLocale().toString(static_cast<qulonglong>(progress.total))),
                false, false});
    }
    // The index service not reached (GitHub #21): said here, the reason in the settings.
    if (m_serviceWanted && m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None) {
        const auto at = std::ranges::find_if(items, [](const shell::MenuItem& i) { return i.id == IndexInfo; });
        items.insert(at + 1, {AccessInfo, tr("索引服务未连接，正在遍历文件夹"), false, false});
    }
    if (m_updater->available() && !m_updater->skipped()) // at the top, where it is seen
        items.insert(items.begin(),
            {{UpdateCommand, tr("Win顺 %1 可以更新了…").arg(m_updater->availableVersion())}, shell::MenuItem::separator()});
    POINT pos {};
    ::GetCursorPos(&pos);
    const DWORD built = ::GetTickCount();
    // A slow menu leaves a line in the log: which part it waited on.
    const int chosen = shell::popupMenu(m_messages->hwnd(), items, pos, [&] {
        const DWORD shown = ::GetTickCount();
        if (shown - input.dwTime >= kSlowMenuMs)
            qWarning().nospace() << "Tray menu shown " << shown - input.dwTime << " ms after the click: "
                                 << asked - input.dwTime << " ms until it was asked for, " << built - asked
                                 << " ms to build, " << shown - built << " ms for Windows to show it";
    });
    if (chosen != QuitCommand && chosen != RestartCommand)
        autostart::refresh(); // changed outside WinShun? Ready for the next time
    switch (chosen) {
    case ShowCommand:
        showLauncher();
        break;
    case ClipboardCommand:
        showClipboard();
        break;
    case PauseClipboardCommand:
        setClipboardPaused(!m_clipboardPaused);
        break;
    case RebuildCommand:
        m_index->rebuild();
        m_messages->showNotification(tr("正在重建索引"), tr("可以照常搜索，完成后结果会自动更新。"));
        break;
    case AutostartCommand:
        autostart::setEnabled(!autostart::isEnabled());
        break;
    case SettingsCommand:
        showSettings();
        break;
    case UpdateCommand:
        showUpdate();
        break;
    case CheckUpdateCommand:
        // A version already known is shown at once; otherwise the dialog says "checking".
        if (!m_updater->available())
            m_updater->check(true);
        showUpdate();
        break;
    case RestartCommand:
        restart({u"--background"_s});
        break;
    case QuitCommand:
        QGuiApplication::quit();
        break;
    default:
        break;
    }
}

void App::restart(const QStringList& arguments, Elevation elevation)
{
    m_restartArguments = arguments;
    m_restartElevation = elevation;
    // From the settings: they open again where they were.
    if (arguments.contains(u"--settings"_s) && !arguments.contains(u"--settings-at"_s) && m_settingsWindow
        && m_settingsWindow->isVisible()) {
        QVariant place;
        if (QMetaObject::invokeMethod(m_settingsWindow, "place", Q_RETURN_ARG(QVariant, place)) && place.isValid())
            *m_restartArguments << u"--settings-at"_s << place.toString();
    }
    QGuiApplication::quit();
}

// ---- 索引方式: how the volumes are read (GitHub #21) --------------------------

void App::applyAutostartLevel()
{
    const std::optional<bool> task = autostart::taskElevated();
    // A portable copy upgraded in place: its task from before 0.8.0 runs it
    // elevated at login, while a double click runs it with normal rights now
    // (the manifest's asInvoker), with another way to the volumes and other
    // folders seen. The old way is kept, as the mode, said so in the
    // settings; another mode chosen rewrites the task. An installed copy's
    // task is rewritten by the installer (--take-autostart).
    if (task == true && m_settings.indexAccess == u"auto" && !Settings::portableFolder().isEmpty()) {
        m_settings.indexAccess = u"direct"_s;
        m_settings.save();
        m_accessTakenFromTask = true; // worded in refreshIndexAccess, once the language is applied
    }
    autostart::setElevated(m_settings.indexAccess == u"direct" && userIsAdministrator());
    reconcileAutostart(task);
}

void App::reconcileAutostart(const std::optional<bool>& task)
{
    const bool want = autostart::elevated();
    if (!task || *task == want)
        return;
    // Only an elevated process may: a task that runs elevated is refused to
    // one with normal rights even to make it run with those (or to delete
    // it; measured 2026-10-14, pitfalls), and one may not raise a task
    // either. The elevated copy does it as it starts: at login, by the very
    // task, or restarted as administrator from the settings.
    if (!integrity::processElevated())
        return;
    qWarning("WinShun: the autostart task now starts Win顺 %s", want ? "as administrator" : "with normal rights");
    autostart::setEnabled(true);
}

void App::applyIndexAccess()
{
    const bool elevated = integrity::processElevated();
    const bool wantElevated = m_settings.indexAccess == u"direct" && userIsAdministrator();
    autostart::setElevated(wantElevated);
    if (wantElevated && !elevated) {
        // Only an elevated process may register the task so: the copy started
        // as administrator does, as it starts. UAC asks once.
        restart({u"--settings"_s, u"--settings-at"_s, u"indexAccess"_s}, Elevation::Elevated);
        return;
    }
    reconcileAutostart(autostart::taskElevated()); // elevated, this copy may write either level
    if (!wantElevated && elevated) {
        restart({u"--settings"_s, u"--settings-at"_s, u"indexAccess"_s}, Elevation::Unelevated); // the user's normal rights, as the task has it now
        return;
    }
    // The same rights as before: the way to the volumes changes in place.
    AccessChoice choice = chooseVolumeAccess(m_settings.indexAccess);
    m_serviceWanted = choice.wantsService;
    m_serviceWhy = choice.why;
    const bool none = choice.access->kind() == ntfs::VolumeAccess::Kind::None;
    m_index->setVolumeAccess(std::move(choice.access));
    m_serviceRetry.stop();
    if (m_serviceWanted) {
        if (none)
            startServiceRetry();
        else
            m_serviceRetry.start(kServiceRecheck);
    }
    refreshIndexAccess();
}

void App::startServiceRetry()
{
    m_serviceRetrySince.start();
    m_serviceRetry.start(kServiceRetry);
}

void App::probeService()
{
    if (m_serviceProbing.exchange(true))
        return; // the last look is not back yet
    if (m_serviceProbe.joinable())
        m_serviceProbe.join(); // done: its outcome was handled (or dropped) on this thread
    // Not on the GUI thread: connecting waits for an instance of the pipe
    // (2 s at most) when every one is taken.
    m_serviceProbe = std::jthread([this] {
        auto service = std::make_shared<ntfs::ServiceVolumeAccess>();
        const bool reached = service->probe();
        QMetaObject::invokeMethod(
            this, [this, service = std::move(service), reached] { serviceProbed(service, reached); },
            Qt::QueuedConnection);
    });
}

void App::serviceProbed(std::shared_ptr<ntfs::ServiceVolumeAccess> service, bool reached)
{
    m_serviceProbing = false;
    if (!m_serviceWanted || !m_index)
        return;
    if (reached) {
        // Not in use yet (it came up after us), or in use but failing of late
        // (away for a while: its journals were given up, those volumes
        // walked): taken from now on, the walked volumes read from their MFT.
        const auto inUse = std::dynamic_pointer_cast<ntfs::ServiceVolumeAccess>(m_index->volumeAccess());
        if (!inUse || !inUse->lastError().empty()) {
            qWarning("WinShun: the index service (%ls) is used from now on", service->serviceVersion().c_str());
            m_index->setVolumeAccess(service);
        }
        m_serviceWhy.clear();
    } else {
        m_serviceWhy = QString::fromStdWString(service->lastError());
    }
    const bool none = m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::None;
    const bool soon = none && m_serviceRetrySince.isValid()
        && m_serviceRetrySince.elapsed() < std::chrono::milliseconds(kServiceWait).count();
    m_serviceRetry.start(soon ? kServiceRetry : kServiceRecheck);
    refreshIndexAccess();
}

QString App::serviceProblem() const
{
    if (m_serviceWhy.contains(u"not running")) {
        return ntfs::ServiceVolumeAccess::installed()
            ? tr("索引服务没有运行（Windows 服务“WinShun”已停止或被禁用）")
            : tr("没有安装索引服务");
    }
    if (m_serviceWhy.contains(u"another version"))
        return tr("索引服务的版本和 Win顺 不配，请重新运行安装程序");
    return m_serviceWhy;
}

void App::refreshIndexAccess()
{
    if (!m_settingsEditor)
        return;
    SettingsEditor::IndexAccessState state;
    const ntfs::VolumeAccess::Kind kind = m_index->volumeAccessKind();
    state.elevated = integrity::processElevated();
    state.administrator = userIsAdministrator();
    state.portable = !Settings::portableFolder().isEmpty();
    state.serviceInstalled = ntfs::ServiceVolumeAccess::installed();
    state.serviceBusy = m_serviceBusy;
    state.serviceMessage = m_serviceMessage;
    switch (kind) {
    case ntfs::VolumeAccess::Kind::Direct:
        state.effective = u"direct"_s;
        state.status = tr("现在：以管理员身份直接读取磁盘的主文件表和变更日志");
        break;
    case ntfs::VolumeAccess::Kind::Service: {
        state.effective = u"service"_s;
        const auto service = std::dynamic_pointer_cast<ntfs::ServiceVolumeAccess>(m_index->volumeAccess());
        const QString version = service ? QString::fromStdWString(service->serviceVersion()) : QString();
        state.status = version.isEmpty() ? tr("现在：通过索引服务读取磁盘的主文件表和变更日志")
                                         : tr("现在：通过索引服务读取磁盘的主文件表和变更日志（服务版本 %1）").arg(version);
        break;
    }
    case ntfs::VolumeAccess::Kind::None:
        state.effective = u"walk"_s;
        state.status = m_serviceWanted ? tr("现在：按文件夹遍历，因为%1").arg(serviceProblem())
                                       : tr("现在：按文件夹遍历，不读取主文件表");
        break;
    }
    QStringList notes;
    if (m_settings.indexAccess == u"direct" && !state.elevated) {
        notes << (state.administrator ? tr("“管理员直读”要以管理员身份运行；这次是以普通权限启动的，点“以管理员身份重新启动”。")
                                      : tr("这个账户不是管理员，“管理员直读”在这里没有意义，请改选别的方式。"));
    }
    if (m_accessTakenFromTask)
        notes << tr("升级前开机自启是以管理员身份运行的，所以记成了“管理员直读”；改选别的方式，开机自启就改用普通权限。");
    if (const std::optional<bool> task = autostart::taskElevated(); task && *task != autostart::elevated()) {
        // Written by the elevated copy only (reconcileAutostart).
        notes << (*task ? tr("开机自启的任务还在以管理员身份启动 Win顺：下次登录由它启动时会改成普通权限。")
                        : tr("开机自启的任务还在以普通权限启动 Win顺：以管理员身份重新启动后会改过来。"));
    }
    state.note = notes.join(u'\n');
    m_settingsEditor->setIndexAccessState(state);
}

void App::installIndexService(bool install)
{
    if (m_serviceBusy)
        return;
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + u"/WinShunService.exe"_s);
    if (!QFile::exists(exe)) {
        m_serviceMessage = tr("Win顺 旁边没有 WinShunService.exe");
        refreshIndexAccess();
        return;
    }
    m_serviceBusy = true;
    m_serviceMessage = install ? tr("正在安装索引服务…") : tr("正在卸载索引服务…");
    refreshIndexAccess();
    // Registered from where this copy is: moved, it has to be installed again.
    shell::runElevated(exe.toStdWString(), install ? L"--install" : L"--uninstall",
        [this, install](std::optional<unsigned long> code) {
            QMetaObject::invokeMethod(
                this,
                [this, install, code] {
                    m_serviceBusy = false;
                    if (!code) {
                        m_serviceMessage = tr("没有进行：需要管理员权限（在 UAC 里选“是”）");
                    } else if (*code != 0) {
                        m_serviceMessage = (install ? tr("安装索引服务失败（错误 %1）") : tr("卸载索引服务失败（错误 %1）"))
                                               .arg(*code);
                    } else if (install) {
                        m_serviceMessage = tr("索引服务已安装并启动。移动了 Win顺 的文件夹后要重新安装。");
                        probeService(); // taken into use as soon as it answers
                    } else {
                        m_serviceMessage = tr("索引服务已卸载。");
                        if (m_index->volumeAccessKind() == ntfs::VolumeAccess::Kind::Service)
                            m_index->setVolumeAccess(std::make_shared<ntfs::NoVolumeAccess>()); // walked and watched from here on
                    }
                    refreshIndexAccess();
                },
                Qt::QueuedConnection);
        });
}

// ---- AI 助手: the MCP server --------------------------------------------------

namespace {

// Each call the AI clients make, in %LOCALAPPDATA%\WinShun\mcp.log (over
// 1 MB it becomes mcp.old.log and a new one starts).
void logMcpCall(const QString& line)
{
    const QString path = Settings::dataDir() + u"\\mcp.log"_s;
    if (QFile::exists(path) && QFile(path).size() > (1 << 20)) {
        const QString old = Settings::dataDir() + u"\\mcp.old.log"_s;
        QFile::remove(old);
        QFile::rename(path, old);
    }
    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text))
        file.write(line.toUtf8() + '\n');
}

QString clipKindName(ClipKind kind)
{
    switch (kind) {
    case ClipKind::Link:
        return u"link"_s;
    case ClipKind::Path:
        return u"path"_s;
    case ClipKind::Files:
        return u"files"_s;
    case ClipKind::Image:
        return u"image"_s;
    case ClipKind::Text:
        break;
    }
    return u"text"_s;
}

} // namespace

mcp::Policy App::mcpPolicy() const
{
    mcp::Policy policy;
    policy.allowOpen = m_settings.mcpAllowOpen;
    policy.allowClipboard = m_settings.mcpClipboard;
    policy.excludedPaths = m_settings.mcpExcludedPaths;
    policy.content = m_settings.contentSearch;
    policy.contentExtensions = m_settings.contentExtensions;
    policy.contentSizeLimits = m_settings.contentSizeLimits();
    policy.contentDocuments = m_settings.contentDocuments;
    policy.contentInLowPriority = m_settings.contentInLowPriority;
    policy.kindOrder = m_settings.foldersFirst ? KindOrder::FoldersFirst : KindOrder::FilesFirst;
    return policy;
}

// What the tool hosts need from the app: the AI's (applyMcp) and the
// plugins' (pluginTools) alike, each on an engine of its own.
mcp::Services App::toolServices() const
{
    mcp::Services services;
    services.index = m_index.get();
    services.version = QStringLiteral(WINSHUN_VERSION);
    services.history = [this] { return m_history->items(); };
    services.open = [](const QString& path, std::function<void(bool)> done) { shell::open(path, false, std::move(done)); };
    services.reveal = [](const QString& path, std::function<void(bool)> done) {
        shell::reveal(path);
        done(true);
    };
    services.clipboard = [this](const mcp::ClipQuery& query) {
        std::vector<mcp::ClipSummary> out;
        ClipFilter filter;
        filter.query = query.query;
        for (const int row : m_clipStore->find(filter)) {
            const Clip& c = m_clipStore->clips()[static_cast<std::size_t>(row)];
            const QString kind = clipKindName(c.kind);
            if (query.kind != u"all" && kind != query.kind)
                continue;
            if (std::ssize(out) >= query.limit)
                break;
            mcp::ClipSummary summary;
            summary.id = c.id;
            summary.kind = kind;
            summary.text = c.text.left(query.maxChars);
            summary.chars = c.textLength;
            summary.created = c.created;
            summary.used = c.used;
            summary.source = c.source;
            if (const ClipGroup* group = c.group != 0 ? m_clipStore->group(c.group) : nullptr)
                summary.group = group->name;
            summary.pinned = c.group == ClipStore::kPinned;
            summary.width = c.width;
            summary.height = c.height;
            out.push_back(std::move(summary));
        }
        return out;
    };
    services.clipboardText = [this](qint64 id) -> std::optional<QString> {
        if (!m_clipStore->clip(id))
            return std::nullopt;
        return m_clipStore->payload(id).text;
    };
    services.log = [](const QString& line) { logMcpCall(line); };
    return services;
}

void App::applyMcp()
{
    if (!m_settings.mcp) {
        m_aiBridge.reset();
        m_mcpServer.reset();
        m_mcpHost.reset();
        m_mcpEngine.reset();
        refreshMcpStatus();
        return;
    }
    if (!m_mcpServer) {
        // Its own engine: an engine runs only its newest request, so one
        // shared with the launcher would drop the launcher's searches.
        m_mcpEngine = std::make_unique<SearchEngine>(m_index.get(), m_apps.get(), m_places.get());
        mcp::Services services = toolServices();
        services.engine = m_mcpEngine.get();
        m_mcpHost = std::make_unique<mcp::ToolHost>(std::move(services));
        m_mcpHost->setPolicy(mcpPolicy());
        m_mcpServer = std::make_unique<mcp::McpServer>(m_mcpHost.get(), QStringLiteral(WINSHUN_VERSION));
        connect(m_mcpServer.get(), &mcp::McpServer::sessionsChanged, this, &App::refreshMcpStatus);
        connect(m_mcpHost.get(), &mcp::ToolHost::toolsChanged, this, &App::writeMcpToolsCache);
        DWORD error = 0;
        if (!m_mcpServer->listen(pipe::defaultName(), &error))
            qWarning("MCP: cannot serve on the pipe (error %lu)", error);
        writeMcpToolsCache();
    } else {
        m_mcpHost->setPolicy(mcpPolicy());
    }
    applyAiBridge();
    refreshMcpStatus();
}

void App::applyAiBridge()
{
    if (!m_mcpHost || !m_pluginHost)
        return;
    if (!m_aiBridge)
        m_aiBridge = std::make_unique<plugin::AiBridge>(m_pluginHost.get(), m_mcpHost.get());
    plugin::AiBridge::Options options;
    options.enabled = m_settings.mcpPlugins;
    options.actions = m_settings.mcpPluginActions;
    for (const PluginConfig& c : m_settings.plugins) {
        if (!c.ai)
            options.excluded.insert(c.id);
    }
    m_aiBridge->setOptions(options);
}

// The plugins' tools: not the AI's host (its switch is the AI's, and its
// engine would make the two cancel each other), one of their own, made
// when a plugin first asks (plugin::Tools, the plugin design §6.3).
plugin::Tools& App::pluginTools()
{
    if (!m_pluginTools) {
        m_pluginTools = std::make_unique<plugin::Tools>(toolServices(), m_apps.get(), m_places.get());
        m_pluginTools->setPolicy(mcpPolicy());
    }
    return *m_pluginTools;
}

// ---- 插件: the plugin host ------------------------------------------------------

void App::createPluginHost()
{
    const QString dataDir = Settings::dataDir();
    const QString appDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    plugin::Host::Options options;
    options.userPluginDir = dataDir + u"\\plugins"_s;
    if (QFileInfo::exists(appDir + u"\\plugins"_s))
        options.bundledPluginDir = appDir + u"\\plugins"_s;
    options.logPath = dataDir + u"\\plugins.log"_s;
    options.runnerExe = appDir + u"\\WinShunPluginHost.exe"_s;
    options.locale = m_language;
    options.theme = isDarkMode() ? u"dark"_s : u"light"_s;
    options.version = QStringLiteral(WINSHUN_VERSION);
    m_pluginHost = std::make_unique<plugin::Host>(options);
    connect(&m_pluginHost->catalog(), &plugin::Catalog::changed, this, &App::pluginsChanged);
    connect(m_pluginHost.get(), &plugin::Host::disabledAfterCrashes, this, [this](const QString& id) {
        const plugin::Entry* e = m_pluginHost->catalog().find(id);
        const QString name = e && e->manifest ? e->manifest->name.pick(m_language) : id;
        m_messages->showNotification(tr("插件已停用"), tr("“%1” 连续崩溃，已停用。详见 plugins.log。").arg(name));
    });
    connect(m_pluginHost.get(), &plugin::Host::reloaded, this, [this](const QString& id) {
        const plugin::Entry* e = m_pluginHost->catalog().find(id);
        m_launcher->flash(tr("已重新加载 %1").arg(e && e->manifest ? e->manifest->name.pick(m_language) : id));
    });
    // Its winshun/tool calls: the plugins' own tools, made when first asked.
    plugin::ToolBridge bridge;
    bridge.call = [this](std::uint64_t callId, const QString& client, const QString& tool, const QJsonObject& arguments,
                      std::function<void(mcp::ToolResult)> done) { pluginTools().host().call(callId, client, tool, arguments, std::move(done)); };
    bridge.cancel = [this](std::uint64_t callId) {
        if (m_pluginTools)
            m_pluginTools->host().cancel(callId);
    };
    bridge.known = [this](const QString& tool) { return pluginTools().host().known(tool); };
    m_pluginHost->setToolBridge(std::move(bridge));
    pluginsChanged();
    m_pluginHost->catalog().setWatching(true); // a folder put into plugins\ is a plugin at once
    m_launcher->setPluginHost(m_pluginHost.get());
    // Resident plugins that want starting early: after the start-up rescan
    // has had its say (see RescanOnStartup), not with it.
    QTimer::singleShot(10s, this, [this] {
        if (m_pluginHost)
            m_pluginHost->preload();
    });
    // On the way out: shutdown requests, two seconds, then the job ends them.
    // Not the two seconds when Windows ends the session (Qt says aboutToQuit
    // then too): they are the seconds the index needs to be saved.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (m_pluginHost)
            m_pluginHost->shutdown(::GetSystemMetrics(SM_SHUTTINGDOWN) == 0);
    });
}

void App::pluginsChanged()
{
    QHash<QString, QString> folders;
    for (const plugin::Entry& e : m_pluginHost->catalog().entries())
        folders.insert(e.id, e.dir);
    FileIconProvider::setPluginFolders(std::move(folders));
}

void App::applyPlugins()
{
    if (!m_pluginHost)
        return;
    m_pluginHost->setDeveloperMode(m_settings.pluginDeveloperMode);
    if (m_pluginTools)
        m_pluginTools->setPolicy(mcpPolicy());
    plugin::Catalog& catalog = m_pluginHost->catalog();
    QSet<QString> enabled;
    QHash<QString, QString> overrides;
    for (const PluginConfig& c : m_settings.plugins) {
        if (c.enabled)
            enabled.insert(c.id);
        for (const auto& [command, keyword] : c.keywords)
            overrides.insert(c.id + u'/' + command, keyword);
    }
    // Through the host where the state changes (it forgets a plugin's
    // crashes when it is turned on again), the catalog otherwise.
    for (const plugin::Entry& e : catalog.entries()) {
        if (catalog.enabled(e.id) != enabled.contains(e.id))
            m_pluginHost->setEnabled(e.id, enabled.contains(e.id));
    }
    catalog.setEnabled(enabled); // the ones not installed yet count too
    m_pluginHost->setKeywordOverrides(overrides);
    QList<std::pair<QString, QString>> web;
    for (const WebShortcut& w : m_settings.webShortcuts)
        web.append({w.keyword, w.name});
    m_pluginHost->setWebKeywords(web); // [WebSearch] keeps its keywords: a plugin's alike is set aside
    m_pluginHost->setFavoriteKeywords(favoriteKeywords(*m_favorites)); // so do the favorites
    // Each plugin's settings: the INI's values by the manifest's types, the
    // passwords from the Credential Manager (never in the INI).
    for (const plugin::Entry& e : catalog.entries()) {
        if (!e.manifest || e.manifest->settings.isEmpty())
            continue;
        const PluginConfig* c = m_settings.plugin(e.id);
        m_pluginHost->setPluginSettings(e.id, plugin::secrets::settingsOf(e.id, *e.manifest, c ? c->settings : QList<std::pair<QString, QString>>()));
    }
    applyAiBridge(); // the cards' "给 AI 用" switches
}

void App::refreshMcpStatus()
{
    if (!m_settingsEditor)
        return;
    QString status;
    if (m_mcpServer && m_mcpServer->listening()) {
        QStringList names;
        for (const mcp::McpServer::SessionInfo& s : m_mcpServer->sessions()) {
            if (s.client.isEmpty())
                continue;
            const QString name = s.calls > 0 ? tr("%1（%2 次调用）").arg(s.client).arg(s.calls) : s.client;
            if (!names.contains(name))
                names.push_back(name);
        }
        status = names.isEmpty() ? tr("还没有 AI 客户端连接；连上以后显示在这里") : tr("已连接：%1").arg(names.join(u"、"_s));
    } else if (m_settings.mcp) {
        status = tr("没能建立连接通道，详见 WinShun.log");
    }
    m_settingsEditor->setMcpStatus(status);
}

void App::writeMcpToolsCache()
{
    if (!m_mcpHost)
        return;
    QFile file(Settings::dataDir() + u"\\mcp-tools.json"_s);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(m_mcpHost->tools()).toJson(QJsonDocument::Compact));
}

} // namespace ws
