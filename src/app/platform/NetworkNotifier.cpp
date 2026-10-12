#include "NetworkNotifier.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

namespace ws {

NetworkNotifier::NetworkNotifier(std::function<void()> changed)
    : m_changed(std::move(changed))
{
    ::NotifyIpInterfaceChange(
        AF_UNSPEC,
        [](void* context, MIB_IPINTERFACE_ROW*, MIB_NOTIFICATION_TYPE type) {
            if (type != MibInitialNotification)
                static_cast<NetworkNotifier*>(context)->m_changed();
        },
        this, FALSE, &m_interfaces);
    // Windows 10 2004 (19041) and later only: looked up, not imported, or
    // WinShun would not start at all on older Windows 10 (LTSC 2019 is 17763).
    // Without it, adapter and address changes above still come through.
    using HintChange = decltype(&::NotifyNetworkConnectivityHintChange);
    if (HMODULE iphlpapi = ::GetModuleHandleW(L"iphlpapi.dll"))
        if (auto notify = reinterpret_cast<HintChange>(
                ::GetProcAddress(iphlpapi, "NotifyNetworkConnectivityHintChange")))
            notify([](void* context,
                       NL_NETWORK_CONNECTIVITY_HINT) { static_cast<NetworkNotifier*>(context)->m_changed(); },
                this, FALSE, &m_connectivity);
}

NetworkNotifier::~NetworkNotifier()
{
    if (m_interfaces)
        ::CancelMibChangeNotify2(m_interfaces);
    if (m_connectivity)
        ::CancelMibChangeNotify2(m_connectivity);
}

} // namespace ws
