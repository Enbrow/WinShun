#pragma once

namespace ws {

// The program used to be called 快搜 / QuickFind. On the first start under the
// new name this asks a running QuickFind to quit (it saves its index), then
// moves %APPDATA%\QuickFind and %LOCALAPPDATA%\QuickFind over to WinShun, so
// settings, the index and the history carry over, and moves autostart to the
// new logon task. Runs before anything reads the settings; a no-op afterwards.
void migrateFromQuickFind();

// A portable copy (Settings::portableFolder) started for the first time:
// the settings and data in the usual places (%APPDATA%\WinShun,
// %LOCALAPPDATA%\WinShun) are copied into its folder, so switching to it
// loses nothing: history, clipboard, favorites, the index. Copied, not
// moved: an installed copy may still use them. Run while no other copy is
// running (their files are closed); a no-op once the folder has settings.
void copyIntoPortable();

} // namespace ws
