#include "SettingsEditor.h"

#include "FavoritesSettings.h"
#include "FileIconProvider.h"
#include "IndexFolder.h"
#include "McpClients.h"
#include "PluginSettings.h"
#include "Query.h"
#include "ShareLister.h"
#include "SystemCatalog.h"
#include "WebShortcut.h"
#include "Win32Util.h"
#include "platform/FileManagers.h"
#include "platform/NumberKeys.h"
#include "platform/Shell.h"
#include "platform/ShortcutCapture.h"
#include "platform/SiteIcons.h"
#include "platform/TaskbarSearch.h"
#include "platform/TaskbarSearchBox.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStyleHints>
#include <QVariantMap>
#include <QWinEventNotifier>

#include <algorithm>
#include <thread>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

QString displayPath(const QString& path)
{
    return QString::fromStdWString(win32::expandEnvironment(QDir::toNativeSeparators(path.trimmed()).toStdWString()));
}

bool containsIgnoreCase(const QStringList& list, const QString& value)
{
    return list.contains(value, Qt::CaseInsensitive);
}

// Adds a program file to `list` ("KeePass", "keepass.exe", or a whole path
// pasted in); false when there is none or it is there already.
bool addProgram(QStringList& list, const QString& name)
{
    QString app = QFileInfo(name.trimmed()).fileName();
    if (app.isEmpty())
        return false;
    if (!app.endsWith(u".exe", Qt::CaseInsensitive))
        app += u".exe"_s;
    if (containsIgnoreCase(list, app))
        return false;
    list.append(app);
    return true;
}

} // namespace

// Changed outside Win顺 (the user pins the button, hides Windows' search in
// the Settings app): read again at once, so that the steps of 代替任务栏上的
// Windows 搜索 tick themselves off.
struct SettingsEditor::TaskbarWatch {
    QFileSystemWatcher pinned; // a shortcut there per program pinned to the taskbar
    win32::UniqueKey searchKey; // SearchboxTaskbarMode's
    win32::UniqueHandle searchChanged;
    std::unique_ptr<QWinEventNotifier> notifier;

    bool arm() // once per change
    {
        return searchKey.get() && searchChanged.valid()
            && ::RegNotifyChangeKeyValue(searchKey.get(), FALSE, REG_NOTIFY_CHANGE_LAST_SET, searchChanged.get(), TRUE)
            == ERROR_SUCCESS;
    }
};

