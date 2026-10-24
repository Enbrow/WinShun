#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace ws::searchtyping {

// What typing somewhere else means for Win顺 (platform/TypeToSearch): in the
// Start menu (a setting) and in a File Explorer folder window (another), the
// first key that types something opens the launcher with it. Pure logic over
// Windows' virtual-key codes and what has the keyboard focus, unit-tested;
// the hook decides from it alone, with no messages sent.

// Virtual-key codes (winuser.h), named here so that the core needs no
// windows.h; the tests use the same numbers.
inline constexpr unsigned kVkBack = 0x08;
inline constexpr unsigned kVkTab = 0x09;
inline constexpr unsigned kVkReturn = 0x0D;
inline constexpr unsigned kVkShift = 0x10;
inline constexpr unsigned kVkControl = 0x11;
inline constexpr unsigned kVkMenu = 0x12;
inline constexpr unsigned kVkCapital = 0x14;
inline constexpr unsigned kVkEscape = 0x1B;
inline constexpr unsigned kVkSpace = 0x20;
inline constexpr unsigned kVkLeft = 0x25;
inline constexpr unsigned kVkDelete = 0x2E;
inline constexpr unsigned kVkLWin = 0x5B;
inline constexpr unsigned kVkRWin = 0x5C;
inline constexpr unsigned kVkNumpad0 = 0x60;
inline constexpr unsigned kVkDivide = 0x6F;
inline constexpr unsigned kVkF1 = 0x70;
inline constexpr unsigned kVkF24 = 0x87;
inline constexpr unsigned kVkNumlock = 0x90;
inline constexpr unsigned kVkLShift = 0xA0;
inline constexpr unsigned kVkRMenu = 0xA5;
inline constexpr unsigned kVkOem1 = 0xBA;
inline constexpr unsigned kVkOem3 = 0xC0;
inline constexpr unsigned kVkOem4 = 0xDB;
inline constexpr unsigned kVkOem8 = 0xDF;
inline constexpr unsigned kVkOem102 = 0xE2;
inline constexpr unsigned kVkProcessKey = 0xE5;

// GetGUIThreadInfo's flags (winuser.h): the thread is in a menu's loop, that
// of a pop-up menu (a context menu), or of the window's system menu.
inline constexpr unsigned kGuiInMenuMode = 0x04;
inline constexpr unsigned kGuiSystemMenuMode = 0x08;
inline constexpr unsigned kGuiPopupMenuMode = 0x10;

// Shift, Ctrl, Alt, the Windows keys and the locks: seen as they are, never
// typing.
inline constexpr bool isModifier(unsigned vk) noexcept
{
    return vk == kVkShift || vk == kVkControl || vk == kVkMenu || vk == kVkLWin || vk == kVkRWin
        || (vk >= kVkLShift && vk <= kVkRMenu) || vk == kVkCapital || vk == kVkNumlock;
}

// Keys that start a search: letters, digits, punctuation, the number pad's
// digits and signs. Not Space or Backspace, which do nothing in an empty
// search box (Listary leaves Space to Explorer too), nor Enter, Esc, Tab,
// arrows, Delete, F keys: those are commands in Explorer's view (open,
// rename, select). With a Chinese input method on, the hook still sees the
// letter's own code (VK_PROCESSKEY appears only once a window translates
// the key), so pinyin starts a search like any typing.
inline constexpr bool startsTyping(unsigned vk) noexcept
{
    return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= kVkNumpad0 && vk <= kVkDivide)
        || (vk >= kVkOem1 && vk <= kVkOem3) || (vk >= kVkOem4 && vk <= kVkOem8) || vk == kVkOem102;
}

// What has the keyboard focus in the Explorer window in front, by window
// classes (platform/FolderViewHit: DirectUIHWND under SHELLDLL_DefView).
enum class Focus {
    ItemsView, // the files and folders shown: typing there jumps to a name in Explorer
    Menu, // a menu of the window's is open (a context menu): the keys are its, 属性(R) (GitHub #37)
    Other, // a name being renamed (an Edit), the address bar, Explorer's search box, the navigation pane…
};

// What has the keyboard in the Explorer window, from GetGUIThreadInfo on its
// thread: its `flags`, and whether its focus window is the files and folders
// shown. A menu open comes first: while the classic context menu is up
// (Shift+right-click, 显示更多选项) the focus stays on the files, the thread
// in menu mode (0x14 measured), and every key is the menu's, whether or not
// it is one of its access keys. Windows 11's own context menu needs no flag:
// it takes the focus into the window's XAML island (docs/pitfalls.md).
inline constexpr Focus focusOf(unsigned guiFlags, bool itemsViewFocused) noexcept
{
    if ((guiFlags & (kGuiInMenuMode | kGuiSystemMenuMode | kGuiPopupMenuMode)) != 0)
        return Focus::Menu;
    return itemsViewFocused ? Focus::ItemsView : Focus::Other;
}

