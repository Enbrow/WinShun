#include "NumberKeys.h"

#include <windows.h>

namespace ws::numberkeys {

std::vector<int> heldAsHotkeys(Modifier modifier)
{
    constexpr int kProbeId = 0xBF00; // a thread's own hotkey ids go up to 0xBFFF
    const UINT modifiers = (modifier == Modifier::Alt ? MOD_ALT : MOD_CONTROL) | MOD_NOREPEAT;
    std::vector<int> held;
    for (int n = 1; n <= 9; ++n) {
        if (::RegisterHotKey(nullptr, kProbeId, modifiers, static_cast<UINT>('0' + n)))
            ::UnregisterHotKey(nullptr, kProbeId); // free: let go at once
        else if (::GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED)
            held.push_back(n);
    }
    return held;
}

unsigned heldMask(Modifier modifier)
{
    unsigned mask = 0;
    for (const int n : heldAsHotkeys(modifier))
        mask |= 1u << n;
    return mask;
}

} // namespace ws::numberkeys