SettingsEditor::SettingsEditor(const Settings& settings, const QString& runningRenderer, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_runningRenderer(runningRenderer)
    , m_knownShares(unc::knownShares())
    , m_taskbarWatch(std::make_unique<TaskbarWatch>())
    , m_plugins(new PluginSettings(this, this))
    , m_favorites(new FavoritesSettings(this, this))
{
    connect(siteicons::notifier(), &siteicons::Notifier::fetched, this, [this] {
        ++m_siteIconsArrived;
        emit siteIconArrived();
    });
    TaskbarWatch& watch = *m_taskbarWatch;
    const QString pinned = qEnvironmentVariable("APPDATA") + u"/Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar"_s;
    if (QFileInfo(pinned).isDir())
        watch.pinned.addPath(pinned);
    connect(&watch.pinned, &QFileSystemWatcher::directoryChanged, this, &SettingsEditor::refreshTaskbarState);
    ::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", 0, KEY_NOTIFY,
        watch.searchKey.out());
    watch.searchChanged = win32::UniqueHandle(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (watch.arm()) {
        watch.notifier = std::make_unique<QWinEventNotifier>(watch.searchChanged.get());
        connect(watch.notifier.get(), &QWinEventNotifier::activated, this, [this] {
            m_taskbarWatch->arm();
            refreshTaskbarState();
        });
    }
}

SettingsEditor::~SettingsEditor() = default; // here, where ShortcutCapture and TaskbarWatch are complete

void SettingsEditor::commit()
{
    m_settings.save();
    emit changed();
    emit edited(m_settings);
}

void SettingsEditor::setSettings(const Settings& settings)
{
    if (settings == m_settings)
        return;
    const bool keys = settings.numberKeys != m_settings.numberKeys || settings.hotkey != m_settings.hotkey
        || settings.clipboardNumberKeys != m_settings.clipboardNumberKeys
        || settings.clipboardHotkey != m_settings.clipboardHotkey;
    const bool shares = settings.shares != m_settings.shares;
    m_settings = settings;
    emit changed();
    if (shares)
        emit sharesChanged();
    if (keys)
        refreshNumberKeys();
}

void SettingsEditor::setFoldersFirst(bool on)
{
    if (m_settings.foldersFirst == on)
        return;
    m_settings.foldersFirst = on;
    commit();
}

void SettingsEditor::setShowModified(bool on)
{
    if (m_settings.showModified == on)
        return;
    m_settings.showModified = on;
    commit();
}

void SettingsEditor::setSort(const QString& sort)
{
    if (m_settings.sort == sort || (sort != u"match" && sort != u"newest" && sort != u"oldest"))
        return;
    m_settings.sort = sort;
    commit();
}

void SettingsEditor::setStartList(const QString& list)
{
    if (m_settings.startList == list || (list != u"opened" && list != u"modified"))
        return;
    m_settings.startList = list;
    commit();
}

void SettingsEditor::setNumberKeys(const QString& keys)
{
    if (m_settings.numberKeys == keys || (keys != u"ctrl" && keys != u"alt" && keys != u"off"))
        return;
    m_settings.numberKeys = keys;
    commit();
    refreshNumberKeys();
}

void SettingsEditor::setClipboardNumberKeys(const QString& keys)
{
    if (m_settings.clipboardNumberKeys == keys || (keys != u"ctrl" && keys != u"alt" && keys != u"off"))
        return;
    m_settings.clipboardNumberKeys = keys;
    commit();
    refreshNumberKeys();
}

namespace {

// What the user should know of the number keys `keys` ("ctrl", "alt"):
// those another program holds as global hotkeys, and those that are one of
// Win顺's own shortcuts. `whose`: whose they are when.
QString describeNumberKeys(const QString& keys, const Settings& settings, const QString& whose)
{
    if (keys != u"ctrl" && keys != u"alt")
        return {};
    const QString modifier = keys == u"alt" ? u"Alt"_s : u"Ctrl"_s;
    const auto chord = [&](int n) { return modifier + u'+' + QString::number(n); };
    QStringList own;
    QStringList others;
    const std::vector<int> held
        = NumberKeys::heldAsHotkeys(keys == u"alt" ? NumberKeys::Modifier::Alt : NumberKeys::Modifier::Ctrl);
    for (int n = 1; n <= 9; ++n) {
        const QString c = chord(n);
        const bool mine = settings.hotkey.compare(c, Qt::CaseInsensitive) == 0
            || settings.clipboardHotkey.compare(c, Qt::CaseInsensitive) == 0;
        if (mine)
            own.append(c);
        else if (std::find(held.begin(), held.end(), n) != held.end())
            others.append(c);
    }
    QStringList parts;
    if (!others.isEmpty())
        parts.append(SettingsEditor::tr("%1 也是别的程序的快捷键：%2。").arg(others.join(SettingsEditor::tr("、")), whose));
    if (!own.isEmpty())
        parts.append(SettingsEditor::tr("%1 是你给 Win顺 设的快捷键，在这里按它不再打开或关闭窗口，建议换一个。")
                         .arg(own.join(SettingsEditor::tr("、"))));
    return parts.join(u' ');
}

} // namespace

void SettingsEditor::refreshNumberKeys()
{
    const QString launcher = describeNumberKeys(
        m_settings.numberKeys, m_settings, tr("搜索框在前面时归 Win顺"));
    const QString clipboard = describeNumberKeys(m_settings.clipboardNumberKeys, m_settings,
        tr("剪贴板在别的程序上打开时归 Win顺，从 Win顺 自己的搜索框打开时归那个程序"));
    if (launcher == m_numberKeysNote && clipboard == m_clipboardNumberKeysNote)
        return;
    m_numberKeysNote = launcher;
    m_clipboardNumberKeysNote = clipboard;
    emit numberKeysNoteChanged();
}

void SettingsEditor::setDoubleCtrl(bool on)
{
    if (m_settings.doubleCtrl == on)
        return;
    m_settings.doubleCtrl = on;
    commit();
}

void SettingsEditor::setDoubleCtrlPauseInGames(bool on)
{
    if (m_settings.doubleCtrlPauseInGames == on)
        return;
    m_settings.doubleCtrlPauseInGames = on;
    commit();
}

void SettingsEditor::setDoubleCtrlPauseInFullScreen(bool on)
{
    if (m_settings.doubleCtrlPauseInFullScreen == on)
        return;
    m_settings.doubleCtrlPauseInFullScreen = on;
    commit();
}

bool SettingsEditor::addDoubleCtrlExcludedApp(const QString& name)
{
    if (!addProgram(m_settings.doubleCtrlExcludedApps, name))
        return false;
    commit();
    return true;
}

void SettingsEditor::removeDoubleCtrlExcludedApp(int index)
{
    if (index < 0 || index >= m_settings.doubleCtrlExcludedApps.size())
        return;
    m_settings.doubleCtrlExcludedApps.removeAt(index);
    commit();
}

void SettingsEditor::setHotkey(const QString& hotkey)
{
    const QString value = hotkey.trimmed();
    if (m_settings.hotkey == value)
        return;
    m_settings.hotkey = value;
    commit();
}

void SettingsEditor::setHotkeyError(const QString& error)
{
    if (m_hotkeyError == error)
        return;
    m_hotkeyError = error;
    emit hotkeyErrorChanged();
}

void SettingsEditor::setRecordingHotkey(bool recording)
{
    if (m_recordingHotkey == recording)
        return;
    m_recordingHotkey = recording;
    m_capture.reset();
    if (recording && m_window) {
        // The recorder gets its keys from the keyboard itself, ahead of Qt,
        // Windows and other programs (see ShortcutCapture), as ordinary key events.
        m_capture = std::make_unique<ShortcutCapture>(reinterpret_cast<HWND>(m_window->winId()),
            [this](const ShortcutCapture::Key& key) {
                QMetaObject::invokeMethod(this, [this, key] {
                    if (!m_recordingHotkey || !m_window)
                        return; // the recording ended meanwhile
                    QKeyEvent event(key.press ? QEvent::KeyPress : QEvent::KeyRelease, key.key, key.modifiers,
                        QString(), key.autoRepeat);
                    QCoreApplication::sendEvent(m_window, &event);
                }, Qt::QueuedConnection);
            });
        if (!m_capture->isActive())
            m_capture.reset(); // Qt's own key events then, which lack Alt+Space
    }
    emit recordingHotkeyChanged();
}

bool SettingsEditor::autostart() const
{
    return autostart::isEnabled();
}

void SettingsEditor::setAutostart(bool on)
{
    if (autostart::isEnabled() == on)
        return;
    autostart::setEnabled(on);
    emit autostartChanged();
}

void SettingsEditor::setIncludeRemovableDrives(bool on)
{
    if (m_settings.includeRemovableDrives == on)
        return;
    m_settings.includeRemovableDrives = on;
    commit();
}

void SettingsEditor::setRescanOnStartup(bool on)
{
    if (m_settings.rescanOnStartup == on)
        return;
    m_settings.rescanOnStartup = on;
    commit();
}

QStringList SettingsEditor::excludedPaths() const
{
    QStringList paths;
    for (const QString& p : m_settings.excludedPaths)
        paths.append(displayPath(p));
    return paths;
}

void SettingsEditor::setContentMaxSizeMB(int kind, int mb)
{
    if (kind < 0 || kind >= static_cast<int>(ContentSizeLimits::kKinds))
        return;
    mb = std::clamp(mb, 1, 4096);
    int& limit = m_settings.contentMaxSizeMB[static_cast<std::size_t>(kind)];
    if (limit == mb)
        return;
    limit = mb;
    commit();
}

QStringList SettingsEditor::contentKindExtensions() const
{
    constexpr qsizetype kShown = 10;
    std::array<QStringList, ContentSizeLimits::kKinds> kinds;
    for (const QString& ext : m_settings.contentExtensions)
        kinds[ContentSizeLimits::kindOf(ext.toStdString())].append(u'.' + ext);
    QStringList out;
    for (const QStringList& list : kinds)
        out.append(list.size() > kShown ? tr("%1 等").arg(list.first(kShown).join(u' ')) : list.join(u' '));
    return out;
}

void SettingsEditor::setContentSearch(bool on)
{
    if (m_settings.contentSearch == on)
        return;
    m_settings.contentSearch = on;
    commit();
}

void SettingsEditor::setContentIndex(bool on)
{
    if (m_settings.contentIndex == on)
        return;
    m_settings.contentIndex = on;
    commit();
}

void SettingsEditor::setContentInLowPriority(bool on)
{
    if (m_settings.contentInLowPriority == on)
        return;
    m_settings.contentInLowPriority = on;
    commit();
}

void SettingsEditor::setContentDocuments(bool on)
{
    if (m_settings.contentDocuments == on)
        return;
    m_settings.contentDocuments = on;
    commit();
}

void SettingsEditor::setContentIndexStatus(const QString& status)
{
    if (m_contentIndexStatus == status)
        return;
    m_contentIndexStatus = status;
    emit contentIndexStatusChanged();
}

void SettingsEditor::setRenderer(const QString& renderer)
{
    if (m_settings.renderer == renderer)
        return;
    m_settings.renderer = renderer;
    commit();
}

void SettingsEditor::setTheme(const QString& theme)
{
    if (m_settings.theme == theme)
        return;
    m_settings.theme = theme;
    commit();
}

void SettingsEditor::setLanguage(const QString& language)
{
    if (m_settings.language == language)
        return;
    m_settings.language = language;
    commit();
}

void SettingsEditor::setTransparency(const QString& transparency)
{
    if (m_settings.transparency == transparency)
        return;
    m_settings.transparency = transparency;
    commit();
}

void SettingsEditor::setRecordHistory(bool on)
{
    if (m_settings.recordHistory == on)
        return;
    m_settings.recordHistory = on;
    commit();
}

void SettingsEditor::setDialogBar(bool on)
{
    if (m_settings.dialogBar == on)
        return;
    m_settings.dialogBar = on;
    commit();
}

void SettingsEditor::setDialogBarPlace(const QString& place)
{
    if (m_settings.dialogBarPlace == place)
        return;
    m_settings.dialogBarPlace = place;
    commit();
}

void SettingsEditor::setDialogJump(bool on)
{
    if (m_settings.dialogJump == on)
        return;
    m_settings.dialogJump = on;
    commit();
}

void SettingsEditor::setFileManager(const QString& manager)
{
    if (m_settings.fileManager == manager)
        return;
    m_settings.fileManager = manager;
    commit();
}

void SettingsEditor::refreshFileManagers()
{
    QStringList managers;
    for (const auto& [kind, name] : filemanager::kSettingNames) {
        if (filemanager::installed(kind))
            managers.append(QString::fromLatin1(name));
    }
    if (managers == m_fileManagers)
        return;
    m_fileManagers = managers;
    emit fileManagersChanged();
}

void SettingsEditor::setDialogAutoJump(bool on)
{
    if (m_settings.dialogAutoJump == on)
        return;
    m_settings.dialogAutoJump = on;
    commit();
}

void SettingsEditor::removeDialogBarExcludedApp(int index)
{
    if (index < 0 || index >= m_settings.dialogBarExcludedApps.size())
        return;
    m_settings.dialogBarExcludedApps.removeAt(index);
    commit();
}

void SettingsEditor::setHistoryCount(int count)
{
    if (m_historyCount == count)
        return;
    m_historyCount = count;
    emit historyCountChanged();
}

void SettingsEditor::setAutoUpdate(bool on)
{
    if (m_settings.autoUpdate == on)
        return;
    m_settings.autoUpdate = on;
    commit();
}

void SettingsEditor::setClipboard(bool on)
{
    if (m_settings.clipboard == on)
        return;
    m_settings.clipboard = on;
    commit();
}

void SettingsEditor::setClipboardWinV(bool on)
{
    if (m_settings.clipboardWinV == on)
        return;
    m_settings.clipboardWinV = on;
    commit();
}

void SettingsEditor::setWinVState(const QString& state, bool canRestartExplorer)
{
    if (m_winVState == state && m_canRestartExplorer == canRestartExplorer)
        return;
    m_winVState = state;
    m_canRestartExplorer = canRestartExplorer;
    emit winVStateChanged();
}

void SettingsEditor::setTaskbarWinS(bool on)
{
    if (m_settings.taskbarWinS == on)
        return;
    m_settings.taskbarWinS = on;
    commit();
}

void SettingsEditor::setTaskbarStartTyping(bool on)
{
    if (m_settings.taskbarStartTyping == on)
        return;
    m_settings.taskbarStartTyping = on;
    commit();
}

void SettingsEditor::setTaskbarSearchBox(bool on)
{
    if (m_settings.taskbarSearchBox == on)
        return;
    m_settings.taskbarSearchBox = on;
    commit();
}

void SettingsEditor::setTaskbarStartGap(bool on)
{
    if (m_settings.taskbarStartGap == on)
        return;
    m_settings.taskbarStartGap = on;
    commit();
}

void SettingsEditor::setStartGapState(const QString& state, const QString& reason)
{
    if (m_startGapState == state && m_startGapReason == reason)
        return;
    m_startGapState = state;
    m_startGapReason = reason;
    emit startGapStateChanged();
}

bool SettingsEditor::searchBoxSupported() const
{
    return taskbar::SearchBox::supported();
}

void SettingsEditor::setSearchBoxShown(bool shown)
{
    if (m_searchBoxShown == shown)
        return;
    m_searchBoxShown = shown;
    emit searchBoxShownChanged();
}

void SettingsEditor::setWinSState(const QString& state, bool canRestartExplorer)
{
    if (m_canRestartExplorer != canRestartExplorer) {
        m_canRestartExplorer = canRestartExplorer;
        emit winVStateChanged(); // canRestartExplorer's
    }
    if (m_winSState == state)
        return;
    m_winSState = state;
    emit winSStateChanged();
}

namespace {

std::wstring taskbarButton()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + u"/WinShunSearch.exe").toStdWString();
}

} // namespace

