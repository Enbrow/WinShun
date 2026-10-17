#pragma once

#include "DoubleClickDetector.h"
#include "DoubleTapDetector.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
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
    struct Tap {
        std::string sent;
        std::uint32_t key = 0;
        std::uint32_t time = 0; // the input's clock, shared by both listeners
    };
    struct Callbacks {
        // None: keyboards not listened to. `sent` describes the last keys
        // when a program made some of the taps (SendInput), for the log:
        // which modifier and Ctrl keys, how far apart; empty otherwise.
        std::function<void(const Tap&)> doubleCtrl;
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
    HWND window() const noexcept { return m_window.load(); }
    void setServiceProcess(DWORD pid) noexcept { m_servicePid = pid; }
    bool serviceActive() const noexcept;

    // Whether the folder menu is up (from any thread): a double click whose
    // first press closed it is not reported (DoubleClickDetector).
    void setMenuOpen(bool open) noexcept { m_menuOpen = open; }

    // Which Ctrl counts (from any thread); the next press goes by it.
    void setSide(DoubleTapDetector::Side side) noexcept { m_side = side; }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void onInput(HRAWINPUT input);
    void onMouse(const RAWMOUSE& mouse);
    void listenToMouse(bool on);
    std::string sentTrace() const;
    void deliver(Tap tap);

    // The last keys, for sentTrace: modifiers and Ctrl by name, any other
    // only as "a key" (nothing typed is kept).
    struct Recent {
        std::uint8_t key = 0; // 0 none, else an index into the names
        bool up = false;
        bool sent = false; // no device: SendInput
        std::uint32_t time = 0;
    };

    Callbacks m_callbacks;
    DoubleTapDetector m_detector; // only touched on the listener thread
    DoubleClickDetector m_clicks; // likewise
    bool m_mouseOn = false; // listener thread only
    std::array<Recent, 10> m_recent {}; // listener thread only; a ring
    std::size_t m_recentNext = 0;
    std::atomic<bool> m_menuOpen {false};
    std::atomic<DoubleTapDetector::Side> m_side {DoubleTapDetector::Side::Both};
    std::atomic<HWND> m_window {nullptr};
    std::atomic<bool> m_active {false};
    std::atomic<DWORD> m_servicePid {0};
    std::atomic<DWORD> m_readyPid {0};
    std::atomic<ULONGLONG> m_heartbeat {0};
    bool m_usingService = false; // listener thread only
    bool m_delivered = false;
    std::uint32_t m_lastDelivered = 0;
    std::jthread m_thread;
};

} // namespace ws
