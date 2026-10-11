#include "TaskbarStartGap.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "TapProtocol.h" // src/tap

namespace ws::taskbar {

namespace {

using InitializeFn = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);

constexpr DWORD kAckMs = 3000;
constexpr DWORD kSitedMs = 15000; // from the call to the TAP's first word
constexpr DWORD kAppliedMs = 5000; // from "ready" to the Start button seen and done
constexpr int kAttachTries = 30; // a second apart: Explorer starting, its taskbar not up yet

// Programs known to take Explorer's one XAML diagnostics slot, by process
// name and by what is loaded in explorer (TranslucentTB's TAP stays after
// TranslucentTB.exe ends).
constexpr const wchar_t* kProcesses[] = {L"translucenttb.exe", L"windhawk.exe", L"explorerblurmica.exe"};
constexpr const wchar_t* kModules[] = {L"explorertap.dll", L"windhawk", L"explorerblurmica"};

void trace(const wchar_t* format, ...)
{
    wchar_t line[512];
    va_list args;
    va_start(args, format);
    std::vswprintf(line, 512, format, args);
    va_end(args);
    ::OutputDebugStringW((std::wstring(L"[StartGap] ") + line + L"\n").c_str());
}

std::wstring lower(std::wstring s)
{
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring hex(HRESULT hr)
{
    wchar_t text[16];
    std::swprintf(text, 16, L"0x%08X", static_cast<unsigned>(hr));
    return text;
}

// The first 16 hex digits of the file's SHA-256; empty if unreadable.
std::wstring sha256Prefix(const std::wstring& path)
{
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    std::vector<unsigned char> bytes;
    LARGE_INTEGER size {};
    if (::GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024 * 1024) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
            bytes.clear();
    }
    ::CloseHandle(file);
    if (bytes.empty())
        return {};
    unsigned char digest[32];
    if (!BCRYPT_SUCCESS(::BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()),
            digest, sizeof digest)))
        return {};
    std::wstring text;
    for (int i = 0; i < 8; ++i) {
        wchar_t pair[3];
        std::swprintf(pair, 3, L"%02x", digest[i]);
        text += pair;
    }
    return text;
}

std::wstring exeDir()
{
    wchar_t path[MAX_PATH];
    const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, length);
    const size_t slash = dir.find_last_of(L'\\');
    return slash == std::wstring::npos ? dir : dir.substr(0, slash);
}

std::wstring programData()
{
    wchar_t* path = nullptr;
    std::wstring dir;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &path)) && path)
        dir = path;
    ::CoTaskMemFree(path);
    return dir;
}

// The folder Explorer loads the DLL from: only administrators and SYSTEM may
// write it (C:\ProgramData itself lets any user make files), and inheritance
// is cut so that is what it stays.
bool protect(const std::wstring& dir)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)", SDDL_REVISION_1, &descriptor, nullptr))
        return false;
    PACL dacl = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    bool ok = ::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present
        && ::SetNamedSecurityInfoW(const_cast<wchar_t*>(dir.c_str()), SE_FILE_OBJECT,
               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr)
            == ERROR_SUCCESS;
    ::LocalFree(descriptor);
    return ok;
}

// The DLL next to the exe, copied to %ProgramData%\WinShun\tap\<hash>\ (never
// loaded from the install folder: the deploy script renames files there, and
// Explorer would hold them). What is there already is trusted by its hash,
// not by being there.
std::optional<std::wstring> stage(std::wstring& why)
{
    const std::wstring source = exeDir() + L"\\WinShunTap.dll";
    const std::wstring hash = sha256Prefix(source);
    if (hash.empty()) {
        why = L"WinShunTap.dll missing next to the program";
        return std::nullopt;
    }
    const std::wstring base = programData();
    if (base.empty()) {
        why = L"no ProgramData folder";
        return std::nullopt;
    }
    const std::wstring tap = base + L"\\WinShun\\tap";
    ::CreateDirectoryW((base + L"\\WinShun").c_str(), nullptr);
    ::CreateDirectoryW(tap.c_str(), nullptr);
    if (!protect(tap)) {
        why = L"could not secure " + tap;
        return std::nullopt;
    }
    const std::wstring dir = tap + L"\\" + hash;
    const std::wstring target = dir + L"\\WinShunTap.dll";
    if (sha256Prefix(target) == hash)
        return target;
    ::CreateDirectoryW(dir.c_str(), nullptr);
    if (!::CopyFileW(source.c_str(), target.c_str(), FALSE) || sha256Prefix(target) != hash) {
        why = L"could not copy the DLL to " + dir;
        return std::nullopt;
    }
    return target;
}