void SettingsEditor::refreshTaskbarState()
{
    const bool pinned = taskbar::buttonPinned(taskbarButton());
    const bool shown = taskbar::windowsSearchShown();
    if (pinned == m_taskbarButtonPinned && shown == m_windowsSearchShown)
        return;
    m_taskbarButtonPinned = pinned;
    m_windowsSearchShown = shown;
    emit taskbarStateChanged();
}

bool SettingsEditor::pinTaskbarButton()
{
    const std::wstring button = taskbarButton();
    if (!QFileInfo::exists(QString::fromStdWString(button)))
        return false;
    // Windows asks only for a program in the Start menu: the installer's
    // shortcut, else one made now.
    const std::wstring shortcut = taskbar::buttonShortcut(button, tr("Win顺 搜索").toStdWString());
    if (shortcut.empty())
        return false;
    // Through Explorer, with the user's rights: that program goes on the
    // taskbar, so it asks for itself (and Win顺, elevated, could not).
    shell::run(u'"' + QString::fromStdWString(button) + u"\" --pin \""_s + QString::fromStdWString(shortcut) + u'"');
    return true;
}

bool SettingsEditor::showTaskbarButton()
{
    const std::wstring button = taskbarButton();
    if (!QFileInfo::exists(QString::fromStdWString(button)))
        return false;
    const std::wstring shortcut = taskbar::buttonShortcut(button, tr("Win顺 搜索").toStdWString());
    if (shortcut.empty())
        return false;
    shell::revealInExplorer(QString::fromStdWString(shortcut)); // its menu pins it
    return true;
}

