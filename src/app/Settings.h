#pragma once

#include "ContentIndex.h"
#include "Crawler.h"
#include "SearchTypes.h"
#include "ShareIndexer.h"
#include "WebShortcut.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ws {

// A content size limit as kept: MB to a tenth, 0.1 to 4096.
inline double roundedSizeMB(double mb)
{
    return std::clamp(std::round(mb * 10) / 10, 0.1, 4096.0);
}

// A network share searched with the local disks (ShareIndexer).
struct NetworkShare {
    QString path; // "\\host\share", or a folder in it (unc::normalize)
    QString name; // shown instead of the path; empty: the path

    bool operator==(const NetworkShare&) const = default;
};

// What the user set for one plugin (src/plugin): its [Plugin.<id>] section.
// Kept as written, values and all, so that save() loses nothing of what the
// settings page (to come) or a hand will put there.
struct PluginConfig {
    QString id;
    bool enabled = false; // Enabled=; plugins start out off
    QList<std::pair<QString, QString>> keywords; // Keyword.<command>=: the user's keyword for a command (empty: none)
    QList<std::pair<QString, QString>> settings; // Setting.<key>=: a declared setting's value, as text (never a password: the Credential Manager keeps those)
    // Confirmed=: the permissions the user said yes to when turning it on;
    // a version asking for more is asked about again.
    QStringList confirmed;
    // Ai=: the card's "给 AI 用" switch (its commands as MCP tools, with
    // [Mcp] Plugins on); on unless turned off.
    bool ai = true;

    bool operator==(const PluginConfig&) const = default;
};

// User settings, edited in the settings window and stored as an INI file
// (%APPDATA%\WinShun\WinShun.ini). Missing keys are written back with
// their defaults so every option is discoverable in the file.
struct Settings {
    // [Launcher]
    bool doubleCtrl = true;
    bool doubleCtrlPauseInGames = true; // not while a game has the mouse or exclusive full screen (GameGuard)
    bool doubleCtrlPauseInFullScreen = false; // not over any full-screen window
    QStringList doubleCtrlExcludedApps; // program files ("TheFinals.exe") in front of which double Ctrl does nothing
    QString hotkey; // e.g. "Alt+Space"; empty = none
    QString renderer; // software | d3d11; defaults() picks by memory (defaultRenderer)
    bool recordHistory = true; // remember what was opened: listed with nothing typed, first among matches
    bool dialogJump = true; // Ctrl+G in an Open or Save dialog goes to the folder open in a file manager (filemanager::)
    bool dialogBar = true; // a search bar under Open and Save dialogs (DialogBar)
    QString dialogBarPlace = QStringLiteral("auto"); // auto | below | left | right: where it goes by the dialog
    bool dialogAutoJump = false; // file dialogs go to the folder open in a file manager by themselves
    // open | back: as a dialog comes up, or only when the user comes back to
    // it from the file manager (GitHub #14: it opens where Windows left it).
    QString dialogAutoJumpWhen = QStringLiteral("open");
    // The user was asked under the dialog bar whether to go by itself (or set it in Settings): asked once.
    bool dialogAutoJumpAsked = false;
    QStringList dialogBarExcludedApps; // program files ("notepad.exe") whose file dialogs go without the bar
    // explorer | totalcmd | dopus: where folders open and items are shown
    // (shell::setFileManager); Explorer while the one chosen is not installed.
    QString fileManager = QStringLiteral("explorer");
    bool explorerMenu = true; // "用 Win顺 搜索此处" in Explorer's right-click menu (shellmenu::)
    // auto | on | off: a double click on the blank space of a File Explorer
    // folder or of the desktop brings up a menu of favorite and recent
    // folders (FolderMenu, GitHub #22); auto: not while Listary runs, which
    // does the same.
    QString folderMenu = QStringLiteral("auto");
    bool folderMenuExplorer = true; // in File Explorer's folders
    bool folderMenuDesktop = true; // on the desktop

    // [Results]
    bool foldersFirst = false; // 全部 and 文件 list the folders found before the files (else after)
    bool showModified = true; // each file and folder found says when it was last written
    // match | newest | oldest: how 文件 and 文件夹 order what they find, by
    // best match or by when last written (the footer's switch, remembered).
    QString sort = QStringLiteral("match");
    // opened | modified: with nothing typed, what was opened last, or the
    // files written last (the footer's switch, remembered).
    QString startList = QStringLiteral("opened");
    // ctrl | alt | off: Ctrl+1…9 opens the nth row shown, in the launcher and
    // the bar by file dialogs (other programs' hotkeys stay theirs, see numberkeys).
    QString numberKeys = QStringLiteral("ctrl");

