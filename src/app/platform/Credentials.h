#pragma once

#include <windows.h>

#include <string>
#include <string_view>

// Signing in to a server on the network, for its shares.
//
// Win顺 runs elevated: its network connections are not File Explorer's, and
// a password typed in Explorer without "remember" does not reach it. What
// Windows keeps in the Credential Manager does, both ways: a password saved
// here serves Explorer too. Win顺 keeps no password itself.
namespace ws::credentials {

struct Entered {
    std::wstring user;
    std::wstring password;
    bool save = false; // "remember": into the Credential Manager once it works

    Entered() = default;
    Entered(const Entered&) = delete;
    Entered& operator=(const Entered&) = delete;
    Entered(Entered&&) = default;
    Entered& operator=(Entered&&) = default;
    ~Entered(); // wipes the password
};

// Windows' own dialog for a user name and password, owned by `owner`.
// `error` is why it is asked again (ERROR_LOGON_FAILURE: says the password
// was wrong), `user` the name to start with. False when cancelled.
bool prompt(HWND owner, const std::wstring& caption, const std::wstring& message, unsigned long error,
    std::wstring_view user, Entered& entered);

// Connects to `path` ("\\host\share", or "\\host" for the server itself) as
// `entered` says; a connection to it under another name is dropped first.
// Once it works and `entered.save`, the Credential Manager keeps it for the
// host. Blocks for as long as the server takes: any thread. Returns a Win32
// error, 0 when connected.
unsigned long connect(std::wstring_view path, const Entered& entered);

} // namespace ws::credentials
