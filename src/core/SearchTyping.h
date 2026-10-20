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