// Whether a key pressed in a File Explorer folder window starts a search with
// Win顺 in that folder. `combination`: Ctrl, Alt or a Windows key held
// (Ctrl+A, Alt+Enter, Win+E stay Explorer's; Shift alone types a capital).
// `elevated`: the window belongs to a process of higher integrity than
// Win顺's; Windows would let neither the hook see its keys nor Win顺 hand
// them back, so nothing is taken from it.
inline constexpr bool explorerStarts(unsigned vk, bool press, bool combination, Focus focus, bool elevated) noexcept
{
    return press && !combination && !elevated && focus == Focus::ItemsView && !isModifier(vk) && startsTyping(vk);
}

// The File Explorer window a key typed now counts in, or none: `front` is the
// window really in front at the key (GetForegroundWindow), `seen` the Explorer
// folder window that was in front at the last foreground event, `behind` one
// that raised that event while another window stayed in front. A folder window
// opened by a program that may not take the front (a tool in the background,
// a script) does that: it says it came forward, stays behind, and when a click
// on it or on its taskbar button brings it to the front later, no event comes
// at all (measured 2026-10-24, docs/pitfalls.md). So the key decides by what is
// in front then; that also keeps a key from counting for `seen` should another
// window have come forward the same silent way. Window: HWND, or a number in
// the tests.
template <class Window>
constexpr Window typingWindow(Window front, Window seen, Window behind) noexcept
{
    return front && (front == seen || front == behind) ? front : Window {};
}

// What to do with the keys held back once the launcher says it has the
// keyboard (or the next batch of them is due). They go out only while a window
// of Win顺's is really in front: Qt can call the launcher active while the
// Start menu still is, or takes the front back a few milliseconds later (it was
// still opening when its Esc came, or Windows was slow to start it), and keys
// typed then went into Windows' search instead (GitHub #66). With the Start menu
// in front again, the launcher is asked for once more (its Esc closes it), not
// sooner than kAskAgainMs after the last ask, which may still be closing it, and
// at most kMaxAsksAgain times; otherwise the keys wait, until the hold runs out.
// Not once some have been typed into it in this hold: shown again, it selects
// its words, and the next keys would replace them.
enum class Delivery {
    Type, // Win顺 is in front: type them
    Wait, // not yet: hold on, look again shortly
    AskAgain, // the Start menu is back in front: ask for the launcher again, then wait
};
inline constexpr unsigned kAskAgainMs = 600; // closing the Start menu for the launcher takes up to 500 ms
inline constexpr int kMaxAsksAgain = 2;

inline constexpr Delivery delivery(bool oursInFront, bool startMenuInFront, bool forStartMenu, bool typedAlready,
    int asksAgain, unsigned msSinceAsked) noexcept
{
    if (oursInFront)
        return Delivery::Type;
    if (startMenuInFront && forStartMenu && !typedAlready && asksAgain < kMaxAsksAgain && msSinceAsked >= kAskAgainMs)
        return Delivery::AskAgain;
    return Delivery::Wait;
}

// The keys held back and typed again into the launcher, in the order sent,
// until each has gone down the hook chain. Until the last has, a key typed
// meanwhile is held too and follows them: let through, it overtook the ones
// still on their way ("qz" came out "zq" right after a jump, GitHub #39).
// Win顺's other keys (a paste, the Esc that closes the Start menu) do not
// count: only the next one sent does.
class Retyped {
public:
    struct Key {
        unsigned vk = 0;
        bool release = false;
    };

    void sent(std::vector<Key> keys) noexcept
    {
        m_keys = std::move(keys);
        m_next = 0;
    }
    // One of Win顺's own keys went by: true when it was the last of those sent.
    bool passed(unsigned vk, bool release) noexcept
    {
        if (m_next >= m_keys.size() || m_keys[m_next].vk != vk || m_keys[m_next].release != release)
            return false;
        return ++m_next == m_keys.size();
    }
    bool pending() const noexcept { return m_next < m_keys.size(); }
    void clear() noexcept
    {
        m_keys.clear();
        m_next = 0;
    }

private:
    std::vector<Key> m_keys;
    std::size_t m_next = 0;
};

} // namespace ws::searchtyping
