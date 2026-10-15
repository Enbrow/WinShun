#pragma once

#include "Settings.h"
#include "Unc.h"

#include <QDir>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

namespace ws {

class FavoritesSettings;
class PluginSettings;
class ShortcutCapture;
class SystemCatalog;

// The view-model behind the settings window. Every change is saved right
// away and reported through edited(); there is no OK / Cancel step.
class SettingsEditor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool doubleCtrl READ doubleCtrl WRITE setDoubleCtrl NOTIFY changed FINAL)
    Q_PROPERTY(QString doubleCtrlKey READ doubleCtrlKey WRITE setDoubleCtrlKey NOTIFY changed FINAL) // both | left | right
    // The Ctrl PowerToys' Find My Mouse also opens on with a double tap,
    // when that is one Win顺 listens to as well: left | right; empty when
    // none (refreshDoubleCtrlClash).
    Q_PROPERTY(QString doubleCtrlClash READ doubleCtrlClash NOTIFY doubleCtrlClashChanged FINAL)
    Q_PROPERTY(bool doubleCtrlPauseInGames READ doubleCtrlPauseInGames WRITE setDoubleCtrlPauseInGames NOTIFY changed FINAL)
    Q_PROPERTY(bool doubleCtrlPauseInFullScreen READ doubleCtrlPauseInFullScreen WRITE setDoubleCtrlPauseInFullScreen
            NOTIFY changed FINAL)
    Q_PROPERTY(QStringList doubleCtrlExcludedApps READ doubleCtrlExcludedApps NOTIFY changed FINAL)
    Q_PROPERTY(QString hotkey READ hotkey WRITE setHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString hotkeyError READ hotkeyError NOTIFY hotkeyErrorChanged FINAL)
    Q_PROPERTY(bool recordingHotkey READ recordingHotkey WRITE setRecordingHotkey NOTIFY recordingHotkeyChanged FINAL)
    Q_PROPERTY(bool autostart READ autostart WRITE setAutostart NOTIFY autostartChanged FINAL)
    Q_PROPERTY(bool recordHistory READ recordHistory WRITE setRecordHistory NOTIFY changed FINAL)
    Q_PROPERTY(int historyCount READ historyCount NOTIFY historyCountChanged FINAL)
    Q_PROPERTY(bool dialogJump READ dialogJump WRITE setDialogJump NOTIFY changed FINAL)
    Q_PROPERTY(bool dialogBar READ dialogBar WRITE setDialogBar NOTIFY changed FINAL)
    Q_PROPERTY(QString dialogBarPlace READ dialogBarPlace WRITE setDialogBarPlace NOTIFY changed FINAL)
    Q_PROPERTY(bool dialogAutoJump READ dialogAutoJump WRITE setDialogAutoJump NOTIFY changed FINAL)
    Q_PROPERTY(QString dialogAutoJumpWhen READ dialogAutoJumpWhen WRITE setDialogAutoJumpWhen NOTIFY changed FINAL)
    Q_PROPERTY(QStringList dialogBarExcludedApps READ dialogBarExcludedApps NOTIFY changed FINAL)
    // Where folders open (Settings::fileManager), and what there is to choose
    // from: "explorer" and the others installed, looked for again as the
    // window comes to the front (refreshFileManagers).
    Q_PROPERTY(QString fileManager READ fileManager WRITE setFileManager NOTIFY changed FINAL)
    Q_PROPERTY(QStringList fileManagers READ fileManagers NOTIFY fileManagersChanged FINAL)
    // The command lines of "custom" (Settings::fileManagerOpenCommand,
    // fileManagerRevealCommand), and what is wrong with them: no open
    // command, a program not found (empty: nothing).
    Q_PROPERTY(QString fileManagerOpenCommand READ fileManagerOpenCommand WRITE setFileManagerOpenCommand NOTIFY changed FINAL)
    Q_PROPERTY(QString fileManagerRevealCommand READ fileManagerRevealCommand WRITE setFileManagerRevealCommand NOTIFY changed FINAL)
    Q_PROPERTY(QString fileManagerProblem READ fileManagerProblem NOTIFY changed FINAL)
    Q_PROPERTY(bool explorerMenu READ explorerMenu WRITE setExplorerMenu NOTIFY changed FINAL)
    // The folder menu on a double click (Settings::folderMenu: on unless "off").
    Q_PROPERTY(bool folderMenu READ folderMenu WRITE setFolderMenu NOTIFY changed FINAL)
    Q_PROPERTY(bool folderMenuExplorer READ folderMenuExplorer WRITE setFolderMenuExplorer NOTIFY changed FINAL)
    // The folder menu in Directory Opus, which runs a command of its settings
    // on a double click on blank space (refreshOpusFolderMenu): on | stale
    // (another copy's WinShunSearch.exe) | off | taken (a command of the
    // user's own: opusFolderMenuTaken) | manual (no settings file, or one
    // not understood: set up by hand) | busy (setOpusFolderMenu at work).
    Q_PROPERTY(QString opusFolderMenu READ opusFolderMenu NOTIFY opusFolderMenuChanged FINAL)
    Q_PROPERTY(QString opusFolderMenuTaken READ opusFolderMenuTaken NOTIFY opusFolderMenuChanged FINAL)
    Q_PROPERTY(QString opusFolderMenuProblem READ opusFolderMenuProblem NOTIFY opusFolderMenuChanged FINAL)
    Q_PROPERTY(bool opusRunning READ opusRunning NOTIFY opusFolderMenuChanged FINAL) // setting it restarts Opus
    Q_PROPERTY(bool folderMenuDesktop READ folderMenuDesktop WRITE setFolderMenuDesktop NOTIFY changed FINAL)
    // The hotkey that brings the folder menu up anywhere (GitHub #25); its
    // error as the app found it (taken, not understood), like hotkeyError.
    Q_PROPERTY(QString folderMenuHotkey READ folderMenuHotkey WRITE setFolderMenuHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString folderMenuHotkeyError READ folderMenuHotkeyError NOTIFY hotkeyErrorChanged FINAL)
    Q_PROPERTY(bool includeRemovableDrives READ includeRemovableDrives WRITE setIncludeRemovableDrives NOTIFY changed FINAL)
    Q_PROPERTY(bool rescanOnStartup READ rescanOnStartup WRITE setRescanOnStartup NOTIFY changed FINAL)
    // 索引方式 (Settings::indexAccess, GitHub #21): auto | service | direct |
    // walk, and, as the app has it (setIndexAccessState): the way in use
    // (direct | service | walk) and what it says, notes worth reading, what
    // this copy and account can offer (direct wants an administrator; the
    // service's install buttons are a portable copy's), and how installing
    // or uninstalling the service goes.
    Q_PROPERTY(QString indexAccess READ indexAccess WRITE setIndexAccess NOTIFY changed FINAL)
    Q_PROPERTY(QString indexAccessEffective READ indexAccessEffective NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(QString indexAccessStatus READ indexAccessStatus NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(QString indexAccessNote READ indexAccessNote NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool isAdministrator READ isAdministrator NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool isElevated READ isElevated NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool isPortable READ isPortable NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool indexServiceInstalled READ indexServiceInstalled NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(bool indexServiceBusy READ indexServiceBusy NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(QString indexServiceMessage READ indexServiceMessage NOTIFY indexAccessStateChanged FINAL)
    Q_PROPERTY(QStringList excludedPaths READ excludedPaths NOTIFY changed FINAL) // expanded, for display
    Q_PROPERTY(QStringList excludedNames READ excludedNames NOTIFY changed FINAL)
    Q_PROPERTY(bool contentSearch READ contentSearch WRITE setContentSearch NOTIFY changed FINAL)
    Q_PROPERTY(QStringList contentExtensions READ contentExtensions NOTIFY changed FINAL)
    // Size limits in MB by kind of file (ContentSizeLimits::Kind: text, code, data),
    // and the listed extensions of each kind (".c .cpp .h").
    Q_PROPERTY(QList<double> contentMaxSizeMB READ contentMaxSizeMB NOTIFY changed FINAL)
    Q_PROPERTY(QStringList contentKindExtensions READ contentKindExtensions NOTIFY changed FINAL)
    Q_PROPERTY(bool contentIndex READ contentIndex WRITE setContentIndex NOTIFY changed FINAL)
    Q_PROPERTY(bool contentInLowPriority READ contentInLowPriority WRITE setContentInLowPriority NOTIFY changed FINAL)
    Q_PROPERTY(bool contentDocuments READ contentDocuments WRITE setContentDocuments NOTIFY changed FINAL)
    Q_PROPERTY(QString contentIndexStatus READ contentIndexStatus NOTIFY contentIndexStatusChanged FINAL)
    // 结果列表
    Q_PROPERTY(bool foldersFirst READ foldersFirst WRITE setFoldersFirst NOTIFY changed FINAL)
    Q_PROPERTY(bool showModified READ showModified WRITE setShowModified NOTIFY changed FINAL)
    Q_PROPERTY(bool thumbnails READ thumbnails WRITE setThumbnails NOTIFY changed FINAL)
    Q_PROPERTY(QString sort READ sort WRITE setSort NOTIFY changed FINAL) // match | newest | oldest
    Q_PROPERTY(QString startList READ startList WRITE setStartList NOTIFY changed FINAL) // opened | modified
    Q_PROPERTY(QString numberKeys READ numberKeys WRITE setNumberKeys NOTIFY changed FINAL) // ctrl | alt | off
    // Which of the chosen number keys are other programs' hotkeys, or Win顺's
    // own shortcuts, said for the user; empty when none (refreshNumberKeys).
    Q_PROPERTY(QString numberKeysNote READ numberKeysNote NOTIFY numberKeysNoteChanged FINAL)
    // 剪贴板
    Q_PROPERTY(bool clipboard READ clipboard WRITE setClipboard NOTIFY changed FINAL)
    Q_PROPERTY(bool clipboardWinV READ clipboardWinV WRITE setClipboardWinV NOTIFY changed FINAL)
    // Where taking Win+V over stands (set by the app): off | on | waiting
    // (Explorer still holds it: restart Explorer) | releasing (Explorer gets
    // it back once it restarts) | failed (the registry could not be written).
    Q_PROPERTY(QString winVState READ winVState NOTIFY winVStateChanged FINAL)
    Q_PROPERTY(bool canRestartExplorer READ canRestartExplorer NOTIFY winVStateChanged FINAL)
    Q_PROPERTY(QString clipboardHotkey READ clipboardHotkey WRITE setClipboardHotkey NOTIFY changed FINAL)
    Q_PROPERTY(QString clipboardHotkeyError READ clipboardHotkeyError NOTIFY hotkeyErrorChanged FINAL)
    Q_PROPERTY(int clipboardMaxItems READ clipboardMaxItems WRITE setClipboardMaxItems NOTIFY changed FINAL)
    Q_PROPERTY(int clipboardMaxDays READ clipboardMaxDays WRITE setClipboardMaxDays NOTIFY changed FINAL)
    Q_PROPERTY(bool clipboardImages READ clipboardImages WRITE setClipboardImages NOTIFY changed FINAL)
    Q_PROPERTY(QStringList clipboardExcludedApps READ clipboardExcludedApps NOTIFY changed FINAL)
    Q_PROPERTY(int clipboardCount READ clipboardCount NOTIFY clipboardCountChanged FINAL)
    Q_PROPERTY(QString clipboardNumberKeys READ clipboardNumberKeys WRITE setClipboardNumberKeys NOTIFY changed FINAL)
    Q_PROPERTY(QString clipboardNumberKeysNote READ clipboardNumberKeysNote NOTIFY numberKeysNoteChanged FINAL)
    // 代替 Windows 搜索 (taskbar::)
    Q_PROPERTY(bool taskbarWinS READ taskbarWinS WRITE setTaskbarWinS NOTIFY changed FINAL)
    Q_PROPERTY(QString winSState READ winSState NOTIFY winSStateChanged FINAL) // as winVState, for Win+S
    Q_PROPERTY(bool taskbarStartTyping READ taskbarStartTyping WRITE setTaskbarStartTyping NOTIFY changed FINAL)
    // Win顺's own search box on the taskbar: the setting, whether Windows has
    // the taskbar for it (11), whether it is on the taskbar now (not where
    // the icons leave no room for it).
    Q_PROPERTY(bool taskbarSearchBox READ taskbarSearchBox WRITE setTaskbarSearchBox NOTIFY changed FINAL)
    Q_PROPERTY(bool searchBoxSupported READ searchBoxSupported CONSTANT FINAL)
    Q_PROPERTY(bool searchBoxShown READ searchBoxShown NOTIFY searchBoxShownChanged FINAL)
    Q_PROPERTY(QString searchBoxHidden READ searchBoxHidden NOTIFY searchBoxShownChanged FINAL)
    // The box right of Start (taskbar::StartGap): the setting, and how it went
    // ("off", "attaching", "ready", "applied", "nogap", "conflict",
    // "incompatible", "failed"; the reason in English, for the details).
    Q_PROPERTY(bool taskbarStartGap READ taskbarStartGap WRITE setTaskbarStartGap NOTIFY changed FINAL)
    Q_PROPERTY(QString startGapState READ startGapState NOTIFY startGapStateChanged FINAL)
    Q_PROPERTY(QString startGapReason READ startGapReason NOTIFY startGapStateChanged FINAL)
    // Read again as the window comes to the front (refreshTaskbarState): the
    // user pins the button and hides Windows' search outside Win顺.
    Q_PROPERTY(bool taskbarButtonPinned READ taskbarButtonPinned NOTIFY taskbarStateChanged FINAL)
    Q_PROPERTY(bool windowsSearchShown READ windowsSearchShown NOTIFY taskbarStateChanged FINAL)
    // 网页搜索: for each, {keyword, name, url, home, searches, shownUrl, shownHome, homeUrl}
    Q_PROPERTY(QVariantList webShortcuts READ webShortcuts NOTIFY changed FINAL)
    // The kinds of file 文件 narrows to: [{name, extensions: [..]}], named
    // in the language shown; fileTypesDefault while they are as they came.
    Q_PROPERTY(QVariantList fileTypes READ fileTypes NOTIFY changed FINAL)
    Q_PROPERTY(bool fileTypesDefault READ fileTypesDefault NOTIFY changed FINAL)
    // Counts the sites' icons that arrived: a binding on webIconUrl() reads it
    // to be asked again (without the list made anew, and a form open in it lost).
    Q_PROPERTY(int siteIconsArrived READ siteIconsArrived NOTIFY siteIconArrived FINAL)
    // 网络共享: for each, {path, name, tone (ok | busy | idle | warn | error),
    // status (where it stands, said for the user), signIn (it needs a user
    // name and password), suspect (a pass found much less: accept it?), busy
    // (being read)}. As the app reports (setShareStatuses).
    Q_PROPERTY(QVariantList shares READ shares NOTIFY sharesChanged FINAL)
    // Shares the user opened in File Explorer or mapped, not added yet: [{path, drive}]
    Q_PROPERTY(QVariantList knownShares READ knownShares NOTIFY sharesChanged FINAL)
    Q_PROPERTY(QString renderer READ renderer WRITE setRenderer NOTIFY changed FINAL)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY changed FINAL)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY changed FINAL)
    Q_PROPERTY(QString transparency READ transparency WRITE setTransparency NOTIFY changed FINAL)
    Q_PROPERTY(QString taskbarIcon READ taskbarIcon WRITE setTaskbarIcon NOTIFY changed FINAL) // color | mono
    Q_PROPERTY(QString uiSize READ uiSize WRITE setUiSize NOTIFY changed FINAL) // compact | standard | roomy
    Q_PROPERTY(bool autoUpdate READ autoUpdate WRITE setAutoUpdate NOTIFY changed FINAL)
    Q_PROPERTY(QString backupFolder READ backupFolder NOTIFY changed FINAL) // empty: no backups (see Backup)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY changed FINAL)
    Q_PROPERTY(bool isDefault READ isDefault NOTIFY changed FINAL)
    Q_PROPERTY(QString dataFolder READ dataFolder CONSTANT FINAL)
    // 索引位置 (set by the app): where the index files are, and how moving them goes
    Q_PROPERTY(QString indexFolder READ indexFolder NOTIFY indexFolderChanged FINAL)
    Q_PROPERTY(bool indexFolderIsDefault READ indexFolderIsDefault NOTIFY indexFolderChanged FINAL)
    Q_PROPERTY(QString indexSize READ indexSize NOTIFY indexFolderChanged FINAL)
    Q_PROPERTY(int indexMoveProgress READ indexMoveProgress NOTIFY indexFolderChanged FINAL) // -1: not moving
    Q_PROPERTY(QString indexFolderProblem READ indexFolderProblem NOTIFY indexFolderChanged FINAL)
    Q_PROPERTY(QString indexFolderNote READ indexFolderNote NOTIFY indexFolderChanged FINAL)
    // AI 助手 (the MCP server, src/mcp): the switches, the folders AI never
    // sees, who is connected (set by the app), the clients found on this PC
    // and how the last connection made went (McpClients).
    Q_PROPERTY(bool mcp READ mcp WRITE setMcp NOTIFY changed FINAL)
    Q_PROPERTY(bool mcpAllowOpen READ mcpAllowOpen WRITE setMcpAllowOpen NOTIFY changed FINAL)
    Q_PROPERTY(bool mcpClipboard READ mcpClipboard WRITE setMcpClipboard NOTIFY changed FINAL)
    Q_PROPERTY(bool mcpPlugins READ mcpPlugins WRITE setMcpPlugins NOTIFY changed FINAL) // the plugins' commands as tools
    Q_PROPERTY(bool mcpPluginActions READ mcpPluginActions WRITE setMcpPluginActions NOTIFY changed FINAL) // and their actions
    Q_PROPERTY(QStringList mcpExcludedPaths READ mcpExcludedPaths NOTIFY changed FINAL) // expanded, for display
    Q_PROPERTY(QString mcpStatus READ mcpStatus NOTIFY mcpStatusChanged FINAL)
    Q_PROPERTY(QVariantList mcpClients READ mcpClients NOTIFY mcpClientsChanged FINAL)
    Q_PROPERTY(QString mcpRelayPath READ mcpRelayPath NOTIFY mcpClientsChanged FINAL) // empty: WinShunMcp.exe is missing
    Q_PROPERTY(QString mcpMessage READ mcpMessage NOTIFY mcpMessageChanged FINAL)
    // 插件: the page's own view-model (PluginSettings), which edits the
    // [Plugin.<id>] sections here.
    Q_PROPERTY(ws::PluginSettings* plugins READ plugins CONSTANT FINAL)
    // 收藏夹: the page's own view-model (FavoritesSettings), on the app's
    // favorites (not in the INI: 恢复默认设置 leaves them).
    Q_PROPERTY(ws::FavoritesSettings* favorites READ favorites CONSTANT FINAL)

public:
    SettingsEditor(const Settings& settings, const QString& runningRenderer, QObject* parent = nullptr);
    ~SettingsEditor() override;

    bool doubleCtrl() const { return m_settings.doubleCtrl; }
    void setDoubleCtrl(bool on);
    QString doubleCtrlKey() const { return m_settings.doubleCtrlKey; }
    void setDoubleCtrlKey(const QString& key);
    QString doubleCtrlClash() const { return m_doubleCtrlClash; }
    Q_INVOKABLE void refreshDoubleCtrlClash();
    bool doubleCtrlPauseInGames() const { return m_settings.doubleCtrlPauseInGames; }
    void setDoubleCtrlPauseInGames(bool on);
    bool doubleCtrlPauseInFullScreen() const { return m_settings.doubleCtrlPauseInFullScreen; }
    void setDoubleCtrlPauseInFullScreen(bool on);
    QStringList doubleCtrlExcludedApps() const { return m_settings.doubleCtrlExcludedApps; }
    QString hotkey() const { return m_settings.hotkey; }
    void setHotkey(const QString& hotkey);
    QString hotkeyError() const { return m_hotkeyError; }
    void setHotkeyError(const QString& error);
    bool recordingHotkey() const { return m_recordingHotkey; }
    void setRecordingHotkey(bool recording);
    bool autostart() const;
    void setAutostart(bool on);
    bool recordHistory() const { return m_settings.recordHistory; }
    void setRecordHistory(bool on);
    int historyCount() const { return m_historyCount; }
    void setHistoryCount(int count); // set by the app
    bool dialogJump() const { return m_settings.dialogJump; }
    void setDialogJump(bool on);
    bool dialogBar() const { return m_settings.dialogBar; }
    void setDialogBar(bool on);
    QString dialogBarPlace() const { return m_settings.dialogBarPlace; }
    void setDialogBarPlace(const QString& place);
    bool dialogAutoJump() const { return m_settings.dialogAutoJump; }
    void setDialogAutoJump(bool on);
    QString dialogAutoJumpWhen() const { return m_settings.dialogAutoJumpWhen; }
    void setDialogAutoJumpWhen(const QString& when); // "open", "back"
    QStringList dialogBarExcludedApps() const { return m_settings.dialogBarExcludedApps; }
    QString fileManager() const { return m_settings.fileManager; }
    void setFileManager(const QString& manager);
    QStringList fileManagers() const { return m_fileManagers; }
    QString fileManagerOpenCommand() const { return m_settings.fileManagerOpenCommand; }
    void setFileManagerOpenCommand(const QString& command);
    QString fileManagerRevealCommand() const { return m_settings.fileManagerRevealCommand; }
    void setFileManagerRevealCommand(const QString& command);
    QString fileManagerProblem() const;
    // Asks for a program: a command line running it with {path}, to fill a
    // command in with; empty when none was picked.
    Q_INVOKABLE QString pickFileManagerProgram();
    bool explorerMenu() const { return m_settings.explorerMenu; }
    void setExplorerMenu(bool on);
    bool folderMenu() const { return m_settings.folderMenu != u"off"; }
    void setFolderMenu(bool on);
    bool folderMenuExplorer() const { return m_settings.folderMenuExplorer; }
    void setFolderMenuExplorer(bool on);
    bool folderMenuDesktop() const { return m_settings.folderMenuDesktop; }
    void setFolderMenuDesktop(bool on);
    QString folderMenuHotkey() const { return m_settings.folderMenuHotkey; }
    void setFolderMenuHotkey(const QString& hotkey);
    QString folderMenuHotkeyError() const { return m_folderMenuHotkeyError; }
    void setFolderMenuHotkeyError(const QString& error);
    Q_INVOKABLE void refreshFileManagers();
    // The command Directory Opus runs for a double click on blank space, to
    // bring up the folder menu there (FolderMenu::requested): for its
    // Preferences > File Displays > Mouse > Background Events.
    Q_INVOKABLE QString opusFolderMenuCommand() const;
    Q_INVOKABLE void openOpusPreferences() const;
    QString opusFolderMenu() const { return m_opusFolderMenu; }
    QString opusFolderMenuTaken() const { return m_opusFolderMenuTaken; }
    QString opusFolderMenuProblem() const { return m_opusFolderMenuProblem; }
    bool opusRunning() const { return m_opusRunning; }
    Q_INVOKABLE void refreshOpusFolderMenu();
    // Puts that command into Opus's settings (what was there comes back with
    // `on` false), closing and starting again an Opus that runs.
    Q_INVOKABLE void setOpusFolderMenu(bool on);
    bool includeRemovableDrives() const { return m_settings.includeRemovableDrives; }
    void setIncludeRemovableDrives(bool on);
    bool rescanOnStartup() const { return m_settings.rescanOnStartup; }
    void setRescanOnStartup(bool on);
    QString indexAccess() const { return m_settings.indexAccess; }
    void setIndexAccess(const QString& mode); // "auto", "service", "direct" (administrators only), "walk"
    struct IndexAccessState {
        QString effective; // direct | service | walk: the way in use
        QString status; // what is in use, or why not what was asked for, said for the user
        QString note; // worth reading: the mode taken from an old task, the task's level not the mode's ...
        bool elevated = false; // Win顺 runs as administrator now
        bool administrator = false; // the account is an administrator's (direct means something)
        bool portable = false; // a portable copy: the service is installed from here
        bool serviceInstalled = false;
        bool serviceBusy = false; // --install / --uninstall running
        QString serviceMessage; // how the last install or uninstall went
        bool operator==(const IndexAccessState&) const = default;
    };
    void setIndexAccessState(const IndexAccessState& state); // set by the app
    QString indexAccessEffective() const { return m_indexAccessState.effective; }
    QString indexAccessStatus() const { return m_indexAccessState.status; }
    QString indexAccessNote() const { return m_indexAccessState.note; }
    bool isAdministrator() const { return m_indexAccessState.administrator; }
    bool isElevated() const { return m_indexAccessState.elevated; }
    bool isPortable() const { return m_indexAccessState.portable; }
    bool indexServiceInstalled() const { return m_indexAccessState.serviceInstalled; }
    bool indexServiceBusy() const { return m_indexAccessState.serviceBusy; }
    QString indexServiceMessage() const { return m_indexAccessState.serviceMessage; }
    // The app restarts Win顺 as administrator (UAC asks once), installs or
    // uninstalls the index service (WinShunService.exe, elevated, UAC once).
    Q_INVOKABLE void restartAsAdministrator() { emit elevatedRestartRequested(); }
    Q_INVOKABLE void installIndexService() { emit indexServiceInstallRequested(true); }
    Q_INVOKABLE void uninstallIndexService() { emit indexServiceInstallRequested(false); }
    QStringList excludedPaths() const;
    QStringList excludedNames() const { return m_settings.excludedNames; }
    bool contentSearch() const { return m_settings.contentSearch; }
    void setContentSearch(bool on);
    QStringList contentExtensions() const { return m_settings.contentExtensions; }
    QList<double> contentMaxSizeMB() const { return {m_settings.contentMaxSizeMB.begin(), m_settings.contentMaxSizeMB.end()}; }
    Q_INVOKABLE void setContentMaxSizeMB(int kind, double mb);
    QStringList contentKindExtensions() const;
    bool contentIndex() const { return m_settings.contentIndex; }
    void setContentIndex(bool on);
    bool contentInLowPriority() const { return m_settings.contentInLowPriority; }
    void setContentInLowPriority(bool on);
    bool contentDocuments() const { return m_settings.contentDocuments; }
    void setContentDocuments(bool on);
    QString contentIndexStatus() const { return m_contentIndexStatus; }
    void setContentIndexStatus(const QString& status); // "已收录 … 个文件"; set by the app
    QString renderer() const { return m_settings.renderer; }
    void setRenderer(const QString& renderer);
    QString theme() const { return m_settings.theme; }
    void setTheme(const QString& theme);
    QString language() const { return m_settings.language; }
    void setLanguage(const QString& language);
    QString transparency() const { return m_settings.transparency; }
    void setTransparency(const QString& transparency);
    QString taskbarIcon() const { return m_settings.taskbarIcon; }
    void setTaskbarIcon(const QString& style);
    QString uiSize() const { return m_settings.uiSize; }
    void setUiSize(const QString& size);
    bool autoUpdate() const { return m_settings.autoUpdate; }
    QString backupFolder() const { return QDir::toNativeSeparators(m_settings.backupFolder); }
    void setAutoUpdate(bool on);
    bool restartRequired() const { return Settings::resolveRenderer(m_settings.renderer) != m_runningRenderer; }
    bool isDefault() const;
    QString dataFolder() const { return Settings::dataDir(); }

    // 索引位置, as the app has it.
    struct IndexFolderState {
        QString folder; // where the index files are, with native separators
        bool isDefault = true; // that is Settings::dataDir()
        QString size; // of the files there, "420 MB"
        int moveProgress = -1; // percent, while they are being moved
        QString problem; // why they could not be moved, or are not where the settings say
        QString note; // worth knowing, nothing wrong: they are on a hard disk
        bool operator==(const IndexFolderState&) const = default;
    };
    void setIndexFolderState(const IndexFolderState& state);
    QString indexFolder() const { return m_indexFolderState.folder; }
    bool indexFolderIsDefault() const { return m_indexFolderState.isDefault; }
    QString indexSize() const { return m_indexFolderState.size; }
    int indexMoveProgress() const { return m_indexFolderState.moveProgress; }
    QString indexFolderProblem() const { return m_indexFolderState.problem; }
    QString indexFolderNote() const { return m_indexFolderState.note; }

    bool clipboard() const { return m_settings.clipboard; }
    void setClipboard(bool on);
    bool clipboardWinV() const { return m_settings.clipboardWinV; }
    void setClipboardWinV(bool on);
    QString winVState() const { return m_winVState; }
    void setWinVState(const QString& state, bool canRestartExplorer); // set by the app
    bool canRestartExplorer() const { return m_canRestartExplorer; }
    QString clipboardHotkey() const { return m_settings.clipboardHotkey; }
    void setClipboardHotkey(const QString& hotkey);
    QString clipboardHotkeyError() const { return m_clipboardHotkeyError; }
    void setClipboardHotkeyError(const QString& error);
    int clipboardMaxItems() const { return m_settings.clipboardMaxItems; }
    void setClipboardMaxItems(int items);
    int clipboardMaxDays() const { return m_settings.clipboardMaxDays; }
    void setClipboardMaxDays(int days);
    bool clipboardImages() const { return m_settings.clipboardImages; }
    void setClipboardImages(bool on);
    QStringList clipboardExcludedApps() const { return m_settings.clipboardExcludedApps; }
    int clipboardCount() const { return m_clipboardCount; }
    void setClipboardCount(int count); // set by the app
    QString clipboardNumberKeys() const { return m_settings.clipboardNumberKeys; }
    void setClipboardNumberKeys(const QString& keys);
    QString clipboardNumberKeysNote() const { return m_clipboardNumberKeysNote; }

    bool foldersFirst() const { return m_settings.foldersFirst; }
    void setFoldersFirst(bool on);
    bool showModified() const { return m_settings.showModified; }
    void setShowModified(bool on);
    bool thumbnails() const { return m_settings.thumbnails; }
    void setThumbnails(bool on);
    QString sort() const { return m_settings.sort; }
    void setSort(const QString& sort);
    QString startList() const { return m_settings.startList; }
    void setStartList(const QString& list);
    QString numberKeys() const { return m_settings.numberKeys; }
    void setNumberKeys(const QString& keys);
    QString numberKeysNote() const { return m_numberKeysNote; }
    // Looks again at which number keys are taken (as the window comes to the
    // front: a program may have been started or closed meanwhile).
    Q_INVOKABLE void refreshNumberKeys();

    bool taskbarWinS() const { return m_settings.taskbarWinS; }
    void setTaskbarWinS(bool on);
    QString winSState() const { return m_winSState; }
    void setWinSState(const QString& state, bool canRestartExplorer); // set by the app
    bool taskbarStartTyping() const { return m_settings.taskbarStartTyping; }
    void setTaskbarStartTyping(bool on);
    bool taskbarSearchBox() const { return m_settings.taskbarSearchBox; }
    void setTaskbarSearchBox(bool on);
    bool searchBoxSupported() const;
    bool searchBoxShown() const { return m_searchBoxHidden.isEmpty(); }
    QString searchBoxHidden() const { return m_searchBoxHidden; } // why not: "pending", "noroom"... (App)
    void setSearchBoxShown(const QString& hidden); // set by the app: empty when shown
    bool taskbarStartGap() const { return m_settings.taskbarStartGap; }
    void setTaskbarStartGap(bool on);
    QString startGapState() const { return m_startGapState; }
    QString startGapReason() const { return m_startGapReason; }
    void setStartGapState(const QString& state, const QString& reason); // set by the app
    bool taskbarButtonPinned() const { return m_taskbarButtonPinned; }
    bool windowsSearchShown() const { return m_windowsSearchShown; }
    Q_INVOKABLE void refreshTaskbarState();
    // Has the taskbar button ask Windows to pin it (WinShunSearch.exe --pin):
    // the user says yes in Windows' own dialog. Where Windows does not ask,
    // the button's Start menu shortcut is selected in File Explorer for the
    // user to pin. False when there is no button program.
    Q_INVOKABLE bool pinTaskbarButton();
    // The Start menu shortcut to the taskbar button, selected in Explorer for
    // the user to pin. False when there is none.
    Q_INVOKABLE bool showTaskbarButton();

    QVariantList webShortcuts() const;
    QVariantList fileTypes() const;
    bool fileTypesDefault() const { return m_settings.fileTypes == Settings::defaultFileTypes(); }
    // Each returns what is wrong ("已有这个类型"), empty when done.
    Q_INVOKABLE QString addFileType(const QString& name, const QString& extensions);
    Q_INVOKABLE QString renameFileType(int index, const QString& name);
    Q_INVOKABLE QString setFileTypeExtensions(int index, const QString& text); // "jpg, .PNG *.gif"
    Q_INVOKABLE void removeFileType(int index);
    Q_INVOKABLE void moveFileType(int index, int delta); // up (-1) or down (1) the strip
    Q_INVOKABLE void resetFileTypes();

    QVariantList shares() const;
    QVariantList knownShares() const;
    void setShareStatuses(std::vector<ShareIndexer::Status> statuses); // set by the app
    // "\\host", "\\host\": a server, whose shares can be listed; its name, else empty.
    Q_INVOKABLE QString serverOf(const QString& text) const;
    // Adds a share typed or chosen ("\\host\share[\folder]", or a mapped "Z:\...").
    // Returns why it cannot be, or an empty string once it is.
    Q_INVOKABLE QString addShare(const QString& text);
    Q_INVOKABLE void removeShare(int index);
    Q_INVOKABLE void verifyShare(int index);
    Q_INVOKABLE void signInToShare(int index);
    Q_INVOKABLE void acceptShareMissing(int index);
    // The shares of a server, in the background: serverSharesListed().
    Q_INVOKABLE void listServerShares(const QString& server);
    Q_INVOKABLE void signInToServer(const QString& server); // to list its shares
    void setSignedIn(const QString& path, const QString& problem); // set by the app: how signing in went

    void setSettings(const Settings& settings); // the file was changed outside this editor
    void setWindow(QWindow* window) { m_window = window; }
    void setPlaces(SystemCatalog* places) { m_places = places; }

    // For a search of the settings that found nothing: the settings and
    // tools of Windows that fit `query` (as the launcher finds them), best
    // first, at most `max`: [{name, icon (an image URL), command}].
    Q_INVOKABLE QVariantList findWindowsSettings(const QString& query, int max) const;
    Q_INVOKABLE void openWindowsSetting(const QString& command);

    Q_INVOKABLE void addExcludedFolder(); // asks for the folder
    Q_INVOKABLE void removeExcludedPath(int index);
    Q_INVOKABLE bool addExcludedName(const QString& name);
    Q_INVOKABLE void removeExcludedName(int index);
    Q_INVOKABLE void removeDialogBarExcludedApp(int index);
    Q_INVOKABLE bool addContentExtensions(const QString& text); // "md, .log *.csv"
    Q_INVOKABLE void removeContentExtension(int index);
    Q_INVOKABLE bool addClipboardExcludedApp(const QString& name); // "KeePass", "keepass.exe"
    Q_INVOKABLE void removeClipboardExcludedApp(int index);
    Q_INVOKABLE bool addDoubleCtrlExcludedApp(const QString& name); // "TheFinals", "thefinals.exe"
    Q_INVOKABLE void removeDoubleCtrlExcludedApp(int index);
    // Adds a web shortcut (`index` -1) or changes the one at `index`. Returns
    // why it cannot be saved, or an empty string once it is.
    // `icon`: a picture chosen with pickWebIcon, empty for the site's own.
    Q_INVOKABLE QString saveWebShortcut(int index, const QString& keyword, const QString& name, const QString& url,
        const QString& home, const QString& icon = {});
    Q_INVOKABLE void removeWebShortcut(int index);
    Q_INVOKABLE void openWebShortcut(int index); // its site, to try it
    // Asks for a PNG or ICO to show for a shortcut and keeps it: {icon: its
    // name} to save with it, {error: why not}, or {} when cancelled.
    Q_INVOKABLE QVariantMap pickWebIcon();
    // Asks for a file and adds the shortcuts in it (a keyword already here is
    // replaced), or writes them all to one: what came of it, to show.
    Q_INVOKABLE QString importWebShortcuts();
    Q_INVOKABLE QString exportWebShortcuts();
    // The icon of a shortcut's site (FileIconProvider::webIconUrl), by its home page.
    Q_INVOKABLE QString webIconUrl(const QString& homeUrl, bool dark) const;
    int siteIconsArrived() const { return m_siteIconsArrived; }
    Q_INVOKABLE void clearClipboard() { emit clipboardClearRequested(); } // what is in no group
    Q_INVOKABLE void restartExplorer() { emit explorerRestartRequested(); }
    Q_INVOKABLE void restoreDefaults();
    Q_INVOKABLE void clearHistory() { emit historyClearRequested(); }
    Q_INVOKABLE void openDataFolder();
    // Automatic backups of the settings: into a folder asked for here; off.
    // Returns whether they are on now.
    Q_INVOKABLE bool chooseBackupFolder();
    Q_INVOKABLE void stopBackups();
    // The app moves the index files once the setting changes (App::applyIndexFolder).
    Q_INVOKABLE void chooseIndexFolder(); // asks for the folder
    Q_INVOKABLE void resetIndexFolder() { setIndexFolder({}); } // back to the default place
    Q_INVOKABLE void openIndexFolder();
    Q_INVOKABLE void restart() { emit restartRequested(); } // after changing the renderer

    // 拾穗计划 page
    Q_INVOKABLE void openUrl(const QString& url);
    Q_INVOKABLE void copyText(const QString& text);

    // AI 助手 page
    bool mcp() const { return m_settings.mcp; }
    void setMcp(bool on);
    bool mcpAllowOpen() const { return m_settings.mcpAllowOpen; }
    void setMcpAllowOpen(bool on);
    bool mcpClipboard() const { return m_settings.mcpClipboard; }
    void setMcpClipboard(bool on);
    bool mcpPlugins() const { return m_settings.mcpPlugins; }
    void setMcpPlugins(bool on);
    bool mcpPluginActions() const { return m_settings.mcpPluginActions; }
    void setMcpPluginActions(bool on);
    QStringList mcpExcludedPaths() const;
    Q_INVOKABLE void addMcpExcludedFolder(); // asks for the folder
    Q_INVOKABLE void removeMcpExcludedPath(int index);
    QString mcpStatus() const { return m_mcpStatus; }
    void setMcpStatus(const QString& status); // set by the app: who is connected
    QVariantList mcpClients() const { return m_mcpClients; }
    QString mcpRelayPath() const { return m_mcpRelayPath; }
    Q_INVOKABLE void refreshMcpClients(); // looks again (as the window comes to the front)
    Q_INVOKABLE void connectMcpClient(const QString& id); // writes Win顺 into its configuration; mcpMessage says how it went
    Q_INVOKABLE void copyMcpSnippet(const QString& id); // its configuration (or command) to the clipboard; "": the generic JSON
    QString mcpMessage() const { return m_mcpMessage; }
    Q_INVOKABLE void clearMcpMessage() { setMcpMessage({}); }
    Q_INVOKABLE void openMcpLog(); // mcp.log, or the data folder while there is none

    // 插件 page
    PluginSettings* plugins() const { return m_plugins; }

    // 收藏夹 page
    FavoritesSettings* favorites() const { return m_favorites; }

signals:
    void historyCountChanged();
    void historyClearRequested();
    void changed();
    void hotkeyErrorChanged();
    void contentIndexStatusChanged();
    void indexFolderChanged();
    void recordingHotkeyChanged(); // the app suspends its hotkeys meanwhile
    void autostartChanged();
    void winVStateChanged();
    void winSStateChanged();
    void searchBoxShownChanged();
    void startGapStateChanged();
    void taskbarStateChanged();
    void fileManagersChanged();
    void clipboardCountChanged();
    void numberKeysNoteChanged();
    void doubleCtrlClashChanged();
    void opusFolderMenuChanged();
    void opusRestarted(); // setOpusFolderMenu started Opus again: it came to the front
    void clipboardClearRequested();
    void explorerRestartRequested(); // Win+V changes hands when Explorer starts again
    void edited(const ws::Settings& settings);
    void restartRequested();
    void sharesChanged();
    void siteIconArrived();
    // `problem` is empty when the server answered; `signIn` when it wants a user name first.
    void serverSharesListed(const QString& server, const QStringList& shares, const QString& problem, bool signIn);
    void shareVerifyRequested(const QString& path);
    void shareAcceptRequested(const QString& path);
    void signInRequested(const QString& path); // a share, or "\\host"
    void signedIn(const QString& path, const QString& problem); // empty problem: connected
    void mcpStatusChanged();
    void mcpClientsChanged();
    void mcpMessageChanged();
    void indexAccessStateChanged();
    void elevatedRestartRequested();
    void indexServiceInstallRequested(bool install); // false: uninstall

private:
    friend class PluginSettings; // edits m_settings' plugin sections and commits
    friend class FavoritesSettings; // reads the web shortcuts' keywords and the window
    void commit();
    void setIndexFolder(const QString& folder);
    void setMcpMessage(const QString& message);

    Settings m_settings;
    QString m_runningRenderer; // resolved: never "auto"
    QString m_hotkeyError;
    QString m_numberKeysNote;
    QString m_clipboardNumberKeysNote;
    QString m_doubleCtrlClash;
    QString m_folderMenuHotkeyError;
    QString m_opusFolderMenu = QStringLiteral("manual");
    QString m_opusFolderMenuTaken;
    QString m_opusFolderMenuProblem;
    bool m_opusRunning = false;
    int m_historyCount = 0;
    int m_siteIconsArrived = 0;
    QString m_contentIndexStatus;
    IndexFolderState m_indexFolderState;
    IndexAccessState m_indexAccessState;
    QStringList m_fileManagers {QStringLiteral("explorer")};
    bool m_recordingHotkey = false;
    QString m_winVState = QStringLiteral("off");
    QString m_winSState = QStringLiteral("off");
    bool m_canRestartExplorer = false;
    bool m_taskbarButtonPinned = false;
    bool m_windowsSearchShown = true;
    QString m_searchBoxHidden = QStringLiteral("pending");
    QString m_startGapState = QStringLiteral("off");
    QString m_startGapReason;
    QString m_clipboardHotkeyError;
    int m_clipboardCount = 0;
    QPointer<QWindow> m_window;
    QString m_pickedWebIcon; // chosen in a form not saved yet: kept from pruning
    std::vector<ShareIndexer::Status> m_shareStatuses;
    std::vector<unc::Known> m_knownShares; // read once, with the window
    SystemCatalog* m_places = nullptr; // the app's; outlives the window
    std::unique_ptr<ShortcutCapture> m_capture; // while recording a hotkey
    struct TaskbarWatch; // the taskbar's pinned programs and Windows' search box setting
    std::unique_ptr<TaskbarWatch> m_taskbarWatch;
    QString m_mcpStatus;
    QVariantList m_mcpClients;
    QString m_mcpRelayPath;
    QString m_mcpMessage;
    PluginSettings* m_plugins = nullptr; // a child
    FavoritesSettings* m_favorites = nullptr; // a child
};

} // namespace ws