void SettingsEditor::setClipboardHotkey(const QString& hotkey)
{
    if (m_settings.clipboardHotkey == hotkey)
        return;
    m_settings.clipboardHotkey = hotkey;
    commit();
}

void SettingsEditor::setClipboardHotkeyError(const QString& error)
{
    if (m_clipboardHotkeyError == error)
        return;
    m_clipboardHotkeyError = error;
    emit hotkeyErrorChanged();
}

void SettingsEditor::setClipboardMaxItems(int items)
{
    if (m_settings.clipboardMaxItems == items || items < 10)
        return;
    m_settings.clipboardMaxItems = items;
    commit();
}

void SettingsEditor::setClipboardMaxDays(int days)
{
    if (m_settings.clipboardMaxDays == days || days < 0)
        return;
    m_settings.clipboardMaxDays = days;
    commit();
}

void SettingsEditor::setClipboardImages(bool on)
{
    if (m_settings.clipboardImages == on)
        return;
    m_settings.clipboardImages = on;
    commit();
}

void SettingsEditor::setClipboardCount(int count)
{
    if (m_clipboardCount == count)
        return;
    m_clipboardCount = count;
    emit clipboardCountChanged();
}

bool SettingsEditor::addClipboardExcludedApp(const QString& name)
{
    if (!addProgram(m_settings.clipboardExcludedApps, name))
        return false;
    commit();
    return true;
}

void SettingsEditor::removeClipboardExcludedApp(int index)
{
    if (index < 0 || index >= m_settings.clipboardExcludedApps.size())
        return;
    m_settings.clipboardExcludedApps.removeAt(index);
    commit();
}

QVariantList SettingsEditor::webShortcuts() const
{
    QVariantList list;
    for (const WebShortcut& shortcut : m_settings.webShortcuts) {
        list.append(QVariantMap {
            {u"keyword"_s, shortcut.keyword},
            {u"name"_s, shortcut.name},
            {u"url"_s, shortcut.url},
            {u"home"_s, shortcut.home},
            {u"searches"_s, shortcut.searches()},
            {u"shownUrl"_s, displayWebUrl(shortcut.url)},
            {u"shownHome"_s, displayWebUrl(shortcut.homeUrl())},
            {u"homeUrl"_s, shortcut.homeUrl()},
            {u"icon"_s, shortcut.icon},
            {u"iconRef"_s, shortcut.iconRef()},
        });
    }
    return list;
}

QString SettingsEditor::webIconUrl(const QString& homeUrl, bool dark) const
{
    return FileIconProvider::webIconUrl(homeUrl, dark);
}

namespace {

// The pictures no shortcut uses any more go; `picked` is one a form still has.
void pruneWebIcons(const WebShortcuts& shortcuts, const QString& picked)
{
    QStringList inUse {picked};
    for (const WebShortcut& shortcut : shortcuts)
        inUse.append(shortcut.icon);
    siteicons::pruneCustom(inUse);
}

constexpr qsizetype kMaxWebFile = 16 << 20; // a file of shortcuts, icons and all

} // namespace

QString SettingsEditor::saveWebShortcut(int index, const QString& keyword, const QString& name, const QString& url,
    const QString& home, const QString& icon)
{
    WebShortcuts& shortcuts = m_settings.webShortcuts;
    if (index >= shortcuts.size())
        return tr("这一项已经不在了"); // the file was changed meanwhile

    WebShortcut shortcut;
    shortcut.keyword = keyword.trimmed();
    if (shortcut.keyword.isEmpty())
        return tr("请填写关键词");
    if (!isValidWebKeyword(shortcut.keyword))
        return tr("关键词里不能有空格或引号");
    for (qsizetype i = 0; i < shortcuts.size(); ++i) {
        if (i != index && shortcuts[i].keyword.compare(shortcut.keyword, Qt::CaseInsensitive) == 0)
            return tr("关键词“%1”已经给了“%2”").arg(shortcuts[i].keyword, shortcuts[i].name);
    }
    if (const QString favorite = m_favorites->keywordOwner(shortcut.keyword); !favorite.isEmpty())
        return tr("关键词“%1”已经给了收藏“%2”").arg(shortcut.keyword, favorite);
    shortcut.url = normalizeWebUrl(url);
    if (shortcut.url.isEmpty())
        return url.trimmed().isEmpty() ? tr("请填写网址") : tr("打不开这个网址：它应当以 https:// 开头");
    shortcut.home = normalizeWebUrl(home);
    if (shortcut.home.isEmpty() && !home.trimmed().isEmpty())
        return tr("打不开主页的网址：它应当以 https:// 开头");
    if (isValidCustomIconName(icon))
        shortcut.icon = icon;
    shortcut.name = name.trimmed();
    if (shortcut.name.isEmpty()) { // the site's name ("github.com"); for an app's link, the keyword
        const QString shown = displayWebUrl(shortcut.homeUrl());
        const QString site = shown.left(shown.indexOf(u'/'));
        shortcut.name = site.isEmpty() || site.contains(u':') ? shortcut.keyword : site;
    }

    m_pickedWebIcon.clear(); // saved with it, or let go
    if (index < 0)
        shortcuts.append(shortcut);
    else if (shortcuts[index] != shortcut)
        shortcuts[index] = shortcut;
    else
        return {};
    commit();
    pruneWebIcons(shortcuts, m_pickedWebIcon);
    return {};
}

