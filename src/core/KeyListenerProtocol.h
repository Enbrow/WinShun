#pragma once

#include <windows.h>

namespace ws::keyproto {

// Only notifications, to the requesting process's KeyListener window.
// No key injection, window activation or arbitrary commands in this channel.
constexpr wchar_t kWindowClass[] = L"WinShun.KeyListener";
constexpr UINT kReady = WM_APP + 71;
constexpr UINT kTap = WM_APP + 72;
constexpr DWORD kHeartbeatMs = 1000;
constexpr DWORD kLeaseMs = 3000;

// Ready's lParam: which taps the listener tells of besides Ctrl's. One from
// before double Alt sends 0: the GUI's own Raw Input then tells of Alt's.
constexpr LPARAM kReportsAlt = 1;
// Likewise for double Shift: an older one tells of Ctrl and Alt only.
constexpr LPARAM kReportsShift = 2;

// WinShun is x64. The sender PID is in the upper half; the lower half is
// VK_LCONTROL / VK_RCONTROL / VK_LMENU / VK_RMENU / VK_LSHIFT / VK_RSHIFT, plus whether any tap
// came from SendInput.
constexpr WPARAM tapWord(DWORD pid, DWORD key, bool sent) noexcept
{
    return (static_cast<WPARAM>(pid) << 32) | key | (sent ? 0x100 : 0);
}

} // namespace ws::keyproto
