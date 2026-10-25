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

// Global "double-tap Ctrl" (or Alt, or Shift) detection through Raw Input. The mouse is
// listened to as well, from a press of the key until a second tap can no
// longer follow: a key held for a click, a drag or the wheel is not a tap.
//
// Key presses reach a hidden window on its own thread as WM_INPUT messages,
// after the fact. Unlike a low-level keyboard hook, which every key press in
// the system waits on, this never slows anyone's typing, and Windows cannot
// quietly drop it the way it drops a hook that answers too slowly.
//
// With `doubleClicks`, the mouse is listened to all the time, for double
// clicks of the left button (the folder menu, GitHub #22). Windows keeps one
// mouse registration per process: this is the only one, or the next Ctrl
// press here would replace another's and take it away again. Likewise with
// `dragMenu` (the drop pie), which follows the pointer while the primary
// button is held.
class KeyListener {
public:
    struct Tap {
        std::string sent;
        std::uint32_t key = 0; // VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT or VK_RSHIFT
        std::uint32_t time = 0; // the input's clock, shared by both listeners
    };
    struct Callbacks {
        // None: keyboards not listened to. A double Ctrl, Alt or Shift
        // (setKeys; the name is from before Alt). `sent` describes the last keys when
        // a program made some of the taps (SendInput), for the log: which
        // modifier keys, how far apart; empty otherwise.
        std::function<void(const Tap&)> doubleCtrl;
        // A double click with no other button or modifier held: the window
        // under the pointer and where (physical pixels). None: the mouse only
        // around a press of the key.
        std::function<void(HWND window, POINT pt)> doubleClick;
        // The key under Esc pressed (` on most layouts, · in a Chinese input
        // method) with a file held or dragged, once an OLE drag is under way
        // (told by its source thread's capture window; a file held still,
        // the drag is begun for it): where the pointer is (physical pixels),
        // the source's capture window and process. With it the mouse is
        // listened to all the time, as for doubleClick, and keyboards too;
        // from a press on a list of files or a drag's start until the button
        // is let go, the key is this listener's hotkey, so nothing types it.
        std::function<void(POINT pt, HWND captureWindow, DWORD sourcePid)> dragMenu;
        // The primary button let go (with dragMenu): the drag is over.
        std::function<void()> leftButtonUp;
        // Lines for the log (on the listener thread): that double Alt over
        // administrator windows needs the service updated, once; with
        // WINSHUN_KEYLOG=1 also every key event and what came of it, the
        // modifiers by name, any other key only as "key".
        std::function<void(const std::string&)> log;
    };

    // Whose double taps count: the key set, or all for the UIAccess
    // listener, whose GUI keeps the one set.
    enum class Keys : std::uint8_t { Ctrl, Alt, Shift, All };

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

    // Which key's double tap counts, and which of its two (from any thread);
    // the next press goes by them.
    void setKeys(Keys keys) noexcept { m_keys = keys; }
    void setSide(DoubleTapDetector::Side side) noexcept { m_side = side; }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void onInput(HRAWINPUT input);
    void onMouse(const RAWMOUSE& mouse);
    void onDrag(const RAWMOUSE& mouse);
    void onDragKey(const RAWKEYBOARD& key);
    void onDragKeyPressed();
    // The window holding the mouse for an OLE drag, and its process; none
    // when no drag is under way.
    struct DragSource {
        HWND capture = nullptr;
        DWORD pid = 0;
    };
    DragSource oleDrag() const;
    void guardDragKey(bool on);
    void startDrag();
    void onStartDragTimer();
    void replayDragKey();
    void listenToMouse(bool on);
    bool mouseWanted() const noexcept { return m_callbacks.doubleClick || m_callbacks.dragMenu; }
    bool keyboardWanted() const noexcept { return m_callbacks.doubleCtrl || m_callbacks.dragMenu; }
    bool serviceTells() const noexcept;
    void followService();
    bool keyHeld() const;
    bool wanted(std::uint32_t key) const noexcept;
    std::string sentTrace(DoubleTapDetector::Key key) const;
    void deliver(Tap tap);
    void log(const std::string& line) const;

    // The last keys, for sentTrace: modifiers by name, any other only as
    // "a key" (nothing typed is kept).
    struct Recent {
        std::uint8_t key = 0; // 0 none, else an index into the names
        bool up = false;
        bool sent = false; // no device: SendInput
        std::uint32_t time = 0;
    };

    Callbacks m_callbacks;
    std::array<DoubleTapDetector, 3> m_detectors; // Ctrl's, Alt's, Shift's; only touched on the listener thread
    DoubleClickDetector m_clicks; // likewise
    // The primary button held, and whether the pointer has left the drag
    // rectangle round the press (a drag has begun); listener thread only.
    bool m_pressed = false;
    bool m_dragging = false;
    SIZE m_dragRect {4, 4}; // SM_CXDRAG, SM_CYDRAG at the monitor pressed on
    POINT m_pressedAt {}; // where the primary button went down (physical pixels)
    bool m_dragKeyDown = false; // the drop pie's key, listener thread only: its repeats are not presses
    bool m_dragFound = false; // an OLE drag seen while the button is held; listener thread only
    std::uint32_t m_dragLooked = 0; // when it was last looked for (message time)
    bool m_dragKeyGuarded = false; // the drop pie's key held back from the drag's source
    // The key pressed on a file still held, not dragged yet: the drag being
    // started for it (startDrag). Listener thread only.
    bool m_starting = false;
    int m_startTicks = 0;
    POINT m_startFrom {};
    bool m_mouseOn = false; // listener thread only
    std::array<Recent, 10> m_recent {}; // listener thread only; a ring
    std::size_t m_recentNext = 0;
    std::atomic<bool> m_menuOpen {false};
    std::atomic<Keys> m_keys {Keys::Ctrl};
    std::atomic<DoubleTapDetector::Side> m_side {DoubleTapDetector::Side::Both};
    std::atomic<HWND> m_window {nullptr};
    std::atomic<bool> m_active {false};
    std::atomic<DWORD> m_servicePid {0};
    std::atomic<DWORD> m_readyPid {0};
    std::atomic<LPARAM> m_serviceReports {0}; // the taps it tells of besides Ctrl's (keyproto::kReportsAlt, kReportsShift)
    std::atomic<ULONGLONG> m_heartbeat {0};
    bool m_usingService = false; // listener thread only
    bool m_keyLog = false; // WINSHUN_KEYLOG=1
    bool m_oldServiceLogged = false; // listener thread only
    bool m_delivered = false;
    std::uint32_t m_lastDelivered = 0;
    std::jthread m_thread;
};

} // namespace ws
