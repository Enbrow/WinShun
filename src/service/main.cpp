// WinShunService.exe: the index service (GitHub #21). Win顺 itself runs with
// the user's normal rights; this service (LocalSystem, "WinShun") does the
// two things those rights cannot: read the NTFS volumes' master file tables
// and their change journals. It streams the parsed records over the pipe
// \\.\pipe\WinShun.Volumes (src/core/VolumeProtocol.h) to any interactive
// user's program that asks; the index itself stays in WinShun.exe. It never
// hands out raw volume bytes or handles, holds no volume open between
// requests, and keeps nothing in memory.
//
//   WinShunService.exe                  as a service (the Service Control Manager starts it)
//   WinShunService.exe --install        registers and starts it (the installer; administrator)
//   WinShunService.exe --uninstall      stops and removes it
//   WinShunService.exe --console [--pipe NAME] [--no-privileges]
//                                       the same server in this console (administrator), for
//                                       tests: Win顺 and tst_service reach it with WINSHUN_PIPE=NAME;
//                                       --no-privileges drops every privilege but
//                                       SeChangeNotifyPrivilege first, as the service runs
//
// Like Everything's service, it filters nothing by the caller's permissions:
// any interactive user on this computer can learn the names, folders,
// attributes and times of every file on every local NTFS volume (not their
// contents). docs/architecture.md says so.

#include "Log.h"
#include "Server.h"
#include "VolumeProtocol.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace {

using ws::service::DeviceWatch;
using ws::service::Server;

constexpr wchar_t kDisplayName[] = L"Win顺 索引服务 (WinShun Index Service)";
constexpr wchar_t kDescription[]
    = L"Reads the NTFS volumes' master file tables and change journals for Win顺 (WinShun), which runs with normal "
      L"rights. 为以普通权限运行的 Win顺 读取 NTFS 磁盘的主文件表和变更日志。";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent = nullptr;
DeviceWatch* g_deviceWatch = nullptr;

std::wstring ownPath()
{
    wchar_t path[MAX_PATH * 2] {};
    const DWORD n = ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return n > 0 && n < std::size(path) ? std::wstring(path, n) : std::wstring();
}

void report(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHintMs = 0)
{
    SERVICE_STATUS status {};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    status.dwWin32ExitCode = exitCode;
    status.dwWaitHint = waitHintMs;
    ::SetServiceStatus(g_statusHandle, &status);
}

DWORD WINAPI handler(DWORD control, DWORD eventType, void* eventData, void*)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        report(SERVICE_STOP_PENDING, NO_ERROR, 20'000);
        ::SetEvent(g_stopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    case SERVICE_CONTROL_DEVICEEVENT:
        if (g_deviceWatch)
            g_deviceWatch->onEvent(eventType, eventData);
        return NO_ERROR; // never vetoes: what is held is let go of
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// Runs the server until `stop` is set, telling `running` once it listens.
// The device watch takes its notifications from the service handle, or
// from a window of its own. Returns the Win32 error that kept it from
// listening, else 0.
template <typename Running>
DWORD serve(const std::wstring& pipeName, HANDLE stop, HANDLE recipient, DWORD recipientFlags, Running running)
{
    Server::Options options;
    options.pipeName = pipeName;
    Server server(options);
    DeviceWatch watch(recipient, recipientFlags, [&](const std::wstring& root) { server.closeVolume(root, 0, 2000); });
    g_deviceWatch = &watch;
    server.setDeviceWatch(&watch);
    DWORD error = 0;
    if (!server.start(&error)) {
        ws::service::log::write(L"cannot listen on \\\\.\\pipe\\%ls (error %lu)", pipeName.c_str(), error);
        g_deviceWatch = nullptr;
        return error ? error : ERROR_PIPE_BUSY;
    }
    running();
    ::WaitForSingleObject(stop, INFINITE);
    server.stop();
    g_deviceWatch = nullptr;
    return 0;
}

void WINAPI serviceMain(DWORD, wchar_t**)
{
    g_statusHandle = ::RegisterServiceCtrlHandlerExW(ws::volproto::kServiceName, handler, nullptr);
    if (!g_statusHandle)
        return;
    report(SERVICE_START_PENDING, NO_ERROR, 5000);
    g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ws::service::log::write(L"starting, version %hs", WINSHUN_VERSION);
    // RUNNING only once the pipe is there; the start takes milliseconds.
    const DWORD error = serve(ws::volproto::kPipeName, g_stopEvent, g_statusHandle, DEVICE_NOTIFY_SERVICE_HANDLE,
        [] { report(SERVICE_RUNNING); });
    if (error)
        ws::service::log::toEventLog(L"WinShun index service: cannot listen on \\\\.\\pipe\\WinShun.Volumes (error "
            + std::to_wstring(error) + L")");
    report(SERVICE_STOPPED, error);
}

// Drops every privilege but SeChangeNotifyPrivilege (which everyone has):
// the volumes are opened by the DACL (administrators, the system), not by
// privilege. --console --no-privileges shows that.
bool dropPrivileges()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token))
        return false;
    DWORD size = 0;
    ::GetTokenInformation(token, TokenPrivileges, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    bool ok = size > 0 && ::GetTokenInformation(token, TokenPrivileges, buffer.data(), size, &size);
    if (ok) {
        auto* privileges = reinterpret_cast<TOKEN_PRIVILEGES*>(buffer.data());
        LUID keep {};
        ::LookupPrivilegeValueW(nullptr, SE_CHANGE_NOTIFY_NAME, &keep);
        for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
            LUID_AND_ATTRIBUTES& p = privileges->Privileges[i];
            p.Attributes = p.Luid.LowPart == keep.LowPart && p.Luid.HighPart == keep.HighPart ? SE_PRIVILEGE_ENABLED
                                                                                                : SE_PRIVILEGE_REMOVED;
        }
        ok = ::AdjustTokenPrivileges(token, FALSE, privileges, 0, nullptr, nullptr) && ::GetLastError() == ERROR_SUCCESS;
    }
    ::CloseHandle(token);
    return ok;
}

