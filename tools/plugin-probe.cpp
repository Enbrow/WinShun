// plugin-probe.exe: stage 0 of the plugin system (the plugin design of
// 2026-10-11, §3.2 and §8; its findings are in docs/pitfalls.md, "插件系统").
// Plain Win32, no Qt, static C runtime: what it measures is the cost of
// starting a plugin process, so it must load nothing itself.
//
// One program, three roles:
//
//   plugin-probe --parent [--runs N] [--out FILE] [--simulate-elevated-explorer] [--share \\host\share]
//       Starts itself as a child every way a plugin process could be started
//       (see `routes` in parentMain) and reports what each child got: its
//       token (integrity, elevation, Administrators group, logon session),
//       whether it is in our job with our limits, whether the mitigation
//       policy took, whether stdin and stdout both work, what it can do
//       (start a process, read the clipboard), what environment it has, and
//       how long each start took (N timed runs: median, p95). Meant to be
//       run elevated (as WinShun is) and once more not elevated.
//   plugin-probe --child
//       Writes a "hello" line at once (that is what the parent times), then,
//       when WS_PROBE_FULL=1, one line of JSON with the facts about itself,
//       then reads one line from stdin and echoes it back (both directions
//       of stdio shown to work), then exits.
//   plugin-probe --runner
//       Route B' (§3.2): a process without administrator rights that starts
//       the children with a plain CreateProcessW. Says hello, reports its
//       own facts, then for every "spawn" / "spawn full" line on stdin
//       starts a child, drives it as the parent would and reports the
//       child's facts and timings on one line; "quit" ends it.
//   plugin-probe --noop
//       Exits at once: what a child starts to show whether it may.

#include <windows.h>

#include <tlhelp32.h>
#include <userenv.h>
#include <winnetwk.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

// ---- small helpers ---------------------------------------------------------

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) noexcept
        : h(handle == INVALID_HANDLE_VALUE ? nullptr : handle)
    {
    }
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept
        : h(std::exchange(other.h, nullptr))
    {
    }
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            reset();
            h = std::exchange(other.h, nullptr);
        }
        return *this;
    }
    bool valid() const { return h != nullptr; }
    HANDLE get() const { return h; }
    HANDLE release() { return std::exchange(h, nullptr); }
    void reset(HANDLE handle = nullptr)
    {
        if (h)
            ::CloseHandle(h);
        h = handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }
};

std::string utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string format(const char* fmt, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    return buffer;
}

std::string hex(unsigned long long v)
{
    return format("0x%llx", v);
}

long long microsecondsSince(const LARGE_INTEGER& start)
{
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    return (now.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart;
}

LARGE_INTEGER tick()
{
    LARGE_INTEGER t;
    ::QueryPerformanceCounter(&t);
    return t;
}

std::wstring ownPath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring environmentVariable(const wchar_t* name)
{
    wchar_t buffer[4096];
    const DWORD n = ::GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return n > 0 && n < std::size(buffer) ? std::wstring(buffer, n) : std::wstring();
}

// A JSON writer good enough for one line of facts.
class Json {
public:
    std::string text;

    void beginObject(const char* key = nullptr)
    {
        open(key);
        text += '{';
        m_first.push_back(true);
    }
    void endObject()
    {
        text += '}';
        m_first.pop_back();
    }
    void beginArray(const char* key = nullptr)
    {
        open(key);
        text += '[';
        m_first.push_back(true);
    }
    void endArray()
    {
        text += ']';
        m_first.pop_back();
    }
    void str(const char* key, const std::string& value)
    {
        open(key);
        quote(value);
    }
    void str(const char* key, const std::wstring& value) { str(key, utf8(value)); }
    void num(const char* key, long long value)
    {
        open(key);
        text += std::to_string(value);
    }
    void boolean(const char* key, bool value)
    {
        open(key);
        text += value ? "true" : "false";
    }
    void null(const char* key)
    {
        open(key);
        text += "null";
    }
    void raw(const char* key, const std::string& json)
    {
        open(key);
        text += json.empty() ? "null" : json;
    }

private:
    std::vector<bool> m_first;

    void open(const char* key)
    {
        if (!m_first.empty()) {
            if (!m_first.back())
                text += ',';
            m_first.back() = false;
        }
        if (key) {
            quote(key);
            text += ':';
        }
    }
    void quote(const std::string& s)
    {
        text += '"';
        for (const char c : s) {
            switch (c) {
            case '"': text += "\\\""; break;
            case '\\': text += "\\\\"; break;
            case '\n': text += "\\n"; break;
            case '\r': text += "\\r"; break;
            case '\t': text += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    text += format("\\u%04x", c);
                else
                    text += c;
            }
        }
        text += '"';
    }
};

// The raw value of a top-level or nested key in one line of JSON written by
// Json above (keys are unique per line); empty when absent.
std::string jsonValue(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t at = json.find(needle);
    if (at == std::string::npos)
        return {};
    std::size_t i = at + needle.size();
    if (i < json.size() && json[i] == '"') {
        std::string out;
        for (++i; i < json.size() && json[i] != '"'; ++i) {
            if (json[i] == '\\' && i + 1 < json.size())
                ++i;
            out += json[i];
        }
        return out;
    }
    std::size_t end = i;
    int depth = 0;
    for (; end < json.size(); ++end) {
        const char c = json[end];
        if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']') {
            if (depth == 0)
                break;
            --depth;
        } else if (c == ',' && depth == 0)
            break;
    }
    return json.substr(i, end - i);
}

// ---- token facts ----------------------------------------------------------

template <typename T>
std::vector<char> tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS what, T** out)
{
    DWORD size = 0;
    ::GetTokenInformation(token, what, nullptr, 0, &size);
    std::vector<char> buffer(size ? size : 1);
    if (size == 0 || !::GetTokenInformation(token, what, buffer.data(), size, &size)) {
        *out = nullptr;
        return buffer;
    }
    *out = reinterpret_cast<T*>(buffer.data());
    return buffer;
}

DWORD integrityRid(HANDLE token)
{
    TOKEN_MANDATORY_LABEL* label = nullptr;
    const auto buffer = tokenInfo(token, TokenIntegrityLevel, &label);
    if (!label)
        return 0;
    return *::GetSidSubAuthority(label->Label.Sid, *::GetSidSubAuthorityCount(label->Label.Sid) - 1);
}

bool tokenElevated(HANDLE token)
{
    TOKEN_ELEVATION elevation {};
    DWORD size = 0;
    return ::GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated;
}

int tokenElevationType(HANDLE token)
{
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD size = 0;
    return ::GetTokenInformation(token, TokenElevationType, &type, sizeof type, &size) ? static_cast<int>(type) : 0;
}

// "enabled", "deny-only", "disabled" or "absent": how the token holds the
// Administrators group.
std::string administratorsGroup(HANDLE token)
{
    BYTE admins[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof admins;
    if (!::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &size))
        return "?";
    TOKEN_GROUPS* groups = nullptr;
    const auto buffer = tokenInfo(token, TokenGroups, &groups);
    if (!groups)
        return "?";
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if (!::EqualSid(groups->Groups[i].Sid, admins))
            continue;
        const DWORD attributes = groups->Groups[i].Attributes;
        if (attributes & SE_GROUP_USE_FOR_DENY_ONLY)
            return "deny-only";
        return attributes & SE_GROUP_ENABLED ? "enabled" : "disabled";
    }
    return "absent";
}

std::string authenticationId(HANDLE token)
{
    TOKEN_STATISTICS statistics {};
    DWORD size = 0;
    if (!::GetTokenInformation(token, TokenStatistics, &statistics, sizeof statistics, &size))
        return "?";
    return format("%08lx:%08lx", statistics.AuthenticationId.HighPart, statistics.AuthenticationId.LowPart);
}

int privilegeCount(HANDLE token)
{
    TOKEN_PRIVILEGES* privileges = nullptr;
    const auto buffer = tokenInfo(token, TokenPrivileges, &privileges);
    return privileges ? static_cast<int>(privileges->PrivilegeCount) : -1;
}

bool hasPrivilege(HANDLE token, const wchar_t* name)
{
    LUID luid {};
    if (!::LookupPrivilegeValueW(nullptr, name, &luid))
        return false;
    TOKEN_PRIVILEGES* privileges = nullptr;
    const auto buffer = tokenInfo(token, TokenPrivileges, &privileges);
    if (!privileges)
        return false;
    for (DWORD i = 0; i < privileges->PrivilegeCount; ++i)
        if (privileges->Privileges[i].Luid.LowPart == luid.LowPart && privileges->Privileges[i].Luid.HighPart == luid.HighPart)
            return true;
    return false;
}