    // [FileTypes]: what 文件 can be narrowed to, in the strip under the
    // search box (图片: jpg, png…), in its user's order. Written as an
    // array: size=, 1\Id=, 1\Name=, 1\Extensions=; a built-in one keeps
    // its Id and no Name until renamed, so it shows in either language.
    FileTypes fileTypes;
    static FileTypes defaultFileTypes();
    FileTypes namedFileTypes() const; // each with its name, a built-in one's in the language shown
    // "jpg, .PNG *.gif" → {"jpg", "png", "gif"}: lower case, no dots, each once.
    static QStringList fileExtensions(const QString& text);

    // [Appearance]
    QString theme = QStringLiteral("system"); // system | light | dark
    QString language = QStringLiteral("system"); // system | zh | en
    QString transparency; // off | on: Mica behind the windows, where Windows 11 has it; defaults() picks by memory
    QString taskbarIcon = QStringLiteral("color"); // color | mono: the tray's and the taskbar box's icon (monologo)
    QString uiSize = QStringLiteral("standard"); // compact | standard | roomy: type and row heights (uisize::)

    // [Update]
    bool autoUpdate = true; // look for new versions on GitHub (see Updater)

    // [Backup]
    QString backupFolder; // where the settings are backed up (see Backup); empty: not

    // [Index]
    QStringList excludedPaths;
    QStringList excludedNames;
    bool includeRemovableDrives = false;
    bool rescanOnStartup = true;
    // auto | service | direct | walk: where NTFS volumes are read from
    // (GitHub #21; App::volumeAccessFor). auto: directly when running as
    // administrator, else through the index service (WinShunService.exe)
    // once it answers, else by walking the folders. direct (管理员直读) has
    // Win顺 itself run as administrator: its logon task runs elevated and
    // the setting restarts it so, which only means something on an
    // administrator's account; service and walk never read the volumes
    // themselves, and the task runs with the user's normal rights.
    QString indexAccess = QStringLiteral("auto");
    // Where the index files (index.bin, content\) are kept, '/' separated;
    // %VARS% are expanded. Empty: dataDir(), the default.
    QString indexFolder;

    // [Content]
    // Off: no 内容 tab, 全部 finds names only, and no file is read for its
    // text (no content index either).
    bool contentSearch = true;
    QStringList contentExtensions;
    // Larger files are not searched, by kind (ContentSizeLimits::Kind): plain
    // text and logs, source code, data and web pages, documents. In MB, to a
    // tenth (0.1 to 4096). What people write is small; the large ones are
    // mostly logs, generated code and exported data (measured 2026-10-11 on
    // a developer's PC: these leave out under 1% of files, half the bytes).
    std::array<double, ContentSizeLimits::kKinds> contentMaxSizeMB {16, 2, 4, 100};
    bool contentIndex = true; // which files have which Chinese, Japanese and Korean characters
    bool contentInLowPriority = false; // 内容 also looks in system, program and tool folders
    bool contentDocuments = true; // and in Word, Excel, PowerPoint and PDF files (documentExtensions())

    // [Clipboard]
    bool clipboard = true; // keep a history of what is copied (Win+V page); at first as Windows' own clipboard history is
    bool clipboardWinV = false; // Win+V opens it, instead of Windows' clipboard history (see winv::)
    QString clipboardHotkey; // another shortcut for it; empty = none
    int clipboardMaxItems = 200; // entries kept outside groups; defaults() keeps 100 with 16 GB of RAM or less
    int clipboardMaxDays = 30; // since last copied or pasted; 0 = no limit
    bool clipboardImages = true;
    QStringList clipboardExcludedApps; // program files whose copies are not kept, "KeePass.exe"
    QString clipboardNumberKeys = QStringLiteral("alt"); // alt | ctrl | off: Alt+1…9 pastes the nth row

    // [Taskbar]: Win顺 in the place of Windows' search (see taskbar::)
    bool taskbarWinS = false; // Win+S opens it over the taskbar, instead of Windows' search (see winv::)
    bool taskbarStartTyping = false; // typing in the Start menu searches with Win顺 (StartMenuTyping)
    bool taskbarSearchBox = false; // Win顺's own search box on the taskbar (taskbar::SearchBox)
    bool taskbarStartGap = false; // ... right of Start, the icons moved over for it (taskbar::StartGap)

