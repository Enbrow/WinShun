#pragma once

#include "SearchTypes.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Paths in text that other programs show or hand over, or that the user
// types: what was copied, what a file dialog's controls say, a path being
// typed into the search bar under file dialogs (and what it names on disk).
namespace ws::pathtext {

// A path copied as text: the first line, maybe in quotes (Explorer's "Copy
// as path"), maybe a file:/// URL or with %VARIABLES%. An absolute path on a
// drive ("C:\…") or a share ("\\server\share\…"), with backslashes and
// without a trailing one (but "C:\"); else empty.
QString pathFromText(const QString& text);

// A path being typed, split where the name being typed starts:
// "D:\Pro" -> {"D:\", "Pro"}, "D:\Projects\" -> {"D:\Projects", ""},
// "D:" -> {"D:\", ""}. Taken as pathFromText() takes text (quotes, file:
// URLs, %VARIABLES%). An empty `folder`: the text is no path, or the name
// has characters no name can have ("D:\*.pdf").
struct TypedPath {
    QString folder;
    QString name;
};
TypedPath splitTyped(const QString& text);

// A path typed from some folder along the way, not from a drive:
// "tools/screenshots/" -> {{"tools", "screenshots"}, true},
// "src\app\App.cpp" -> {{"src", "app", "App.cpp"}, false}. Empty `names`
// for what is no such path: no separator, an absolute path (splitTyped),
// what no name can be ("*.pdf", "!draft\", "a/ b", "..").
struct RelativePath {
    QStringList names;
    bool folder = false; // ends in a separator: the last name is a folder's
};
RelativePath splitRelative(const QString& text);

// What `typed.folder` holds that `typed.name` matches, as the search box
// matches names (anywhere in the name, by pinyin too; everything when no
// name is typed): folders first, then the best matches, then in Explorer's
// order (numbers by their value). With no name typed, the folder itself
// comes first. Hidden entries are left out, as Explorer leaves them out; at
// most `max`. Reads the disk: not on the GUI thread.
SearchResults listTyped(const TypedPath& typed, bool foldersOnly, int max);

// The extensions a file dialog's file type names, in lower case: "Text
// files (*.txt;*.log)" -> {"txt", "log"}. Empty when it takes every file
// ("*.*") or names no plain extension.
QStringList filterExtensions(const QString& fileType);

// The folder an address bar's caption names: "Address: C:\Windows" ->
// "C:\Windows". Empty for a place that is no folder on disk ("Address:
// This PC").
QString folderFromAddress(const QString& caption);

// Whether two paths name the same folder: case and a trailing backslash aside.
bool sameFolder(const QString& a, const QString& b);

// The folder a panel of Total Commander shows, from the text over its file
// list: "c:\Windows\*.*" -> "C:\Windows" (the filter after the last
// backslash goes). Empty for what is no folder on a drive or share: an FTP
// site, a plugin's "\\\Uninstaller64\*.*".
QString folderFromTotalCommander(const QString& text);

// A folder tab of Directory Opus, as `dopusrt /info <file>,paths` lists the
// tabs of its windows ("listers").
struct OpusTab {
    QString path; // on a drive or share; tabs on other places are left out
    quintptr lister = 0; // its window
    // 1: the tab in front on the side with the focus; 2: the one in front
    // on the other side (two file displays); 0: a tab behind.
    int state = 0;
};
QList<OpusTab> opusTabs(const QByteArray& xml);

// A folder tab of XYplorer, from what its window answers when asked which
// folders it shows (filemanager::, a script sent by WM_COPYDATA): a first
// line "1" when both panes are shown ("0": one), then a line per pane, the
// active one first, holding its tabs' paths joined by '|', the tab on top
// first, as pathreal() gives them ("C:" for a drive, no backslash at the
// end). What is no folder on a drive or share is left out: This PC, a
// search's results. A tab with a quick search ("C:\?*.bmp") stands for its
// folder. The other pane's tabs count only while it is shown.
struct XYplorerTab {
    QString path;
    int state = 0; // as OpusTab::state
};
QList<XYplorerTab> xyplorerTabs(const QString& reply);

// `text` as a string in an XYplorer script: in single quotes, in which
// nothing is resolved (%VARIABLES%, <variables>), one inside doubled.
QString xyplorerQuoted(const QString& text);

// A command line of the user's own for a file manager Win顺 does not know
// (Settings::fileManagerOpenCommand, fileManagerRevealCommand), to run for
// `path`: the program, in quotes when its path has spaces, then its
// arguments with {path} replaced by the path, quoted as the program's
// arguments are parsed (commandArgument); the path goes last when there is
// no {path}. %VARIABLES% are expanded. An empty program: no command.
struct CustomCommand {
    QString program;
    QString arguments;
};
CustomCommand customCommand(const QString& commandLine, const QString& path);
// `path` as one argument on a command line, as CommandLineToArgv (the C
// runtime, Rust, .NET) reads it: in quotes when it has a space or is empty,
// backslashes before the closing quote doubled ("C:\" would end in \").
QString commandArgument(const QString& path);

// What Directory Opus runs on a left double click on the blank space of a
// file display ("Preferences › File Displays › Mouse › Background Events",
// the line "Default"), as its prefs.oxc keeps it: <dblclk> … <bg_commands>
// <event command="…" enable="yes" id="0" />.
struct OpusBlankClick {
    bool hasPlace = false; // there is a <dblclk> to keep it in
    QByteArray element; // the <event> as written; empty when there is none
    QString command;
    bool enabled = false;
};
OpusBlankClick opusBlankClick(const QByteArray& prefs);
// The <event> that runs `command`.
QByteArray opusBlankClickEvent(const QString& command);
// `prefs` with that <event> replaced by `element` (an empty one takes it
// away), a <bg_commands> put in for it if need be; every other byte as it
// was. Empty when there is no place for it.
QByteArray withOpusBlankClick(const QByteArray& prefs, const QByteArray& element);

} // namespace ws::pathtext