std::string tokenUserName(HANDLE token)
{
    TOKEN_USER* user = nullptr;
    const auto buffer = tokenInfo(token, TokenUser, &user);
    if (!user)
        return "?";
    wchar_t name[256], domain[256];
    DWORD nameSize = 256, domainSize = 256;
    SID_NAME_USE use;
    if (!::LookupAccountSidW(nullptr, user->User.Sid, name, &nameSize, domain, &domainSize, &use))
        return "?";
    return utf8(std::wstring(domain) + L"\\" + name);
}

std::string tokenTypeName(HANDLE token)
{
    TOKEN_TYPE type {};
    DWORD size = 0;
    if (!::GetTokenInformation(token, TokenType, &type, sizeof type, &size))
        return "?";
    if (type == TokenPrimary)
        return "primary";
    SECURITY_IMPERSONATION_LEVEL level {};
    if (!::GetTokenInformation(token, TokenImpersonationLevel, &level, sizeof level, &size))
        return "impersonation(?)";
    const char* names[] = {"anonymous", "identification", "impersonation", "delegation"};
    return std::string("impersonation:") + (level >= 0 && level <= 3 ? names[level] : "?");
}

// The hard check of §3.2: a token is only used for a plugin when it is not
// elevated and below high integrity. The elevated parent's own token must
// be refused by it; Explorer's token, when the user started Explorer "as
// administrator", too. TokenElevationType is reported but not judged: a
// restricted token made from the elevated one still says "full" (2) while
// TokenIsElevated is 0 and the integrity is what was set.
struct Elevation {
    bool elevated = false;
    int type = 0;
    DWORD integrity = 0;
    std::string describe() const
    {
        return format("elevated=%d type=%d integrity=%lu", elevated ? 1 : 0, type, integrity);
    }
};

Elevation elevationOf(HANDLE token)
{
    return {tokenElevated(token), tokenElevationType(token), integrityRid(token)};
}

bool usableAsPluginToken(const Elevation& e)
{
    return !e.elevated && e.integrity < SECURITY_MANDATORY_HIGH_RID;
}

Handle ownToken(DWORD access = TOKEN_QUERY)
{
    HANDLE token = nullptr;
    ::OpenProcessToken(::GetCurrentProcess(), access, &token);
    return Handle(token);
}

// ---- the sandbox pieces (DocExtractor's, without low integrity forced) ----

// Our own token without Administrators and privileges, at `integrity`
// (SECURITY_MANDATORY_MEDIUM_RID or _LOW_RID): route B.
Handle restrictedToken(DWORD integrity, DWORD* error)
{
    *error = 0;
    const Handle own = ownToken(TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT);
    if (!own.valid()) {
        *error = ::GetLastError();
        return {};
    }
    BYTE administrators[SECURITY_MAX_SID_SIZE];
    BYTE localAdministrator[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof administrators;
    ::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &size);
    size = sizeof localAdministrator;
    ::CreateWellKnownSid(WinLocalAccountAndAdministratorSid, nullptr, localAdministrator, &size);
    SID_AND_ATTRIBUTES deny[] = {{administrators, 0}, {localAdministrator, 0}};
    HANDLE restricted = nullptr;
    if (!::CreateRestrictedToken(own.get(), DISABLE_MAX_PRIVILEGE, 2, deny, 0, nullptr, 0, nullptr, &restricted)) {
        *error = ::GetLastError();
        return {};
    }
    Handle token(restricted);
    BYTE label[SECURITY_MAX_SID_SIZE];
    size = sizeof label;
    ::CreateWellKnownSid(integrity == SECURITY_MANDATORY_LOW_RID ? WinLowLabelSid : WinMediumLabelSid, nullptr, label, &size);
    TOKEN_MANDATORY_LABEL mandatory {};
    mandatory.Label.Attributes = SE_GROUP_INTEGRITY;
    mandatory.Label.Sid = label;
    if (!::SetTokenInformation(restricted, TokenIntegrityLevel, &mandatory, sizeof mandatory + ::GetLengthSid(label))) {
        *error = ::GetLastError();
        return {};
    }
    // Default DACL: the user and the system (an elevated token's names only
    // Administrators, which the child is not).
    BYTE userBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    BYTE system[SECURITY_MAX_SID_SIZE];
    size = sizeof system;
    DWORD got = 0;
    if (::GetTokenInformation(own.get(), TokenUser, userBuffer, sizeof userBuffer, &got)
        && ::CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size)) {
        const PSID user = reinterpret_cast<TOKEN_USER*>(userBuffer)->User.Sid;
        BYTE aclBuffer[256];
        auto* acl = reinterpret_cast<PACL>(aclBuffer);
        if (::InitializeAcl(acl, sizeof aclBuffer, ACL_REVISION) && ::AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, user)
            && ::AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, system)) {
            TOKEN_DEFAULT_DACL dacl {acl};
            ::SetTokenInformation(restricted, TokenDefaultDacl, &dacl, sizeof dacl);
        }
    }
    return token;
}

// Explorer's token as a primary token: route A. `pid` is the shell's.
Handle explorerToken(DWORD* pid, DWORD* error, std::string* how)
{
    *error = 0;
    *pid = 0;
    const HWND shellWindow = ::GetShellWindow();
    if (shellWindow)
        ::GetWindowThreadProcessId(shellWindow, pid);
    *how = *pid ? "GetShellWindow" : "none";
    if (!*pid) {
        // No shell window (another shell, or Explorer is restarting): the
        // first explorer.exe in our session, if any.
        const Handle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        PROCESSENTRY32W entry {sizeof entry};
        DWORD session = 0;
        ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
        if (snapshot.valid() && ::Process32FirstW(snapshot.get(), &entry)) {
            do {
                DWORD candidateSession = 0;
                if (_wcsicmp(entry.szExeFile, L"explorer.exe") == 0 && ::ProcessIdToSessionId(entry.th32ProcessID, &candidateSession)
                    && candidateSession == session) {
                    *pid = entry.th32ProcessID;
                    *how = "Toolhelp";
                    break;
                }
            } while (::Process32NextW(snapshot.get(), &entry));
        }
    }
    if (!*pid) {
        *error = ERROR_NOT_FOUND;
        return {};
    }
    const Handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pid));
    if (!process.valid()) {
        *error = ::GetLastError();
        return {};
    }
    HANDLE source = nullptr;
    if (!::OpenProcessToken(process.get(), TOKEN_DUPLICATE | TOKEN_QUERY, &source)) {
        *error = ::GetLastError();
        return {};
    }
    const Handle sourceToken(source);
    HANDLE primary = nullptr;
    if (!::DuplicateTokenEx(sourceToken.get(),
            TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID
                | TOKEN_IMPERSONATE,
            nullptr, SecurityImpersonation, TokenPrimary, &primary)) {
        *error = ::GetLastError();
        return {};
    }
    return Handle(primary);
}

enum class JobKind { None, Limited, Unlimited };

// Limited: what a plugin gets (§3.3 without `processes`, `clipboard:*`):
// ends with the job, one process, 512 MB, no clipboard. Unlimited: ends
// with the job, nothing else (the runner of route B').
Handle makeJob(JobKind kind, DWORD* error)
{
    *error = 0;
    Handle job(::CreateJobObjectW(nullptr, nullptr));
    if (!job.valid()) {
        *error = ::GetLastError();
        return {};
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (kind == JobKind::Limited) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY
            | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        limits.ProcessMemoryLimit = 512u * 1024 * 1024;
    }
    if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
        *error = ::GetLastError();
        return {};
    }
    if (kind == JobKind::Limited) {
        JOBOBJECT_BASIC_UI_RESTRICTIONS ui {};
        ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
        if (!::SetInformationJobObject(job.get(), JobObjectBasicUIRestrictions, &ui, sizeof ui)) {
            *error = ::GetLastError();
            return {};
        }
    }
    return job;
}

// The user's environment from `token` (CreateEnvironmentBlock: registry and
// system, never this process's variables), plus `extra`. Falls back to this
// process's variables when the token cannot be used, and says so.
std::vector<wchar_t> environmentFor(HANDLE token, const std::vector<std::pair<std::wstring, std::wstring>>& extra,
    std::string* source)
{
    std::vector<std::wstring> entries;
    void* block = nullptr;
    if (token && ::CreateEnvironmentBlock(&block, token, FALSE)) {
        for (const wchar_t* p = static_cast<wchar_t*>(block); *p; p += std::wcslen(p) + 1)
            entries.emplace_back(p);
        ::DestroyEnvironmentBlock(block);
        *source = "CreateEnvironmentBlock";
    } else {
        *source = format("GetEnvironmentStrings (CreateEnvironmentBlock error %lu)", token ? ::GetLastError() : 0);
        wchar_t* env = ::GetEnvironmentStringsW();
        for (const wchar_t* p = env; *p; p += std::wcslen(p) + 1)
            entries.emplace_back(p);
        ::FreeEnvironmentStringsW(env);
    }
    for (const auto& [name, value] : extra) {
        const std::wstring prefix = name + L'=';
        std::erase_if(entries, [&](const std::wstring& e) {
            return e.size() > prefix.size() && _wcsnicmp(e.c_str(), prefix.c_str(), prefix.size()) == 0;
        });
        entries.push_back(prefix + value);
    }
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::vector<wchar_t> flat;
    for (const auto& e : entries) {
        flat.insert(flat.end(), e.begin(), e.end());
        flat.push_back(L'\0');
    }
    flat.push_back(L'\0');
    return flat;
}

