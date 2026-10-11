#pragma once

// Win顺's end of the pipe (Pipe.h): serves any number of relays at once,
// each a connection with an id. Lines in, lines out. The callbacks run on
// the pipe's own threads: the server hops to its thread (McpServer).

#include <QByteArray>

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ws::pipe {

class Server {
public:
    using Id = std::uint64_t;
    struct Callbacks {
        std::function<void(Id id)> connected;
        // Before its first line: who the client is, as an impersonation token
        // of its own (at the level the client allowed: impersonation, else
        // identification), for access checks; the receiver owns it. Null
        // when it could not be had (an anonymous client).
        std::function<void(Id id, HANDLE token)> identified;
        std::function<void(Id id, QByteArray line)> line; // without its newline
        std::function<void(Id id)> disconnected; // once per connected()
    };

    Server(std::wstring name, Callbacks callbacks);
    ~Server(); // stop()
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Makes the first instance of the pipe and starts accepting. False (with
    // the Win32 code in `error`) when the name is taken already (another
    // program's pipe: ERROR_ACCESS_DENIED) or the security descriptor cannot
    // be made.
    bool start(DWORD* error = nullptr);
    void stop(); // ends every connection (disconnected() for each) and the accepting; idempotent
    bool running() const noexcept { return m_running; }
    const std::wstring& name() const noexcept { return m_name; }

    // Any thread. A line is queued and written by the connection's thread;
    // dropped when the connection is gone.
    void send(Id id, QByteArray line);
    void close(Id id);
    int connectionCount() const;

private:
    struct Connection;
    void acceptLoop();
    void serve(std::shared_ptr<Connection> connection);
    HANDLE createInstance(bool first, DWORD* error);

    std::wstring m_name;
    Callbacks m_callbacks;
    PSECURITY_DESCRIPTOR m_descriptor = nullptr;
    HANDLE m_stopEvent = nullptr; // manual-reset: stop() sets it
    HANDLE m_listening = INVALID_HANDLE_VALUE; // the instance waiting for a client
    std::thread m_acceptor;
    bool m_running = false;
    mutable std::mutex m_mutex; // guards the connections
    std::condition_variable m_finished; // a connection's thread ended
    std::map<Id, std::shared_ptr<Connection>> m_connections; // their threads run detached
    Id m_nextId = 1;
};

} // namespace ws::pipe
