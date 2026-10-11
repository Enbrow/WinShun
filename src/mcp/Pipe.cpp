#include "Pipe.h"

#include <sddl.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ws::pipe {

std::wstring userSid()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    wchar_t* text = nullptr;
    const bool ok = size != 0 && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size)
        && ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text);
    ::CloseHandle(token);
    if (!ok)
        return {};
    std::wstring sid = text;
    ::LocalFree(text);
    return sid;
}

std::wstring defaultName()
{
    return L"WinShun.Mcp." + userSid();
}

std::wstring fullPath(std::wstring_view name)
{
    return L"\\\\.\\pipe\\" + std::wstring(name);
}

PSECURITY_DESCRIPTOR createSecurityDescriptor()
{
    const std::wstring sid = userSid();
    if (sid.empty())
        return nullptr;
    // Everything for the system and administrators (Win顺 itself); reading
    // and writing for the user. Protected: nothing inherited. The label lets
    // medium-integrity processes in (made by an elevated process, the pipe
    // would be high, and they could not open it), and keeps low ones out.
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;FRFW;;;" + sid + L")S:(ML;;NW;;;ME)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return nullptr;
    return descriptor;
}

Client::Client()
    : m_readEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
    , m_writeEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
}

Client::~Client()
{
    close();
    ::CloseHandle(m_readEvent);
    ::CloseHandle(m_writeEvent);
}

bool Client::connect(std::wstring_view name, DWORD timeoutMs, DWORD* error)
{
    close();
    const std::wstring path = fullPath(name);
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    for (;;) {
        // Overlapped: a read waiting on one thread must not hold up a write
        // on another (synchronous operations on one handle take turns).
        // SECURITY_IMPERSONATION: the server may act as us on this computer
        // (it checks what we may see with our token), not beyond it.
        const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
            ::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);
            m_handle = handle;
            m_buffer.clear();
            return true;
        }
        const DWORD code = ::GetLastError();
        if (code != ERROR_PIPE_BUSY) {
            if (error)
                *error = code;
            return false;
        }
        // Every instance is taken: wait for one to free up.
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline || !::WaitNamedPipeW(path.c_str(), static_cast<DWORD>(deadline - now))) {
            if (error)
                *error = ERROR_PIPE_BUSY;
            return false;
        }
    }
}

bool Client::connected() const noexcept
{
    return m_handle != INVALID_HANDLE_VALUE;
}

void Client::close()
{
    if (m_handle == INVALID_HANDLE_VALUE)
        return;
    const HANDLE handle = m_handle;
    m_handle = INVALID_HANDLE_VALUE;
    ::CancelIoEx(handle, nullptr); // a read waiting on another thread returns
    ::CloseHandle(handle);
}

bool Client::write(const QByteArray& line)
{
    const std::lock_guard lock(m_writeMutex);
    const HANDLE handle = m_handle;
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const char* p = line.constData();
    std::size_t n = static_cast<std::size_t>(line.size());
    while (n > 0) {
        OVERLAPPED overlapped {};
        overlapped.hEvent = m_writeEvent;
        ::ResetEvent(m_writeEvent);
        DWORD put = 0;
        if (!::WriteFile(handle, p, static_cast<DWORD>(std::min<std::size_t>(n, 1u << 20)), &put, &overlapped)) {
            if (::GetLastError() != ERROR_IO_PENDING || !::GetOverlappedResult(handle, &overlapped, &put, TRUE))
                return false;
        }
        if (put == 0)
            return false;
        p += put;
        n -= put;
    }
    return true;
}

bool Client::readLine(QByteArray& line)
{
    for (;;) {
        if (const qsizetype at = m_buffer.indexOf('\n'); at >= 0) {
            line = m_buffer.left(at);
            if (line.endsWith('\r'))
                line.chop(1);
            m_buffer.remove(0, at + 1);
            return true;
        }
        const HANDLE handle = m_handle;
        if (handle == INVALID_HANDLE_VALUE || static_cast<std::size_t>(m_buffer.size()) > kMaxLineBytes)
            return false;
        char chunk[64 * 1024];
        OVERLAPPED overlapped {};
        overlapped.hEvent = m_readEvent;
        ::ResetEvent(m_readEvent);
        DWORD got = 0;
        if (!::ReadFile(handle, chunk, sizeof chunk, &got, &overlapped)) {
            if (::GetLastError() != ERROR_IO_PENDING || !::GetOverlappedResult(handle, &overlapped, &got, TRUE))
                return false;
        }
        if (got == 0)
            return false;
        m_buffer.append(chunk, static_cast<qsizetype>(got));
    }
}

} // namespace ws::pipe