// ---- starting a child ------------------------------------------------------

enum class Method { Plain, AsUser, WithToken };
enum class Variant { Plain = 0, Ex = 1, ExHandles = 2, ExHandlesMitigation = 3 };

const char* methodName(Method m)
{
    switch (m) {
    case Method::Plain: return "CreateProcessW";
    case Method::AsUser: return "CreateProcessAsUserW";
    case Method::WithToken: return "CreateProcessWithTokenW";
    }
    return "?";
}

const char* variantName(Variant v)
{
    switch (v) {
    case Variant::Plain: return "STARTUPINFO+USESTDHANDLES";
    case Variant::Ex: return "STARTUPINFOEX(empty)";
    case Variant::ExHandles: return "STARTUPINFOEX+HANDLE_LIST";
    case Variant::ExHandlesMitigation: return "STARTUPINFOEX+HANDLE_LIST+MITIGATION";
    }
    return "?";
}

struct LaunchSpec {
    Method method = Method::Plain;
    Variant variant = Variant::ExHandlesMitigation;
    JobKind job = JobKind::Limited;
    HANDLE token = nullptr; // AsUser / WithToken: the child's; also the environment's
    std::wstring role = L"--child";
    bool full = false; // WS_PROBE_FULL=1: the child reports its facts
    std::wstring explorerLuid;
    std::wstring clipHash;
    std::wstring share;
};

struct Child {
    Handle process;
    Handle job;
    Handle stdinServer; // we write
    Handle stdoutServer; // we read
    Handle readEvent;
    Handle writeEvent;
    Handle leakEvent; // inheritable, not in the handle list: must not arrive
    std::string buffer;
    bool eof = false;

