#pragma once

#include <cstdint>

namespace ws {

// Marks the input Win顺 sends itself: SendInput's dwExtraInfo, which the
// hooks see as such and Raw Input as ExtraInformation. KeyRouter and
// TypeToSearch let it through; KeyListener does not take it for another
// program's.
inline constexpr std::uintptr_t kOwnInput = 0x5753'4B52; // "WSKR"

// KeyRouter's masking key sent just before a Shift or Alt pressed alone
// goes on to the program, so that no menu bar opens: for a double Alt, no
// key in between. The one it sends in place of a key it took (Alt+1) is
// kOwnInput, a key in between.
inline constexpr std::uintptr_t kOwnMask = 0x5753'4B4D; // "WSKM"

constexpr bool isOwnInput(std::uintptr_t extra) noexcept
{
    return extra == kOwnInput || extra == kOwnMask;
}

} // namespace ws