void SettingsEditor::removeWebShortcut(int index)
{
    if (index < 0 || index >= m_settings.webShortcuts.size())
        return;
    m_settings.webShortcuts.removeAt(index);
    commit();
    pruneWebIcons(m_settings.webShortcuts, m_pickedWebIcon);
}

void SettingsEditor::openWebShortcut(int index)
{
    if (index >= 0 && index < m_settings.webShortcuts.size())
        shell::openUrl(m_settings.webShortcuts[index].homeUrl());
}

QVariantMap SettingsEditor::pickWebIcon()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString path = shell::pickFile(owner, tr("选择图标"),
        {{tr("图片（PNG、ICO）"), u"*.png;*.ico"_s}, {tr("所有文件"), u"*.*"_s}});
    if (path.isEmpty())
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {{u"error"_s, tr("读不了这个文件")}};
    if (file.size() > 1 << 20)
        return {{u"error"_s, tr("图片太大了：请用 1 MB 以内的 PNG")}};
    const QString icon = siteicons::keepCustom(file.readAll());
    if (icon.isEmpty())
        return {{u"error"_s, tr("用不了这张图片：请用 PNG 或 ICO，至少 16×16")}};
    m_pickedWebIcon = icon;
    return {{u"icon"_s, icon}, {u"iconRef"_s, u"custom:"_s + icon}};
}

QString SettingsEditor::importWebShortcuts()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString path = shell::pickFile(owner, tr("导入网页搜索"),
        {{tr("网页搜索（JSON）"), u"*.json"_s}, {tr("所有文件"), u"*.*"_s}});
    if (path.isEmpty())
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return tr("读不了这个文件");
    if (file.size() > kMaxWebFile)
        return tr("这个文件太大了，不像是导出的网页搜索");
    bool newer = false;
    const std::optional<WebShortcutFile> read = readWebShortcutsFile(file.readAll(), &newer);
    if (!read)
        return newer ? tr("这个文件是更新版本的 Win顺 导出的，请先更新 Win顺") : tr("这不是 Win顺 导出的网页搜索文件");

    WebShortcuts imported;
    int badIcons = 0;
    for (const WebShortcutFileEntry& entry : read->entries) {
        WebShortcut shortcut = entry.shortcut;
        if (!entry.iconData.isEmpty()) {
            shortcut.icon = siteicons::keepCustom(entry.iconData);
            badIcons += shortcut.icon.isEmpty();
        }
        // One already here keeps the picture its user gave it, when the file has none.
        if (shortcut.icon.isEmpty()) {
            for (const WebShortcut& own : m_settings.webShortcuts) {
                if (own.keyword.compare(shortcut.keyword, Qt::CaseInsensitive) == 0)
                    shortcut.icon = own.icon;
            }
        }
        imported.append(shortcut);
    }
    const WebMerge merge = mergeWebShortcuts(m_settings.webShortcuts, imported);
    if (merge.added + merge.updated > 0)
        commit();
    pruneWebIcons(m_settings.webShortcuts, m_pickedWebIcon);

    if (imported.isEmpty() && read->skipped == 0)
        return tr("文件里没有网页搜索");
    QStringList parts;
    parts.append(tr("新增 %n 个", "", merge.added));
    if (merge.updated > 0)
        parts.append(tr("更新 %n 个", "", merge.updated));
    if (merge.unchanged > 0)
        parts.append(tr("%n 个本来就有", "", merge.unchanged));
    if (read->skipped > 0)
        parts.append(tr("%n 个用不了，跳过了", "", read->skipped));
    if (badIcons > 0)
        parts.append(tr("%n 个图标读不了，用网站自己的", "", badIcons));
    // Taken all the same (a web shortcut's keyword comes first), but not unsaid.
    const int favoriteKeywords = static_cast<int>(std::ranges::count_if(
        imported, [this](const WebShortcut& s) { return !m_favorites->keywordOwner(s.keyword).isEmpty(); }));
    if (favoriteKeywords > 0)
        parts.append(tr("%n 个和收藏的关键词重复，输入时先打开网页", "", favoriteKeywords));
    return tr("已导入：") + parts.join(tr("，"));
}

QString SettingsEditor::exportWebShortcuts()
{
    const WebShortcuts& shortcuts = m_settings.webShortcuts;
    if (shortcuts.isEmpty())
        return tr("还没有网页搜索可以导出");
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString path = shell::pickSaveFile(owner, tr("导出网页搜索"), tr("Win顺 网页搜索.json"),
        tr("网页搜索（JSON）"), u"json"_s);
    if (path.isEmpty())
        return {};
    const QByteArray json = writeWebShortcutsFile(shortcuts, [](const QString& icon) { return siteicons::customPng(icon); });
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit())
        return tr("写不了这个文件：%1").arg(QDir::toNativeSeparators(path));
    return tr("已导出 %n 个到 %1", "", static_cast<int>(shortcuts.size())).arg(QDir::toNativeSeparators(path));
}