    // One line without its newline; nullopt on timeout or end of pipe.
    std::optional<std::string> readLine(int timeoutMs)
    {
        for (;;) {
            if (const std::size_t nl = buffer.find('\n'); nl != std::string::npos) {
                std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return line;
            }
            if (eof)
                return std::nullopt;
            char chunk[4096];
            OVERLAPPED overlapped {};
            overlapped.hEvent = readEvent.get();
            DWORD moved = 0;
            if (!::ReadFile(stdoutServer.get(), chunk, sizeof chunk, nullptr, &overlapped)) {
                const DWORD error = ::GetLastError();
                if (error == ERROR_BROKEN_PIPE) {
                    eof = true;
                    continue;
                }
                if (error != ERROR_IO_PENDING) {
                    eof = true;
                    continue;
                }
                if (::WaitForSingleObject(readEvent.get(), static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0) {
                    ::CancelIoEx(stdoutServer.get(), &overlapped);
                    ::GetOverlappedResult(stdoutServer.get(), &overlapped, &moved, TRUE);
                    if (moved)
                        buffer.append(chunk, moved);
                    return std::nullopt;
                }
            }
            if (!::GetOverlappedResult(stdoutServer.get(), &overlapped, &moved, TRUE)) {
                eof = true;
                continue;
            }
            buffer.append(chunk, moved);
        }
    }

    bool writeLine(const std::string& line)
    {
        const std::string text = line + '\n';
        OVERLAPPED overlapped {};
        overlapped.hEvent = writeEvent.get();
        DWORD moved = 0;
        if (!::WriteFile(stdinServer.get(), text.data(), static_cast<DWORD>(text.size()), nullptr, &overlapped)
            && ::GetLastError() != ERROR_IO_PENDING)
            return false;
        return ::GetOverlappedResult(stdinServer.get(), &overlapped, &moved, TRUE) && moved == text.size();
    }

    DWORD waitExit(int timeoutMs, bool* timedOut)
    {
        *timedOut = ::WaitForSingleObject(process.get(), static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0;
        DWORD code = 0;
        ::GetExitCodeProcess(process.get(), &code);
        if (*timedOut)
            ::TerminateProcess(process.get(), 9);
        return code;
    }
};

struct Launched {
    std::unique_ptr<Child> child;
    std::string stage; // empty when started; else where it failed
    DWORD error = 0;
    long long createUs = 0; // the CreateProcess* call itself
    bool inJobBefore = false; // already in a job when created (inherited from us)
    std::string envSource;
    LARGE_INTEGER started {};
};

Launched launch(const LaunchSpec& spec)
{
    Launched result;
    auto child = std::make_unique<Child>();
    static unsigned counter = 0;
    wchar_t base[96];
    std::swprintf(base, std::size(base), L"\\\\.\\pipe\\WinShun.probe.%lu.%u.%llu", ::GetCurrentProcessId(), counter++,
        static_cast<unsigned long long>(::GetTickCount64()));
    const std::wstring inName = std::wstring(base) + L".in";
    const std::wstring outName = std::wstring(base) + L".out";
    // Our ends: overlapped. The child's ends: plain (it reads and writes
    // synchronously, as any plugin would), inheritable.
    child->stdinServer.reset(::CreateNamedPipeW(inName.c_str(),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024, 0, nullptr));
    child->stdoutServer.reset(::CreateNamedPipeW(outName.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024, 0, nullptr));
    if (!child->stdinServer.valid() || !child->stdoutServer.valid()) {
        result.stage = "CreateNamedPipe";
        result.error = ::GetLastError();
        return result;
    }
    SECURITY_ATTRIBUTES inheritable {sizeof inheritable, nullptr, TRUE};
    Handle stdinClient(::CreateFileW(inName.c_str(), GENERIC_READ, 0, &inheritable, OPEN_EXISTING, 0, nullptr));
    Handle stdoutClient(::CreateFileW(outName.c_str(), GENERIC_WRITE, 0, &inheritable, OPEN_EXISTING, 0, nullptr));
    child->readEvent.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    child->writeEvent.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    child->leakEvent.reset(::CreateEventW(&inheritable, TRUE, FALSE, nullptr));
    if (!stdinClient.valid() || !stdoutClient.valid() || !child->readEvent.valid() || !child->writeEvent.valid()
        || !child->leakEvent.valid()) {
        result.stage = "CreateFile(pipe client)";
        result.error = ::GetLastError();
        return result;
    }

    // The environment: the user's, from the token, plus WS_*.
    const Handle own = ownToken(TOKEN_QUERY | TOKEN_DUPLICATE);
    const std::wstring exe = ownPath();
    const std::wstring folder = exe.substr(0, exe.find_last_of(L'\\'));
    std::vector<std::pair<std::wstring, std::wstring>> extra = {
        {L"WS_QUERY", L"探针 probe"},
        {L"WS_PLUGIN_DIR", folder},
        {L"WS_LOCALE", L"zh"},
        {L"WS_PROBE_FULL", spec.full ? L"1" : L"0"},
        {L"WS_PROBE_LEAK_HANDLE",
            wide(format("%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(child->leakEvent.get()))))},
    };
    if (!spec.explorerLuid.empty())
        extra.emplace_back(L"WS_EXPLORER_LUID", spec.explorerLuid);
    if (!spec.clipHash.empty())
        extra.emplace_back(L"WS_CLIP_HASH", spec.clipHash);
    if (!spec.share.empty())
        extra.emplace_back(L"WS_SHARE", spec.share);
    std::vector<wchar_t> environment = environmentFor(spec.token ? spec.token : own.get(), extra, &result.envSource);

    // The startup information, by variant.
    const bool extended = spec.variant != Variant::Plain;
    std::vector<char> attributes;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    HANDLE inherited[2] = {stdinClient.get(), stdoutClient.get()};
    DWORD64 mitigations[2] = {PROCESS_CREATION_MITIGATION_POLICY_HEAP_TERMINATE_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON
            | PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON,
        0};
    if (extended) {
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        attributes.resize(size);
        list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!::InitializeProcThreadAttributeList(list, 2, 0, &size)) {
            result.stage = "InitializeProcThreadAttributeList";
            result.error = ::GetLastError();
            return result;
        }
        if (spec.variant >= Variant::ExHandles
            && !::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr, nullptr)) {
            result.stage = "UpdateProcThreadAttribute(HANDLE_LIST)";
            result.error = ::GetLastError();
            ::DeleteProcThreadAttributeList(list);
            return result;
        }
        if (spec.variant >= Variant::ExHandlesMitigation
            && !::UpdateProcThreadAttribute(
                list, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, mitigations, sizeof mitigations, nullptr, nullptr)) {
            result.stage = "UpdateProcThreadAttribute(MITIGATION_POLICY)";
            result.error = ::GetLastError();
            ::DeleteProcThreadAttributeList(list);
            return result;
        }
    }
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = extended ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdinClient.get();
    startup.StartupInfo.hStdOutput = stdoutClient.get();
    startup.StartupInfo.hStdError = stdoutClient.get();
    startup.lpAttributeList = list;
    const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT
        | (extended ? EXTENDED_STARTUPINFO_PRESENT : 0);
    std::wstring commandLine = L"\"" + exe + L"\" " + spec.role;
    PROCESS_INFORMATION info {};
    BOOL created = FALSE;
    result.started = tick();
    switch (spec.method) {
    case Method::Plain:
        created = ::CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags, environment.data(),
            folder.c_str(), &startup.StartupInfo, &info);
        break;
    case Method::AsUser:
        created = ::CreateProcessAsUserW(spec.token, exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags,
            environment.data(), folder.c_str(), &startup.StartupInfo, &info);
        break;
    case Method::WithToken:
        created = ::CreateProcessWithTokenW(spec.token, 0, exe.c_str(), commandLine.data(), flags, environment.data(),
            folder.c_str(), &startup.StartupInfo, &info);
        break;
    }
    const DWORD createError = ::GetLastError();
    result.createUs = microsecondsSince(result.started);
    if (list)
        ::DeleteProcThreadAttributeList(list);
    // Our copies of the child's pipe ends go now, or the pipe never breaks
    // when the child exits.
    stdinClient.reset();
    stdoutClient.reset();
    if (!created) {
        result.stage = methodName(spec.method);
        result.error = createError;
        return result;
    }
    child->process.reset(info.hProcess);
    const Handle thread(info.hThread);
    BOOL inJob = FALSE;
    ::IsProcessInJob(info.hProcess, nullptr, &inJob);
    result.inJobBefore = inJob;
    if (spec.job != JobKind::None) {
        DWORD jobError = 0;
        child->job = makeJob(spec.job, &jobError);
        if (!child->job.valid()) {
            result.stage = "CreateJobObject";
            result.error = jobError;
            ::TerminateProcess(info.hProcess, 1);
            return result;
        }
        if (!::AssignProcessToJobObject(child->job.get(), info.hProcess)) {
            result.stage = "AssignProcessToJobObject";
            result.error = ::GetLastError();
            ::TerminateProcess(info.hProcess, 1);
            return result;
        }
    }
    ::ResumeThread(info.hThread);
    result.child = std::move(child);
    return result;
}

// One child driven through its whole life: hello, facts (when full), echo,
// exit. The times are from before the CreateProcess* call.
struct Run {
    std::string stage; // empty: everything worked
    DWORD error = 0;
    long long createUs = 0, helloUs = 0, factsUs = 0, echoUs = 0, exitUs = 0;
    bool inJobBefore = false;
    bool exitTimedOut = false;
    DWORD exitCode = 0;
    std::string envSource;
    std::string hello, facts, echo;
};

Run runOnce(const LaunchSpec& spec)
{
    Run run;
    Launched launched = launch(spec);
    run.createUs = launched.createUs;
    run.inJobBefore = launched.inJobBefore;
    run.envSource = launched.envSource;
    if (!launched.child) {
        run.stage = launched.stage;
        run.error = launched.error;
        return run;
    }
    Child& child = *launched.child;
    const auto hello = child.readLine(5000);
    run.helloUs = microsecondsSince(launched.started);
    if (!hello) {
        run.stage = "hello (no line from the child's stdout)";
        bool timedOut = false;
        run.exitCode = child.waitExit(500, &timedOut);
        run.error = run.exitCode;
        return run;
    }
    run.hello = *hello;
    if (spec.full) {
        const auto facts = child.readLine(15000);
        run.factsUs = microsecondsSince(launched.started);
        if (!facts) {
            run.stage = "facts (no second line)";
            bool timedOut = false;
            run.exitCode = child.waitExit(500, &timedOut);
            return run;
        }
        run.facts = *facts;
    }
    const std::string ping = format("ping %lu %lld", ::GetCurrentProcessId(), run.helloUs);
    const LARGE_INTEGER before = tick();
    if (!child.writeLine(ping)) {
        run.stage = "echo (writing to the child's stdin)";
        run.error = ::GetLastError();
        return run;
    }
    const auto echo = child.readLine(5000);
    run.echoUs = microsecondsSince(before);
    if (!echo) {
        run.stage = "echo (no reply from the child)";
        return run;
    }
    run.echo = *echo;
    if (jsonValue(*echo, "echo") != ping)
        run.stage = "echo (wrong reply: " + *echo + ")";
    const LARGE_INTEGER beforeExit = tick();
    run.exitCode = child.waitExit(5000, &run.exitTimedOut);
    run.exitUs = microsecondsSince(beforeExit);
    return run;
}

// ---- the child's facts -----------------------------------------------------

// "format:size:hash" of the first clipboard format whose data can be read
// (whatever the user has there: text, files, an image), or "" when there is
// none or none can be read; `formats` is how many the clipboard holds.
std::string clipboardHash(DWORD* error, int* formats)
{
    *error = 0;
    *formats = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (::OpenClipboard(nullptr))
            break;
        *error = ::GetLastError();
        ::Sleep(20);
        if (attempt == 4)
            return {};
    }
    *formats = ::CountClipboardFormats();
    std::string result;
    for (UINT f = ::EnumClipboardFormats(0); f; f = ::EnumClipboardFormats(f)) {
        if (f == CF_BITMAP || f == CF_PALETTE || f == CF_METAFILEPICT || f == CF_ENHMETAFILE || f == CF_DSPBITMAP
            || f == CF_DSPENHMETAFILE || f == CF_DSPMETAFILEPICT)
            continue; // GDI handles, not memory
        const HANDLE data = ::GetClipboardData(f);
        if (!data) {
            *error = ::GetLastError();
            continue;
        }
        const SIZE_T size = ::GlobalSize(data);
        if (const auto* bytes = static_cast<const unsigned char*>(::GlobalLock(data))) {
            unsigned long long h = 14695981039346656037ull;
            for (SIZE_T i = 0; i < size; ++i) {
                h ^= bytes[i];
                h *= 1099511628211ull;
            }
            ::GlobalUnlock(data);
            result = format("%u:%zu:%llx", f, static_cast<std::size_t>(size), h);
            *error = 0;
            break;
        }
        *error = ::GetLastError();
    }
    if (result.empty() && *error == 0)
        *error = ERROR_NO_DATA;
    ::CloseClipboard();
    return result;
}

struct ShareProbe {
    std::wstring path;
    DWORD error = 0;
    bool ok = false;
};

DWORD WINAPI shareProbeThread(void* parameter)
{
    auto* probe = static_cast<ShareProbe*>(parameter);
    WIN32_FIND_DATAW data {};
    const HANDLE find = ::FindFirstFileW((probe->path + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        probe->error = ::GetLastError();
    else {
        probe->ok = true;
        ::FindClose(find);
    }
    return 0;
}

std::string collectFacts(const char* role, bool full)
{
    Json j;
    j.beginObject();
    j.str("role", role);
    j.num("pid", ::GetCurrentProcessId());
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    j.num("session", session);

    const Handle token = ownToken(TOKEN_QUERY);
    j.num("integrity", integrityRid(token.get()));
    j.boolean("elevated", tokenElevated(token.get()));
    j.num("elevationType", tokenElevationType(token.get()));
    j.str("admins", administratorsGroup(token.get()));
    const std::string authId = authenticationId(token.get());
    j.str("authId", authId);
    const std::wstring explorerLuid = environmentVariable(L"WS_EXPLORER_LUID");
    if (explorerLuid.empty())
        j.null("authIdMatchesExplorer");
    else
        j.boolean("authIdMatchesExplorer", utf8(explorerLuid) == authId);
    j.num("privileges", privilegeCount(token.get()));
    j.str("user", tokenUserName(token.get()));
    DWORD virtualization = 0, size = 0;
    ::GetTokenInformation(token.get(), TokenVirtualizationEnabled, &virtualization, sizeof virtualization, &size);
    j.boolean("virtualization", virtualization != 0);

    // The job: ours, with our limits, or some other?
    BOOL inJob = FALSE;
    ::IsProcessInJob(::GetCurrentProcess(), nullptr, &inJob);
    j.boolean("inJob", inJob != FALSE);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui {};
    if (inJob && ::QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof limits, nullptr)) {
        ::QueryInformationJobObject(nullptr, JobObjectBasicUIRestrictions, &ui, sizeof ui, nullptr);
        j.str("jobLimitFlags", hex(limits.BasicLimitInformation.LimitFlags));
        j.boolean("jobKillOnClose", (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0);
        j.num("jobActiveProcessLimit",
            (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) ? limits.BasicLimitInformation.ActiveProcessLimit : 0);
        j.num("jobMemoryMB",
            (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) ? static_cast<long long>(limits.ProcessMemoryLimit / (1024 * 1024)) : 0);
        j.str("jobUiRestrictions", hex(ui.UIRestrictionsClass));
        j.boolean("jobClipboardLimited", (ui.UIRestrictionsClass & JOB_OBJECT_UILIMIT_READCLIPBOARD) != 0);
    } else {
        j.null("jobLimitFlags");
        j.boolean("jobKillOnClose", false);
        j.num("jobActiveProcessLimit", -1);
        j.num("jobMemoryMB", -1);
        j.null("jobUiRestrictions");
        j.boolean("jobClipboardLimited", false);
    }

    // Mitigations: those the design asks for, and dynamic code, which it
    // leaves on (interpreters need it).
    PROCESS_MITIGATION_ASLR_POLICY aslr {};
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extension {};
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad {};
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode {};
    ::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessASLRPolicy, &aslr, sizeof aslr);
    ::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessExtensionPointDisablePolicy, &extension, sizeof extension);
    ::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessImageLoadPolicy, &imageLoad, sizeof imageLoad);
    ::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessDynamicCodePolicy, &dynamicCode, sizeof dynamicCode);
    j.boolean("aslrBottomUp", aslr.EnableBottomUpRandomization != 0);
    j.boolean("aslrHighEntropy", aslr.EnableHighEntropy != 0);
    j.boolean("aslrForceRelocate", aslr.EnableForceRelocateImages != 0);
    j.boolean("extensionPointsDisabled", extension.DisableExtensionPoints != 0);
    j.boolean("noRemoteImages", imageLoad.NoRemoteImages != 0);
    j.boolean("noLowLabelImages", imageLoad.NoLowMandatoryLabelImages != 0);
    j.boolean("dynamicCodeProhibited", dynamicCode.ProhibitDynamicCode != 0);

    // The console and the standard handles.
    const HWND console = ::GetConsoleWindow();
    j.boolean("consoleWindow", console != nullptr);
    j.boolean("consoleVisible", console && ::IsWindowVisible(console));
    j.num("stdinType", ::GetFileType(::GetStdHandle(STD_INPUT_HANDLE)));
    j.num("stdoutType", ::GetFileType(::GetStdHandle(STD_OUTPUT_HANDLE)));

    // The environment: WS_* received, the parent's own variables not.
    j.beginArray("wsEnv");
    int envCount = 0;
    if (wchar_t* env = ::GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += std::wcslen(p) + 1) {
            ++envCount;
            if (_wcsnicmp(p, L"WS_", 3) == 0) {
                std::wstring entry(p);
                if (entry.size() > 60)
                    entry = entry.substr(0, 57) + L"...";
                j.str(nullptr, entry);
            }
        }
        ::FreeEnvironmentStringsW(env);
    }
    j.endArray();
    j.num("envCount", envCount);
    j.str("wsQuery", environmentVariable(L"WS_QUERY"));
    j.boolean("parentEnvLeaked", !environmentVariable(L"PROBE_PARENT_MARK").empty());
    j.str("userprofile", environmentVariable(L"USERPROFILE"));
    j.str("temp", environmentVariable(L"TEMP"));
    j.str("appdata", environmentVariable(L"APPDATA"));
    j.str("username", environmentVariable(L"USERNAME"));

    // The event the parent made inheritable but left out of the handle list.
    const std::wstring leak = environmentVariable(L"WS_PROBE_LEAK_HANDLE");
    if (leak.empty())
        j.null("handleLeaked");
    else {
        const HANDLE h = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::wcstoull(leak.c_str(), nullptr, 16)));
        DWORD flags = 0;
        const bool leaked = ::GetHandleInformation(h, &flags) && ::WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
        j.boolean("handleLeaked", leaked);
    }

    if (full) {
        // May it start a process?
        {
            const std::wstring exe = ownPath();
            std::wstring commandLine = L"\"" + exe + L"\" --noop";
            STARTUPINFOW startup {sizeof startup};
            PROCESS_INFORMATION info {};
            if (::CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                    &startup, &info)) {
                ::WaitForSingleObject(info.hProcess, 3000);
                DWORD code = 0;
                ::GetExitCodeProcess(info.hProcess, &code);
                BOOL childInJob = FALSE;
                ::IsProcessInJob(info.hProcess, nullptr, &childInJob);
                ::CloseHandle(info.hThread);
                ::CloseHandle(info.hProcess);
                j.boolean("spawnOk", true);
                j.num("spawnError", 0);
                j.boolean("spawnedInJob", childInJob != FALSE);
            } else {
                j.boolean("spawnOk", false);
                j.num("spawnError", ::GetLastError());
                j.null("spawnedInJob");
            }
        }
        // May it read the clipboard?
        {
            DWORD error = 0;
            int formats = 0;
            const std::string hash = clipboardHash(&error, &formats);
            const std::wstring expected = environmentVariable(L"WS_CLIP_HASH");
            j.boolean("clipRead", !hash.empty());
            j.num("clipError", error);
            j.num("clipFormats", formats);
            if (expected.empty())
                j.null("clipMatches");
            else
                j.boolean("clipMatches", hash == utf8(expected));
        }
        // Mapped network drives: list the root of each.
        j.beginArray("netDrives");
        const DWORD drives = ::GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(drives & (1u << i)))
                continue;
            const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
            if (::GetDriveTypeW(root.c_str()) != DRIVE_REMOTE)
                continue;
            j.beginObject();
            j.str("drive", root);
            wchar_t remote[1024];
            DWORD remoteSize = static_cast<DWORD>(std::size(remote));
            j.str("remote", ::WNetGetConnectionW(root.substr(0, 2).c_str(), remote, &remoteSize) == NO_ERROR ? std::wstring(remote) : L"?");
            WIN32_FIND_DATAW data {};
            const HANDLE find = ::FindFirstFileW((root + L"*").c_str(), &data);
            j.boolean("listOk", find != INVALID_HANDLE_VALUE);
            j.num("error", find != INVALID_HANDLE_VALUE ? 0 : ::GetLastError());
            if (find != INVALID_HANDLE_VALUE)
                ::FindClose(find);
            j.endObject();
        }
        j.endArray();
        // A share named by the parent, with a cap on the wait.
        const std::wstring share = environmentVariable(L"WS_SHARE");
        if (share.empty())
            j.null("share");
        else {
            auto* probe = new ShareProbe {share};
            const LARGE_INTEGER before = tick();
            const Handle thread(::CreateThread(nullptr, 0, shareProbeThread, probe, 0, nullptr));
            const bool finished = thread.valid() && ::WaitForSingleObject(thread.get(), 3000) == WAIT_OBJECT_0;
            j.beginObject("share");
            j.str("path", share);
            j.boolean("ok", finished && probe->ok);
            j.num("error", finished ? probe->error : WAIT_TIMEOUT);
            j.num("ms", microsecondsSince(before) / 1000);
            j.endObject();
            if (finished)
                delete probe; // else the thread still owns it; the process ends soon anyway
        }
    }
    j.endObject();
    return j.text;
}

