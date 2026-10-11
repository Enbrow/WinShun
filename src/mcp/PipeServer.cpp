#include "PipeServer.h"

#include "LineChannel.h"
#include "Pipe.h"

#include <QDebug>

#include <algorithm>
#include <condition_variable>

namespace ws::pipe {

namespace {

// The client's token as an impersonation token of its own, at the level the
// client allowed: SECURITY_IMPERSONATION, else SECURITY_IDENTIFICATION
// (which access checks are content with). Null for an anonymous client.
HANDLE clientToken(HANDLE pipe)
{
    if (!::ImpersonateNamedPipeClient(pipe))
        return nullptr;
    HANDLE thread = nullptr;
    const bool opened = ::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE, TRUE, &thread);
    ::RevertToSelf();
    if (!opened)
        return nullptr;
    HANDLE token = nullptr;
    constexpr DWORD kAccess = TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE;
    if (!::DuplicateTokenEx(thread, kAccess, nullptr, SecurityImpersonation, TokenImpersonation, &token)
        && !::DuplicateTokenEx(thread, kAccess, nullptr, SecurityIdentification, TokenImpersonation, &token))
        token = nullptr;
    ::CloseHandle(thread);
    return token;
}

} // namespace

struct Server::Connection {
    Id id = 0;
    std::unique_ptr<LineChannel> channel; // owns the handle
};

Server::Server(std::wstring name, Callbacks callbacks)
    : m_name(std::move(name))
    , m_callbacks(std::move(callbacks))
{
}

Server::~Server()
{
    stop();
    if (m_descriptor)
        ::LocalFree(m_descriptor);
    if (m_stopEvent)
        ::CloseHandle(m_stopEvent);
}

HANDLE Server::createInstance(bool first, DWORD* error)
{
    SECURITY_ATTRIBUTES attributes {sizeof attributes, m_descriptor, FALSE};
    DWORD flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (first)
        flags |= FILE_FLAG_FIRST_PIPE_INSTANCE; // not another program's pipe of this name
    const HANDLE handle = ::CreateNamedPipeW(fullPath(m_name).c_str(), flags,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
        64 * 1024, 64 * 1024, 0, &attributes);
    if (handle == INVALID_HANDLE_VALUE && error)
        *error = ::GetLastError();
    return handle;
}

bool Server::start(DWORD* error)
{
    if (m_running)
        return true;
    if (!m_descriptor)
        m_descriptor = createSecurityDescriptor();
    if (!m_descriptor) {
        if (error)
            *error = ERROR_INVALID_SECURITY_DESCR;
        return false;
    }
    m_listening = createInstance(true, error);
    if (m_listening == INVALID_HANDLE_VALUE)
        return false;
    if (!m_stopEvent)
        m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::ResetEvent(m_stopEvent);
    m_running = true;
    m_acceptor = std::thread([this] { acceptLoop(); });
    return true;
}

void Server::stop()
{
    if (!m_running)
        return;
    m_running = false;
    ::SetEvent(m_stopEvent);
    if (m_acceptor.joinable())
        m_acceptor.join();
    if (m_listening != INVALID_HANDLE_VALUE) {
        ::CloseHandle(m_listening);
        m_listening = INVALID_HANDLE_VALUE;
    }
    // The connections' threads end themselves (and take themselves out of
    // the map); wait until the last has.
    std::unique_lock lock(m_mutex);
    for (auto& [id, connection] : m_connections)
        connection->channel->stop();
    m_finished.wait(lock, [this] { return m_connections.empty(); });
}

void Server::acceptLoop()
{
    OVERLAPPED overlapped {};
    overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (m_running) {
        ::ResetEvent(overlapped.hEvent);
        bool connected = ::ConnectNamedPipe(m_listening, &overlapped) != 0;
        if (!connected) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_PIPE_CONNECTED) {
                connected = true; // the client came first
            } else if (code == ERROR_IO_PENDING) {
                const HANDLE events[] = {m_stopEvent, overlapped.hEvent};
                if (::WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
                    ::CancelIoEx(m_listening, &overlapped);
                    DWORD unused = 0;
                    ::GetOverlappedResult(m_listening, &overlapped, &unused, TRUE);
                    break;
                }
                DWORD unused = 0;
                connected = ::GetOverlappedResult(m_listening, &overlapped, &unused, TRUE) != 0;
            } else {
                qWarning("WinShun MCP: ConnectNamedPipe failed (%lu)", code);
                if (::WaitForSingleObject(m_stopEvent, 500) == WAIT_OBJECT_0)
                    break;
            }
        }
        if (!m_running)
            break;
        if (!connected)
            continue;

        auto connection = std::make_shared<Connection>();
        {
            const std::lock_guard lock(m_mutex);
            connection->id = m_nextId++;
            m_connections.emplace(connection->id, connection);
        }
        const Id id = connection->id;
        const HANDLE handle = m_listening;
        LineChannel::Callbacks callbacks;
        callbacks.firstData = [this, id, handle] {
            // Only once the client has written can it be impersonated.
            HANDLE token = clientToken(handle);
            if (m_callbacks.identified)
                m_callbacks.identified(id, token);
            else if (token)
                ::CloseHandle(token);
        };
        callbacks.line = [this, id](QByteArray line) {
            if (m_callbacks.line)
                m_callbacks.line(id, std::move(line));
        };
        connection->channel = std::make_unique<LineChannel>(handle, std::move(callbacks));
        std::thread([this, connection] { serve(connection); }).detach();

        // The next instance, for the next client. A failure (handles run
        // out?) is tried again after a moment.
        DWORD error = 0;
        m_listening = createInstance(false, &error);
        while (m_listening == INVALID_HANDLE_VALUE && m_running) {
            qWarning("WinShun MCP: cannot make another pipe instance (%lu)", error);
            if (::WaitForSingleObject(m_stopEvent, 1000) == WAIT_OBJECT_0)
                break;
            m_listening = createInstance(false, &error);
        }
        if (m_listening == INVALID_HANDLE_VALUE)
            break;
    }
    ::CloseHandle(overlapped.hEvent);
}

void Server::serve(std::shared_ptr<Connection> connection)
{
    const Id id = connection->id;
    if (m_callbacks.connected)
        m_callbacks.connected(id);

    // Lines both ways (LineChannel) until the client goes, a line never
    // ends, close() or stop().
    connection->channel->run();
    ::DisconnectNamedPipe(connection->channel->handle());
    if (m_callbacks.disconnected)
        m_callbacks.disconnected(id);
    // Under the lock: stop() may be waiting, and the server may go as soon as
    // it returns.
    const std::lock_guard lock(m_mutex);
    m_connections.erase(id);
    m_finished.notify_all();
}

void Server::send(Id id, QByteArray line)
{
    std::shared_ptr<Connection> connection;
    {
        const std::lock_guard lock(m_mutex);
        const auto it = m_connections.find(id);
        if (it == m_connections.end())
            return;
        connection = it->second;
    }
    connection->channel->send(std::move(line));
}

void Server::close(Id id)
{
    std::shared_ptr<Connection> connection;
    {
        const std::lock_guard lock(m_mutex);
        const auto it = m_connections.find(id);
        if (it == m_connections.end())
            return;
        connection = it->second;
    }
    connection->channel->stop();
}

int Server::connectionCount() const
{
    const std::lock_guard lock(m_mutex);
    return static_cast<int>(m_connections.size());
}

} // namespace ws::pipe
