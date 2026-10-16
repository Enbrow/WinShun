#pragma once

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
// bar, its search box or the navigation pane) is taken the same way, and the
// launcher is asked for with that window's folder (typedInExplorer). That
// key and the ones typed after it are held back until the launcher has the
// keyboard (deliver), then typed again in order (SendInput, marked as
// Win顺's own): they go through the input method as if typed into the search
// box. When the folder turns out to be none (This PC, Home, the Recycle
// Bin), the keys are given back to Explorer instead (giveBack), which then
// jumps by them as it would have.
//
// A low-level keyboard hook, which every key press in the system waits on
// (see KeyListener for why that matters): installed only while the Start
// menu, Windows' search or an Explorer folder window is in front, as an
// out-of-context event on the foreground tells, and while keys are held
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
    void foregroundChanged(HWND window);
    void setHooked(bool hooked);
    void typeHeld(); // deliver(), on the hook's thread
    void returnHeld(); // giveBack(), on the hook's thread
    void dropHeld(); // the launcher never came
    void endHolding();

    const Surfaces m_surfaces;
    Callbacks m_callbacks;
    std::atomic<DWORD> m_threadId = 0;

    // The hook's thread only.
    HHOOK m_hook = nullptr;
    bool m_startInFront = false;
    HWND m_explorer = nullptr; // the Explorer folder window in front, when typing there counts
    bool m_windowsSearch = false; // Left Alt in the Start menu: its own search this time
    bool m_holding = false; // keys are held back for the launcher
    bool m_holdingForExplorer = false; // ... typed in Explorer: given back when the launcher does not come
    std::vector<Held> m_held;
    UINT_PTR m_timer = 0; // while holding: gives up on the launcher

    std::jthread m_thread; // last: joined before the members above go
};

} // namespace ws