namespace {

// "刚刚", "10 分钟前", "昨天 18:02", "10月3日": when a share was last read in full.
QString sinceText(FileTime time)
{
    const QDateTime then = QDateTime::fromSecsSinceEpoch(time);
    const qint64 minutes = std::max<qint64>(0, then.secsTo(QDateTime::currentDateTime()) / 60);
    if (minutes < 1)
        return SettingsEditor::tr("刚刚");
    if (minutes < 60)
        return SettingsEditor::tr("%n 分钟前", "", static_cast<int>(minutes));
    const QDate day = then.date();
    const QDate today = QDate::currentDate();
    const QString clock = then.toString(u"HH:mm"_s);
    if (day == today)
        return SettingsEditor::tr("今天 %1").arg(clock);
    if (day == today.addDays(-1))
        return SettingsEditor::tr("昨天 %1").arg(clock);
    return SettingsEditor::tr("%1月%2日").arg(day.month()).arg(day.day());
}

QString itemsText(std::size_t items)
{
    return SettingsEditor::tr("%L1 项").arg(static_cast<qulonglong>(items));
}

// Why a share cannot be read, said for the user, with what to do about it.
QString problemText(const ShareIndexer::Status& s)
{
    using P = share::Problem;
    const QString host = QString::fromStdWString(std::wstring(unc::host(s.path)));
    switch (s.problem) {
    case P::None:
    case P::Offline:
        return SettingsEditor::tr("离线：连不上 %1（已关机、不在同一网络，或 VPN 没连）").arg(host);
    case P::Dropped:
        return SettingsEditor::tr("连接中断了，稍后自动重试");
    case P::NotFound:
        return SettingsEditor::tr("找不到这个共享文件夹：%1 在线，但上面没有它（改名或删除了？）").arg(host);
    case P::NeedsSignIn:
        return SettingsEditor::tr("需要登录：Windows 还没有 %1 的用户名和密码").arg(host);
    case P::GuestBlocked:
        return SettingsEditor::tr(
            "%1 只允许来宾（不用密码）访问，Windows 默认不允许这样连接。请在 %1 上建一个带密码的账户，再点“登录”；"
            "或者请管理员在组策略里启用“不安全的来宾登录”（会降低安全性）")
            .arg(host);
    case P::Signing:
        return SettingsEditor::tr("%1 不支持 Windows 要求的 SMB 签名，多见于老 NAS：请升级它的固件，或请管理员调整签名设置").arg(host);
    case P::Smb1Only:
        return SettingsEditor::tr("%1 只支持老旧的 SMB1，Windows 不再用它连接：请在 %1 上启用 SMB 2 或 SMB 3").arg(host);
    case P::Denied:
        return SettingsEditor::tr("没有权限读取这个文件夹：换一个有权限的账户登录试试");
    case P::Other:
        break;
    }
    return SettingsEditor::tr("出错了（错误 %1），稍后自动重试").arg(s.error);
}

} // namespace

QVariantList SettingsEditor::shares() const
{
    using State = ShareIndexer::State;
    QVariantList list;
    for (const NetworkShare& share : m_settings.shares) {
        const auto it = std::ranges::find_if(m_shareStatuses, [&](const ShareIndexer::Status& s) {
            return QString::fromStdWString(s.path).compare(share.path, Qt::CaseInsensitive) == 0;
        });
        QString tone = u"idle"_s;
        QString status = tr("稍后开始收录");
        bool signIn = false;
        bool busy = false;
        if (it != m_shareStatuses.end()) {
            const ShareIndexer::Status& s = *it;
            const QString verified = s.lastVerified ? tr("%1核对过").arg(sinceText(s.lastVerified)) : QString();
            switch (s.state) {
            case State::Waiting:
                if (s.lastVerified)
                    status = QStringList {itemsText(s.items), verified, tr("稍后连接")}.join(u" · "_s);
                break;
            case State::Building:
                tone = u"busy"_s;
                busy = true;
                status = tr("正在收录…已收录 %1").arg(itemsText(s.items));
                break;
            case State::Verifying:
                tone = u"busy"_s;
                busy = true;
                status = tr("正在核对… · %1").arg(itemsText(s.items));
                break;
            case State::Online:
                tone = u"ok"_s;
                status = QStringList {tr("在线"), itemsText(s.items), verified,
                    s.watching ? tr("实时跟踪变化")
                               : tr("服务器不通知变化，每 %n 小时核对一次", "", m_settings.networkVerifyHoursUnwatched)}
                             .join(u" · "_s);
                break;
            case State::Problem: {
                const bool away = s.problem == share::Problem::Offline || s.problem == share::Problem::Dropped;
                tone = away ? u"idle"_s : u"error"_s;
                signIn = s.problem == share::Problem::NeedsSignIn || s.problem == share::Problem::GuestBlocked
                    || s.problem == share::Problem::Denied;
                status = problemText(s);
                if (s.items > 0)
                    status += u"\n"_s + tr("已收录的 %1 照常能搜到").arg(itemsText(s.items))
                        + (s.lastVerified ? u" · "_s + verified : QString());
                break;
            }
            case State::Suspect:
                tone = u"warn"_s;
                status = tr("这次核对少了 %1（原有 %2），先不删除：可能是服务器的磁盘还没准备好。半小时后再核对一次；"
                            "确实删掉了，就点“按现在的内容更新”")
                             .arg(itemsText(s.missing), itemsText(s.items));
                break;
            }
        }
        list.append(QVariantMap {
            {u"path"_s, share.path},
            {u"name"_s, share.name.isEmpty() ? share.path : share.name},
            {u"tone"_s, tone},
            {u"status"_s, status},
            {u"signIn"_s, signIn},
            {u"suspect"_s, it != m_shareStatuses.end() && it->state == State::Suspect},
            {u"busy"_s, busy},
        });
    }
    return list;
}

QVariantList SettingsEditor::knownShares() const
{
    QVariantList list;
    for (const unc::Known& known : m_knownShares) {
        const QString path = QString::fromStdWString(known.path);
        if (std::ranges::any_of(m_settings.shares,
                [&](const NetworkShare& s) { return s.path.compare(path, Qt::CaseInsensitive) == 0; }))
            continue;
        list.append(QVariantMap {{u"path"_s, path}, {u"drive"_s, QString::fromStdWString(known.drive)}});
    }
    return list;
}

void SettingsEditor::setShareStatuses(std::vector<ShareIndexer::Status> statuses)
{
    // The words of each are what counts ("10 分钟前" moves on by itself).
    const QVariantList before = shares();
    m_shareStatuses = std::move(statuses);
    if (shares() != before)
        emit sharesChanged();
}

QString SettingsEditor::serverOf(const QString& text) const
{
    QString t = QDir::toNativeSeparators(text.trimmed());
    if (!t.startsWith(u"\\\\"_s))
        return {};
    t = t.mid(2);
    while (t.endsWith(u'\\'))
        t.chop(1);
    if (t.isEmpty() || t.contains(u'\\') || t.startsWith(u'?') || t.startsWith(u'.'))
        return {};
    return t;
}

