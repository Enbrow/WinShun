#pragma once

#include <QString>

// Explorer's right-click "用 Win顺 搜索此处" (GitHub #20) on a folder, in a
// folder's empty space, and on a drive: it runs WinShunSearch.exe
// --search-here "<folder>" (src/stub), which hands the folder to Win顺.
// Classic verbs under HKCU\Software\Classes: no administrator needed, the
// same for an installed and a portable copy; on Windows 11 they are under
// "显示更多选项" (Shift+right-click). They name the copy that set them; a
// copy moved or updated sets them again at its next start.
namespace ws::shellmenu {

// Sets the verbs with this title, pointing at the WinShunSearch.exe beside
// this program (`on`), or removes the ones this copy set (`!on`; another
// copy's stay). Writes only what differs; Explorer is told when anything did.
void apply(bool on, const QString& title);

} // namespace ws::shellmenu