// ---- the child and runner roles -------------------------------------------

bool writeStdout(const std::string& line)
{
    const std::string text = line + '\n';
    DWORD moved = 0;
    return ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &moved, nullptr)
        && moved == text.size();
}

std::optional<std::string> readStdinLine()
{
    std::string line;
    for (;;) {
        char c = 0;
        DWORD moved = 0;
        if (!::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), &c, 1, &moved, nullptr) || moved == 0)
            return line.empty() ? std::nullopt : std::optional<std::string>(line);
        if (c == '\n')
            return line;
        if (c != '\r')
            line += c;
    }
}

int childMain()
{
    if (!writeStdout(format("{\"hello\":1,\"pid\":%lu}", ::GetCurrentProcessId())))
        return 2;
    if (environmentVariable(L"WS_PROBE_FULL") == L"1" && !writeStdout(collectFacts("child", true)))
        return 3;
    const auto line = readStdinLine();
    if (!line)
        return 4;
    Json j;
    j.beginObject();
    j.str("echo", *line);
    j.endObject();
    return writeStdout(j.text) ? 0 : 5;
}

int runnerMain()
{
    if (!writeStdout(format("{\"hello\":1,\"pid\":%lu,\"runner\":1}", ::GetCurrentProcessId())))
        return 2;
    // Started through CreateProcessWithTokenW, the runner got no mitigation
    // policy of its own: it sets on itself what a process may set once it
    // runs (its facts show whether that took).
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extension {};
    extension.DisableExtensionPoints = 1;
    ::SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &extension, sizeof extension);
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad {};
    imageLoad.NoRemoteImages = 1;
    imageLoad.NoLowMandatoryLabelImages = 1;
    ::SetProcessMitigationPolicy(ProcessImageLoadPolicy, &imageLoad, sizeof imageLoad);
    if (!writeStdout(collectFacts("runner", true)))
        return 3;
    for (;;) {
        const auto line = readStdinLine();
        if (!line || *line == "quit")
            return 0;
        if (line->rfind("spawn", 0) != 0)
            continue;
        LaunchSpec spec;
        spec.method = Method::Plain;
        spec.variant = Variant::ExHandlesMitigation;
        spec.job = JobKind::Limited;
        spec.full = *line == "spawn full";
        spec.explorerLuid = environmentVariable(L"WS_EXPLORER_LUID");
        spec.clipHash = environmentVariable(L"WS_CLIP_HASH");
        spec.share = environmentVariable(L"WS_SHARE");
        const Run run = runOnce(spec);
        Json j;
        j.beginObject();
        j.str("stage", run.stage);
        j.num("error", run.error);
        j.num("createUs", run.createUs);
        j.num("helloUs", run.helloUs);
        j.num("factsUs", run.factsUs);
        j.num("echoUs", run.echoUs);
        j.num("exitUs", run.exitUs);
        j.boolean("inJobBefore", run.inJobBefore);
        j.str("envSource", run.envSource);
        j.num("exitCode", run.exitCode);
        j.raw("child", run.facts);
        j.endObject();
        if (!writeStdout(j.text))
            return 5;
    }
}

