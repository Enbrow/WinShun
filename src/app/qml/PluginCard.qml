pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// A plugin in the settings (PluginSettings.list): its icon, name, version
// and author, where it stands, its switch and its menu; under them its
// keywords (changeable, with who has one it cannot use), its permissions
// (unfolded on request: what Windows enforces ticked, the rest "仅声明")
// and the settings its manifest declares, each drawn with Win顺's own
// controls (the plugin design, §4.7). Built like ShareCard.
Rectangle {
    id: card

    required property PluginSettings plugins
    required property string pluginId
    required property string name
    required property string version
    required property string author
    required property string homepage
    required property string description
    required property bool bundled
    required property string icon
    required property bool pluginEnabled // not `enabled`: that is the Item's own
    required property bool usable
    required property string status
    required property string tone
    required property string error
    required property bool resident
    required property var keywords
    required property var permissions
    required property var settings
    required property bool hasReadme
    required property bool hasData
    required property bool aiExpose // the manifest offers its commands to the AI: the switch below shows
    required property bool aiEnabled
    required property string aiNote
    property bool marked: false // what the settings search found

    property bool permissionsOpen: false
    property bool menuOpen: false

    readonly property color toneColor: tone === "ok" ? Theme.success
                                     : tone === "busy" ? Theme.accent
                                     : tone === "warn" ? Theme.caution
                                     : tone === "error" ? Theme.danger
                                     : Theme.faint

    function onPixel(v) { return Math.round(v * Screen.devicePixelRatio) / Screen.devicePixelRatio }

    implicitHeight: body.implicitHeight + 24
    radius: 4
    color: "transparent"
    border.width: 1
    border.color: marked ? Theme.accent : Theme.cardBorder
    z: menuOpen ? 1 : 0 // the menu over the cards below

    Column {
        id: body
        x: 14
        y: 12
        width: parent.width - 28
        spacing: 10

        Item { // the header: icon, names, state; the switch and the menu on the right
            id: header
            width: parent.width
            height: Math.max(texts.implicitHeight, controls.implicitHeight)

            Item {
                id: iconSlot
                width: 32
                height: 32
                y: 2

                Image { // whole device pixels, unsmoothed (see ResultRow)
                    readonly property real dpr: Screen.devicePixelRatio
                    readonly property int pixels: Math.round(32 * dpr)

                    x: 1 / 64
                    y: 1 / 64
                    width: pixels / dpr
                    height: pixels / dpr
                    source: card.icon
                    sourceSize.width: pixels / dpr
                    sourceSize.height: pixels / dpr
                    smooth: false
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                }
            }

            Column {
                id: texts
                anchors.left: iconSlot.right
                anchors.leftMargin: 12
                anchors.right: controls.left
                anchors.rightMargin: 12
                spacing: 3

                Row {
                    width: parent.width
                    spacing: 8

                    Text {
                        id: nameText
                        text: card.name
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        width: Math.min(implicitWidth, parent.width - (bundledTag.visible ? bundledTag.width + 8 : 0))
                    }
                    Rectangle { // 随包: it came with Win顺
                        id: bundledTag
                        visible: card.bundled
                        anchors.verticalCenter: parent.verticalCenter
                        width: bundledText.implicitWidth + 12
                        height: 20
                        radius: 3
                        color: Theme.badge

                        Text {
                            id: bundledText
                            anchors.centerIn: parent
                            text: qsTr("随包")
                            color: Theme.accent
                            font.pixelSize: 12
                        }
                    }
                }
                Text {
                    width: parent.width
                    visible: text.length > 0
                    text: [card.version, card.author].filter(s => s.length > 0).join(" · ")
                          + (card.description.length > 0 ? (card.version.length + card.author.length > 0 ? " · " : "") + card.description : "")
                    textFormat: Text.PlainText
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                }
                Item { // where it stands, at a glance; the words say it
                    width: parent.width
                    height: statusText.implicitHeight

                    Rectangle {
                        id: dot
                        y: Math.round(statusText.fontInfo.pixelSize * 0.62 - height / 2)
                        width: 8
                        height: 8
                        radius: 4
                        color: card.toneColor

                        SequentialAnimation on opacity { // starting
                            running: card.tone === "busy" && SystemTheme.animations
                            loops: Animation.Infinite
                            alwaysRunToEnd: true
                            NumberAnimation { to: 0.3; duration: 600; easing.type: Easing.InOutSine }
                            NumberAnimation { to: 1; duration: 600; easing.type: Easing.InOutSine }
                        }
                    }
                    Text {
                        id: statusText
                        anchors.left: dot.right
                        anchors.leftMargin: 8
                        anchors.right: parent.right
                        text: card.status
                        textFormat: Text.PlainText
                        color: card.tone === "error" ? Theme.danger : card.tone === "warn" ? Theme.caution : Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                        lineHeight: 1.15
                    }
                }
            }

            Row {
                id: controls
                anchors.right: parent.right
                y: 0
                spacing: 8

                ToggleSwitch {
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: card.usable
                    checked: card.pluginEnabled
                    onToggled: (on) => card.plugins.setEnabled(card.pluginId, on)
                }
                FlatButton { // the menu: 说明, 打开文件夹, 重新加载, 查看日志, 卸载
                    id: menuButton
                    anchors.verticalCenter: parent.verticalCenter
                    implicitHeight: 32
                    implicitWidth: 32
                    flat: true
                    glyph: "" // More
                    onClicked: card.menuOpen = !card.menuOpen
                }
            }
        }

        Column { // the keywords, one line per command
            width: parent.width
            spacing: 6
            visible: card.keywords.length > 0

            Repeater {
                model: card.keywords

                delegate: Item {
                    id: keywordRow

                    required property var modelData

                    width: parent.width
                    height: keywordColumn.implicitHeight

                    Column {
                        id: keywordColumn
                        width: parent.width
                        spacing: 4

                        Item {
                            width: parent.width
                            height: 32

                            Text {
                                id: keywordLabel
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("关键字 · %1").arg(keywordRow.modelData.title)
                                textFormat: Text.PlainText
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                                elide: Text.ElideRight
                                width: Math.min(implicitWidth, parent.width - keywordBox.width - resetButton.width - 24)
                            }
                            InputBox {
                                id: keywordBox
                                anchors.right: resetButton.visible ? resetButton.left : parent.right
                                anchors.rightMargin: resetButton.visible ? 8 : 0
                                anchors.verticalCenter: parent.verticalCenter
                                width: 170 // room for three short keywords ("md5 sha1 sha256")
                                implicitHeight: 32
                                text: keywordRow.modelData.keyword
                                placeholder: qsTr("无")
                                onEditingFinished: {
                                    if (text !== keywordRow.modelData.keyword)
                                        card.plugins.setKeyword(card.pluginId, keywordRow.modelData.command, text)
                                }
                            }
                            FlatButton {
                                id: resetButton
                                visible: keywordRow.modelData.user
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                implicitHeight: 32
                                flat: true
                                text: qsTr("恢复默认")
                                onClicked: card.plugins.resetKeyword(card.pluginId, keywordRow.modelData.command)
                            }
                        }
                        Text { // who has it
                            visible: keywordRow.modelData.conflict.length > 0
                            width: parent.width
                            text: keywordRow.modelData.conflict
                            textFormat: Text.PlainText
                            color: Theme.danger
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }

        Item { // 给 AI 用: its commands as the AI's tools (the AI page's switch gates it)
            width: parent.width
            height: aiColumn.implicitHeight
            visible: card.aiExpose

            Column {
                id: aiColumn
                anchors.left: parent.left
                anchors.right: aiSwitch.left
                anchors.rightMargin: 12
                spacing: 2

                Text {
                    width: parent.width
                    text: qsTr("给 AI 用")
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    visible: text.length > 0
                    text: card.aiNote
                    textFormat: Text.PlainText
                    color: Theme.faint
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
            ToggleSwitch {
                id: aiSwitch
                anchors.right: parent.right
                y: Math.round((aiColumn.implicitHeight - height) / 2)
                checked: card.aiEnabled
                onToggled: (on) => card.plugins.setAiEnabled(card.pluginId, on)
            }
        }

        Column { // the permissions, unfolded on request
            width: parent.width
            spacing: 4
            visible: card.permissions.length > 0

            Item {
                width: parent.width
                height: 24

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    Glyph {
                        anchors.verticalCenter: parent.verticalCenter
                        glyph: card.permissionsOpen ? "" : "" // ChevronDown / ChevronRight
                        size: 10
                        color: Theme.subtext
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("权限 · %n 项", "", card.permissions.length)
                        color: permissionsArea.containsMouse ? Theme.text : Theme.subtext
                        font.pixelSize: Theme.fontCaption
                    }
                }
                MouseArea {
                    id: permissionsArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: card.permissionsOpen = !card.permissionsOpen
                }
            }

            Column {
                visible: card.permissionsOpen
                width: parent.width
                spacing: 2

                Repeater {
                    model: card.permissionsOpen ? card.permissions : []

                    delegate: Item {
                        id: permissionRow

                        required property var modelData

                        // A line of 24; a long sentence (the sandbox's) wraps, the
                        // tick or tag staying on its first line.
                        width: parent.width
                        height: Math.max(24, permissionText.implicitHeight + 6)

                        Glyph { // enforced by Windows: a tick
                            id: tick
                            x: 16
                            y: Math.round((24 - height) / 2)
                            visible: permissionRow.modelData.enforced
                            glyph: "" // CheckMark
                            size: 12
                            color: Theme.success
                        }
                        Rectangle { // declared only: Win顺 cannot hold the plugin to it
                            id: declaredTag
                            x: 6
                            y: 3
                            visible: !permissionRow.modelData.enforced
                            width: declaredText.implicitWidth + 10
                            height: 18
                            radius: 3
                            color: "transparent"
                            border.width: 1
                            border.color: Theme.controlBorder

                            Text {
                                id: declaredText
                                anchors.centerIn: parent
                                text: qsTr("仅声明")
                                color: Theme.subtext
                                font.pixelSize: 11
                            }
                        }
                        Text {
                            id: permissionText
                            anchors.left: parent.left
                            anchors.leftMargin: 64
                            anchors.right: parent.right
                            y: 3
                            text: permissionRow.modelData.text
                                  + (permissionRow.modelData.note.length > 0 ? "（" + permissionRow.modelData.note + "）" : "")
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                            lineHeight: 1.15
                        }
                    }
                }
            }
        }

        Column { // the settings the manifest declares
            width: parent.width
            spacing: 6
            visible: card.settings.length > 0

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.divider
            }

            Repeater {
                model: card.settings

                delegate: Item {
                    id: settingRow

                    required property var modelData
                    readonly property string kind: modelData.type

                    width: parent.width
                    height: Math.max(36, settingControl.implicitHeight) + (settingHint.visible ? settingHint.implicitHeight + 4 : 0)

                    Text {
                        id: settingTitle
                        anchors.left: parent.left
                        anchors.right: settingControl.left
                        anchors.rightMargin: 12
                        y: Math.round((Math.max(36, settingControl.implicitHeight) - height) / 2)
                        text: settingRow.modelData.title
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideRight
                    }
                    Text {
                        id: settingHint
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: settingControl.bottom
                        anchors.topMargin: 4
                        visible: text.length > 0
                        text: settingRow.modelData.hint ?? ""
                        textFormat: Text.PlainText
                        color: Theme.faint
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }

                    Item { // the control for its type
                        id: settingControl

                        readonly property Item control: controlLoader.item as Item

                        anchors.right: parent.right
                        implicitWidth: control ? control.implicitWidth : 0
                        implicitHeight: control ? control.implicitHeight : 0
                        width: implicitWidth
                        height: implicitHeight

                        Loader {
                            id: controlLoader
                            sourceComponent: settingRow.kind === "bool" ? boolControl
                                           : settingRow.kind === "choice" ? choiceControl
                                           : settingRow.kind === "folder" || settingRow.kind === "file" ? pathControl
                                           : settingRow.kind === "hotkey" ? hotkeyControl
                                           : textControl
                        }

                        Component {
                            id: boolControl
                            ToggleSwitch {
                                checked: settingRow.modelData.value === true
                                onToggled: (on) => card.plugins.setSettingValue(card.pluginId, settingRow.modelData.key, on)
                            }
                        }
                        Component {
                            id: textControl
                            InputBox { // text, number, password (masked, kept by the Credential Manager)
                                width: 220
                                implicitHeight: 32
                                text: settingRow.kind === "password" ? "" : String(settingRow.modelData.value ?? "")
                                placeholder: settingRow.kind === "password" ? (settingRow.modelData.set ? qsTr("已保存，输入可替换") : qsTr("未设置")) : ""
                                echoMode: settingRow.kind === "password" ? TextInput.Password : TextInput.Normal
                                validator: settingRow.kind === "number" ? numberValidator : null
                                onEditingFinished: {
                                    if (settingRow.kind === "password") {
                                        if (text.length > 0) {
                                            card.plugins.setSettingValue(card.pluginId, settingRow.modelData.key, text)
                                            clear()
                                        }
                                    } else if (text !== String(settingRow.modelData.value ?? "")) {
                                        card.plugins.setSettingValue(card.pluginId, settingRow.modelData.key, text)
                                    }
                                }

                                DoubleValidator { id: numberValidator; notation: DoubleValidator.StandardNotation }
                            }
                        }
                        Component {
                            id: choiceControl
                            ScopeTabs {
                                labels: settingRow.modelData.options ?? []
                                current: settingRow.modelData.optionIndex ?? 0
                                onActivated: (index) => card.plugins.setSettingValue(card.pluginId, settingRow.modelData.key,
                                                                                     settingRow.modelData.optionValues[index])
                            }
                        }
                        Component {
                            id: pathControl
                            Row {
                                spacing: 8

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.min(implicitWidth, 260)
                                    text: String(settingRow.modelData.value ?? "").length > 0 ? String(settingRow.modelData.value) : qsTr("未设置")
                                    textFormat: Text.PlainText
                                    color: String(settingRow.modelData.value ?? "").length > 0 ? Theme.text : Theme.faint
                                    font.pixelSize: Theme.fontCaption
                                    elide: Text.ElideMiddle
                                }
                                FlatButton {
                                    implicitHeight: 32
                                    text: qsTr("选择…")
                                    onClicked: settingRow.kind === "folder" ? card.plugins.pickFolderSetting(card.pluginId, settingRow.modelData.key)
                                                                            : card.plugins.pickFileSetting(card.pluginId, settingRow.modelData.key)
                                }
                            }
                        }
                        Component {
                            id: hotkeyControl
                            HotkeyRecorder {
                                implicitHeight: 32
                                hotkey: String(settingRow.modelData.value ?? "")
                                onRecordingChanged: card.plugins.recordingHotkey = recording
                                onRecorded: (hotkey) => card.plugins.setSettingValue(card.pluginId, settingRow.modelData.key, hotkey)
                            }
                        }
                    }
                }
            }
        }
    }

    // The "…" menu, under its button; a click anywhere else closes it.
    MouseArea {
        visible: card.menuOpen
        x: -10000
        y: -10000
        width: 20000
        height: 20000
        z: 1
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => {
            card.menuOpen = false
            mouse.accepted = false
        }
    }
    Rectangle {
        id: menu

        readonly property var entries: [
            { text: qsTr("说明"), glyph: "", act: () => card.plugins.openReadme(card.pluginId), shown: card.hasReadme, danger: false }, // Document
            { text: qsTr("主页"), glyph: "", act: () => card.plugins.openHomepage(card.pluginId), shown: card.homepage.length > 0, danger: false }, // Link
            { text: qsTr("打开文件夹"), glyph: "", act: () => card.plugins.openFolder(card.pluginId), shown: true, danger: false }, // FolderOpen
            { text: qsTr("重新加载"), glyph: "", act: () => card.plugins.reload(card.pluginId), shown: true, danger: false }, // Refresh
            { text: qsTr("查看日志"), glyph: "", act: () => card.plugins.openLog(), shown: true, danger: false }, // Log
            { text: qsTr("卸载"), glyph: "", act: () => card.plugins.uninstall(card.pluginId), shown: !card.bundled, danger: true } // Delete
        ].filter(e => e.shown)

        visible: card.menuOpen
        z: 2
        x: parent.width - width - 8
        y: header.y + body.y + menuButton.height + 4
        width: 180
        height: menuColumn.implicitHeight + 8
        radius: 6
        color: Theme.menuBackground
        border.width: 1
        border.color: Theme.windowEdge

        Column {
            id: menuColumn
            x: 4
            y: 4
            width: parent.width - 8

            Repeater {
                model: menu.entries

                delegate: Rectangle {
                    id: entry

                    required property var modelData

                    width: parent.width
                    height: 34
                    radius: 4
                    color: entryArea.containsMouse ? Theme.menuHover : "transparent"

                    Glyph {
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        glyph: entry.modelData.glyph
                        size: 14
                        color: entry.modelData.danger ? Theme.danger : Theme.text
                    }
                    Text {
                        x: 36
                        anchors.verticalCenter: parent.verticalCenter
                        text: entry.modelData.text
                        color: entry.modelData.danger ? Theme.danger : Theme.text
                        font.pixelSize: Theme.fontCaption
                    }
                    MouseArea {
                        id: entryArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            card.menuOpen = false
                            entry.modelData.act()
                        }
                    }
                }
            }
        }
    }
}
