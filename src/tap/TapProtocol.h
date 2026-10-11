#pragma once

// The channel between WinShun and WinShunTap.dll, which lives in explorer.exe
// (docs/architecture.md, "the taskbar's Start gap"). One shared-memory
// section and two events per TAP instance, named with explorer's pid and the
// XAML diagnostics endpoint index the TAP was attached through. Explorer
// restarted: a new pid, a new set of names, so a resident TAP is found
// (OpenFileMapping succeeds) or not, never confused with an old one.
//
// WinShun writes a Request into the section and sets the request event. The
// TAP applies it on explorer's taskbar (XAML) thread, writes the Reply and sets
// the ack event. The TAP keeps a handle to the WinShun process: when it ends
// for any reason, the TAP clears the margin by itself.
//
// Plain C++ with no dependencies: shared by the DLL and the app
// (platform/TaskbarStartGap). The layout only ever grows at its end; a TAP
// resident in Explorer may be older or newer than the app talking to it.

#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <string>

namespace wstap {

constexpr uint32_t kMagic = 0x50545357; // "WSTP"
// Bumped when the layout below changes incompatibly (major) or grows at the
// end (minor). A resident TAP with another major cannot be talked to.
constexpr uint16_t kProtocolMajor = 1;
constexpr uint16_t kProtocolMinor = 0;
constexpr uint32_t kTapVersion = 1; // the DLL's own build number, for reports

constexpr int kFirstEndpoint = 1; // "VisualDiagConnection1" ...
constexpr int kLastEndpoint = 8;
constexpr int32_t kMaxMarginCentiDips = 100000; // 1000 DIPs: anything beyond is a bad request

// {53b40147-7db6-43bc-ae24-91921963d0c8}
constexpr GUID kTapClsid = {0x53b40147, 0x7db6, 0x43bc, {0xae, 0x24, 0x91, 0x92, 0x19, 0x63, 0xd0, 0xc8}};

// Where the TAP got to, in Shared::state.
enum class State : uint32_t {
    Starting = 0, // SetSite ran; AdviseVisualTreeChange not yet
    Primary = 1, // on the primary taskbar's XAML thread, watching its tree
    WrongCore = 2, // this endpoint belongs to another XAML thread of explorer: idle for good
    Failed = 3, // see lastError
};

struct Shared {
    // Written by the TAP at SetSite, read-only for WinShun.
    uint32_t magic = kMagic;
    uint16_t protocolMajor = kProtocolMajor;
    uint16_t protocolMinor = kProtocolMinor;
    uint32_t tapVersion = kTapVersion;
    uint32_t explorerPid = 0;
    uint32_t xamlThreadId = 0; // the thread SetSite ran on
    uint32_t taskbarThreadId = 0; // Shell_TrayWnd's
    uint32_t endpoint = 0; // VisualDiagConnection<endpoint>
    uint32_t state = static_cast<uint32_t>(State::Starting);
    int32_t lastError = 0; // an HRESULT
    wchar_t rootClass[32] {}; // the top-level window class the StartButton's island is in, once known

    // The request: WinShun writes all of it, then sets the request event.
    uint32_t requestSeq = 0; // incremented per request
    int32_t marginCentiDips = 0; // 0: back to the default (the local value cleared)
    uint32_t flags = 0; // reserved, 0
    uint32_t clientPid = 0;
    uint64_t clientHandle = 0; // a SYNCHRONIZE handle to WinShun, duplicated into explorer; 0: the TAP opens one itself

    // The reply: the TAP writes, then sets the ack event.
    uint32_t appliedSeq = 0;
    int32_t appliedCentiDips = 0; // what the StartButton has now (0 after a clear)
    int32_t applyError = 0; // an HRESULT
    uint32_t startSeen = 0; // 1 while a primary-taskbar StartButton is known
    uint32_t adds = 0; // visual tree Add callbacks seen (for the probe)
    int32_t lastCallbackError = 0; // a callback's failure (always answered S_OK to the runtime, see the DLL)
    uint32_t callbackErrors = 0;
};

inline std::wstring sectionName(DWORD explorerPid, int endpoint)
{
    wchar_t name[64];
    std::swprintf(name, 64, L"Local\\WinShunTap.%lu.%d", static_cast<unsigned long>(explorerPid), endpoint);
    return name;
}

inline std::wstring requestEventName(DWORD explorerPid, int endpoint) { return sectionName(explorerPid, endpoint) + L".request"; }
inline std::wstring ackEventName(DWORD explorerPid, int endpoint) { return sectionName(explorerPid, endpoint) + L".ack"; }

} // namespace wstap
