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
    ::NotifyNetworkConnectivityHintChange(
        [](void* context, NL_NETWORK_CONNECTIVITY_HINT) { static_cast<NetworkNotifier*>(context)->m_changed(); }, this,
        FALSE, &m_connectivity);
}

NetworkNotifier::~NetworkNotifier()
{
    if (m_interfaces)
        ::CancelMibChangeNotify2(m_interfaces);
    if (m_connectivity)
        ::CancelMibChangeNotify2(m_connectivity);
}

} // namespace ws