QString SettingsEditor::addShare(const QString& text)
{
    const QString path = Settings::sharePath(text);
    if (path.isEmpty()) {
        const QString t = text.trimmed();
        if (t.size() >= 2 && t[1] == u':')
            return tr("%1 不是映射的网络驱动器").arg(t.left(2).toUpper());
        if (const QString host = serverOf(t); !host.isEmpty())
            return tr("还要写上共享文件夹的名字，例如 \\\\%1\\资料").arg(host);
        return tr("请输入网络路径，例如 \\\\192.168.1.10\\资料");
    }
    for (const NetworkShare& share : m_settings.shares) {
        if (share.path.compare(path, Qt::CaseInsensitive) == 0)
            return tr("已经添加过了");
        if (unc::contains(share.path.toStdWString(), path.toStdWString()))
            return tr("它在已添加的 %1 里面，已经能搜到").arg(share.path);
    }
    // A share holding one already added takes its place: no item twice.
    m_settings.shares.removeIf([&](const NetworkShare& share) {
        return unc::contains(path.toStdWString(), share.path.toStdWString());
    });
    m_settings.shares.append({path, {}});
    commit();
    emit sharesChanged();
    return {};
}

void SettingsEditor::removeShare(int index)
{
    if (index < 0 || index >= m_settings.shares.size())
        return;
    m_settings.shares.removeAt(index);
    commit();
    emit sharesChanged();
}

void SettingsEditor::verifyShare(int index)
{
    if (index >= 0 && index < m_settings.shares.size())
        emit shareVerifyRequested(m_settings.shares[index].path);
}

void SettingsEditor::signInToShare(int index)
{
    if (index >= 0 && index < m_settings.shares.size())
        emit signInRequested(m_settings.shares[index].path);
}

void SettingsEditor::acceptShareMissing(int index)
{
    if (index >= 0 && index < m_settings.shares.size())
        emit shareAcceptRequested(m_settings.shares[index].path);
}

void SettingsEditor::listServerShares(const QString& server)
{
    const QString host = serverOf(u"\\\\"_s + server);
    if (host.isEmpty())
        return;
    // NetShareEnum waits long for a server that is not there: first a quick look.
    std::thread([guard = QPointer(this), host] {
        QStringList found;
        QString problem;
        bool signIn = false;
        if (!share::reachable(host.toStdWString(), std::chrono::milliseconds(1500), true)) {
            problem = tr("连不上 %1（已关机、不在同一网络，或 VPN 没连）").arg(host);
        } else {
            unsigned long error = 0;
            for (const std::wstring& name : share::listShares(host.toStdWString(), &error))
                found.append(QString::fromStdWString(name));
            if (found.isEmpty()) {
                const share::Problem p = share::classify(error, true);
                signIn = p == share::Problem::NeedsSignIn || p == share::Problem::Denied
                    || p == share::Problem::GuestBlocked;
                if (p == share::Problem::GuestBlocked || p == share::Problem::Signing || p == share::Problem::Smb1Only)
                    problem = problemText({.path = L"\\\\" + host.toStdWString(), .problem = p, .error = error});
                else
                    problem = signIn ? tr("%1 要先登录才能列出共享").arg(host)
                        : error  ? tr("%1 没有列出共享（错误 %2）；可以直接输入共享的路径").arg(host).arg(error)
                                 : tr("%1 上没有共享文件夹").arg(host);
            }
        }
        QMetaObject::invokeMethod(qApp, [guard, host, found, problem, signIn] {
            if (guard)
                emit guard->serverSharesListed(host, found, problem, signIn);
        });
    }).detach();
}

void SettingsEditor::signInToServer(const QString& server)
{
    const QString host = serverOf(u"\\\\"_s + server);
    if (!host.isEmpty())
        emit signInRequested(u"\\\\"_s + host);
}

void SettingsEditor::setSignedIn(const QString& path, const QString& problem)
{
    emit signedIn(path, problem);
}

bool SettingsEditor::isDefault() const
{
    return m_settings == Settings::defaults();
}

void SettingsEditor::addExcludedFolder()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString folder = shell::pickFolder(owner, tr("选择不需要搜索的文件夹"));
    if (folder.isEmpty() || containsIgnoreCase(excludedPaths(), displayPath(folder)))
        return;
    m_settings.excludedPaths.append(folder);
    commit();
}

void SettingsEditor::removeExcludedPath(int index)
{
    if (index < 0 || index >= m_settings.excludedPaths.size())
        return;
    m_settings.excludedPaths.removeAt(index);
    commit();
}

bool SettingsEditor::addExcludedName(const QString& name)
{
    QString value = QDir::fromNativeSeparators(name.trimmed());
    while (value.endsWith(u'/'))
        value.chop(1);
    if (value.isEmpty() || containsIgnoreCase(m_settings.excludedNames, value))
        return false;
    m_settings.excludedNames.append(value);
    commit();
    return true;
}

void SettingsEditor::removeExcludedName(int index)
{
    if (index < 0 || index >= m_settings.excludedNames.size())
        return;
    m_settings.excludedNames.removeAt(index);
    commit();
}

bool SettingsEditor::addContentExtensions(const QString& text)
{
    static const QRegularExpression separators(u"[\\s,;，；、]+"_s);
    bool added = false;
    for (QString ext : text.split(separators, Qt::SkipEmptyParts)) {
        while (ext.startsWith(u'*') || ext.startsWith(u'.'))
            ext.remove(0, 1);
        ext = ext.toLower();
        if (ext.isEmpty() || containsIgnoreCase(m_settings.contentExtensions, ext))
            continue;
        m_settings.contentExtensions.append(ext);
        added = true;
    }
    if (added)
        commit();
    return added;
}

void SettingsEditor::removeContentExtension(int index)
{
    if (index < 0 || index >= m_settings.contentExtensions.size())
        return;
    m_settings.contentExtensions.removeAt(index);
    commit();
}

void SettingsEditor::restoreDefaults()
{
    if (isDefault())
        return;
    m_settings = Settings::defaults();
    commit();
}

