#pragma once

#include "SearchTyping.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace ws {

// Typing somewhere else searches with Win顺: in the Start menu instead of
// Windows (a setting, off by default; EverythingToolbar does the same), and
// in a File Explorer folder window, in the folder it shows (another setting,
// off by default, as Listary does it; it takes the place of Explorer's own
// jump to a name by its first letters).
//
// Windows' search takes over the Start menu as soon as one types there. Here
// the first key that types something while the Start menu (or Windows'
// search) is in front is taken from it, and the launcher is asked for: it
// opens over the taskbar. In Explorer, the first key typed while the files
// and folders shown have the keyboard (not a name being renamed, the address
// bar, its search box or the navigation pane) and no menu of the window's is
// open (its keys are the menu's: 属性(R), GitHub #37) is taken the same way,
// and the launcher is asked for with that window's folder (typedInExplorer). That
// key and the ones typed after it are held back until the launcher has the
// keyboard (deliver), then typed again in order (SendInput, marked as
// Win顺's own): they go through the input method as if typed into the search
// box. The hold lasts until those have gone down the hook chain: a key typed
// in the meantime follows them instead of slipping in among them (GitHub
// #39). When the folder turns out to be none (This PC, Home, the Recycle
// Bin), the keys are given back to Explorer instead (giveBack), which then
// jumps by them as it would have.
//
// A low-level keyboard hook, which every key press in the system waits on
// (see KeyListener for why that matters): installed only while the Start
// menu, Windows' search or an Explorer folder window is in front, as an
// out-of-context event on the foreground tells (or an Explorer window raised
// that event from behind: opened in the background, it comes to the front
// later with no event, so the key checks what is in front), and while keys are held
// back; on a thread of its own, deciding at once from the key and the
// window with the focus alone (core/TypeToSearch.h; no messages sent). Win,
// Ctrl and Alt combinations stay Windows'. Left Alt pressed in the Start
// menu leaves its search alone until it closes: a way to Windows' own search
// when it is wanted. An elevated Explorer is left alone: Windows would show
// the hook none of its keys, nor take them back (UIPI).
class TypeToSearch {
public:
    struct Surfaces {
        bool startMenu = false;
        bool explorer = false;
    };
    struct Callbacks {
        // The first key was taken: show the launcher. On the hook's thread.
        std::function<void()> typed; // in the Start menu: over the taskbar
        // ... in the Explorer window `root`, whose tab (ShellTabWindowClass;
        // the window itself on Windows 10) shows the folder to search; `vk`
        // is the key, for the log.
        std::function<void(HWND root, HWND tab, unsigned vk)> typedInExplorer;
    };

    TypeToSearch(Surfaces surfaces, Callbacks callbacks);
    ~TypeToSearch();

    TypeToSearch(const TypeToSearch&) = delete;
    TypeToSearch& operator=(const TypeToSearch&) = delete;

    Surfaces surfaces() const noexcept { return m_surfaces; }

    // The launcher has the keyboard: the keys held back are typed into it.
    // Any thread.
    void deliver();
    // The launcher is not coming (no folder there): the keys held back go on
    // to the window they were typed in. Any thread.
    void giveBack();

private:
    struct Held {
        WORD vk = 0;
        WORD scanCode = 0;
        bool extended = false;
        bool release = false;
        bool shift = false; // held with Shift (its press)
    };

    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static void CALLBACK foregroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD);
    bool handleKey(WPARAM message, const KBDLLHOOKSTRUCT& event); // true: taken
    bool explorerFocused(HWND& tab) const; // the view of m_explorer's folder has the focus: its tab
    void foregroundChanged(HWND window, HWND named); // window: in front now; named: the event's
    bool typingExplorer(HWND window) const; // an Explorer folder window, typing there on, not elevated
    void setHooked(bool hooked);
    void typeHeld(); // deliver(), on the hook's thread
    void returnHeld(); // giveBack(), on the hook's thread
    void dropHeld(); // the launcher never came, or the keys typed again never went by
    std::vector<INPUT> takeHeld(); // m_held as input to send; emptied
    void sendHeld(); // types m_held again; held on until those have gone by
    void ownKeyPassed(WPARAM message, const KBDLLHOOKSTRUCT& event); // a key of Win顺's went down the chain
    void heldPassed(); // ... the last of those typed again
    void restartTimer(UINT ms);
    void endHolding();

    const Surfaces m_surfaces;
    Callbacks m_callbacks;
    std::atomic<DWORD> m_threadId = 0;

    // The hook's thread only.
    HHOOK m_hook = nullptr;
    bool m_startInFront = false;
    HWND m_explorer = nullptr; // the Explorer folder window in front, when typing there counts
    // ... one that said it came forward while another stayed in front: it
    // gets there later with no event (a click), the key checks
    HWND m_explorerBehind = nullptr;
    bool m_windowsSearch = false; // Left Alt in the Start menu: its own search this time
    bool m_holding = false; // keys are held back for the launcher
    bool m_holdingForExplorer = false; // ... typed in Explorer: given back when the launcher does not come
    std::vector<Held> m_held;
    searchtyping::Retyped m_retyped; // typed again, not all gone by: what is typed now waits behind them
    UINT_PTR m_timer = 0; // while holding: gives up on the launcher (on those typed again, once sent)

    std::jthread m_thread; // last: joined before the members above go
};

} // namespace ws
