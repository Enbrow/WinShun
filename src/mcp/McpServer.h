#pragma once

// The MCP server: sessions over the pipe (one per relay, so one per AI
// client), the protocol's lifecycle and the tools/* methods, answered with
// the ToolHost's tools. Lives on one thread with an event loop (the GUI
// thread in Win顺); the pipe's threads hand it lines through the queue.

#include "McpTools.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace ws::pipe {
class Server;
}

namespace ws::mcp {

// The protocol revisions answered. The client's is echoed when it is one of
// these; else the newest.
inline constexpr const char* kProtocolVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
inline constexpr const char* kServerName = "winshun";

class McpServer : public QObject {
    Q_OBJECT

public:
    using SessionId = std::uint64_t;
    struct SessionInfo {
        SessionId id = 0;
        QString client; // clientInfo.name, once initialized
        QString clientVersion;
        QDateTime since;
        int calls = 0;
        QString lastTool;
        QDateTime lastCall;
        bool identified = false; // it brought a token of its own (pipe sessions); else the default one is used
    };

    McpServer(ToolHost* host, QString version, QObject* parent = nullptr);
    ~McpServer() override;

    // The pipe: start serving on `name` (Pipe.h); false with the Win32 code
    // when it cannot be made.
    bool listen(const std::wstring& name, DWORD* error = nullptr);
    void stop(); // ends the sessions and the pipe
    bool listening() const noexcept { return m_pipe != nullptr; }

    // Sessions without a pipe (tests): what the server sends goes to `sink`.
    SessionId openSession(std::function<void(const QByteArray& line)> sink);
    void closeSession(SessionId id);
    void receive(SessionId id, const QByteArray& line); // one message

    std::vector<SessionInfo> sessions() const; // the initialized ones first
    int sessionCount() const { return static_cast<int>(m_sessions.size()); }

signals:
    void sessionsChanged(); // one came, went, or called a tool

private:
    struct Session;
    void handle(Session& session, const QByteArray& line);
    void handleRequest(Session& session, const QJsonValue& id, const QString& method, const QJsonObject& params);
    void handleNotification(Session& session, const QString& method, const QJsonObject& params);
    void callTool(Session& session, const QJsonValue& id, const QJsonObject& params);
    void endSession(SessionId id);
    void toolsChanged();
    static QJsonObject toolResult(const ToolResult& result);

    ToolHost* m_host;
    QString m_version;
    std::unique_ptr<pipe::Server> m_pipe;
    std::map<SessionId, std::unique_ptr<Session>> m_sessions;
    SessionId m_nextLocalId = SessionId {1} << 40; // in-process sessions, apart from the pipe's ids
    std::uint64_t m_nextCall = 1;
};

} // namespace ws::mcp
