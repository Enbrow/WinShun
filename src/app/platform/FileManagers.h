#pragma once

#include <windows.h>

#include <QByteArray>
#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The file managers Win顺 works with: File Explorer, Total Commander,
// Directory Opus and XYplorer. They are asked which folders their windows
// show (for the Open and Save dialogs: Ctrl+G, the bar beside them, going
// there by themselves), and told to open a folder or to show where an item
// is (Settings::fileManager, through shell::open and shell::reveal). Any
// other one (Kind::Custom) only opens folders and shows items, by command
// lines of the user's own (setCustomCommands): it cannot be asked.
//
// Total Commander answers window messages (WM_USER+50) at once. Directory
// Opus answers through its dopusrt.exe, started with the user's normal
// rights: started elevated, as Win顺 is, it waits for an answer that never
// comes (docs/pitfalls.md). XYplorer runs a script sent by WM_COPYDATA and
// answers by WM_COPYDATA to a window of ours, while the message is still
// being sent (GitHub #23).
namespace ws::filemanager {

enum class Kind {
    Explorer,
    TotalCommander,
    DirectoryOpus,
    XYplorer,
    Custom,
};

// The names Settings::fileManager knows them by.
inline constexpr std::pair<Kind, const char*> kSettingNames[] {
    {Kind::Explorer, "explorer"},
    {Kind::TotalCommander, "totalcmd"},
    {Kind::DirectoryOpus, "dopus"},
    {Kind::XYplorer, "xyplorer"},
    {Kind::Custom, "custom"},
};

// The command lines for Kind::Custom (Settings::fileManagerOpenCommand and
// fileManagerRevealCommand; pathtext::customCommand says how they are read).
// No reveal command: an item's folder is opened instead.
void setCustomCommands(const QString& open, const QString& reveal);
// The program such a command line names, found as Windows finds it (on the
// PATH when it has no folder); empty when there is none.
QString customProgram(const QString& commandLine);

// Which one's main window this is (a top-level window); nullopt for any other.
std::optional<Kind> kindOf(HWND window);

struct Folder {
    std::wstring path;
    Kind shownIn;
};

// The folders their windows show: each window's folder in front (the tab on
// top, the panel with the focus), front to back, then the others (tabs
// behind, the other panel). Folders on disk only: no Home, This PC,
// searches, FTP sites or archives. Needs COM; asks the other programs
// (Directory Opus only while one of its windows is open: asking starts it),
// so not on the GUI thread.
std::vector<Folder> openFolders();

// An Explorer tab by its window (ShellTabWindowClass; the top-level window
// on Windows 10, which has none): the folder on disk it shows (empty for
// Home, This PC, a search and the like), and taking it to another folder
// there, as going there in it would (its Back button comes back). Need COM.
// False when the tab is not among the shell's windows (they can miss one).
std::wstring explorerFolder(HWND tab);
bool goTo(HWND tab, const std::wstring& folder);

// The program that takes the commands: TOTALCMD64.EXE (TOTALCMD.EXE),
// dopusrt.exe, XYplorer.exe, the custom open command's; empty when it is
// not installed. A Total Commander or XYplorer that runs (maybe a portable
// one) before the one installed. Explorer: always empty.
std::wstring program(Kind kind);
bool installed(Kind kind); // Explorer and Custom always: Custom is to be set up

// With the user's normal rights, in a new tab (Directory Opus and XYplorer:
// the tab that shows it already, if any), starting the program if it does
// not run. False when it is not installed or did not start. Not for
// Explorer: that is shell::open's and shell::reveal's own.
bool openFolder(Kind kind, const std::wstring& folder);
// Directory Opus's lister used last, in its tab in front, goes to `folder`
// (its Back button comes back), as the folder menu's double click there
// asks. False when Opus is not installed.
bool goToInOpus(const std::wstring& folder);
// Directory Opus's Preferences window.
bool openOpusPreferences();
// Directory Opus's settings file (prefs.oxc); empty when there is none.
std::wstring opusPrefsFile();
bool opusRunning(); // with or without a window open
// Changes Directory Opus's settings file: `edit` gets what it holds and
// gives what it is to hold (nullopt: leave it). Opus reads the file only
// when it starts, and writes it when its Preferences are confirmed: one
// that runs is closed first and started again after (its windows come
// back, to the front: waited for, 3 s at most; one that ran in the tray
// only stays so). Takes seconds: not on the GUI thread.
enum class PrefsEdit {
    Done,
    OpusStayed, // Opus would not close (it asked about a copy still going, say)
    Failed, // the file could not be read or written
};
PrefsEdit editOpusPrefs(const std::function<std::optional<QByteArray>(const QByteArray&)>& edit);
// Each item's folder, with the item selected (in Total Commander, which has
// one cursor, the first item of each folder).
bool reveal(Kind kind, const std::vector<std::wstring>& paths);

} // namespace ws::filemanager
