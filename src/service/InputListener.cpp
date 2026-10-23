#include "InputListener.h"

#include "KeyListener.h"
#include "KeyListenerProtocol.h"
#include "Log.h"
#include "Server.h"
#include "Win32Util.h"

#include <wtsapi32.h>

#include <array>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ws::service {
namespace {
using win32::UniqueHandle;

bool targetOf(HWND window, DWORD pid)
{
    DWORD owner = 0;
    wchar_t name[64] {};
    return ::GetWindowThreadProcessId(window, &owner) && owner == pid && ::GetClassNameW(window, name, 64)
        && std::wcscmp(name, keyproto::kWindowClass) == 0;
}

std::vector<std::byte> tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS kind)
{
    DWORD size = 0;
    ::GetTokenInformation(token, kind, nullptr, 0, &size);
    std::vector<std::byte> bytes(size);
    if (!size || !::GetTokenInformation(token, kind, bytes.data(), size, &size))
        bytes.clear();
    return bytes;
}

// Privileges are enabled on this thread's private token, never on a volume
// reader's thread. Reverted before waiting on a client or serving its data.
class LaunchPrivileges {
public:
    LaunchPrivileges()
    {
        HANDLE original = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_DUPLICATE, &original))
            return;
        const UniqueHandle owned(original);
        HANDLE copy = nullptr;
        if (!::DuplicateTokenEx(original, TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE, nullptr,
                SecurityImpersonation, TokenImpersonation, &copy))
            return;
        const UniqueHandle token(copy);
        for (const wchar_t* name : {SE_TCB_NAME, SE_ASSIGNPRIMARYTOKEN_NAME, SE_INCREASE_QUOTA_NAME}) {
            TOKEN_PRIVILEGES p {1};
            if (!::LookupPrivilegeValueW(nullptr, name, &p.Privileges[0].Luid))
                return;
            p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (!::AdjustTokenPrivileges(copy, FALSE, &p, 0, nullptr, nullptr) || ::GetLastError() != ERROR_SUCCESS)
                return;
        }
        active = ::SetThreadToken(nullptr, copy) != FALSE;
    }
    ~LaunchPrivileges()
    {
        if (active && !::RevertToSelf())
            std::terminate(); // never continue a server thread with the launch token
    }
    bool active = false;
};

// One listener per client process, including while it is being started.
std::mutex listenersMutex;
std::set<DWORD> listeners;
struct Claim {
    explicit Claim(DWORD pid)
        : pid(pid)
    {
        const std::lock_guard lock(listenersMutex);
        held = listeners.insert(pid).second;
    }
    ~Claim()
    {
        if (held) {
            const std::lock_guard lock(listenersMutex);
            listeners.erase(pid);
        }
    }
    DWORD pid;
    bool held = false;
};

bool startListener(
    Connection& conn, HWND target, HANDLE job, UniqueHandle& child, DWORD& childPid, const wchar_t*& operation)
{
    operation = L"validate listener session";
    DWORD session = 0;
    // Session 0 cannot resolve the user's HWNDs. The worker validates the
    // window class and owning PID after entering this authenticated session.
    if (!::GetNamedPipeClientSessionId(conn.channel.handle(), &session) || session == 0) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    operation = L"open client token";
    const UniqueHandle client(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, conn.pid));
    HANDLE clientTokenRaw = nullptr;
    if (!client.valid() || !::OpenProcessToken(client.get(), TOKEN_QUERY, &clientTokenRaw))
        return false;
    const UniqueHandle clientToken(clientTokenRaw);
    const auto clientUser = tokenInfo(clientToken.get(), TokenUser);
    operation = L"enable launch privileges";
    const LaunchPrivileges privileges;
    if (!privileges.active)
        return false;
    operation = L"WTSQueryUserToken";
    HANDLE userRaw = nullptr;
    if (!::WTSQueryUserToken(session, &userRaw))
        return false;
    const UniqueHandle user(userRaw);
    const auto sessionUser = tokenInfo(user.get(), TokenUser);
    operation = L"validate session user";
    if (clientUser.empty() || sessionUser.empty()
        || !::EqualSid(reinterpret_cast<const TOKEN_USER*>(clientUser.data())->User.Sid,
            reinterpret_cast<const TOKEN_USER*>(sessionUser.data())->User.Sid)) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    // WTS supplies the session's user token, never SYSTEM or a linked
    // administrator token. UIAccess does not grant volume/file privileges.
    operation = L"DuplicateTokenEx";
    HANDLE tokenRaw = nullptr;
    if (!::DuplicateTokenEx(user.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &tokenRaw))
        return false;
    const UniqueHandle token(tokenRaw);
    DWORD uiAccess = 1;
    BYTE sid[SECURITY_MAX_SID_SIZE] {};
    DWORD sidSize = sizeof sid;
    TOKEN_MANDATORY_LABEL label {{sid, SE_GROUP_INTEGRITY}};
    operation = L"set listener integrity and UIAccess";
    if (!::CreateWellKnownSid(WinHighLabelSid, nullptr, sid, &sidSize)
        || !::SetTokenInformation(token.get(), TokenIntegrityLevel, &label, sizeof label + sidSize)
        || !::SetTokenInformation(token.get(), TokenUIAccess, &uiAccess, sizeof uiAccess)
        || !::AdjustTokenPrivileges(token.get(), TRUE, nullptr, 0, nullptr, nullptr))
        return false;
    // High IL protects the listener from writes/injection by medium-IL
    // processes. Only the listener gets this label; the GUI stays medium.
    operation = L"resolve listener executable";
    wchar_t exe[32768] {};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (!n || n >= std::size(exe))
        return false;
    wchar_t system[MAX_PATH] {};
    if (!::GetSystemDirectoryW(system, MAX_PATH))
        return false;
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --listen-keys " + std::to_wstring(conn.pid) + L" "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(target));
    STARTUPINFOW startup {sizeof startup};
    wchar_t desktop[] = L"winsta0\\default";
    startup.lpDesktop = desktop;
    PROCESS_INFORMATION process {};
    operation = L"CreateProcessAsUser";
    if (!::CreateProcessAsUserW(token.get(), exe, command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, system, &startup, &process))
        return false;
    child.reset(process.hProcess);
    const UniqueHandle thread(process.hThread);
    operation = L"assign listener job and resume";
    if (!::AssignProcessToJobObject(job, child.get()) || ::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const DWORD error = ::GetLastError();
        ::TerminateProcess(child.get(), error); // only our suspended child, never the user's program
        ::SetLastError(error);
        return false;
    }
    childPid = process.dwProcessId;
    return true;
}
} // namespace

