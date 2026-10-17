#pragma once

#include <windows.h>

#include <functional>
#include <thread>

namespace ws {

// Keeps a service-owned UIAccess listener alive for this window. A missing
// or older service leaves KeyListener's ordinary Raw Input listener working.
// All pipe I/O and retries run off the GUI thread; destruction cancels them.
class ServiceKeyListener {
public:
    using Changed = std::function<void(DWORD pid)>; // zero: disconnected
    ServiceKeyListener(HWND window, Changed changed);
    ~ServiceKeyListener();
    ServiceKeyListener(const ServiceKeyListener&) = delete;
    ServiceKeyListener& operator=(const ServiceKeyListener&) = delete;

private:
    void run(HWND window, const Changed& changed);
    HANDLE m_stop = nullptr;
    std::thread m_thread;
};

} // namespace ws
