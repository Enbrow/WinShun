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

// WinShun is x64. The sender PID is in the upper half; the lower half is
// VK_LCONTROL / VK_RCONTROL, plus whether any tap came from SendInput.
constexpr WPARAM tapWord(DWORD pid, DWORD key, bool sent) noexcept
{
    return (static_cast<WPARAM>(pid) << 32) | key | (sent ? 0x100 : 0);
}

} // namespace ws::keyproto
