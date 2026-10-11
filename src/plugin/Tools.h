#pragma once

// Win顺's tools for the plugins (the plugin design, §6.3; stage 3): the
// same tools the AI clients get (mcp::ToolHost), on a host and a search
// engine of the plugins' own -- an engine runs only its newest request, so
// neither the launcher's nor the AI's can be shared -- and whatever the AI
// switches say: a plugin's manifest decides what it may call
// (Host::onToolCall), and what a plugin sees is what the user's normal
// rights see (Visibility's default token).

#include "Host.h"
#include "McpTools.h"

#include <QObject>

#include <memory>

namespace ws {
class AppCatalog;
class SearchEngine;
class SystemCatalog;
}

namespace ws::plugin {

class Tools : public QObject {
    Q_OBJECT

public:
    // `services.engine` is made here from `services.index` and the
    // catalogs (may be null); the rest as for mcp::ToolHost.
    Tools(mcp::Services services, AppCatalog* apps, SystemCatalog* places, QObject* parent = nullptr);
    ~Tools() override;

    // How searches are set up (the content extensions and limits, the order
    // of folders and files): from the AI's policy, with every tool on and
    // nothing hidden -- the plugins run as the user.
    void setPolicy(mcp::Policy policy);
    mcp::ToolHost& host() { return *m_host; }
    ToolBridge bridge() { return toolBridge(m_host.get()); }

private:
    std::unique_ptr<SearchEngine> m_engine; // before the host, which runs on it
    std::unique_ptr<mcp::ToolHost> m_host;
};

} // namespace ws::plugin