std::vector<std::wstring> conflicts(DWORD explorerPid)
{
    std::vector<std::wstring> found;
    const HANDLE processes = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry {sizeof entry};
        for (BOOL more = ::Process32FirstW(processes, &entry); more; more = ::Process32NextW(processes, &entry)) {
            const std::wstring name = lower(entry.szExeFile);
            for (const wchar_t* known : kProcesses) {
                if (name == known)
                    found.push_back(entry.szExeFile);
            }
        }
        ::CloseHandle(processes);
    }
    const HANDLE modules = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, explorerPid);
    if (modules != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry {sizeof entry};
        for (BOOL more = ::Module32FirstW(modules, &entry); more; more = ::Module32NextW(modules, &entry)) {
            const std::wstring name = lower(entry.szModule);
            for (const wchar_t* known : kModules) {
                if (name.find(known) != std::wstring::npos)
                    found.push_back(std::wstring(entry.szModule) + L" in explorer.exe");
            }
        }
        ::CloseHandle(modules);
    }
    return found;
}

// A SYNCHRONIZE handle to this process in explorer's handle table, for the
// TAP to wait on; 0 if that cannot be had (the TAP then opens one itself).
uint64_t handleForExplorer(DWORD explorerPid)
{
    const HANDLE explorer = ::OpenProcess(PROCESS_DUP_HANDLE, FALSE, explorerPid);
    if (!explorer)
        return 0;
    HANDLE theirs = nullptr;
    const BOOL ok = ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(), explorer, &theirs,
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0);
    ::CloseHandle(explorer);
    return ok ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(theirs)) : 0;
}

bool alive(DWORD pid)
{
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    DWORD code = 0;
    const bool running = ::GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    ::CloseHandle(process);
    return running;
}

// The section and the events of one TAP instance (TapProtocol.h).
struct Channel {
    HANDLE section = nullptr;
    wstap::Shared* shared = nullptr;
    HANDLE request = nullptr;
    HANDLE ack = nullptr;
    DWORD pid = 0;
    int endpoint = 0;

    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&& o) noexcept { *this = std::move(o); }
    Channel& operator=(Channel&& o) noexcept
    {
        std::swap(section, o.section);
        std::swap(shared, o.shared);
        std::swap(request, o.request);
        std::swap(ack, o.ack);
        pid = o.pid;
        endpoint = o.endpoint;
        return *this;
    }
    ~Channel()
    {
        if (shared)
            ::UnmapViewOfFile(shared);
        for (const HANDLE h : {section, request, ack}) {
            if (h)
                ::CloseHandle(h);
        }
    }

    static std::optional<Channel> open(DWORD explorerPid, int endpoint)
    {
        Channel c;
        c.pid = explorerPid;
        c.endpoint = endpoint;
        c.section = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wstap::sectionName(explorerPid, endpoint).c_str());
        if (!c.section)
            return std::nullopt;
        c.shared = static_cast<wstap::Shared*>(::MapViewOfFile(c.section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(wstap::Shared)));
        c.request = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, wstap::requestEventName(explorerPid, endpoint).c_str());
        c.ack = ::OpenEventW(SYNCHRONIZE, FALSE, wstap::ackEventName(explorerPid, endpoint).c_str());
        if (!c.shared || !c.request || !c.ack)
            return std::nullopt;
        return c;
    }

    bool compatible() const { return shared->magic == wstap::kMagic && shared->protocolMajor == wstap::kProtocolMajor; }
    wstap::State state() const { return static_cast<wstap::State>(shared->state); }

    // Writes a request; true once the TAP has answered it (the reply fields are then current).
    bool send(int32_t centiDips, uint64_t clientHandle)
    {
        shared->marginCentiDips = centiDips;
        shared->flags = 0;
        shared->clientPid = ::GetCurrentProcessId();
        shared->clientHandle = clientHandle;
        const uint32_t seq = ++shared->requestSeq;
        ::SetEvent(request);
        return ::WaitForSingleObject(ack, kAckMs) == WAIT_OBJECT_0 && shared->appliedSeq == seq;
    }
};

