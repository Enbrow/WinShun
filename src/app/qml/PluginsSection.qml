pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → 插件: the plugins installed, each a card (PluginCard), and
// how to add one: a folder or an archive picked, or dropped on the page.
// What the view-model says of the last install or removal shows under
// the cards.
SettingsSection {
    id: pluginsSection

    required property PluginSettings plugins

    title: qsTr("插件")
    note: qsTr("插件给搜索框加关键字命令、把结果混进列表。它们永远以你的普通权限运行，不是管理员；装上后默认关着，打开开关时会先列出它要的权限。")

    // Unfolds the permissions of the card at `index` (the offscreen preview).
    function openPermissions(index) {
        const card = cardsRepeater.itemAt(index)
        if (card)
            card.permissionsOpen = true
    }

    // The card of a plugin, or null (SettingsWindow.showPluginPage).
    function cardOf(pluginId) {
        for (let i = 0; i < cardsRepeater.count; ++i) {
            const card = cardsRepeater.itemAt(i)
            if (card && card.pluginId === pluginId)
                return card
        }
        return null
    }
    // Marks the plugin's card for a few seconds, as the settings search
    // marks what it found: the page was opened for it.
    property string revealed
    function reveal(pluginId) {
        revealed = pluginId
        revealTimer.restart()
    }
    Timer {
        id: revealTimer
        interval: 3000
        onTriggered: pluginsSection.revealed = ""
    }

    SettingRow { // the plugins cannot see what Explorer signed in to (Runner::TokenSource::Restricted)
        title: qsTr("插件看不到你在资源管理器里登录的网络位置")
        keywords: qsTr("网络;共享;NAS;登录;令牌;plugin;network").split(";")
        description: pluginsSection.plugins.restrictedNote
        shown: pluginsSection.plugins.restrictedNote.length > 0

        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            glyph: "" // Warning
            size: 20
            color: Theme.caution
        }
    }

    SettingRow {
        id: listRow

        title: qsTr("已安装的插件")
        keywords: qsTr("插件;扩展;关键字;命令;安装;plugin;extension").split(";")
        values: { // the names, read again as the list changes (count tells)
            const names = []
            for (let i = 0; i < pluginsSection.plugins.count; ++i)
                names.push(pluginsSection.plugins.list.data(pluginsSection.plugins.list.index(i, 0), Qt.UserRole + 2))
            return names
        }
        description: pluginsSection.plugins.count > 0
                     ? qsTr("%n 个；文件夹或 .zip / .wsplugin 拖到这里也能安装", "", pluginsSection.plugins.count)
                     : qsTr("还没有插件：点“添加插件…”，或把插件文件夹、.zip / .wsplugin 拖到这里")

        FlatButton {
            id: addButton
            text: qsTr("添加插件…")
            glyph: "" // Add
            onClicked: addMenu.open = !addMenu.open

            Rectangle { // 文件夹… / 压缩包…
                id: addMenu

                property bool open: false

                visible: open
                z: 2
                x: parent.width - width
                y: parent.height + 4
                width: 200
                height: addColumn.implicitHeight + 8
                radius: 6
                color: Theme.menuBackground
                border.width: 1
                border.color: Theme.windowEdge

                Column {
                    id: addColumn
                    x: 4
                    y: 4
                    width: parent.width - 8

                    Repeater {
                        model: [
                            { text: qsTr("插件文件夹…"), glyph: "", archive: false }, // Folder
                            { text: qsTr("压缩包（.zip / .wsplugin）…"), glyph: "", archive: true } // ZipFolder
                        ]

                        delegate: Rectangle {
                            id: addEntry

                            required property var modelData

                            width: parent.width
                            height: 34
                            radius: 4
                            color: addArea.containsMouse ? Theme.menuHover : "transparent"

                            Glyph {
                                x: 10
                                anchors.verticalCenter: parent.verticalCenter
                                glyph: addEntry.modelData.glyph
                                size: 14
                                color: Theme.text
                            }
                            Text {
                                x: 36
                                anchors.verticalCenter: parent.verticalCenter
                                text: addEntry.modelData.text
                                color: Theme.text
                                font.pixelSize: Theme.fontCaption
                            }
                            MouseArea {
                                id: addArea
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    addMenu.open = false
                                    if (addEntry.modelData.archive)
                                        pluginsSection.plugins.installFromArchive()
                                    else
                                        pluginsSection.plugins.installFromFolder()
                                }
                            }
                        }
                    }
                }
            }
            MouseArea { // a click anywhere else closes the menu
                visible: addMenu.open
                x: -10000
                y: -10000
                width: 20000
                height: 20000
                z: 1
                acceptedButtons: Qt.AllButtons
                onPressed: (mouse) => {
                    addMenu.open = false
                    mouse.accepted = false
                }
            }
        }
        FlatButton {
            text: qsTr("打开文件夹")
            glyph: "" // FolderOpen
            onClicked: pluginsSection.plugins.openUserDir()
        }

        body: Item {
            id: dropZone

            width: parent.width
            height: cards.implicitHeight + (dropHint.visible ? dropHint.height + 8 : 0)

            Column {
                id: cards
                width: parent.width
                spacing: 8

                Repeater {
                    id: cardsRepeater
                    model: pluginsSection.plugins.list

                    delegate: PluginCard {
                        width: cards.width
                        plugins: pluginsSection.plugins
                        marked: listRow.valueFound(name) || (pluginsSection.revealed.length > 0 && pluginId === pluginsSection.revealed)
                    }
                }
            }

            Rectangle { // where a dragged folder or archive may be let go of
                id: dropHint
                visible: dropArea.containsDrag
                anchors.bottom: parent.bottom
                width: parent.width
                height: 48
                radius: 4
                color: Theme.selection
                border.width: 1
                border.color: Theme.accent

                Text {
                    anchors.centerIn: parent
                    text: qsTr("松开就安装")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
            }

            DropArea {
                id: dropArea
                anchors.fill: parent
                anchors.margins: -12
                keys: ["text/uri-list"]
                onDropped: (drop) => {
                    for (const url of drop.urls)
                        pluginsSection.plugins.install(String(url))
                    drop.accept()
                }
            }
        }
    }

    SettingRow {
        title: qsTr("刚才的结果")
        description: pluginsSection.plugins.message
        shown: pluginsSection.plugins.message.length > 0

        FlatButton {
            text: qsTr("知道了")
            flat: true
            onClicked: pluginsSection.plugins.clearMessage()
        }
    }
}
