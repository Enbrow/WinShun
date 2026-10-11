#pragma once

// What a viewer may see. Win顺 runs as administrator, but the AI clients
// connect with the user's normal rights: the tools show only what that
// viewer could list, read or open itself. Every decision is an AccessCheck
// of the file's security descriptor against the viewer's token: the pipe
// client's own (McpServer takes it with ImpersonateNamedPipeClient), or,
// for a session without one, this process's linked token (the user's
// unelevated rights) — never Win顺's own elevated token.

#include <QHash>
#include <QString>

#include <windows.h>

#include <cstdint>

namespace ws::mcp {

class Visibility {
public:
    // `token`: an impersonation token at identification level or higher;
    // not owned. Null: defaultToken().
    explicit Visibility(HANDLE token);

    // The folder can be listed (FILE_LIST_DIRECTORY): the names in it are
    // the viewer's to see. Cached by folder.
    bool canList(const QString& folder);
    // The file's data (FILE_READ_DATA), its attributes (FILE_READ_ATTRIBUTES).
    bool canRead(const QString& file);
    bool canReadAttributes(const QString& path);
    // The entry is there for the viewer: its folder can be listed (a volume
    // root: itself).
    bool sees(const QString& path);

    int checks() const noexcept { return m_checks; }
    std::int64_t microseconds() const noexcept { return m_us; }

    // The token for sessions that bring none: the linked token of this
    // process (the user's normal rights, when it is elevated), else the
    // process's own as an impersonation token. Kept for the process's life;
    // null if neither can be had (then nothing is visible).
    static HANDLE defaultToken();
    // "C:\dir" of "C:\dir\file"; "C:\" of "C:\" and of "C:\dir".
    static QString parentOf(const QString& path);

private:
    bool check(const QString& path, DWORD desired);

    HANDLE m_token;
    QHash<QString, bool> m_folders; // case-folded, native
    int m_checks = 0;
    std::int64_t m_us = 0;
};

} // namespace ws::mcp
