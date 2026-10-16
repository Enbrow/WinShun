#pragma once

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
    Other, // a name being renamed (an Edit), the address bar, Explorer's search box, the navigation pane…
};

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

} // namespace ws::searchtyping