// Calls InitializeXamlDiagnosticsEx for endpoint `n` from a fresh thread: the
// call is once per thread (a second from the same one says S_OK and does
// nothing), as TranslucentTB found.
HRESULT attachOnce(InitializeFn initialize, DWORD pid, const std::wstring& tap, int endpoint)
{
    HRESULT hr = E_FAIL;
    std::thread([&] {
        wchar_t name[64];
        std::swprintf(name, 64, L"VisualDiagConnection%d", endpoint);
        wchar_t data[32];
        std::swprintf(data, 32, L"ep=%d", endpoint);
        hr = initialize(name, pid, nullptr, tap.c_str(), wstap::kTapClsid, data);
    }).join();
    return hr;
}

} // namespace

struct StartGap::Impl {
    enum class Op { Set, Reattach, NoGapSeen, Quit };
    struct Command {
        Op op;
        double dips = 0;
    };

    Changed changed;
    std::thread thread;

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Command> queue;
    Status status;

    // The thread's alone.
    double wanted = 0; // the latest set()
    std::optional<Channel> channel;
    HMODULE xaml = nullptr; // Windows.UI.Xaml.dll, kept

    void post(Command c)
    {
        {
            std::lock_guard lock(mutex);
            queue.push_back(c);
        }
        wake.notify_one();
    }

    void report(State state, std::wstring reason = {})
    {
        Status s;
        s.state = state;
        s.reason = std::move(reason);
        s.dips = channel && channel->shared ? channel->shared->appliedCentiDips / 100.0 : 0;
        Changed notify;
        {
            std::lock_guard lock(mutex);
            if (status.state == s.state && status.reason == s.reason && status.dips == s.dips)
                return;
            status = s;
            notify = changed; // none once the owner is going (the destructor)
        }
        if (notify)
            notify(s);
    }