    // [WebSearch]: keywords the user gives web pages ("gh" opens GitHub, "gh
    // WinShun" searches it; see WebShortcut). At first just one, "winshun".
    // Written as an array: size=, 1\Keyword=, 1\Name=, 1\Url=, 1\Home=.
    WebShortcuts webShortcuts;

    // [Shares]: network shares searched too, written as an array: size=,
    // 1\Path=, 1\Name=. A mapped drive (Z:\...) is kept as the share it maps.
    QList<NetworkShare> shares;

    // [Network]: how shares are read
    // Read in full again, also when the server tells of changes: Samba (most NAS)
    // tells only of those made over the network, and some tell of none while
    // seeming to. 0: on start only.
    int networkVerifyHours = 1;
    int networkVerifyHoursUnwatched = 1; // when it does not
    int networkThreads = 16; // folders read at a time from a server (half that when verifying)
    QStringList networkExcludedNames; // folders NAS boxes and Macs keep for themselves; on top of [Index]
    QStringList networkExcludedFiles; // file names left out on shares; '*' at the start only

    // [Mcp]: AI clients (Claude, Cursor, VS Code …) use Win顺 through
    // WinShunMcp.exe (src/mcp). The switch covers the read-only tools (off
    // until a client is connected from the settings, which turns it on);
    // opening things and reading the clipboard history have switches of their own.
    bool mcp = false;
    bool mcpAllowOpen = false; // open_item, show_in_folder
    bool mcpClipboard = false; // clipboard_history, clipboard_get
    QStringList mcpExcludedPaths; // folders AI never sees, on top of the index's exclusions
    // Plugins=: the plugins' commands as tools (plugin::AiBridge; queries
    // only); PluginActions=: their actions too (plugin_<id>_<command>_run).
    bool mcpPlugins = false;
    bool mcpPluginActions = false;

    // [Plugins]: the plugin system's own (the plugin design, §5.5).
    bool pluginDeveloperMode = false; // DeveloperMode=: the log pane, scripts reloaded as they change

    // [Plugin.<id>]: one section per plugin the user set anything for.
    QList<PluginConfig> plugins;
    const PluginConfig* plugin(QStringView id) const; // nothing when the user set nothing for it
    PluginConfig& pluginConfig(const QString& id); // the section, made when there is none

    static Settings defaults();
    static bool exists(); // false on the very first run

    void load();
    void save() const;

    bool operator==(const Settings&) const = default;

    CrawlRules crawlRules() const;
    ShareIndexer::Options shareOptions() const;
    ContentSizeLimits contentSizeLimits() const;
    QString indexDir() const; // indexFolder expanded, with native separators; dataDir() when it is empty

    static QString filePath();
    static QString storedRenderer(); // just [Launcher] Renderer; works before QGuiApplication exists
    static QString resolveRenderer(const QString& renderer); // anything but software or d3d11 (an old "auto") -> defaultRenderer()
    static QString defaultRenderer(); // d3d11 with 8 GB of RAM or more, else software
    static bool lowMemory(); // 16 GB of RAM or less: transparency starts off, the clipboard keeps 100
    static QString dataDir(); // %LOCALAPPDATA%\WinShun, or the profile's folder
    // The hidden --profile <folder>: the settings and all data in that folder
    // instead, for screenshots of made-up files with a history and clipboard
    // of their own (tools/make_screenshots.py). Before anything reads them.
    static void useProfile(const QString& folder);
    static QString profile(); // empty without --profile
    // A portable copy (GitHub #19): "data" beside WinShun.exe, when that
    // folder exists, else empty. Used as the profile; works before
    // QCoreApplication exists.
    static QString portableFolder();
    static bool windowsClipboardHistory(); // Windows' own clipboard history is on (Settings > System > Clipboard)
    static bool hasClipboardSettings(); // the file has a [Clipboard] section: written by a version with the clipboard history
    static QString resolveLanguage(const QString& language); // "system" -> zh with a Chinese Windows, else en
    // What is typed or written for a share, as it is kept: "\\host\share[\folder]"
    // (a mapped drive's share instead of the letter); empty when it is no share.
    static QString sharePath(const QString& text);
};

} // namespace ws
