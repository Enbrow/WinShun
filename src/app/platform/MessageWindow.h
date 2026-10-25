#pragma once

#include <QString>

#include <windows.h>

#include <functional>

namespace ws {

// A hidden native window that receives what Qt does not handle for us:
// tray icon events, RegisterHotKey hotkeys, commands from a second instance
// of the app (WM_COPYDATA), drives coming and going (WM_DEVICECHANGE) and
// Windows resuming. It also restores the tray icon when Explorer restarts.
//
// Beside it, a second hidden window ("WinShun.SearchHere") takes a folder
// from Explorer's "search here" (WinShunSearch.exe --search-here, run with
// the user's normal rights, GitHub #20). It alone is open to programs that
// are not elevated, and it takes nothing but a folder's path: the commands
// above (quit, settings) stay closed to them.
class MessageWindow {
public:
    struct Callbacks {
        std::function<void()> trayActivated; // left click
        std::function<void()> trayMenuRequested; // right click
        std::function<void()> notificationClicked; // the last showNotification()
        std::function<void(int id)> hotkeyPressed;
        std::function<void(const QString&)> commandReceived;
        std::function<void(const QString& folder)> searchHere; // "D:\Work", "\\nas\docs"
        // A double click on blank space in Directory Opus showing `folder`
        // (WinShunSearch.exe --folder-menu, run by Opus).
        std::function<void(const QString& folder)> folderMenu;
        std::function<void()> sessionEnding; // Windows is logging off / shutting down
        std::function<LRESULT(WPARAM, LPARAM)> deviceChange; // WM_DEVICECHANGE (see VolumeNotifier)
        std::function<void()> shellRestarted; // Explorer started again (its taskbar is back)
        std::function<void()> resumed; // from sleep or hibernation
        // Going to sleep, back from it, fonts added or removed (WM_POWERBROADCAST,
        // WM_FONTCHANGE): what the diagnostics record notes of how windows draw.
        std::function<void(UINT message, WPARAM wParam)> drawingMessage;
    };

    explicit MessageWindow(Callbacks callbacks);
    ~MessageWindow();

    MessageWindow(const MessageWindow&) = delete;
    MessageWindow& operator=(const MessageWindow&) = delete;

    HWND hwnd() const noexcept { return m_hwnd; }

    void showTrayIcon(const QString& tooltip);
    void setTrayTooltip(const QString& tooltip); // the language changed
    // The tray icon in one colour, the taskbar's (monologo), or Win顺's own
    // colours; the one-colour icon follows the taskbar's light or dark mode.
    void setTrayIconMono(bool mono);
    void showNotification(const QString& title, const QString& text);
    void hideNotification(); // the one on the screen, if any

    bool registerHotkey(int id, const QString& shortcut); // "Alt+Space", "Ctrl+Shift+F"
    void unregisterHotkey(int id);

    // Returns true when another instance received the command.
    static bool sendToRunningInstance(const QString& command);
    static bool parseHotkey(const QString& shortcut, UINT* modifiers, UINT* vk);

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK searchHereProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT msg, WPARAM wParam, LPARAM lParam);
    bool addTrayIcon();
    void removeTrayIcon();
    void loadIcon(); // m_icon, as m_mono says

    Callbacks m_callbacks;
    HWND m_hwnd = nullptr;
    HWND m_searchHere = nullptr;
    HICON m_icon = nullptr;
    bool m_mono = false;
    HPOWERNOTIFY m_powerNotify = nullptr; // so that resuming is told on every kind of standby
    QString m_tooltip;
    bool m_trayVisible = false;
    UINT m_taskbarCreated = 0;
};

} // namespace ws
