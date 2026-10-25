#include "ShareLister.h"

#include "Unc.h"
#include "Win32Util.h"
#include "Wsl.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <lm.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>

namespace ws::share {

namespace {

// HRESULT-style codes some SMB failures come back as (see RtlNtStatusToDosError).
constexpr unsigned long kBadSignature = 0x8009'0006; // STATUS_INVALID_SIGNATURE
constexpr unsigned long kSignInRequired = 0xC05D'0003; // STATUS_SMB_GUEST_LOGON_BLOCKED_SIGNING_REQUIRED
constexpr unsigned long kGuestNoEncryption = 0xC05D'0004; // STATUS_SMB_GUEST_ENCRYPTION_NOT_SUPPORTED

constexpr int kInARow = 3; // problems with the share before a pass gives up

} // namespace

Problem classify(unsigned long error, bool atRoot) noexcept
{
    switch (error) {
    case NO_ERROR:
        return Problem::None;
    case ERROR_BAD_NETPATH:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_HOST_UNREACHABLE:
    case ERROR_PORT_UNREACHABLE:
    case ERROR_SEM_TIMEOUT:
    case ERROR_NO_NETWORK:
    case ERROR_REM_NOT_LIST:
    case ERROR_BAD_NET_RESP:
    case ERROR_NETWORK_BUSY:
    case ERROR_CONNECTION_REFUSED:
        return Problem::Offline;
    case ERROR_NETNAME_DELETED:
    case ERROR_UNEXP_NET_ERR:
    case ERROR_CONNECTION_ABORTED:
    case ERROR_CONNECTION_INVALID:
    case ERROR_GRACEFUL_DISCONNECT:
    case ERROR_VC_DISCONNECTED:
    case ERROR_DEV_NOT_EXIST:
        return Problem::Dropped;
    case ERROR_BAD_NET_NAME:
        return Problem::NotFound;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return atRoot ? Problem::NotFound : Problem::Other; // a folder deleted since its parent was read
    case ERROR_ACCESS_DENIED:
        return atRoot ? Problem::NeedsSignIn : Problem::Denied;
    case ERROR_LOGON_FAILURE:
    case ERROR_INVALID_PASSWORD:
    case ERROR_ACCOUNT_RESTRICTION:
    case ERROR_PASSWORD_EXPIRED:
    case ERROR_ACCOUNT_DISABLED:
    case ERROR_ACCOUNT_LOCKED_OUT:
    case ERROR_SESSION_CREDENTIAL_CONFLICT:
    case ERROR_NOT_AUTHENTICATED:
    case kSignInRequired:
        return Problem::NeedsSignIn;
    case ERROR_SMB_GUEST_LOGON_BLOCKED:
    case kGuestNoEncryption:
        return Problem::GuestBlocked;
    case kBadSignature:
        return Problem::Signing;
    case ERROR_SMB1_NOT_AVAILABLE:
        return Problem::Smb1Only;
    default:
        return Problem::Other;
    }
}

bool affectsShare(Problem problem) noexcept
{
    return problem != Problem::None && problem != Problem::Denied && problem != Problem::Other;
}

namespace {

using Clock = std::chrono::steady_clock;

// Waits until `s` can be written to (connected) or read from, up to `deadline`.
bool ready(SOCKET s, bool read, Clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
    if (left.count() <= 0)
        return false;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    fd_set failed = set;
    timeval wait {static_cast<long>(left.count() / 1'000'000), static_cast<long>(left.count() % 1'000'000)};
    return ::select(0, read ? &set : nullptr, read ? nullptr : &set, &failed, &wait) > 0 && FD_ISSET(s, &set);
}

// The opening request of a connection (NEGOTIATE), behind its NetBIOS
// session header. What Windows sends first is SMB 1 framing that offers
// SMB 1 ("NT LM 0.12") and SMB 2 ("SMB 2.002", "SMB 2.???"): a server that
// speaks SMB 2 answers in SMB 2, an old one in SMB 1. `smb2` is an SMB 2
// NEGOTIATE instead (dialects 2.0.2 to 3.0.2), for servers that close on
// SMB 1 framing.
std::vector<unsigned char> negotiateRequest(bool smb2)
{
    std::vector<unsigned char> m(4); // NetBIOS: type 0, length (24 bits, big-endian) set below
    if (smb2) {
        m.resize(4 + 64 + 36 + 8);
        unsigned char* header = m.data() + 4;
        header[0] = 0xFE; // "\xFESMB"
        header[1] = 'S';
        header[2] = 'M';
        header[3] = 'B';
        header[4] = 64; // StructureSize; Command 0 = NEGOTIATE
        header[14] = 1; // CreditRequest
        unsigned char* negotiate = header + 64;
        negotiate[0] = 36; // StructureSize
        negotiate[2] = 4; // DialectCount
        negotiate[4] = 1; // SecurityMode: signing enabled
        const std::uint16_t dialects[] = {0x0202, 0x0210, 0x0300, 0x0302};
        for (int i = 0; i < 4; ++i) {
            negotiate[36 + 2 * i] = static_cast<unsigned char>(dialects[i] & 0xFF);
            negotiate[37 + 2 * i] = static_cast<unsigned char>(dialects[i] >> 8);
        }
    } else {
        m.resize(4 + 32 + 3);
        unsigned char* header = m.data() + 4;
        header[0] = 0xFF; // "\xFFSMB"
        header[1] = 'S';
        header[2] = 'M';
        header[3] = 'B';
        header[4] = 0x72; // Command: NEGOTIATE
        header[9] = 0x18; // Flags: case-insensitive, canonical paths
        header[10] = 0x01; // Flags2 0xC801: long names, extended security, NT status, Unicode
        header[11] = 0xC8;
        // WordCount 0, then ByteCount and the dialects, each 0x02 "name" NUL.
        for (const std::string_view dialect : {"NT LM 0.12", "SMB 2.002", "SMB 2.???"}) {
            m.push_back(0x02);
            m.insert(m.end(), dialect.begin(), dialect.end());
            m.push_back(0);
        }
        const std::size_t bytes = m.size() - (4 + 32 + 3);
        m[4 + 32 + 1] = static_cast<unsigned char>(bytes & 0xFF);
        m[4 + 32 + 2] = static_cast<unsigned char>(bytes >> 8);
    }
    const std::size_t length = m.size() - 4;
    m[1] = static_cast<unsigned char>(length >> 16);
    m[2] = static_cast<unsigned char>(length >> 8);
    m[3] = static_cast<unsigned char>(length);
    return m;
}

enum class Reply { None, Closed, Smb2, Smb1 };

// What the server on the connected socket `s` replies to negotiateRequest().
// A connection alone proves nothing: proxies in TUN mode (Clash and the
// like) and some VPNs accept every connection themselves and only then find
// the server is not there.
Reply negotiate(SOCKET s, bool smb2, Clock::time_point deadline)
{
    const std::vector<unsigned char> request = negotiateRequest(smb2);
    int sent = 0;
    while (sent < static_cast<int>(request.size())) {
        if (!ready(s, false, deadline))
            return Reply::None;
        const int n = ::send(s, reinterpret_cast<const char*>(request.data()) + sent,
            static_cast<int>(request.size()) - sent, 0);
        if (n <= 0 && ::WSAGetLastError() != WSAEWOULDBLOCK)
            return Reply::Closed;
        sent += std::max(n, 0);
    }
    unsigned char reply[8];
    int got = 0;
    while (got < static_cast<int>(sizeof reply)) {
        if (!ready(s, true, deadline))
            return Reply::None;
        const int n = ::recv(s, reinterpret_cast<char*>(reply) + got, static_cast<int>(sizeof reply) - got, 0);
        if (n == 0 || (n < 0 && ::WSAGetLastError() != WSAEWOULDBLOCK))
            return Reply::Closed;
        got += std::max(n, 0);
    }
    if (reply[5] != 'S' || reply[6] != 'M' || reply[7] != 'B')
        return Reply::None;
    return reply[4] == 0xFE ? Reply::Smb2 : reply[4] == 0xFF ? Reply::Smb1 : Reply::None;
}

} // namespace

Answer probe(std::wstring_view host, std::chrono::milliseconds timeout, bool fresh)
{
    struct Known {
        Answer answer;
        Clock::time_point at;
    };
    static std::mutex mutex;
    static std::map<std::wstring, Known, std::less<>> known;
    std::wstring key(host);
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    if (!fresh) {
        std::lock_guard lock(mutex);
        if (const auto it = known.find(key); it != known.end()) {
            const auto age = Clock::now() - it->second.at;
            if (age < (it->second.answer != Answer::None ? std::chrono::seconds(30) : std::chrono::seconds(10)))
                return it->second.answer;
        }
    }

    static const bool started = [] {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    Answer answer = Answer::None;
    ADDRINFOW hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* found = nullptr;
    if (started && ::GetAddrInfoW(key.c_str(), L"445", &hints, &found) == 0) {
        const auto deadline = Clock::now() + timeout;
        // Each address in turn, as long as time is left (a name may resolve
        // to an IPv6 address the server does not listen on).
        for (const ADDRINFOW* a = found; a && answer == Answer::None && Clock::now() < deadline; a = a->ai_next) {
            // As Windows asks first; a server that hangs up on that, in SMB 2.
            for (const bool smb2 : {false, true}) {
                const SOCKET s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                if (s == INVALID_SOCKET)
                    break;
                u_long nonBlocking = 1;
                ::ioctlsocket(s, FIONBIO, &nonBlocking);
                const bool connected = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0
                    || (::WSAGetLastError() == WSAEWOULDBLOCK && ready(s, false, deadline));
                const Reply reply = connected ? negotiate(s, smb2, deadline) : Reply::None;
                ::closesocket(s);
                if (reply == Reply::Smb2)
                    answer = Answer::Smb2;
                else if (reply == Reply::Smb1)
                    answer = Answer::Smb1Only;
                if (reply != Reply::Closed)
                    break;
            }
        }
        ::FreeAddrInfoW(found);
    }
    std::lock_guard lock(mutex);
    known[key] = {answer, Clock::now()};
    return answer;
}

std::vector<std::wstring> listShares(std::wstring_view host, unsigned long* error)
{
    std::vector<std::wstring> shares;
    const std::wstring server(host);
    DWORD_PTR resume = 0;
    NET_API_STATUS status = 0;
    do {
        LPBYTE buffer = nullptr;
        DWORD read = 0;
        DWORD total = 0;
        status = ::NetShareEnum(const_cast<LPWSTR>(server.c_str()), 1, &buffer, MAX_PREFERRED_LENGTH, &read, &total,
            reinterpret_cast<LPDWORD>(&resume));
        if (status == NERR_Success || status == ERROR_MORE_DATA) {
            const auto* info = reinterpret_cast<const SHARE_INFO_1*>(buffer);
            for (DWORD i = 0; i < read; ++i) {
                const std::wstring_view name(info[i].shi1_netname);
                if ((info[i].shi1_type & STYPE_MASK) == STYPE_DISKTREE && !(info[i].shi1_type & STYPE_SPECIAL)
                    && !name.ends_with(L'$'))
                    shares.emplace_back(name);
            }
        }
        if (buffer)
            ::NetApiBufferFree(buffer);
    } while (status == ERROR_MORE_DATA);
    if (status != NERR_Success && classify(status, true) != Problem::NeedsSignIn) {
        // NetShareEnum calls a guest refused "not found" (53); the server's
        // IPC$ tells why, as opening a share would (1272, 1326, ...).
        const std::wstring ipc = L"\\\\" + server + L"\\IPC$";
        if (::GetFileAttributesW(ipc.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const unsigned long why = ::GetLastError();
            const Problem p = classify(why, true);
            if (p != Problem::Offline && p != Problem::Dropped && p != Problem::Other)
                status = why;
            else if (probe(host) == Answer::Smb1Only)
                status = ERROR_SMB1_NOT_AVAILABLE;
        }
    }
    if (error)
        *error = status == NERR_Success ? 0 : status;
    std::sort(shares.begin(), shares.end(),
        [](const std::wstring& a, const std::wstring& b) { return ::lstrcmpiW(a.c_str(), b.c_str()) < 0; });
    return shares;
}

Lister::Lister(const Crawler& crawler, std::wstring root, Options options, std::stop_source stopPass)
    : m_crawler(crawler)
    , m_root(std::move(root))
    , m_options(std::move(options))
    , m_stopPass(std::move(stopPass))
{
}

Problem Lister::problem() const noexcept
{
    return classify(m_firstError.load(), m_rootFailed.load());
}

bool Lister::excludedFile(std::wstring_view name) const noexcept
{
    for (const std::wstring& pattern : m_options.excludedFiles) {
        if (pattern.ends_with(L'*')) {
            const std::wstring_view prefix = std::wstring_view(pattern).substr(0, pattern.size() - 1);
            if (name.size() >= prefix.size() && win32::equalsIgnoreCase(name.substr(0, prefix.size()), prefix))
                return true;
        } else if (win32::equalsIgnoreCase(name, pattern)) {
            return true;
        }
    }
    return false;
}

bool Lister::fail(unsigned long error, bool atRoot)
{
    const Problem p = classify(error, atRoot);
    if (p == Problem::Denied) {
        m_denied.fetch_add(1, std::memory_order_relaxed); // as in Explorer: it is not there for this user
        return false;
    }
    if (!affectsShare(p))
        return false; // that folder stays as it was in the index
    unsigned long none = 0;
    if (m_firstError.compare_exchange_strong(none, error))
        m_rootFailed = atRoot;
    if (atRoot || m_inARow.fetch_add(1) + 1 >= kInARow) {
        m_stopped = true;
        m_stopPass.request_stop();
    }
    return false;
}

bool reachablePath(std::wstring_view path, bool fresh)
{
    if (!unc::wslDistribution(path).empty())
        return wsl::probe(path, fresh) == wsl::State::Running;
    return !unc::isUnc(path) || reachable(unc::host(path), std::chrono::milliseconds(1500), fresh);
}

bool Lister::listWslDirectory(const Crawler::Root& dir, DirListing& out)
{
    // The 9P provider need not implement the SMB directory information classes.
    // FindFirstFile also supplies reparse tags, so Linux symlink loops are not followed.
    const bool atRoot = unc::equal(dir.path, m_root);
    WIN32_FIND_DATAW data {};
    const win32::UniqueFind find(::FindFirstFileExW((win32::longPath(dir.path) + L"\\*").c_str(),
        FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0));
    if (!find.valid()) {
        const auto error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || fail(error, atRoot);
    }
    do {
        if (m_stopPass.stop_requested())
            return false;
        const std::wstring_view name(data.cFileName);
        const bool isDir = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        if (name != L"." && name != L".." && (isDir || !excludedFile(name))) {
            const bool systemMount = unc::equal(dir.path, unc::shareRoot(dir.path))
                && (name == L"proc" || name == L"sys" || name == L"dev" || name == L"run" || name == L"mnt");
            m_crawler.addToListing(out, dir, {}, name, Crawler::attributeFlags(data.dwFileAttributes),
                !systemMount && Crawler::shouldDescend(data.dwFileAttributes, data.dwReserved0), 0,
                fileTimeOf(win32::ticks(data.ftLastWriteTime)),
                packSize((std::uint64_t {data.nFileSizeHigh} << 32) | data.nFileSizeLow));
        }
    } while (::FindNextFileW(find.get(), &data));
    if (const auto error = ::GetLastError(); error != ERROR_NO_MORE_FILES)
        return fail(error, atRoot);
    m_inARow = 0;
    m_items.fetch_add(out.items.size(), std::memory_order_relaxed);
    if (m_options.progress)
        m_options.progress->fetch_add(out.items.size(), std::memory_order_relaxed);
    return true;
}

bool Lister::operator()(const Crawler::Root& dir, DirListing& out)
{
    out.clear();
    if (m_stopPass.stop_requested())
        return false; // no new wait on the server once the pass is stopped (WinShun quitting)
    if (!unc::wslDistribution(m_root).empty())
        return listWslDirectory(dir, out);
    const bool atRoot = win32::equalsIgnoreCase(dir.path, m_root);
    std::wstring path = win32::longPath(dir.path);
    if (atRoot)
        path.push_back(L'\\'); // "\\?\UNC\host\share\": the share itself
    const win32::UniqueHandle h(::CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!h.valid())
        return fail(::GetLastError(), atRoot);
    out.readBuffer.resize((m_options.bufferBytes + 7) / sizeof(std::uint64_t));
    const auto bytes = static_cast<DWORD>(out.readBuffer.size() * sizeof(std::uint64_t));

    // Without short (8.3) names: a Samba server makes one up for every name
    // asked for. No file ids either: they mean nothing on a share.
    FILE_INFO_BY_HANDLE_CLASS request = m_options.shortNames ? FileIdBothDirectoryRestartInfo : FileFullDirectoryRestartInfo;
    for (;;) {
        if (m_stopPass.stop_requested())
            return false;
        if (!::GetFileInformationByHandleEx(h.get(), request, out.readBuffer.data(), bytes)) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_NO_MORE_FILES)
                return fail(error, atRoot);
            break;
        }
        request = m_options.shortNames ? FileIdBothDirectoryInfo : FileFullDirectoryInfo;
        const auto* p = reinterpret_cast<const std::byte*>(out.readBuffer.data());
        for (;;) {
            // Both start alike; only the name sits elsewhere.
            const auto* info = reinterpret_cast<const FILE_FULL_DIR_INFO*>(p);
            const std::wstring_view name = m_options.shortNames
                ? std::wstring_view(reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(p)->FileName,
                      info->FileNameLength / sizeof(wchar_t))
                : std::wstring_view(info->FileName, info->FileNameLength / sizeof(wchar_t));
            const bool isDir = info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            if (name != L"." && name != L".." && (isDir || !excludedFile(name))) {
                // For reparse points, EaSize holds the reparse tag.
                m_crawler.addToListing(out, dir, {}, name, Crawler::attributeFlags(info->FileAttributes),
                    Crawler::shouldDescend(info->FileAttributes, info->EaSize), 0,
                    fileTimeOf(info->LastWriteTime.QuadPart), packSize(static_cast<std::uint64_t>(info->EndOfFile.QuadPart)));
            }
            if (info->NextEntryOffset == 0)
                break;
            p += info->NextEntryOffset;
        }
    }
    m_inARow = 0;
    m_items.fetch_add(out.items.size(), std::memory_order_relaxed);
    if (m_options.progress)
        m_options.progress->fetch_add(out.items.size(), std::memory_order_relaxed);
    return true;
}

} // namespace ws::share
