#pragma once

#include <windows.h>

#include <functional>

namespace ws {

// Reports changes of the network: an adapter coming or going, an address
// changing (another Wi-Fi, a VPN connecting), Windows seeing the internet or
// a local network come or go. Network shares that could not be reached are
// tried again then (ShareIndexer::networkChanged).
class NetworkNotifier {
public:
    // `changed` is called on a thread of Windows', often several times in a row.
    explicit NetworkNotifier(std::function<void()> changed);
    ~NetworkNotifier(); // returns once no call is under way

    NetworkNotifier(const NetworkNotifier&) = delete;
    NetworkNotifier& operator=(const NetworkNotifier&) = delete;

private:
    std::function<void()> m_changed;
    HANDLE m_interfaces = nullptr;
    HANDLE m_connectivity = nullptr;
};

} // namespace ws