    void run()
    {
        for (;;) {
            Command c;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return !queue.empty(); });
                c = queue.front();
                queue.pop_front();
            }
            switch (c.op) {
            case Op::Quit:
                return;
            case Op::Set:
                wanted = c.dips;
                if (wanted > 0 || channel)
                    apply();
                else
                    report(State::Off);
                break;
            case Op::Reattach:
                channel.reset();
                if (wanted > 0)
                    apply();
                else
                    report(State::Off);
                break;
            case Op::NoGapSeen:
                if (channel)
                    report(State::NoGapSeen);
                break;
            }
        }
    }

    // The margin wanted onto the Start button: a TAP found or attached first.
    void apply()
    {
        if (channel && !alive(channel->pid))
            channel.reset(); // that Explorer is gone; its taskbar will come again (reattach)
        if (!channel && !attach())
            return;
        const int32_t centi = static_cast<int32_t>(std::lround(wanted * 100));
        if (!channel->send(centi, handleForExplorer(channel->pid))) {
            if (!alive(channel->pid)) {
                channel.reset();
                report(State::Attaching, L"Explorer ended");
            } else {
                report(State::Failed, L"the TAP did not answer");
            }
            return;
        }
        const wstap::Shared& s = *channel->shared;
        if (FAILED(s.applyError)) {
            report(State::Failed, L"apply " + hex(s.applyError));
            return;
        }
        if (centi == 0) {
            report(State::Off);
            return;
        }
        // Its tree still being enumerated: the TAP applies by itself once it
        // sees the Start button; given a few seconds for that.
        const auto done = [&] { return s.startSeen && s.appliedCentiDips == centi; };
        const ULONGLONG until = ::GetTickCount64() + kAppliedMs;
        if (!done())
            report(State::Ready);
        while (!done() && ::GetTickCount64() < until)
            ::Sleep(200);
        report(done() ? State::Applied : State::Ready);
    }

    // A TAP of ours on the primary taskbar: one resident in Explorer, else one
    // attached now. Explorer may still be starting: tried for a while.
    bool attach()
    {
        report(State::Attaching);
        for (int attempt = 0; attempt < kAttachTries; ++attempt) {
            if (attempt > 0)
                ::Sleep(1000);
            const HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
            DWORD pid = 0;
            if (!tray || !::GetWindowThreadProcessId(tray, &pid) || !::IsWindowVisible(tray))
                continue;
            if (findResident(pid))
                return true;
            {
                std::lock_guard lock(mutex);
                if (status.state == State::Incompatible)
                    return false;
            }
            const HRESULT hr = inject(pid);
            if (SUCCEEDED(hr))
                return true;
            if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                continue; // no endpoint yet: the taskbar's XAML not up
            return false; // reported by inject
        }
        report(State::Failed, L"no taskbar");
        return false;
    }

    bool findResident(DWORD pid)
    {
        for (int n = wstap::kFirstEndpoint; n <= wstap::kLastEndpoint; ++n) {
            std::optional<Channel> c = Channel::open(pid, n);
            if (!c)
                continue;
            if (!c->compatible()) {
                wchar_t text[64];
                std::swprintf(text, 64, L"resident TAP protocol %u.%u, ours %u.%u", c->shared->protocolMajor,
                    c->shared->protocolMinor, wstap::kProtocolMajor, wstap::kProtocolMinor);
                report(State::Incompatible, text);
                return false;
            }
            if (c->state() == wstap::State::Primary) {
                trace(L"resident TAP in explorer %lu, endpoint %d", pid, n);
                channel = std::move(c);
                return true;
            }
        }
        return false;
    }

    // ERROR_NOT_FOUND: no endpoint to be had yet (try again later); any other
    // failure is reported here.
    HRESULT inject(DWORD pid)
    {
        std::wstring why;
        const std::optional<std::wstring> tap = stage(why);
        if (!tap) {
            report(State::Failed, why);
            return E_FAIL;
        }
        const std::vector<std::wstring> found = conflicts(pid);
        if (!found.empty()) {
            std::wstring reason;
            for (const std::wstring& f : found)
                reason += (reason.empty() ? L"" : L", ") + f;
            report(State::Conflict, reason);
            return E_ACCESSDENIED;
        }
        if (!xaml)
            xaml = ::LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto initialize = xaml ? reinterpret_cast<InitializeFn>(::GetProcAddress(xaml, "InitializeXamlDiagnosticsEx")) : nullptr;
        if (!initialize) {
            report(State::Failed, L"InitializeXamlDiagnosticsEx not found");
            return E_FAIL;
        }
        HRESULT last = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        for (int n = wstap::kFirstEndpoint; n <= wstap::kLastEndpoint; ++n) {
            if (Channel::open(pid, n))
                continue; // an instance of ours sits there already (another core)
            const HRESULT hr = attachOnce(initialize, pid, *tap, n);
            trace(L"InitializeXamlDiagnosticsEx(VisualDiagConnection%d, %lu): 0x%08X", n, pid, hr);
            if (FAILED(hr)) {
                last = hr;
                continue; // no such endpoint, or taken: the next
            }
            // Until the TAP has advised, or settled as the wrong core.
            const ULONGLONG until = ::GetTickCount64() + kSitedMs;
            std::optional<Channel> c;
            while (!c && ::GetTickCount64() < until) {
                c = Channel::open(pid, n);
                if (!c)
                    ::Sleep(100);
            }
            while (c && c->state() == wstap::State::Starting && ::GetTickCount64() < until)
                ::Sleep(100);
            if (!c) {
                // Said yes and did nothing: another consumer answering for it
                // (the Windhawk styler blocks that way), or the DLL not loaded.
                report(State::Conflict, L"Explorer's XAML diagnostics answered but the TAP never came");
                return E_ACCESSDENIED;
            }
            switch (c->state()) {
            case wstap::State::Primary:
                channel = std::move(c);
                return S_OK;
            case wstap::State::WrongCore:
                continue;
            case wstap::State::Starting:
            case wstap::State::Failed:
                report(State::Failed, L"TAP " + hex(c->shared->lastError));
                return E_FAIL;
            }
        }
        if (last == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            return last;
        report(State::Conflict, L"every endpoint refused, last " + hex(last));
        return last;
    }
};

StartGap::StartGap(Changed changed) : m_impl(std::make_shared<Impl>())
{
    m_impl->changed = std::move(changed);
    m_impl->thread = std::thread([impl = m_impl] { impl->run(); });
}

StartGap::~StartGap()
{
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->changed = nullptr; // no word to a caller that is going
    }
    m_impl->post({Impl::Op::Set, 0});
    m_impl->post({Impl::Op::Quit});
    if (::WaitForSingleObject(m_impl->thread.native_handle(), kAckMs + 1000) == WAIT_OBJECT_0)
        m_impl->thread.join();
    else
        m_impl->thread.detach(); // stuck in Explorer: left to end by itself
}

void StartGap::set(double dips) { m_impl->post({Impl::Op::Set, std::max(0.0, dips)}); }

void StartGap::reattach() { m_impl->post({Impl::Op::Reattach}); }

void StartGap::noGapSeen() { m_impl->post({Impl::Op::NoGapSeen}); }

StartGap::Status StartGap::status() const
{
    std::lock_guard lock(m_impl->mutex);
    return m_impl->status;
}

} // namespace ws::taskbar