bool canStartInputListener()
{
    // Keep these disabled on the process token. A launch temporarily enables
    // them only on its own impersonation token, even while volume readers run.
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &raw))
        return false;
    const UniqueHandle token(raw);
    for (const wchar_t* name : {SE_TCB_NAME, SE_ASSIGNPRIMARYTOKEN_NAME, SE_INCREASE_QUOTA_NAME}) {
        TOKEN_PRIVILEGES p {1};
        if (!::LookupPrivilegeValueW(nullptr, name, &p.Privileges[0].Luid)
            || !::AdjustTokenPrivileges(raw, FALSE, &p, 0, nullptr, nullptr))
            return false;
    }
    const LaunchPrivileges privileges;
    return privileges.active;
}

void serveInputListener(Server& server, Connection& conn, std::span<const std::byte> payload)
{
    using namespace volproto;
    Reader r(payload);
    std::uint64_t value = 0;
    if (!r.u64(value) || !r.done() || !value) {
        replyError(conn, ERROR_INVALID_PARAMETER, L"invalid listener window", 1000);
        return;
    }
    const Claim claim(conn.pid);
    if (!claim.held) {
        replyError(conn, ERROR_ALREADY_EXISTS, L"already listening for this process", 1000);
        return;
    }
    // Closing this job handles every exit path, pipe loss, service shutdown
    // and crash alike. The worker never outlives the request that owns it.
    const UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    UniqueHandle child;
    DWORD pid = 0;
    const wchar_t* operation = L"create listener job";
    if (!server.options().keyListener || !job.valid()
        || !::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits)
        || !startListener(conn, reinterpret_cast<HWND>(value), job.get(), child, pid, operation)) {
        const DWORD error = server.options().keyListener ? ::GetLastError() : ERROR_NOT_SUPPORTED;
        log::write(L"input listener for pid %lu: %ls failed (%lu)", conn.pid, operation, error);
        replyError(conn, error, operation, 1000);
        return;
    }
    Writer w;
    w.u32(pid);
    if (!conn.channel.write(w.finish(Type::KeyListenerReady), 1000))
        return;
    log::write(L"input listener %lu for pid %lu", pid, conn.pid);
    Type type {};
    std::vector<std::byte> next;
    while (conn.channel.read(type, next, keyproto::kLeaseMs, conn.abort)) {
        if (type != Type::KeyListenerPing || !next.empty() || ::WaitForSingleObject(child.get(), 0) != WAIT_TIMEOUT
            || !reply(conn, Type::KeyListenerPing, 1000))
            break;
    }
}

int runInputListener(DWORD clientPid, HWND target)
{
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw))
        return ERROR_ACCESS_DENIED;
    const UniqueHandle token(raw);
    DWORD uiAccess = 0, size = 0;
    const auto integrity = tokenInfo(token.get(), TokenIntegrityLevel);
    if (!::GetTokenInformation(token.get(), TokenUIAccess, &uiAccess, sizeof uiAccess, &size) || !uiAccess
        || integrity.empty())
        return ERROR_ACCESS_DENIED;
    const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrity.data())->Label.Sid;
    if (*::GetSidSubAuthority(sid, *::GetSidSubAuthorityCount(sid) - 1) < SECURITY_MANDATORY_HIGH_RID)
        return ERROR_ACCESS_DENIED;
    const UniqueHandle client(::OpenProcess(SYNCHRONIZE, FALSE, clientPid));
    if (!client.valid() || !targetOf(target, clientPid))
        return ERROR_INVALID_WINDOW_HANDLE;
    KeyListener::Callbacks callbacks;
    callbacks.doubleCtrl = [=](const KeyListener::Tap& tap) {
        if (!targetOf(target, clientPid))
            return;
        // UIAccess may grant the GUI foreground activation over an elevated
        // program. The GUI still applies its game/exclusion policy first.
        ::AllowSetForegroundWindow(clientPid);
        ::PostMessageW(
            target, keyproto::kTap, keyproto::tapWord(::GetCurrentProcessId(), tap.key, !tap.sent.empty()), tap.time);
    };
    KeyListener listener(std::move(callbacks));
    if (!listener.isActive())
        return ERROR_NOT_READY;
    listener.setKeys(KeyListener::Keys::All); // the GUI keeps the one set, and its side
    do {
        if (!targetOf(target, clientPid))
            break;
        ::PostMessageW(target, keyproto::kReady, ::GetCurrentProcessId(), keyproto::kReportsAlt | keyproto::kReportsShift);
    } while (::WaitForSingleObject(client.get(), keyproto::kHeartbeatMs) == WAIT_TIMEOUT);
    return 0;
}
} // namespace ws::service
