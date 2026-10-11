#pragma once

// The named pipe between WinShunMcp.exe (the relay the AI clients start, with
// the user's normal rights) and Win顺 (elevated). Messages are lines of
// JSON-RPC (JsonRpc.h). Win32 and Qt Core only, for the relay's sake.
//
// The pipe is the user's own: its name carries the user's SID (pipe names are
// machine-wide, so two users each get their own), its DACL lets only that
// user, administrators and the system open it, and its label is medium
// integrity, so that a process of medium integrity can connect to an object
// made by an elevated one, while low-integrity processes (browser sandboxes)
// cannot.

#include <QByteArray>

#include <windows.h>

#include <mutex>
#include <string>
#include <string_view>

namespace ws::pipe {

// "S-1-5-21-…" of the user this process runs as; empty if it cannot be read.
std::wstring userSid();
// "WinShun.Mcp.S-1-5-21-…": the name Win顺 serves on and the relay opens.
std::wstring defaultName();
std::wstring fullPath(std::wstring_view name); // "\\.\pipe\" + name

// The security descriptor described above; null if it cannot be made. Free
// with ::LocalFree.
PSECURITY_DESCRIPTOR createSecurityDescriptor();

// One end of the pipe, from the client side: connects to the server, writes
// lines, reads lines. Reads block (one thread reads); writes may come from
// any thread, while a read waits; close() ends a blocked read.
class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Opens `name` (without the \\.\pipe\ prefix), waiting at most `timeoutMs`
    // for a free instance when the server is busy. False when there is no
    // server (ERROR_FILE_NOT_FOUND) or it cannot be opened; `error` has the
    // Win32 code.
    bool connect(std::wstring_view name, DWORD timeoutMs, DWORD* error = nullptr);
    bool connected() const noexcept;
    void close();

    // A line with its newline; false once the pipe is closed or broken.
    bool write(const QByteArray& line);
    // The next line (without its newline); false at the end.
    bool readLine(QByteArray& line);

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE; // overlapped
    HANDLE m_readEvent; // one read at a time, one write at a time
    HANDLE m_writeEvent;
    std::mutex m_writeMutex;
    QByteArray m_buffer; // read, not yet returned
};

inline constexpr std::size_t kMaxLineBytes = 16u << 20; // longer lines end the connection

} // namespace ws::pipe