// ---- the parent -----------------------------------------------------------

struct Stats {
    double median = 0, p95 = 0, min = 0, max = 0;
    int n = 0;
};

Stats stats(std::vector<double> values)
{
    Stats s;
    if (values.empty())
        return s;
    std::sort(values.begin(), values.end());
    s.n = static_cast<int>(values.size());
    s.min = values.front();
    s.max = values.back();
    s.median = values[values.size() / 2];
    s.p95 = values[std::min(values.size() - 1, static_cast<std::size_t>(std::ceil(values.size() * 0.95)) - 1)];
    return s;
}

std::string statsText(const Stats& s)
{
    if (s.n == 0)
        return "-";
    return format("median %.1f  p95 %.1f  min %.1f  max %.1f ms (n=%d)", s.median, s.p95, s.min, s.max, s.n);
}

struct Report {
    std::string text;
    void line(const std::string& s = {})
    {
        text += s;
        text += '\n';
        std::fputs((s + '\n').c_str(), stdout);
        std::fflush(stdout);
    }
};

// What the summary table shows of one child's facts.
std::string factsSummary(const std::string& f)
{
    if (f.empty())
        return "(no facts)";
    const auto b = [&](const char* key) { return jsonValue(f, key) == "true" ? "1" : jsonValue(f, key) == "false" ? "0" : "?"; };
    std::string mitigations;
    mitigations += std::string("bu=") + b("aslrBottomUp");
    mitigations += std::string(" he=") + b("aslrHighEntropy");
    mitigations += std::string(" ep=") + b("extensionPointsDisabled");
    mitigations += std::string(" nr=") + b("noRemoteImages");
    mitigations += std::string(" nl=") + b("noLowLabelImages");
    mitigations += std::string(" dc=") + b("dynamicCodeProhibited");
    std::string spawn = jsonValue(f, "spawnOk").empty() ? "-" : (jsonValue(f, "spawnOk") == "true" ? "ok" : "err " + jsonValue(f, "spawnError"));
    std::string clip = jsonValue(f, "clipRead").empty()
        ? "-"
        : (jsonValue(f, "clipRead") == "true" ? std::string("ok match=") + b("clipMatches")
                                               : "err " + jsonValue(f, "clipError") + " formats=" + jsonValue(f, "clipFormats"));
    const std::string wsEnv = jsonValue(f, "wsEnv");
    const std::string wsCount = std::to_string(std::count(wsEnv.begin(), wsEnv.end(), '"') / 2);
    return format("IL=%s elev=%s type=%s admins=%s authId=%s(=explorer:%s) privs=%s | job=%s kill=%s procLimit=%s mem=%sMB clipLimit=%s | %s | console=%s stdio=%s/%s | ws=%s env=%s mark=%s leak=%s | spawn=%s clip=%s",
        jsonValue(f, "integrity").c_str(), b("elevated"), jsonValue(f, "elevationType").c_str(), jsonValue(f, "admins").c_str(),
        jsonValue(f, "authId").c_str(), b("authIdMatchesExplorer"), jsonValue(f, "privileges").c_str(), b("inJob"),
        b("jobKillOnClose"), jsonValue(f, "jobActiveProcessLimit").c_str(), jsonValue(f, "jobMemoryMB").c_str(),
        b("jobClipboardLimited"), mitigations.c_str(), b("consoleWindow"), jsonValue(f, "stdinType").c_str(),
        jsonValue(f, "stdoutType").c_str(), wsCount.c_str(), jsonValue(f, "envCount").c_str(), b("parentEnvLeaked"),
        b("handleLeaked"), spawn.c_str(), clip.c_str());
}

struct RouteResult {
    std::string name;
    Run first; // the full run
    Stats create, hello, echo, exit;
    bool ok = false;
};

// One route: a full run, then `runs` timed ones (hello and echo only).
RouteResult runRoute(Report& report, const std::string& name, LaunchSpec spec, int runs)
{
    RouteResult result;
    result.name = name;
    report.line();
    report.line(format("== %s: %s, %s, job=%s", name.c_str(), methodName(spec.method), variantName(spec.variant),
        spec.job == JobKind::Limited ? "limited" : spec.job == JobKind::Unlimited ? "kill-on-close only" : "none"));
    spec.full = true;
    result.first = runOnce(spec);
    const Run& first = result.first;
    report.line(format("   first run: %s  create %.1f ms, hello %.1f ms, facts %.1f ms, echo %.2f ms, exit %.1f ms%s, exit code %lu, inJobBefore=%d, env=%s",
        first.stage.empty() ? "OK" : ("FAILED at " + first.stage + " (error " + std::to_string(first.error) + ")").c_str(),
        first.createUs / 1000.0, first.helloUs / 1000.0, first.factsUs / 1000.0, first.echoUs / 1000.0, first.exitUs / 1000.0,
        first.exitTimedOut ? " (TIMED OUT, killed)" : "", first.exitCode, first.inJobBefore ? 1 : 0, first.envSource.c_str()));
    if (!first.facts.empty()) {
        report.line("   facts: " + factsSummary(first.facts));
        report.line("   raw: " + first.facts);
    }
    result.ok = first.stage.empty();
    if (!result.ok)
        return result;
    spec.full = false;
    std::vector<double> create, hello, echo, exit;
    int failed = 0;
    for (int i = 0; i < runs; ++i) {
        const Run run = runOnce(spec);
        if (!run.stage.empty()) {
            ++failed;
            report.line(format("   timed run %d FAILED at %s (error %lu)", i + 1, run.stage.c_str(), run.error));
            continue;
        }
        create.push_back(run.createUs / 1000.0);
        hello.push_back(run.helloUs / 1000.0);
        echo.push_back(run.echoUs / 1000.0);
        exit.push_back(run.exitUs / 1000.0);
    }
    result.create = stats(create);
    result.hello = stats(hello);
    result.echo = stats(echo);
    result.exit = stats(exit);
    report.line("   create call:      " + statsText(result.create));
    report.line("   call->first line: " + statsText(result.hello));
    report.line("   stdin->echo:      " + statsText(result.echo));
    report.line("   echo->exit seen:  " + statsText(result.exit) + (failed ? format("   (%d timed runs failed)", failed) : ""));
    return result;
}

std::string serviceState(const wchar_t* name)
{
    const SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
        return format("OpenSCManager error %lu", ::GetLastError());
    const SC_HANDLE service = ::OpenServiceW(manager, name, SERVICE_QUERY_STATUS);
    std::string result;
    if (!service)
        result = format("OpenService error %lu", ::GetLastError());
    else {
        SERVICE_STATUS status {};
        if (::QueryServiceStatus(service, &status))
            result = status.dwCurrentState == SERVICE_RUNNING ? "running" : format("state %lu", status.dwCurrentState);
        else
            result = format("QueryServiceStatus error %lu", ::GetLastError());
        ::CloseServiceHandle(service);
    }
    ::CloseServiceHandle(manager);
    return result;
}

