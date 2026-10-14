#pragma once

#include "DoubleClickDetector.h"
#include "DoubleTapDetector.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <thread>

namespace ws {

// Global "double-tap Ctrl" detection through Raw Input. The mouse is listened
// to as well, from a Ctrl press until a second tap can no longer follow: a
// Ctrl held for a click, a drag or the wheel is not a tap.
//
// Key presses reach a hidden window on its own thread as WM_INPUT messages,
// after the fact. Unlike a low-level keyboard hook, which every key press in
// the system waits on, this never slows anyone's typing, and Windows cannot
// quietly drop it the way it drops a hook that answers too slowly.
//
// With `doubleClicks`, the mouse is listened to all the time, for double
// clicks of the left button (the folder menu, GitHub #22). Windows keeps one
// mouse registration per process: this is the only one, or the next Ctrl
// press here would replace another's and take it away again.
class KeyListener {
public:
    struct Callbacks {
        std::function<void()> doubleCtrl; // none: keyboards not listened to
        // A double click with no other button or modifier held: the window
        // under the pointer and where (physical pixels). None: the mouse only
        // around a Ctrl press.
        std::function<void(HWND window, POINT pt)> doubleClick;
    };

    // The callbacks run on the listener thread: post to the GUI thread from them.
    explicit KeyListener(Callbacks callbacks);
    ~KeyListener();

    KeyListener(const KeyListener&) = delete;
    KeyListener& operator=(const KeyListener&) = delete;

    bool isActive() const noexcept { return m_active.load(); }

    // Whether the folder menu is up (from any thread): a double click whose
    // first press closed it is not reported (DoubleClickDetector).
    void setMenuOpen(bool open) noexcept { m_menuOpen = open; }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void onInput(HRAWINPUT input);
    void onMouse(const RAWMOUSE& mouse);
    void listenToMouse(bool on);

    Callbacks m_callbacks;
    DoubleTapDetector m_detector; // only touched on the listener thread
    DoubleClickDetector m_clicks; // likewise
    bool m_mouseOn = false; // listener thread only
    std::atomic<bool> m_menuOpen {false};
    std::atomic<HWND> m_window {nullptr};
    std::atomic<bool> m_active {false};
    std::jthread m_thread;
};

} // namespace ws
