#include "RegistryWatch.h"

#include <QDebug>
#include <QString>
#include <QWinEventNotifier>

#include <utility>

namespace ws {

RegistryWatch::RegistryWatch(const wchar_t* subkey, std::function<void()> changed)
    : m_changed(std::move(changed))
    , m_event(::CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
    const LSTATUS opened = ::RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_NOTIFY, m_key.out());
    if (opened != ERROR_SUCCESS || !m_event.valid() || !arm()) {
        qWarning() << "Registry watch: not watching" << QString::fromWCharArray(subkey) << opened;
        return;
    }
    m_notifier = std::make_unique<QWinEventNotifier>(m_event.get());
    QObject::connect(m_notifier.get(), &QWinEventNotifier::activated, m_notifier.get(), [this] {
        if (!arm())
            qWarning() << "Registry watch: cannot watch again";
        m_changed();
    });
}

RegistryWatch::~RegistryWatch() = default;

bool RegistryWatch::arm()
{
    return ::RegNotifyChangeKeyValue(m_key.get(), FALSE, REG_NOTIFY_CHANGE_LAST_SET, m_event.get(), TRUE)
        == ERROR_SUCCESS;
}

} // namespace ws