void SettingsEditor::openDataFolder()
{
    shell::open(Settings::dataDir());
}

void SettingsEditor::setIndexFolderState(const IndexFolderState& state)
{
    if (m_indexFolderState == state)
        return;
    m_indexFolderState = state;
    emit indexFolderChanged();
}

void SettingsEditor::chooseIndexFolder()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString picked = shell::pickFolder(owner, tr("选择存放索引的文件夹"));
    if (picked.isEmpty())
        return;
    // An empty folder, or one with the index in it, is used as it is. In any
    // other the files get a WinShun folder of their own: they never mix with
    // other files, and leaving it out of searches hides nothing else.
    const QString folder = QDir::cleanPath(QDir::fromNativeSeparators(picked));
    const auto same = [&](const QString& other) {
        return folder.compare(QDir::fromNativeSeparators(other), Qt::CaseInsensitive) == 0;
    };
    const bool asItIs
        = same(Settings::dataDir()) || same(m_indexFolderState.folder) || indexfolder::onlyIndexFiles(folder);
    setIndexFolder(asItIs ? folder : QDir(folder).filePath(u"WinShun"_s));
}

void SettingsEditor::setIndexFolder(const QString& folder)
{
    // The default place is kept as nothing: it follows %LOCALAPPDATA%.
    QString value = QDir::cleanPath(QDir::fromNativeSeparators(folder.trimmed()));
    if (value.compare(QDir::fromNativeSeparators(Settings::dataDir()), Qt::CaseInsensitive) == 0)
        value.clear();
    if (m_settings.indexFolder == value)
        return;
    m_settings.indexFolder = value;
    commit();
}

void SettingsEditor::openIndexFolder()
{
    shell::open(m_indexFolderState.folder);
}

void SettingsEditor::openUrl(const QString& url)
{
    shell::openUrl(url);
}

void SettingsEditor::copyText(const QString& text)
{
    shell::copyText(text);
}

// ---- AI 助手 -----------------------------------------------------------------

void SettingsEditor::setMcp(bool on)
{
    if (m_settings.mcp == on)
        return;
    m_settings.mcp = on;
    commit();
}

void SettingsEditor::setMcpAllowOpen(bool on)
{
    if (m_settings.mcpAllowOpen == on)
        return;
    m_settings.mcpAllowOpen = on;
    commit();
}

void SettingsEditor::setMcpClipboard(bool on)
{
    if (m_settings.mcpClipboard == on)
        return;
    m_settings.mcpClipboard = on;
    commit();
}

void SettingsEditor::setMcpPlugins(bool on)
{
    if (m_settings.mcpPlugins == on)
        return;
    m_settings.mcpPlugins = on;
    commit();
}

void SettingsEditor::setMcpPluginActions(bool on)
{
    if (m_settings.mcpPluginActions == on)
        return;
    m_settings.mcpPluginActions = on;
    commit();
}

QStringList SettingsEditor::mcpExcludedPaths() const
{
    QStringList out;
    for (const QString& path : m_settings.mcpExcludedPaths)
        out.push_back(displayPath(path));
    return out;
}

void SettingsEditor::addMcpExcludedFolder()
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const QString folder = shell::pickFolder(owner, tr("选择不让 AI 看到的文件夹"));
    if (folder.isEmpty() || containsIgnoreCase(mcpExcludedPaths(), displayPath(folder)))
        return;
    m_settings.mcpExcludedPaths.append(folder);
    commit();
}

void SettingsEditor::removeMcpExcludedPath(int index)
{
    if (index < 0 || index >= m_settings.mcpExcludedPaths.size())
        return;
    m_settings.mcpExcludedPaths.removeAt(index);
    commit();
}

void SettingsEditor::setMcpStatus(const QString& status)
{
    if (m_mcpStatus == status)
        return;
    m_mcpStatus = status;
    emit mcpStatusChanged();
}

void SettingsEditor::refreshMcpClients()
{
    m_mcpClients = mcpclients::toVariant(mcpclients::detect());
    m_mcpRelayPath = mcpclients::relayPath();
    emit mcpClientsChanged();
}

void SettingsEditor::connectMcpClient(const QString& id)
{
    // Connecting a client is wanting the connection: the switch goes on with it.
    if (!m_settings.mcp) {
        m_settings.mcp = true;
        commit();
    }
    setMcpMessage(tr("正在连接…"));
    mcpclients::connect(id, this, [this](const mcpclients::Result& result) {
        setMcpMessage(result.message);
        refreshMcpClients();
    });
}

void SettingsEditor::copyMcpSnippet(const QString& id)
{
    shell::copyText(mcpclients::snippet(id));
    setMcpMessage(id == u"claude-code" ? tr("已复制命令，在终端里运行它就行") : tr("已复制，粘贴到那个客户端的 MCP 配置文件里"));
}

void SettingsEditor::setMcpMessage(const QString& message)
{
    if (m_mcpMessage == message)
        return;
    m_mcpMessage = message;
    emit mcpMessageChanged();
}

void SettingsEditor::openMcpLog()
{
    const QString log = Settings::dataDir() + u"\\mcp.log"_s;
    shell::open(QFileInfo::exists(log) ? log : Settings::dataDir());
}

QVariantList SettingsEditor::findWindowsSettings(const QString& query, int max) const
{
    QVariantList found;
    const auto places = m_places ? m_places->places() : nullptr;
    const ParsedQuery parsed = parseQuery(query);
    if (!places || parsed.isEmpty())
        return found;
    const NameMatcher matcher(parsed);
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    for (const PlaceHit& hit : searchPlaces(*places, parsed, matcher, {})) {
        if (found.size() >= max)
            break;
        const PlaceInfo& place = (*places)[hit.index];
        // As the launcher shows them (ResultModel): the logo of Settings, or the place's own icon.
        const QString icon = place.icon.startsWith(u"app:") ? FileIconProvider::appIconUrl(place.icon.mid(4), true, dark)
                                                            : FileIconProvider::placeIconUrl(place.icon);
        found.append(QVariantMap {{u"name"_s, place.name}, {u"icon"_s, icon}, {u"command"_s, place.command}});
    }
    return found;
}

void SettingsEditor::openWindowsSetting(const QString& command)
{
    shell::run(command);
}

} // namespace ws