int parentMain(int argc, wchar_t** argv)
{
    int runs = 20;
    std::wstring out;
    std::wstring share;
    bool simulateElevatedExplorer = false;
    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--runs" && i + 1 < argc)
            runs = std::max(1, _wtoi(argv[++i]));
        else if (arg == L"--out" && i + 1 < argc)
            out = argv[++i];
        else if (arg == L"--share" && i + 1 < argc)
            share = argv[++i];
        else if (arg == L"--simulate-elevated-explorer")
            simulateElevatedExplorer = true;
    }
    ::SetConsoleOutputCP(CP_UTF8);
    // A variable of ours alone: a child that sees it got our environment,
    // not the user's.
    ::SetEnvironmentVariableW(L"PROBE_PARENT_MARK", L"1");

    Report report;
    report.line("plugin-probe: stage 0 of the plugin system");
    report.line(format("runs per route: %d   simulate elevated Explorer: %d", runs, simulateElevatedExplorer ? 1 : 0));
    report.line();

    // Ourselves.
    const Handle own = ownToken(TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT);
    const Elevation ownElevation = elevationOf(own.get());
    BOOL selfInJob = FALSE;
    ::IsProcessInJob(::GetCurrentProcess(), nullptr, &selfInJob);
    report.line("-- parent --");
    report.line(format("pid %lu  user %s  %s  admins=%s  authId=%s  privileges=%d  inJob=%d  SeImpersonate=%d  SeAssignPrimaryToken=%d  SeTcb=%d  SeIncreaseQuota=%d",
        ::GetCurrentProcessId(), tokenUserName(own.get()).c_str(), ownElevation.describe().c_str(),
        administratorsGroup(own.get()).c_str(), authenticationId(own.get()).c_str(), privilegeCount(own.get()), selfInJob ? 1 : 0,
        hasPrivilege(own.get(), SE_IMPERSONATE_NAME) ? 1 : 0, hasPrivilege(own.get(), SE_ASSIGNPRIMARYTOKEN_NAME) ? 1 : 0,
        hasPrivilege(own.get(), SE_TCB_NAME) ? 1 : 0, hasPrivilege(own.get(), SE_INCREASE_QUOTA_NAME) ? 1 : 0));
    report.line(format("hard check on our own token: usable as a plugin token = %d (%s)", usableAsPluginToken(ownElevation) ? 1 : 0,
        ownElevation.elevated ? "must be 0: we are elevated" : "we are not elevated"));
    report.line("seclogon (Secondary Logon, behind CreateProcessWithTokenW): " + serviceState(L"seclogon"));

    // Explorer.
    DWORD explorerPid = 0, explorerError = 0;
    std::string explorerHow;
    Handle explorer = explorerToken(&explorerPid, &explorerError, &explorerHow);
    std::string explorerLuid;
    bool explorerUsable = false;
    report.line("-- Explorer --");
    if (!explorer.valid())
        report.line(format("no Explorer token: pid %lu (%s), error %lu", explorerPid, explorerHow.c_str(), explorerError));
    else {
        Elevation e = elevationOf(explorer.get());
        explorerLuid = authenticationId(explorer.get());
        report.line(format("pid %lu (%s)  %s  admins=%s  authId=%s  privileges=%d  token=%s", explorerPid, explorerHow.c_str(),
            e.describe().c_str(), administratorsGroup(explorer.get()).c_str(), explorerLuid.c_str(), privilegeCount(explorer.get()),
            tokenTypeName(explorer.get()).c_str()));
        if (simulateElevatedExplorer) {
            e.elevated = true;
            e.type = TokenElevationTypeFull;
            e.integrity = SECURITY_MANDATORY_HIGH_RID;
            report.line("   (simulating an Explorer the user started as administrator: " + e.describe() + ")");
        }
        explorerUsable = usableAsPluginToken(e);
        report.line(format("hard check on Explorer's token: usable = %d%s", explorerUsable ? 1 : 0,
            explorerUsable ? "" : "  -> route A refused, the fallback is route B (medium)"));
        report.line(format("same logon session as the parent: %d", explorerLuid == authenticationId(own.get()) ? 1 : 0));
    }

    // What the clipboard holds now, for the children to compare with.
    DWORD clipError = 0;
    int clipFormats = 0;
    const std::string clipHash = clipboardHash(&clipError, &clipFormats);
    report.line(format("clipboard now: %d formats, first readable %s (error %lu)", clipFormats,
        clipHash.empty() ? "none" : clipHash.c_str(), clipError));
    {
        int remote = 0;
        const DWORD drives = ::GetLogicalDrives();
        for (int i = 0; i < 26; ++i)
            if ((drives & (1u << i)) && ::GetDriveTypeW((std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\").c_str()) == DRIVE_REMOTE)
                ++remote;
        report.line(format("mapped network drives seen by the parent: %d%s", remote, remote ? "" : " (the network-drive test is skipped)"));
    }
    if (!share.empty())
        report.line("share to try from each child: " + utf8(share));

    const auto base = [&](Method method, Variant variant, JobKind job, HANDLE token) {
        LaunchSpec spec;
        spec.method = method;
        spec.variant = variant;
        spec.job = job;
        spec.token = token;
        spec.explorerLuid = wide(explorerLuid);
        spec.clipHash = wide(clipHash);
        spec.share = share;
        return spec;
    };

    std::vector<RouteResult> results;
    Variant bestA = Variant::Plain;
    bool anyA = false;

    // Route A: Explorer's token, CreateProcessWithTokenW, every variant of
    // the startup information.
    report.line();
    report.line("#### Route A: Explorer's token + CreateProcessWithTokenW");
    if (!explorerUsable)
        report.line("skipped: no usable Explorer token (see above)");
    else {
        for (const Variant v : {Variant::Plain, Variant::Ex, Variant::ExHandles, Variant::ExHandlesMitigation}) {
            results.push_back(runRoute(report, format("A-v%d", static_cast<int>(v)), base(Method::WithToken, v, JobKind::Limited, explorer.get()), runs));
            if (results.back().ok) {
                bestA = v;
                anyA = true;
            }
        }
        // For the record: the same token through CreateProcessAsUserW
        // (expected: ERROR_PRIVILEGE_NOT_HELD without SeAssignPrimaryToken).
        results.push_back(runRoute(report, "A-asuser", base(Method::AsUser, Variant::ExHandlesMitigation, JobKind::Limited, explorer.get()), std::min(runs, 3)));
    }

    // Route B: our own token restricted, medium and low.
    report.line();
    report.line("#### Route B: CreateRestrictedToken(own) + CreateProcessAsUserW");
    for (const DWORD integrity : {SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_LOW_RID}) {
        DWORD error = 0;
        const Handle restricted = restrictedToken(integrity, &error);
        const std::string name = integrity == SECURITY_MANDATORY_LOW_RID ? "B-low" : "B-med";
        if (!restricted.valid()) {
            report.line(format("%s: restrictedToken failed, error %lu", name.c_str(), error));
            continue;
        }
        report.line(format("%s token: %s admins=%s authId=%s privileges=%d", name.c_str(), elevationOf(restricted.get()).describe().c_str(),
            administratorsGroup(restricted.get()).c_str(), authenticationId(restricted.get()).c_str(), privilegeCount(restricted.get())));
        results.push_back(runRoute(report, name, base(Method::AsUser, Variant::ExHandlesMitigation, JobKind::Limited, restricted.get()), runs));
    }

    // Route C: the linked token.
    report.line();
    report.line("#### Route C: TokenLinkedToken");
    {
        TOKEN_LINKED_TOKEN linked {};
        DWORD size = 0;
        if (!::GetTokenInformation(own.get(), TokenLinkedToken, &linked, sizeof linked, &size))
            report.line(format("GetTokenInformation(TokenLinkedToken) failed, error %lu (no linked token: not a split-token logon)", ::GetLastError()));
        else {
            const Handle linkedToken(linked.LinkedToken);
            const Elevation e = elevationOf(linkedToken.get());
            report.line(format("linked token: %s  type=%s  admins=%s  authId=%s  privileges=%d", e.describe().c_str(),
                tokenTypeName(linkedToken.get()).c_str(), administratorsGroup(linkedToken.get()).c_str(),
                authenticationId(linkedToken.get()).c_str(), privilegeCount(linkedToken.get())));
            report.line(format("hard check on the linked token: usable = %d", usableAsPluginToken(e) ? 1 : 0));
            HANDLE primary = nullptr;
            if (!::DuplicateTokenEx(linkedToken.get(),
                    TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID | TOKEN_IMPERSONATE,
                    nullptr, SecurityImpersonation, TokenPrimary, &primary))
                report.line(format("DuplicateTokenEx(linked -> primary) failed, error %lu -> route C ends here", ::GetLastError()));
            else {
                const Handle primaryToken(primary);
                report.line("DuplicateTokenEx(linked -> primary): OK, " + tokenTypeName(primaryToken.get()));
                if (usableAsPluginToken(e)) {
                    results.push_back(runRoute(report, "C-asuser", base(Method::AsUser, Variant::ExHandlesMitigation, JobKind::Limited, primaryToken.get()), std::min(runs, 5)));
                    results.push_back(runRoute(report, "C-withtoken", base(Method::WithToken, Variant::ExHandlesMitigation, JobKind::Limited, primaryToken.get()), std::min(runs, 5)));
                } else
                    report.line("not started: the linked token is the elevated one (we are the unelevated half), using it would elevate the plugin");
            }
        }
    }

    // Route D: plain CreateProcessW, as a baseline (elevated: the child is
    // elevated too) and as what an unelevated WinShun does.
    report.line();
    report.line("#### Route D: plain CreateProcessW (baseline; the unelevated fallback)");
    results.push_back(runRoute(report, "D-v3", base(Method::Plain, Variant::ExHandlesMitigation, JobKind::Limited, nullptr), runs));
    results.push_back(runRoute(report, "D-v0-leak", base(Method::Plain, Variant::Plain, JobKind::Limited, nullptr), std::min(runs, 3)));
    results.push_back(runRoute(report, "D-control", base(Method::Plain, Variant::ExHandlesMitigation, JobKind::Unlimited, nullptr), std::min(runs, 3)));

    // Route B': a runner without administrator rights, which starts the
    // children. Started by route A when there is one; without a usable
    // Explorer token, with our own token restricted (route B, medium) when
    // we are elevated, else plainly (we are the user already).
    report.line();
    report.line("#### Route B': runner + CreateProcessW inside it");
    {
        DWORD restrictedError = 0;
        const Handle restricted = !anyA && ownElevation.elevated ? restrictedToken(SECURITY_MANDATORY_MEDIUM_RID, &restrictedError) : Handle();
        LaunchSpec spec = anyA ? base(Method::WithToken, bestA, JobKind::Unlimited, explorer.get())
            : restricted.valid() ? base(Method::AsUser, Variant::ExHandlesMitigation, JobKind::Unlimited, restricted.get())
                                 : base(Method::Plain, Variant::ExHandlesMitigation, JobKind::Unlimited, nullptr);
        spec.role = L"--runner";
        spec.full = true;
        report.line(format("runner started with %s, %s%s", methodName(spec.method), variantName(spec.variant),
            anyA ? " (Explorer's token)" : restricted.valid() ? " (own token restricted to medium: no Explorer token to use)" : " (not elevated: plainly)"));
        Launched launched = launch(spec);
        if (!launched.child)
            report.line(format("runner FAILED at %s (error %lu)", launched.stage.c_str(), launched.error));
        else {
            Child& runner = *launched.child;
            const auto hello = runner.readLine(5000);
            const long long helloUs = microsecondsSince(launched.started);
            const auto facts = runner.readLine(15000);
            report.line(format("runner: create %.1f ms, hello %.1f ms%s", launched.createUs / 1000.0, helloUs / 1000.0, hello ? "" : " (NO HELLO)"));
            if (facts) {
                report.line("   runner facts: " + factsSummary(*facts));
                report.line("   raw: " + *facts);
            }
            RouteResult result;
            result.name = "B'";
            std::vector<double> create, hello2, echo, total;
            for (int i = 0; i <= runs; ++i) {
                const bool full = i == 0;
                const LARGE_INTEGER before = tick();
                if (!runner.writeLine(full ? "spawn full" : "spawn")) {
                    report.line("   writing to the runner failed");
                    break;
                }
                const auto reply = runner.readLine(20000);
                const double totalMs = microsecondsSince(before) / 1000.0;
                if (!reply) {
                    report.line(format("   spawn %d: no reply from the runner", i));
                    break;
                }
                const std::string stage = jsonValue(*reply, "stage");
                if (!stage.empty()) {
                    report.line(format("   spawn %d FAILED at %s (error %s)", i, stage.c_str(), jsonValue(*reply, "error").c_str()));
                    if (full)
                        break;
                    continue;
                }
                if (full) {
                    const std::string child = jsonValue(*reply, "child");
                    report.line(format("   first spawn: runner's create %.1f ms, hello %.1f ms, facts %.1f ms, echo %.2f ms, exit %.1f ms; parent saw the whole thing in %.1f ms; inJobBefore=%s env=%s",
                        std::atof(jsonValue(*reply, "createUs").c_str()) / 1000.0, std::atof(jsonValue(*reply, "helloUs").c_str()) / 1000.0,
                        std::atof(jsonValue(*reply, "factsUs").c_str()) / 1000.0, std::atof(jsonValue(*reply, "echoUs").c_str()) / 1000.0,
                        std::atof(jsonValue(*reply, "exitUs").c_str()) / 1000.0, totalMs, jsonValue(*reply, "inJobBefore").c_str(),
                        jsonValue(*reply, "envSource").c_str()));
                    report.line("   facts: " + factsSummary(child));
                    report.line("   raw: " + child);
                    result.first.facts = child;
                    result.ok = true;
                } else {
                    create.push_back(std::atof(jsonValue(*reply, "createUs").c_str()) / 1000.0);
                    hello2.push_back(std::atof(jsonValue(*reply, "helloUs").c_str()) / 1000.0);
                    echo.push_back(std::atof(jsonValue(*reply, "echoUs").c_str()) / 1000.0);
                    total.push_back(totalMs);
                }
            }
            result.create = stats(create);
            result.hello = stats(hello2);
            result.echo = stats(echo);
            result.exit = stats(total);
            report.line("   runner's create call:      " + statsText(result.create));
            report.line("   runner's call->first line: " + statsText(result.hello));
            report.line("   runner's stdin->echo:      " + statsText(result.echo));
            report.line("   parent: spawn->reply (whole child life + two hops): " + statsText(result.exit));
            runner.writeLine("quit");
            bool timedOut = false;
            const DWORD code = runner.waitExit(3000, &timedOut);
            report.line(format("   runner exit code %lu%s", code, timedOut ? " (TIMED OUT)" : ""));
            results.push_back(result);
        }
    }

    // The table.
    report.line();
    report.line("#### Summary");
    report.line("route        | started | IL    elev admins     =explorer | job kill limit clip | bu he ep nr nl dc | leak mark console | spawn      clip        | create ms (med/p95) | call->hello ms (med/p95) | echo ms (med/p95)");
    for (const RouteResult& r : results) {
        const std::string& f = r.first.facts;
        const auto b = [&](const char* key) { return jsonValue(f, key) == "true" ? "1" : jsonValue(f, key) == "false" ? "0" : "?"; };
        std::string spawn = jsonValue(f, "spawnOk").empty() ? "-" : (jsonValue(f, "spawnOk") == "true" ? "ok" : "err " + jsonValue(f, "spawnError"));
        std::string clip = jsonValue(f, "clipRead").empty()
            ? "-"
            : (jsonValue(f, "clipRead") == "true" ? std::string("ok match=") + b("clipMatches")
                                                   : "err " + jsonValue(f, "clipError") + " n=" + jsonValue(f, "clipFormats"));
        report.line(format("%-12s | %-7s | %-5s %-4s %-10s %-9s | %-3s %-4s %-5s %-4s | %-2s %-2s %-2s %-2s %-2s %-2s | %-4s %-4s %-7s | %-10s %-11s | %5.1f / %-5.1f         | %5.1f / %-5.1f              | %5.2f / %-5.2f",
            r.name.c_str(), r.ok ? "yes" : ("no:" + std::to_string(r.first.error)).c_str(), jsonValue(f, "integrity").c_str(),
            b("elevated"), jsonValue(f, "admins").c_str(), b("authIdMatchesExplorer"), b("inJob"), b("jobKillOnClose"),
            jsonValue(f, "jobActiveProcessLimit").c_str(), b("jobClipboardLimited"), b("aslrBottomUp"), b("aslrHighEntropy"),
            b("extensionPointsDisabled"), b("noRemoteImages"), b("noLowLabelImages"), b("dynamicCodeProhibited"), b("handleLeaked"),
            b("parentEnvLeaked"), b("consoleWindow"), spawn.c_str(), clip.c_str(), r.create.median, r.create.p95, r.hello.median,
            r.hello.p95, r.echo.median, r.echo.p95));
    }

    if (!out.empty()) {
        const Handle file(::CreateFileW(out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD moved = 0;
        if (!file.valid() || !::WriteFile(file.get(), report.text.data(), static_cast<DWORD>(report.text.size()), &moved, nullptr))
            std::fprintf(stderr, "could not write %s (error %lu)\n", utf8(out).c_str(), ::GetLastError());
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::wstring role = argc > 1 ? argv[1] : L"";
    if (role == L"--child")
        return childMain();
    if (role == L"--runner")
        return runnerMain();
    if (role == L"--noop")
        return 0;
    if (role == L"--parent")
        return parentMain(argc, argv);
    std::fputs("usage: plugin-probe --parent [--runs N] [--out FILE] [--share \\\\host\\share] [--simulate-elevated-explorer]\n"
               "       plugin-probe --child | --runner | --noop   (started by --parent)\n",
        stderr);
    return 1;
}
