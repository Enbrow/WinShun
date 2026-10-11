pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → AI 助手: the AI clients found on this PC, each connected to
// Win顺 with one click (McpClients writes WinShunMcp.exe into its
// configuration), and the configuration to paste into any other.
SettingsSection {
    id: clientsSection

    required property SettingsEditor editor

    readonly property bool relayFound: editor.mcpRelayPath.length > 0

    title: qsTr("连接到 AI 客户端")
    note: relayFound ? qsTr("点“连接”就写进那个客户端的配置（原文件先备份成 .bak），上面的“允许 AI 连接”也会一起打开。装了的客户端才列出来。")
                     : qsTr("没找到 WinShunMcp.exe，重新安装 Win顺 就有了。")

    Repeater {
        model: clientsSection.editor.mcpClients

        delegate: SettingRow {
            id: clientRow

            required property var modelData

            title: modelData.name
            keywords: qsTr("连接;配置;MCP;AI;%1").arg(modelData.name).split(";")
            description: modelData.connected ? qsTr("已连接 · %1").arg(modelData.configPath) : modelData.note
            shown: modelData.installed && clientsSection.editor.mcp

            Glyph {
                visible: clientRow.modelData.connected
                anchors.verticalCenter: parent.verticalCenter
                glyph: "" // CheckMark
                size: 20
                color: Theme.accent
            }
            FlatButton {
                text: clientRow.modelData.connected ? qsTr("重新连接") : qsTr("连接")
                glyph: "" // Link
                highlighted: !clientRow.modelData.connected
                enabled: clientsSection.relayFound
                onClicked: clientsSection.editor.connectMcpClient(clientRow.modelData.id)
            }
            FlatButton {
                text: clientRow.modelData.id === "claude-code" ? qsTr("复制命令") : qsTr("复制配置")
                glyph: "" // Copy
                enabled: clientsSection.relayFound
                onClicked: clientsSection.editor.copyMcpSnippet(clientRow.modelData.id)
            }
        }
    }

    SettingRow {
        title: qsTr("其他支持 MCP 的客户端")
        keywords: qsTr("其他;配置;stdio;MCP;AI;JSON").split(";")
        description: qsTr("把 WinShunMcp.exe 加为 stdio 类型的 MCP 服务器，不需要参数：") + "\n" + clientsSection.editor.mcpRelayPath
        shown: clientsSection.editor.mcp

        FlatButton {
            text: qsTr("复制路径")
            glyph: "" // Copy
            enabled: clientsSection.relayFound
            onClicked: clientsSection.editor.copyText(clientsSection.editor.mcpRelayPath)
        }
        FlatButton {
            text: qsTr("复制 JSON")
            enabled: clientsSection.relayFound
            onClicked: clientsSection.editor.copyMcpSnippet("")
        }
    }

    SettingRow {
        title: qsTr("刚才的结果")
        description: clientsSection.editor.mcpMessage
        shown: clientsSection.editor.mcp && clientsSection.editor.mcpMessage.length > 0

        FlatButton {
            text: qsTr("知道了")
            flat: true
            onClicked: clientsSection.editor.clearMcpMessage()
        }
    }
}
