#include "Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace ws::service::log {

namespace {

constexpr std::size_t kMaxBytes = 1u << 20;
std::mutex g_mutex; // lines come from every connection's thread
bool g_console = false;

std::wstring folder()
{
    wchar_t data[MAX_PATH] {};
    const DWORD n = ::GetEnvironmentVariableW(L"ProgramData", data, MAX_PATH);
    return (n > 0 && n < MAX_PATH ? std::wstring(data, n) : std::wstring(L"C:\\ProgramData")) + L"\\WinShun";
}

std::string utf8(const std::wstring& s)
{
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0)
        ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring stamp()
{
    SYSTEMTIME t {};
    ::GetLocalTime(&t);
    wchar_t out[32];
    ::swprintf_s(out, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
        t.wMilliseconds);
    return out;
}

} // namespace

void toConsole()
{
    g_console = true;
}

void write(const wchar_t* format, ...)
{
    std::vector<wchar_t> text(1024);
    va_list args;
    va_start(args, format);
    ::_vsnwprintf_s(text.data(), text.size(), _TRUNCATE, format, args);
    va_end(args);
    const std::string line = utf8(stamp() + L" " + std::wstring(text.data()) + L"\n");

    const std::lock_guard lock(g_mutex);
    if (g_console) {
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
        return;
    }
    const std::wstring dir = folder();
    ::CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\service.log";
    WIN32_FILE_ATTRIBUTE_DATA info {};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)
        && ((std::uint64_t {info.nFileSizeHigh} << 32) | info.nFileSizeLow) + line.size() > kMaxBytes) {
        const std::wstring old = dir + L"\\service.old.log";
        ::DeleteFileW(old.c_str());
        if (!::MoveFileW(path.c_str(), old.c_str()))
            return; // a reader may deny rename: never keep appending beyond the cap
    }
    const HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    ::WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    ::CloseHandle(file);
}

void toEventLog(const std::wstring& message)
{
    const HANDLE source = ::RegisterEventSourceW(nullptr, L"WinShun");
    if (!source)
        return;
    const wchar_t* strings[] {message.c_str()};
    ::ReportEventW(source, EVENTLOG_ERROR_TYPE, 0, 1, nullptr, 1, 0, strings, nullptr);
    ::DeregisterEventSource(source);
}

} // namespace ws::service::log
