#pragma once

#include "Settings.h"
#include "Win32Util.h"
#include "platform/ClipboardWatcher.h"
#include "platform/KeyRouter.h"
#include "platform/TaskbarSearch.h"
#include "platform/TaskbarSearchBox.h"
#include "platform/TaskbarStartGap.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QTranslator>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class QQmlApplicationEngine;
class QQuickItem;
class QQuickWindow;
class QScreen;
class QWinEventNotifier;

namespace ws {

class AppCatalog;
class ClipStore;
class Clipboard;
class ComWorker;
class DialogBar;
class DialogJump;
class Favorites;
class FilePreview;
class FolderMenu;
class SystemCatalog;
class History;
class IndexService;
class IdleTrim;
class KeyListener;
class ServiceKeyListener;
class Launcher;
class MessageWindow;
class NetworkNotifier;
class Placement;
class SearchEngine;
class SettingsEditor;
class TypeToSearch;
class Updater;
class Backup;
class VolumeNotifier;
class WindowFrame;
class WindowLogo;
namespace win {
class TextCaret;
}
namespace mcp {
class McpServer;
class ToolHost;
struct Policy;
struct Services;
}
namespace ntfs {
class ServiceVolumeAccess;
}
namespace plugin {
class AiBridge;
class Host;
class Tools;
}

// Wires the pieces together and owns their lifetimes: settings, index,
// search engine, the QML window, tray icon and global hotkeys.
class App : public QObject {
    Q_OBJECT

public:
    struct StartOptions {
        bool background = false; // start in the tray without showing the window
        QString query;
        bool settings = false; // open the settings window
        QString settingsAt; // there (SettingsWindow.showPlace), after a restart from the settings
    };

    App();
    ~App() override;

    bool start(const StartOptions& options);
    void handleCommand(const QString& command); // from a second instance