BOOL WINAPI onConsoleControl(DWORD)
{
    ::SetEvent(g_stopEvent);
    return TRUE;
}

int runConsole(const std::wstring& pipeName, bool noPrivileges)
{
    ws::service::log::toConsole();
    if (noPrivileges && !dropPrivileges()) {
        std::fwprintf(stderr, L"could not drop the privileges (error %lu)\n", ::GetLastError());
        return 1;
    }
    g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::SetConsoleCtrlHandler(onConsoleControl, TRUE);
    return static_cast<int>(serve(pipeName, g_stopEvent, nullptr, 0, [&] {
        // "ready" on stdout: the tests wait for it.
        std::fwprintf(stdout, L"ready: WinShun index service %hs on \\\\.\\pipe\\%ls; Ctrl+C stops it\n",
            WINSHUN_VERSION, pipeName.c_str());
        std::fflush(stdout);
    }));
}

// ---- --install / --uninstall ------------------------------------------------------

struct ScHandle {
    SC_HANDLE value = nullptr;
    ~ScHandle()
    {
        if (value)
            ::CloseServiceHandle(value);
    }
};

int fail(const wchar_t* what)
{
    const DWORD error = ::GetLastError();
    std::fwprintf(stderr, L"%ls failed (error %lu)\n", what, error);
    return static_cast<int>(error ? error : 1);
}

// Waits for the service to reach `state`, `limitMs` at most.
bool waitForState(SC_HANDLE service, DWORD state, DWORD limitMs)
{
    const ULONGLONG deadline = ::GetTickCount64() + limitMs;
    for (;;) {
        SERVICE_STATUS status {};
        if (!::QueryServiceStatus(service, &status))
            return false;
        if (status.dwCurrentState == state)
            return true;
        if (::GetTickCount64() >= deadline)
            return false;
        ::Sleep(250);
    }
}

