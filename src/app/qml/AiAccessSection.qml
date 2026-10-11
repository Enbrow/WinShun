pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → AI 助手: what AI clients may do through Win顺 (the MCP server,
// src/mcp): the switch, opening things, the clipboard history, the folders
// they never see, and the log of their calls.
SettingsSection {
    id: aiSection

    required property SettingsEditor editor

    title: qsTr("让 AI 使用 Win顺")
    note: qsTr("Claude、Cursor、VS Code 等 AI 助手通过 MCP（Model Context Protocol）连接 Win顺，就能搜文件名、文件里的文字和最近改过的文件：和你在搜索框里一样快，拼音也行。默认只读，不改动任何文件。")

    SettingRow {
        id: enableRow

        title: qsTr("允许 AI 连接")
        keywords: qsTr("MCP;AI;Claude;Cursor;智能;助手;大模型;人工智能").split(";")
        description: !aiSection.editor.mcp ? qsTr("关着时 AI 客户端连不上 Win顺")
                   : aiSection.editor.mcpStatus.length > 0 ? aiSection.editor.mcpStatus
                   : qsTr("AI 客户端启动 WinShunMcp.exe 就能连上")
        unlocks: [openRow, clipboardRow, pluginsRow, actionsRow, excludedRow, logRow]

        ToggleSwitch {
            checked: aiSection.editor.mcp
            onToggled: (on) => aiSection.editor.mcp = on
        }
    }

    SettingRow {
        id: openRow

        title: qsTr("允许 AI 打开文件和文件夹")
        keywords: qsTr("打开;文件夹;显示;open;AI").split(";")
        description: qsTr("用默认程序打开文件、在文件管理器里显示；以普通权限打开，AI 客户端每次会先问你")
        shown: aiSection.editor.mcp

        ToggleSwitch {
            checked: aiSection.editor.mcpAllowOpen
            onToggled: (on) => aiSection.editor.mcpAllowOpen = on
        }
    }

    SettingRow {
        id: clipboardRow

        title: qsTr("允许 AI 读取剪贴板历史")
        keywords: qsTr("剪贴板;剪切板;隐私;clipboard;AI").split(";")
        description: qsTr("AI 能看到复制过的文字、链接和文件名，图片只告诉它大小；密码管理器复制的本来就不记录")
        shown: aiSection.editor.mcp

        ToggleSwitch {
            checked: aiSection.editor.mcpClipboard
            onToggled: (on) => aiSection.editor.mcpClipboard = on
        }
    }

    SettingRow { // the plugins' commands as tools (plugin::AiBridge)
        id: pluginsRow

        title: qsTr("AI 可以使用插件")
        keywords: qsTr("插件;工具;plugin;tool;AI").split(";")
        description: qsTr("标明可给 AI 用的插件，每个命令成为一个工具（plugin_插件_命令）：AI 只能查询，像你在搜索框里输入那样拿到结果；每个插件的卡片上还有一个开关")
        shown: aiSection.editor.mcp
        unlocks: [actionsRow]

        ToggleSwitch {
            checked: aiSection.editor.mcpPlugins
            onToggled: (on) => aiSection.editor.mcpPlugins = on
        }
    }

    SettingRow {
        id: actionsRow

        title: qsTr("允许 AI 执行插件动作")
        keywords: qsTr("插件;动作;执行;结束;plugin;action;AI").split(";")
        description: qsTr("再多一个工具（plugin_插件_命令_run）做 Enter 做的事：结束进程、打开、复制这类会改动东西的操作；AI 客户端每次会先问你")
        shown: aiSection.editor.mcp && aiSection.editor.mcpPlugins

        ToggleSwitch {
            checked: aiSection.editor.mcpPluginActions
            onToggled: (on) => aiSection.editor.mcpPluginActions = on
        }
    }

    SettingRow {
        id: excludedRow

        title: qsTr("不让 AI 看到的文件夹")
        keywords: qsTr("排除;隐私;隐藏;黑名单;exclude;AI").split(";")
        values: aiSection.editor.mcpExcludedPaths
        description: qsTr("这些文件夹里的东西不出现在 AI 的搜索结果里，也不能打开；“搜索范围”里不搜索的文件夹 AI 本来就看不到")
        shown: aiSection.editor.mcp

        FlatButton {
            text: qsTr("添加文件夹…")
            glyph: "" // Add
            onClicked: aiSection.editor.addMcpExcludedFolder()
        }

        body: Rectangle {
            width: parent.width
            height: Math.max(pathList.implicitHeight, 40)
            radius: 4
            color: "transparent"
            border.width: 1
            border.color: Theme.cardBorder

            Text {
                visible: aiSection.editor.mcpExcludedPaths.length === 0
                anchors.centerIn: parent
                text: qsTr("AI 能看到索引里的所有文件夹")
                color: Theme.faint
                font.pixelSize: Theme.fontCaption
            }

            Column {
                id: pathList
                width: parent.width

                Repeater {
                    model: aiSection.editor.mcpExcludedPaths

                    delegate: Item {
                        id: pathRow

                        required property int index
                        required property string modelData

                        width: pathList.width
                        height: 42

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            radius: 3
                            color: rowArea.containsMouse ? Theme.navHover : "transparent"
                        }
                        Rectangle {
                            visible: pathRow.index > 0
                            x: 12
                            width: parent.width - 24
                            height: 1
                            color: Theme.divider
                        }
                        MouseArea {
                            id: rowArea
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                        }
                        Glyph {
                            id: folderIcon
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            glyph: "" // Folder
                            size: 16
                        }
                        Text {
                            anchors.left: folderIcon.right
                            anchors.leftMargin: 10
                            anchors.right: removePath.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: pathRow.modelData
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideMiddle
                        }
                        FlatButton {
                            id: removePath
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            implicitHeight: 32
                            flat: true
                            text: qsTr("移除")
                            onClicked: aiSection.editor.removeMcpExcludedPath(pathRow.index)
                        }
                    }
                }
            }
        }
    }

    SettingRow {
        id: logRow

        title: qsTr("调用记录")
        keywords: qsTr("日志;记录;log;AI").split(";")
        description: qsTr("AI 每次调用都记在 mcp.log 里：时间、客户端、工具和参数、结果数量；不记录剪贴板里的文字")
        shown: aiSection.editor.mcp

        FlatButton {
            text: qsTr("打开")
            glyph: "" // Document
            onClicked: aiSection.editor.openMcpLog()
        }
    }
}
