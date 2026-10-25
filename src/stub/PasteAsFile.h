#pragma once

#include <windows.h>

#include <string>

// 粘贴为文件 (the PasteEx request; core/PasteFile.h decides what and how):
// the words or the picture on the clipboard made a file in a folder, which is
// then selected in the window showing it with its name ready to be typed, as
// Explorer's 新建 does, " (2)" after a name taken. Copied there by the shell
// (IFileOperation) from a file written in %TEMP%: a protected folder asks
// for an administrator as Explorer's own Paste does, and a .zip or a phone
// takes it too. Started by Explorer's right-click (ShellMenu.cpp) and by Win顺 for
// Ctrl+V (TypeToSearch), with the user's rights.
namespace ws::pasteasfile {

// Where the file goes: `folder`, named by Explorer's right-click ("%V", the
// folder shown; Explorer is in front, so the tab on top is where it shows),
// or `view`, where Win顺 took Ctrl+V: an Explorer tab (ShellTabWindowClass,
// or the window on Windows 10), or the desktop's window, Progman or WorkerW.
struct Target {
    std::wstring folder;
    HWND view = nullptr;
};

// Done or told why not (the clipboard holds nothing to make a file of, the
// place cannot hold files): to the right-click; Ctrl+V took only what makes
// a file and is silent where Explorer would have done nothing.
void paste(const Target& target, bool english);

} // namespace ws::pasteasfile