int install()
{
    const std::wstring path = L"\"" + ownPath() + L"\"";
    ScHandle manager {::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT)};
    if (!manager.value)
        return fail(L"OpenSCManager");
    // LocalSystem: a virtual service account is not an administrator and
    // cannot open the volumes. Automatic start, not delayed: delayed would
    // come up minutes after the user's Win顺, which would walk the disks
    // meanwhile.
    ScHandle service {::CreateServiceW(manager.value, ws::volproto::kServiceName, kDisplayName, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, path.c_str(), nullptr, nullptr, nullptr,
        nullptr, nullptr)};
    if (!service.value) {
        if (::GetLastError() != ERROR_SERVICE_EXISTS)
            return fail(L"CreateService");
        service.value = ::OpenServiceW(manager.value, ws::volproto::kServiceName, SERVICE_ALL_ACCESS);
        if (!service.value)
            return fail(L"OpenService");
        // Installed before (an upgrade, or from another folder): the path and
        // start type are made ours again.
        if (!::ChangeServiceConfigW(service.value, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, kDisplayName))
            return fail(L"ChangeServiceConfig");
    }
    SERVICE_DESCRIPTIONW description {const_cast<wchar_t*>(kDescription)};
    ::ChangeServiceConfig2W(service.value, SERVICE_CONFIG_DESCRIPTION, &description);
    // Only the privilege everyone has: the volumes open by their DACL (the
    // system is in it), not by any privilege.
    wchar_t privileges[] = L"SeChangeNotifyPrivilege\0";
    SERVICE_REQUIRED_PRIVILEGES_INFOW required {privileges};
    ::ChangeServiceConfig2W(service.value, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &required);
    SERVICE_SID_INFO sid {SERVICE_SID_TYPE_UNRESTRICTED};
    ::ChangeServiceConfig2W(service.value, SERVICE_CONFIG_SERVICE_SID_INFO, &sid);
    // Started again a second after a crash, three times a day.
    SC_ACTION actions[3] {{SC_ACTION_RESTART, 1000}, {SC_ACTION_RESTART, 1000}, {SC_ACTION_RESTART, 1000}};
    SERVICE_FAILURE_ACTIONSW failures {86400, nullptr, nullptr, 3, actions};
    ::ChangeServiceConfig2W(service.value, SERVICE_CONFIG_FAILURE_ACTIONS, &failures);
    if (!::StartServiceW(service.value, 0, nullptr) && ::GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        return fail(L"StartService");
    if (!waitForState(service.value, SERVICE_RUNNING, 15'000)) {
        std::fwprintf(stderr, L"the service did not start in time\n");
        return 1;
    }
    std::fwprintf(stdout, L"installed and running: %ls\n", path.c_str());
    return 0;
}

int uninstall()
{
    ScHandle manager {::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value)
        return fail(L"OpenSCManager");
    ScHandle service {
        ::OpenServiceW(manager.value, ws::volproto::kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE)};
    if (!service.value) {
        if (::GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
            return 0; // nothing to do
        return fail(L"OpenService");
    }
    SERVICE_STATUS status {};
    if (::ControlService(service.value, SERVICE_CONTROL_STOP, &status) || ::GetLastError() == ERROR_SERVICE_NOT_ACTIVE)
        waitForState(service.value, SERVICE_STOPPED, 15'000);
    if (!::DeleteService(service.value) && ::GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)
        return fail(L"DeleteService");
    std::fwprintf(stdout, L"removed\n");
    return 0;
}

int usage()
{
    std::fwprintf(stderr,
        L"WinShunService.exe: the index service of Win顺 (WinShun) %hs\n"
        L"  --install              register and start the service (as administrator)\n"
        L"  --uninstall            stop and remove it\n"
        L"  --console [--pipe NAME] [--no-privileges]\n"
        L"                         serve in this console instead (as administrator), for tests\n",
        WINSHUN_VERSION);
    return 1;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX); // never a dialog about a drive
    if (argc > 1) {
        const std::wstring_view option = argv[1];
        if (option == L"--install")
            return install();
        if (option == L"--uninstall")
            return uninstall();
        if (option == L"--console") {
            std::wstring pipe = ws::volproto::kPipeName;
            bool noPrivileges = false;
            for (int i = 2; i < argc; ++i) {
                if (std::wstring_view(argv[i]) == L"--pipe" && i + 1 < argc)
                    pipe = argv[++i];
                else if (std::wstring_view(argv[i]) == L"--no-privileges")
                    noPrivileges = true;
                else
                    return usage();
            }
            return runConsole(pipe, noPrivileges);
        }
        return usage();
    }
    SERVICE_TABLE_ENTRYW table[] {{const_cast<wchar_t*>(ws::volproto::kServiceName), serviceMain}, {nullptr, nullptr}};
    if (!::StartServiceCtrlDispatcherW(table)) {
        if (::GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            return usage(); // started by hand
        return fail(L"StartServiceCtrlDispatcher");
    }
    return 0;
}
