#pragma once

#include "Win32Util.h"

#include <functional>
#include <memory>

class QWinEventNotifier;

namespace ws {

// Calls `changed` on the GUI thread whenever a value directly under
// HKEY_CURRENT_USER\`subkey` is set or deleted, by any program
// (RegNotifyChangeKeyValue, as SettingsEditor's TaskbarWatch). Armed again
// before each call, so a change made while `changed` runs is not missed.
// Made on the GUI thread: the registration ends with the thread that asked.
// A key that does not exist is not watched (active() is false).
class RegistryWatch {
public:
    RegistryWatch(const wchar_t* subkey, std::function<void()> changed);
    ~RegistryWatch();

    RegistryWatch(const RegistryWatch&) = delete;
    RegistryWatch& operator=(const RegistryWatch&) = delete;

    [[nodiscard]] bool active() const noexcept { return m_notifier != nullptr; }

private:
    bool arm(); // once per change

    std::function<void()> m_changed;
    win32::UniqueHandle m_event;
    win32::UniqueKey m_key;
    std::unique_ptr<QWinEventNotifier> m_notifier; // last: gone before the key and the event
};

} // namespace ws
