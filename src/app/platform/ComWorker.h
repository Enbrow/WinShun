#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace ws {

// A thread of its own with COM, for work that waits on other programs:
// asking Explorer, driving a file dialog (a single-threaded apartment, as the
// shell wants), or reading and setting the system's switches (Switches: the
// multithreaded apartment, where WinRT's and Core Audio's objects live and
// call back without a message loop). The tasks run one at a time, in the
// order they came.
//
// The owner's members can be used from the tasks: the destructor lets the
// running task finish and drops the others before the owner's members go.
class ComWorker {
public:
    enum class Apartment : unsigned char { SingleThreaded, MultiThreaded };

    // `last` runs on the thread as it ends, after the last task and before
    // COM is let go: what the tasks made there is released there.
    explicit ComWorker(Apartment apartment = Apartment::SingleThreaded, std::function<void()> last = {});
    ~ComWorker();

    ComWorker(const ComWorker&) = delete;
    ComWorker& operator=(const ComWorker&) = delete;

    void post(std::function<void()> task);
    // Requested once the destructor has begun: a task that waits (on WinRT,
    // on another program) looks at it now and then and gives up, as the
    // destructor waits for that task.
    std::stop_token stopToken() const noexcept { return m_thread.get_stop_token(); }

private:
    void run(std::stop_token stop);

    const Apartment m_apartment;
    std::function<void()> m_last;
    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<std::function<void()>> m_tasks;
    std::jthread m_thread; // last: started once the members above exist
};

} // namespace ws
