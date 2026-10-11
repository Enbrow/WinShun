#pragma once

// Renders settings pages to PNG files without showing anything, for checking
// their layout in both themes and scalings without touching the Win顺 that
// is running. Two modes:
//
//   WinShun.exe --preview-plugins <file.png> [--dark] [--scale 1.5] [--dialog] [--page ai]
//     The 插件 page on its own (PluginsPreview.qml) over made-up plugins
//     (PluginSettings::loadPreview), or the AI 助手 page.
//
//   WinShun.exe --preview-settings <folder> [--dark] [--scale 1.5] [--tree]
//     The whole settings window (the real SettingsWindow.qml over a real
//     SettingsEditor, WindowFrame and Updater on a temporary profile): every
//     page and every tab of it as <folder>\pNN-tT.png, the window as tall
//     as the page needs, then the settings search driven with a few words
//     (search-<word>.png, and where Enter would go, checked against the
//     page it should be). The log is <folder>\preview.log.
//
// Nothing of the user's is read or written (a temporary profile), and
// nothing that reaches the running Win顺 is started: no hotkeys, hooks,
// clipboard listener, taskbar box, pipe server or tray icon. The
// environment the rendering needs (the scale factor, the software renderer)
// is set before QGuiApplication by prepare(); run() does the rest once the
// application exists. The exit code is the number of QML warnings, so that
// a check script can tell.

#include <QStringList>

namespace ws::preview {

bool wanted(const QStringList& args); // --preview-plugins or --preview-settings is among them
void prepare(const QStringList& args); // before QGuiApplication
void note(const QString& line); // a line into the log, from the start on
int run(const QStringList& args); // after it; the exit code

} // namespace ws::preview