    void toggleLauncher();
    void toggleClipboard(); // the clipboard window (Win+V)
    // With `spot`, over the taskbar, as Windows' own search (taskbar::);
    // `inBox`: over Win顺's box there, its field drawn in the box when the
    // box is wide enough for it.
    // `on`: the screen to open on (typed in an Explorer window there), else
    // the one under the mouse.
    void showLauncher(const QString& query = {}, const std::optional<taskbar::Spot>& spot = {}, bool inBox = false,
        QScreen* on = nullptr);
    void hideLauncher();
    // The launcher as the search panel (Launcher::panel): the window sized
    // for it (Placement::enterPanel) and resizable, or back as the box at
    // its spot (or over the taskbar it came from). Opening anew is always
    // the box: the panel is a mode its user asks for. Hidden as the panel,
    // it goes back quietly.
    void setPanel(bool on);
    void togglePanel();
    // Launcher::pinned changed. Pinned over Win顺's box on the taskbar, the
    // field comes back into the window and the box goes idle (as for the
    // panel): the window stays where it stands, a window of its own now.
    void launcherPinnedChanged();
    // --dump <file> from another copy (tests): the launcher's state and rows as JSON.
    void dumpLauncher(const QString& file) const;
    void showClipboard();
    void showSettings();
    void showUpdate(); // the settings window with the update dialog
    // Set by "重新启动" in the tray menu or the settings window: the command
    // line for the new copy, which main() starts once this one has let go,
    // and what it runs with: as this one, as administrator (UAC asks once:
    // the 管理员直读 mode chosen, GitHub #21), or with the user's normal
    // rights (that mode left while running elevated).
    enum class Elevation { Same, Elevated, Unelevated };
    std::optional<QStringList> restartArguments() const { return m_restartArguments; }
    Elevation restartElevation() const { return m_restartElevation; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool createWindow();
    bool createClipWindow();
    QQuickWindow* createPreviewWindow(); // FilePreview's, the first time it is wanted
    void onDoubleCtrl(); // toggleLauncher(), unless a game is played (GameGuard)
    // `launcherToo`: and the launcher, when the clipboard was under its
    // search box and the keyboard went elsewhere.
    void hideClipboard(bool launcherToo = true);
    bool clipboardUnderLauncher() const; // opened from the launcher's search box
    bool clipboardInCharge() const; // shown, the keys its own or through KeyRouter
    // Over another program the clipboard leaves it the focus, its keys come
    // through KeyRouter: from its showing until it goes or takes the focus
    // after all (activateClipboard).
    bool startKeyRouting(HWND target);
    void stopKeyRouting();
    void routedKey(const KeyRouter::Key& key);
    void routedForeground(HWND window);
    // Taking the focus after all: to type a group's name (with the input
    // method), to hand over to another program.
    void activateClipboard();
    // Pinned (Clipboard::pinned): what would hide the window gives the keys
    // back instead, to the program it pastes into if `giveFocus` and the
    // window has the focus; engaging takes them again, routed from the
    // program in front, which it then pastes into.
    void releaseClipboard(bool giveFocus);
    void engageClipboard();
    void clipboardPinnedChanged();
    void uncloakClipboard(); // its first frame is up
    void prewarmClipboard();
    void finishClipPrewarm();
    QQuickItem* focusedField() const; // a text field of Win顺's own with the keyboard
    void returnToField(QQuickItem* field); // from the clipboard (Clipboard::fieldRequested)
    void updateCompanions(); // our windows that go with a file dialog (DialogJump)
    // Ctrl+1… on the launcher and the dialog bar: as one of them comes to
    // the front, which digits other programs hold (numberkeys::heldMask).
    void updateNumberKeys();
    void applySettings(bool initial);
    ClipboardWatcher::Options clipboardOptions() const;
    void applyClipboard(); // the [Clipboard] settings
    void applyClipboardHotkeys(); // Win+V and the other shortcut; where Win+V stands, for the settings
    void clipCaptured(const ClipCapture& capture);
    void applyTaskbar(); // the [Taskbar] settings: Win+S, typing in the Start menu
    void applyTaskbarHotkeys(); // Win+S and Win+Shift+S; where Win+S stands, for the settings
    // The taskbar button was clicked (`clicked`), or Win+S pressed: the
    // launcher over the taskbar, or away again.
    void toggleAtTaskbar(bool clicked);
    void startScreenClip(); // Win+Shift+S, which Explorer gives up with Win+S
    void applyTypeToSearch(); // typing in the Start menu, in Explorer (TypeToSearch), as the settings say
    void startMenuTyped(); // typed in the Start menu (TypeToSearch): the launcher over the taskbar
    // Typed in the Explorer window `root` (TypeToSearch): its `tab`'s folder
    // is asked for on a worker, then the launcher opens in that folder, on
    // that window's screen, and the keys follow; with no folder there, the
    // keys go back to Explorer.
    void explorerTyped(HWND root, HWND tab);
    void explorerFolderKnown(HWND root, const QString& folder);
    // Win顺's own search box on the taskbar ([Taskbar] SearchBox, taskbar::SearchBox).
    void applySearchBox();
    void setSearchBoxShown(taskbar::SearchBox::Hidden why); // it is on the taskbar (None), or why not
    void setSearchBoxWide(bool wide); // the whole box, or its label or icon alone (SearchBox::wide)
    void setClassicTaskbar(bool classic); // SearchBox::Callbacks::classicChanged
    void applyStartGap(bool boxWas);
    void setStartGapStatus(const taskbar::StartGap::Status& status);
    void updateSearchBoxLook(); // placeholder, accent
    void searchBoxPressed(const taskbar::SearchBox::Press& press);
    void showLauncherInBox(); // the launcher over the box (or, without one shown, over the taskbar)
    // What the launcher's field has, drawn in the box while it is open over it.
    void mirrorField(const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition);
    std::optional<QRectF> boxCaretInField() const; // the box's caret, in the field's coordinates
    void deliverTyping(); // ... has the keyboard: the keys typed there go to it
    void restartExplorer(); // so that Win+V and Win+S change hands now
    void explorerRestarted(bool back); // `back`: the taskbar is
    void setClipboardPaused(bool paused);
    void showClipboardSettings(); // the settings window on its 剪贴板 page
    void showWebSettings(); // ... on its 网页搜索 page (a web row's menu)
    void showFileTypeSettings(); // ... on its 结果列表 page, at the file types (the + of the launcher's strip)
    void showFavoriteSettings(const QString& path); // ... on its 收藏夹 page, at that favorite (a row's menu)
    void showPluginSettings(const QString& pluginId); // ... on that plugin's card ("winshun:settings")
    void applyHotkey();
    void applyDialogs(); // Ctrl+G, the bar and going by themselves in file dialogs
    void applyFolderMenu(); // the menu on a double click in File Explorer (GitHub #22)
    void applyFolderMenuHotkey(); // ... and on its hotkey, anywhere (GitHub #25)
    void folderMenuHotkey(); // the hotkey was pressed: the menu, the launcher out of the way first
    void excludeFromDialogBar(const QString& app); // "not in this program" from the bar's menu
    void answerAutoJump(const QString& when); // asked under the bar: "open", "back" or "" (DialogBar::autoJumpAnswered)
    bool createBarWindow();
    void reloadSettings();
    void settingsEdited(const Settings& settings);
    void showTrayMenu();
    void restart(const QStringList& arguments, Elevation elevation = Elevation::Same);
    // 索引方式 (Settings::indexAccess, GitHub #21): where the volumes are
    // read from, the index service looked for again while it is wanted and
    // not there, the autostart task's run level kept with the mode.
    void applyAutostartLevel(); // at start: the task's run level follows the mode (and an old elevated task sets the mode, portable)
    // Rewrites the task (`task`: whether it runs Win顺 elevated, as read)
    // when that is not the mode's level, where this process may.
    void reconcileAutostart(const std::optional<bool>& task);
    void applyIndexAccess(); // the setting changed: restart with other rights, or switch in place
    void startServiceRetry(); // every 2 s for 30 s after start, then every 5 min
    void probeService(); // on a thread of its own; serviceProbed() on the GUI thread
    void serviceProbed(std::shared_ptr<ntfs::ServiceVolumeAccess> service, bool reached);
    void refreshIndexAccess(); // shown in the settings window
    QString serviceProblem() const; // why the index service is not in use, for the user
    void installIndexService(bool install); // WinShunService.exe --install / --uninstall, elevated (a portable copy)
    void applyTheme();
    void applyAppearance(); // the chosen theme and language
    QString trayTooltip() const;
    QString doubleCtrlName() const;
    void armReveal();
    void revealLauncher(); // slides in (Placement::slideIn) unless it went away meanwhile
    void finishHidingLauncher(); // slid out (Placement::slideOut), or at once
    bool launcherShown() const; // up, and not on its way out
    void prewarmLauncher();
    void finishPrewarm();
    void refreshContentIndexStatus(); // shown in the settings window
    void refreshShareStatuses(); // network shares: in the settings window, and told once read
    void signInToServer(const QString& path, unsigned long error = 0, const QString& user = {});
    // 索引位置: the index files go where [Index] Folder says, moved in the
    // background; when that fails, the setting goes back to where they are.
    QString startIndexFolder(); // where the index starts out
    void applyIndexFolder();
    QString indexFolderProblem(const QString& folder) const; // why the index cannot go there; empty if it can
    void indexMoved(int error);
    void indexMoveFailed(const QString& attempted, const QString& problem); // `attempted`: the setting
    void refreshIndexFolder(bool measure = false); // shown in the settings window; `measure`: its size too
    // AI 助手: the MCP server (src/mcp) on its pipe while [Mcp] Enabled, its
    // tools gated and shaped by the settings (mcpPolicy); what is connected
    // is shown in the settings window.
    void applyMcp();
    mcp::Policy mcpPolicy() const;
    void refreshMcpStatus();
    void writeMcpToolsCache(); // the tools, for the relay to list while Win顺 is not running
    // The plugins' commands as the AI's tools (plugin::AiBridge): what the
    // [Mcp] Plugins switches and the cards' "给 AI 用" say, applied from
    // applyMcp and applyPlugins alike.
    void applyAiBridge();
    // 插件: the plugin host (src/plugin), told what the [Plugin.<id>]
    // sections and [WebSearch] say; its plugins answer in the launcher.
    void createPluginHost();
    void applyPlugins();
    void pluginsChanged(); // the catalog's entries: the icon provider's folders
    // Win顺's tools for the plugins' winshun/tool (plugin::Tools): made
    // with the first call, whatever the AI switch says.
    plugin::Tools& pluginTools();
    mcp::Services toolServices() const; // what the tool hosts (the AI's, the plugins') need from the app

    Settings m_settings;
    std::unique_ptr<History> m_history;
    std::unique_ptr<Favorites> m_favorites; // the launcher's and the dialog bar's
    std::unique_ptr<IndexService> m_index;
    // The index service (GitHub #21): wanted by the mode, so looked for
    // while not in use; the probe's thread goes before the index does.
    bool m_serviceWanted = false;
    QString m_serviceWhy; // why the last look found it unusable (English, from the pipe client)
    QTimer m_serviceRetry;
    QElapsedTimer m_serviceRetrySince; // the 30 s of frequent tries after start
    std::atomic<bool> m_serviceProbing {false};
    std::jthread m_serviceProbe;
    bool m_accessTakenFromTask = false; // the mode was taken over from an old elevated task: said in the settings
    QString m_serviceMessage; // ... and how installing or uninstalling the service went
    bool m_serviceBusy = false; // WinShunService.exe --install / --uninstall running
    QString m_indexDir; // where the index files are, '/' separated
    QString m_indexFolder; // the [Index] Folder setting m_indexDir stands for
    QString m_indexMoveFolder; // ... and the one being moved to
    QString m_indexMoveDir;
    bool m_indexMoveMadeDir = false; // the move made that folder: it goes again if the move fails
    int m_indexMoveProgress = 0;
    QString m_indexFolderProblem; // shown in the settings window until the next move
    std::uint64_t m_indexBytes = 0; // of the files in m_indexDir, as last measured
    bool m_indexOnHardDisk = false; // likewise
    std::unique_ptr<AppCatalog> m_apps; // before the engine, which reads it
    std::unique_ptr<SystemCatalog> m_places; // likewise
    std::unique_ptr<SearchEngine> m_engine;
    // The dialog bar's own: an engine runs only its newest request, so one
    // shared with the launcher would drop the launcher's (and its rows would
    // wait for results that never come).
    std::unique_ptr<SearchEngine> m_dialogEngine;
    // The MCP server's own engine, host and server (applyMcp); in this
    // order, so the server goes first and the engine before the index.
    std::unique_ptr<SearchEngine> m_mcpEngine;
    std::unique_ptr<mcp::ToolHost> m_mcpHost;
    std::unique_ptr<mcp::McpServer> m_mcpServer;
    std::unique_ptr<plugin::AiBridge> m_aiBridge; // with the server: the plugins' commands among its tools
    std::unique_ptr<plugin::Tools> m_pluginTools; // before the host, whose bridge calls it
    std::unique_ptr<plugin::Host> m_pluginHost; // before the launcher, which asks it
    std::unique_ptr<Launcher> m_launcher;
    std::unique_ptr<ClipStore> m_clipStore; // the clipboard history
    std::unique_ptr<Clipboard> m_clipboard; // its window's view-model
    std::unique_ptr<FilePreview> m_preview; // Alt+P on a result
    std::unique_ptr<WindowFrame> m_previewFrame;
    // Destroyed before the two above: its thread calls back into the app, and
    // the page waits on its writes.
    std::unique_ptr<ClipboardWatcher> m_clipWatcher;
    bool m_clipboardPaused = false; // from the tray menu, until Win顺 restarts
    bool m_clipboardNotified = false; // the last tray notification told of the clipboard history
    std::optional<bool> m_winVApplied; // the Win+V switch, as last put into the registry
    bool m_winVRegistryFailed = false;
    std::optional<bool> m_winSApplied; // the Win+S switch, likewise
    bool m_winSRegistryFailed = false;
    bool m_restartingExplorer = false;
    // restartExplorer's waiting, waited for on exit: quitting between ending
    // Explorer and starting it again would leave no taskbar.
    std::jthread m_explorerRestart;
    win32::UniqueHandle m_taskbarEvent; // set by the taskbar button (WinShunSearch.exe)
    std::unique_ptr<QWinEventNotifier> m_taskbarNotifier; // waits on it; goes first
    QElapsedTimer m_launcherHidden; // since the launcher last went away
    QElapsedTimer m_launcherShown; // since it was last asked for
    bool m_launcherClosing = false; // sliding out (hideLauncher)
    std::unique_ptr<ComWorker> m_typingWorker; // asks Explorer its folder (explorerTyped); before m_typing
    std::unique_ptr<TypeToSearch> m_typing; // while [Taskbar] StartMenuTyping or [Launcher] ExplorerTyping is on
    HWND m_typingExplorer = nullptr; // the launcher is up from typing in this Explorer window: Esc goes back to it
    std::unique_ptr<taskbar::SearchBox> m_searchBox; // while [Taskbar] SearchBox is on (Windows 11)
    bool m_searchBoxShown = false; // ... and it is on the taskbar
    taskbar::SearchBox::Hidden m_searchBoxHidden = taskbar::SearchBox::Hidden::Pending; // ... or why not
    bool m_classicTaskbar = false; // ... measured on Windows 10's taskbar (StartAllBack, ExplorerPatcher): no StartGap
    std::unique_ptr<taskbar::StartGap> m_startGap; // while [Taskbar] StartGap is on too: the room right of Start
    taskbar::StartGap::Status m_startGapStatus; // its latest, for the settings page
    // The launcher is open over the box: typing in it when the box is wide
    // (Launcher::inTaskbarBox), else with its own field, the box looking open.
    bool m_launcherOverBox = false;
    // A press on the box takes the keyboard from the launcher over it (Windows
    // does that for whatever is clicked on the taskbar): it comes back with
    // the press (searchBoxPressed), else it goes once this runs out.
    QTimer m_boxPressGrace;
    // Pinned (Launcher::pinned), it lost the focus to a press on the box:
    // that press closes it (searchBoxPressed). Since when.
    QElapsedTimer m_pinnedLostToBox;
    QPointer<QQuickItem> m_searchField; // the launcher's (SearchBar.qml): the input method asks it where the caret is
    taskbar::SearchBox::Text m_fieldShown; // what it last reported (mirrorField), for the box
    bool m_deliverTyping = false; // the launcher was asked for by typing in the Start menu or in Explorer
    QString m_clipboardShortcut; // what opens the clipboard: "Win+V", another shortcut, or nothing
    std::unique_ptr<MessageWindow> m_messages;
    std::unique_ptr<Updater> m_updater;
    std::unique_ptr<Backup> m_backup; // of the settings, into a folder chosen
    bool m_updateNotified = false; // the last tray notification announced a new version
    std::unique_ptr<VolumeNotifier> m_volumeNotifier; // after m_messages, whose window it uses
    std::unique_ptr<NetworkNotifier> m_network; // after m_index, whose shares it tells
    std::unique_ptr<IdleTrim> m_idleTrim; // gives the memory back while nothing is in use
    std::vector<std::wstring> m_sharesBuilding; // being read for the first time: told when done
    std::unique_ptr<KeyListener> m_keyListener; // double Ctrl, double clicks for the folder menu
    std::unique_ptr<ServiceKeyListener> m_serviceKeyListener; // destroyed before its target KeyListener
    int m_keyListenerMode = 0; // what it listens to: 1 double Ctrl, 2 double clicks
    std::unique_ptr<FolderMenu> m_folderMenu; // before the window: its view-model
    QPointer<QQuickWindow> m_folderMenuWindow;
    QStringList m_doubleCtrlIgnoredLogged; // "program reason", logged once each: a game gets many
    std::unique_ptr<DialogJump> m_dialogJump; // file dialogs: Ctrl+G, the bar; after m_messages, which has the hotkey
    std::unique_ptr<DialogBar> m_dialogBar; // the search bar under file dialogs
    QDeadlineTimer m_barFocusDeadline; // gone back to from the clipboard: until then, it takes the keyboard as it shows
    std::unique_ptr<Placement> m_placement; // where the launcher opens; moving it
    std::unique_ptr<WindowFrame> m_frame; // the launcher's header and footer drag it
    std::unique_ptr<Placement> m_clipPlacement; // likewise for the clipboard window
    std::unique_ptr<WindowFrame> m_clipFrame;
    std::unique_ptr<win::TextCaret> m_textCaret; // where another program's caret is: the clipboard opens there
    std::unique_ptr<KeyRouter> m_keyRouter; // while the clipboard is over a program that keeps the focus
    HWND m_keyRouterTarget = nullptr; // that program's window
    std::uint64_t m_keyRouterRun = 0; // which routing its posted keys belong to
    std::unique_ptr<KeyRouter> m_drainingRouter; // done, until the keys it took are let go (KeyRouter::drain)
    std::uint64_t m_drainingRun = 0;
    std::atomic<bool> m_clipUncloak {false}; // shown cloaked until its first frame
    QTimer m_clipUncloakTimeout;
    std::atomic<bool> m_clipPrewarming {false}; // shown cloaked for one frame (see prewarmClipboard)
    // The launcher is shown cloaked and revealed once it has drawn a frame
    // with fresh results (see showLauncher). Set on the GUI thread, read on
    // the render thread.
    bool m_revealing = false;
    std::atomic<bool> m_revealArmed {false}; // results are in: the next synchronised frame is the one
    std::atomic<bool> m_revealSynced {false}; // that frame has been synchronised
    QTimer m_revealTimeout;
    std::atomic<bool> m_prewarming {false}; // shown cloaked for one frame (see prewarmLauncher)
    std::unique_ptr<QQmlApplicationEngine> m_qml; // destroyed first: QML references the objects above
    QPointer<QQuickWindow> m_window;
    WindowLogo* m_launcherLogo = nullptr; // m_window's
    QPointer<QQuickWindow> m_clipWindow; // the clipboard's; created a moment after the start
    WindowLogo* m_clipLogo = nullptr; // m_clipWindow's
    QPointer<QQuickWindow> m_settingsWindow; // created on demand, deleted when closed
    QPointer<QQuickWindow> m_barWindow; // the dialog bar's; created with the first file dialog
    QPointer<SettingsEditor> m_settingsEditor; // owned by m_settingsWindow
    QString m_renderer; // the one in use; changing it takes a restart
    QString m_language; // resolved: zh or en
    QTranslator m_translator; // English; the source strings are Chinese
    std::optional<QStringList> m_restartArguments;
    Elevation m_restartElevation = Elevation::Same;

    QFileSystemWatcher m_settingsWatcher;
    QTimer m_settingsReload;
    QTimer m_indexOptionsApply; // rebuilding the crawler is costly: batch quick edits
    QTimer m_contentStatusTimer; // while the settings window is open
    QTimer m_darkFrameGuard; // see applyTheme
    QElapsedTimer m_darkFrameWatch;
    bool m_darkFrame = false;
    bool m_diagnosticsExporting = false;
    std::jthread m_diagnosticsExport; // joined before the app's objects are destroyed
};

} // namespace ws
