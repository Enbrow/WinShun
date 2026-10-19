pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Settings → 插件 › 已安装: the plugins installed, each a card (PluginCard),
// folded to three lines until asked for more; how to add one (a folder or
// an archive picked, or dropped on the page), where they are, and the
// switch of developer mode, whose rows are on the 开发者 tab
// (PluginDevSection, which `lockedElsewhere` comes from). What the
// view-model says of the last install or removal shows over the cards.
SettingsSection {
    id: pluginsSection

    required property PluginSettings plugins
    // Rows on the 开发者 tab shown only in developer mode (PluginDevSection.lockedRows).
    property var lockedElsewhere: []
    // The built-in features drawn as cards here (CaptureCard); null in a
    // preview without one.
    property SettingsEditor editor: null
    // A built-in card records a hotkey (SettingsWindow stops the app's hotkeys meanwhile).
    readonly property bool recordingHotkey: captureCard.item ? captureCard.item.recordingHotkey : false

    // The cards unfolded, by plugin id: here, not in the cards, for the list
    // is reset (the cards made again) when a plugin comes or goes. For as
    // long as the window lives; not saved. Assigned whole, for the bindings.
    property var expandedIds: []
    function setExpanded(pluginId, on) {
        const ids = expandedIds.filter(id => id !== pluginId)
        if (on)
            ids.push(pluginId)
        expandedIds = ids
    }
    // Unfolds a plugin's card (SettingsWindow.showPluginPage).
    function expand(pluginId) { setExpanded(pluginId, true) }
    // Unfolds the card at `index` (the offscreen previews).
    function expandAt(index) {
        const card = cardsRepeater.itemAt(index)
        if (card)
            setExpanded(card.pluginId, true)
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

    // Counts the cards' changes (a keyword changed, a plugin added), for
    // what the search reads of them to be read again.
    property int listRevision: 0
    Connections {
        target: pluginsSection.plugins.list
        function onModelReset() { pluginsSection.listRevision++ }
        function onRowsInserted() { pluginsSection.listRevision++ }
        function onRowsRemoved() { pluginsSection.listRevision++ }
        function onDataChanged() { pluginsSection.listRevision++ }
    }

    note: qsTr("插件给搜索框加关键字命令，结果混进列表。它们只用你的普通权限；新装的默认关着，打开时会先列出它要的权限。")

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

    SettingRow { // over the cards: in sight of 添加插件 and of a file dropped
        title: qsTr("刚才的结果")
        description: pluginsSection.plugins.message
        shown: pluginsSection.plugins.message.length > 0

        FlatButton {
            text: qsTr("知道了")
            flat: true
            onClicked: pluginsSection.plugins.clearMessage()
        }
    }

    SettingRow { // what comes with Win顺 but stays off until wanted: the screen capture
        id: builtinRow

        title: qsTr("内置功能")
        keywords: qsTr("截图;截屏;屏幕截图;PrtScn;Print Screen;放大镜;取色;screenshot;capture").split(";")
        values: pluginsSection.editor ? [pluginsSection.editor.captureHotkey] : []
        description: qsTr("随 Win顺 一起提供，默认关着；关着时不占内存，也不注册快捷键")
        shown: pluginsSection.editor !== null

        body: Loader {
            id: captureCard
            width: parent.width
            active: pluginsSection.editor !== null
            sourceComponent: CaptureCard {
                editor: pluginsSection.editor
                marked: builtinRow.searching && builtinRow.found
            }
        }
    }

    SettingRow {
        id: listRow

        title: qsTr("已安装的插件")
        keywords: qsTr("插件;扩展;关键字;命令;安装;plugin;extension").split(";")
        // What the cards hold, to be found by: the names, the commands and
        // their keywords, the titles of the plugins' settings, 给 AI 用.
        // Read from the model's roles (PluginListModel::Role: NameRole is
        // UserRole + 2, KeywordsRole + 15, SettingsRole + 17, AiExposeRole + 20).
        values: {
            pluginsSection.listRevision // read again as the cards change
            const list = pluginsSection.plugins.list
            const found = []
            const add = (value) => {
                if (value && value.length > 0 && found.indexOf(value) < 0)
                    found.push(value)
            }
            for (let i = 0; i < pluginsSection.plugins.count; ++i) {
                const index = list.index(i, 0)
                add(list.data(index, Qt.UserRole + 2))
                for (const k of list.data(index, Qt.UserRole + 15) ?? []) {
                    add(k.title)
                    add(k.keyword)
                }
                for (const s of list.data(index, Qt.UserRole + 17) ?? [])
                    add(s.title)
                if (list.data(index, Qt.UserRole + 20))
                    add(qsTr("给 AI 用"))
            }
            return found
        }
        description: pluginsSection.plugins.count > 0
                     ? qsTr("%n 个；文件夹或 .zip / .wsplugin 拖到这里也能安装", "", pluginsSection.plugins.count)
                     : qsTr("还没有插件：点“添加插件”，或把插件文件夹、.zip / .wsplugin 拖到这里")

        FlatButton {
            id: addButton
            text: qsTr("添加插件")
            glyph: "" // Add
            onClicked: addMenu.open = !addMenu.open

            DropMenu { // 文件夹… / 压缩包…
                id: addMenu
                anchor: addButton
                entries: [
                    { text: qsTr("插件文件夹"), glyph: "" }, // Folder
                    { text: qsTr("压缩包（.zip / .wsplugin）"), glyph: "" } // ZipFolder
                ]
                onActivated: (index) => {
                    if (index === 1)
                        pluginsSection.plugins.installFromArchive()
                    else
                        pluginsSection.plugins.installFromFolder()
                }
            }
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
                        id: pluginCard

                        // The search found something inside it, not only its name: unfolded while it searches.
                        readonly property bool foundInside: listRow.searching
                            && (keywords.some(k => listRow.valueFound(k.title) || listRow.valueFound(k.keyword))
                                || settings.some(s => listRow.valueFound(s.title))
                                || (aiExpose && listRow.valueFound(qsTr("给 AI 用"))))

                        width: cards.width
                        plugins: pluginsSection.plugins
                        animated: !pluginsSection.plugins.preview
                        expanded: pluginsSection.expandedIds.indexOf(pluginId) >= 0 || foundInside
                        marked: listRow.valueFound(name) || foundInside
                                || (pluginsSection.revealed.length > 0 && pluginId === pluginsSection.revealed)
                        onExpandRequested: (on) => pluginsSection.setExpanded(pluginId, on)
                    }
                }
            }

            Rectangle { // where a dragged folder or archive may be let go of
                id: dropHint
                visible: dropArea.containsDrag
                anchors.bottom: parent.bottom
                width: parent.width
                height: 48
                radius: Theme.controlRadius
                color: Theme.selection
                border.width: 1
                border.color: Theme.cardBorder

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
        title: qsTr("插件文件夹")
        keywords: qsTr("位置;路径;文件夹;folder;path").split(";")
        description: pluginsSection.plugins.userDir

        FlatButton {
            text: qsTr("打开")
            glyph: "" // FolderOpen
            onClicked: pluginsSection.plugins.openUserDir()
        }
    }

    SettingRow {
        title: qsTr("开发者模式")
        keywords: qsTr("开发;调试;日志;协议;模板;developer;debug;log").split(";")
        description: qsTr("记下每一条发给插件和插件回来的消息，改了脚本自动重新加载；打开后多一个“开发者”标签")
        unlocks: [...pluginsSection.lockedElsewhere]

        ToggleSwitch {
            checked: pluginsSection.plugins.developerMode
            onToggled: (on) => pluginsSection.plugins.developerMode = on
        }
    }
}
