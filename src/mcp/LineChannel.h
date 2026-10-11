#pragma once

// Lines in, lines out, on one duplex handle opened for overlapped I/O (a
// named pipe's end): what Win顺's MCP pipe (PipeServer) and the plugin
// runner's pipe (plugin::Runner) have in common. Reads happen on the thread
// that calls run(); lines to write are queued from any thread and written
// by that same thread, so one handle never has two threads' I/O waiting on
// each other. Qt Core and Win32 only.

#include <QByteArray>

#include <windows.h>

#include <deque>
#include <functional>
#include <mutex>

namespace ws::pipe {

class LineChannel {
public:
    struct Callbacks {
        std::function<void(QByteArray line)> line; // without its newline; empty lines are dropped
        // The first bytes came: a pipe server may identify its client now
        // (ImpersonateNamedPipeClient works only once the client has written).
        std::function<void()> firstData;
    };

    // Takes the handle; closes it in the destructor.
    LineChannel(HANDLE handle, Callbacks callbacks);
    ~LineChannel();
    LineChannel(const LineChannel&) = delete;
    LineChannel& operator=(const LineChannel&) = delete;

    // Reads and writes until the handle breaks (the other end closed), a
    // line never ends (longer than kMaxLineBytes) or stop() is called; then
    // cancels what is in flight and returns. The caller's thread.
    void run();
    // Any thread. Queued, written by run()'s thread in order; dropped once
    // run() has returned.
    void send(QByteArray line);
    void stop(); // any thread: run() returns soon; idempotent
    bool broken() const noexcept { return m_broken; } // run() ended because the handle broke or a line never ended
    HANDLE handle() const noexcept { return m_handle; }

private:
    HANDLE m_handle;
    HANDLE m_stopEvent; // manual-reset: stop() sets it
    HANDLE m_queueEvent; // auto-reset: send() sets it
    Callbacks m_callbacks;
    std::mutex m_mutex;
    std::deque<QByteArray> m_queue; // lines to write
    bool m_ended = false; // run() returned: nothing more is sent
    bool m_broken = false;
};

} // namespace ws::pipe
