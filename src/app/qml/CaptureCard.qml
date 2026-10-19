pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The screen capture's card on the 插件 page (src/capture; [Capture] in
// WinShun.ini): built in, off until switched on here. Drawn like a plugin's
// card (PluginCard): icon, name, a tag, what it does and where it stands,
// the switch on the right. While it is on, its options show underneath: the
// hotkey (recorded, with what went wrong registering it), PrtScn, and
// whether letting go of a drag copies at once. Turned off, the app makes no
// window, registers no key and holds no graphics device for it.
Rectangle {
    id: card

    required property SettingsEditor editor
    property bool marked: false // what the settings search found
    // The hotkey box records: SettingsWindow's binding stops the app's hotkeys meanwhile.
    readonly property bool recordingHotkey: hotkeyRecorder.recording

    implicitHeight: body.implicitHeight + 24
    radius: Theme.controlRadius
    color: marked ? Theme.selection : "transparent"
    border.width: 1
    border.color: Theme.cardBorder

    Column {
        id: body
        x: 14
        y: 12
        width: parent.width - 28
        spacing: 12

        Item { // the header
            width: parent.width
            height: Math.max(texts.implicitHeight, toggle.implicitHeight)

            Glyph {
                id: icon
                y: 2
                width: 32
                height: 32
                glyph: "" // Camera
                size: 24
                color: Theme.text
            }

            Column {
                id: texts
                anchors.left: icon.right
                anchors.leftMargin: 12
                anchors.right: toggle.left
                anchors.rightMargin: 12
                spacing: 4

                Row {
                    spacing: 8

                    Text {
                        text: qsTr("截图")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                    Tag {
                        text: qsTr("内置")
                    }
                }
                Text {
                    width: parent.width
                    text: qsTr("冻结屏幕，框选区域或点选窗口、控件，复制或保存；放大镜看像素和颜色")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    text: !card.editor.captureEnabled ? qsTr("关着")
                          : card.editor.captureHotkey.length > 0 ? qsTr("开着：按 %1，或从托盘菜单截图").arg(card.editor.captureHotkey)
                          : card.editor.capturePrintScreen ? qsTr("开着：按 PrtScn，或从托盘菜单截图")
                          : qsTr("开着：从托盘菜单截图，或在下面录一个快捷键")
                    color: card.editor.captureEnabled ? Theme.text : Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
            }

            ToggleSwitch {
                id: toggle
                anchors.right: parent.right
                y: 0
                checked: card.editor.captureEnabled
                onToggled: (on) => card.editor.captureEnabled = on
            }
        }

        Column { // the options, while it is on
            visible: card.editor.captureEnabled
            width: parent.width
            spacing: 12

            Rectangle { // between the header and the options
                width: parent.width
                height: 1
                color: Theme.cardBorder
            }

            Item {
                width: parent.width
                height: Math.max(hotkeyTexts.implicitHeight, hotkeyControls.implicitHeight)

                Column {
                    id: hotkeyTexts
                    anchors.left: parent.left
                    anchors.right: hotkeyControls.left
                    anchors.rightMargin: 12
                    spacing: 4

                    Text {
                        width: parent.width
                        text: qsTr("快捷键")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        width: parent.width
                        text: qsTr("按它开始截图，截图时再按一次取消")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                    Text {
                        visible: card.editor.captureHotkeyError.length > 0
                        width: parent.width
                        text: card.editor.captureHotkeyError
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
                Row {
                    id: hotkeyControls
                    anchors.right: parent.right
                    spacing: 8

                    FlatButton {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: card.editor.captureHotkey.length > 0 && !hotkeyRecorder.recording
                        flat: true
                        text: qsTr("清除")
                        onClicked: card.editor.captureHotkey = ""
                    }
                    HotkeyRecorder {
                        id: hotkeyRecorder
                        anchors.verticalCenter: parent.verticalCenter
                        hotkey: card.editor.captureHotkey
                        onRecorded: (hotkey) => card.editor.captureHotkey = hotkey
                    }
                }
            }

            Item { // the screenshot history's keyword (jt)
                width: parent.width
                height: Math.max(keywordTexts.implicitHeight, keywordBox.implicitHeight)

                Column {
                    id: keywordTexts
                    anchors.left: parent.left
                    anchors.right: keywordBox.left
                    anchors.rightMargin: 12
                    spacing: 4

                    Text {
                        width: parent.width
                        text: qsTr("截图历史关键词")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        width: parent.width
                        text: qsTr("在搜索框输入“%1”加空格，列出截过的图，再输入文字按图里识别出的文字筛选；Enter 复制并粘贴，Ctrl+Enter 贴到屏幕，Delete 删除").arg(card.editor.captureKeyword)
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                    Text {
                        id: keywordError
                        visible: text.length > 0
                        width: parent.width
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
                InputBox {
                    id: keywordBox
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 96
                    text: card.editor.captureKeyword
                    maximumLength: 16
                    function commit() {
                        keywordError.text = card.editor.saveCaptureKeyword(text)
                        if (keywordError.text.length === 0)
                            text = Qt.binding(() => card.editor.captureKeyword)
                    }
                    onAccepted: commit()
                    onEditingFinished: commit()
                }
            }

            Item {
                width: parent.width
                height: Math.max(printTexts.implicitHeight, printToggle.implicitHeight)

                Column {
                    id: printTexts
                    anchors.left: parent.left
                    anchors.right: printToggle.left
                    anchors.rightMargin: 12
                    spacing: 4

                    Text {
                        width: parent.width
                        text: qsTr("用 PrtScn 键截图")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        width: parent.width
                        text: qsTr("让 PrtScn 不再打开 Windows 的截图工具，改用这里的截图。Win顺 没在运行时，PrtScn 会把整个屏幕复制到剪贴板；关掉这项恢复原来的设置")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                    Text {
                        visible: card.editor.captureNote.length > 0
                        width: parent.width
                        text: card.editor.captureNote
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
                ToggleSwitch {
                    id: printToggle
                    anchors.right: parent.right
                    checked: card.editor.capturePrintScreen
                    onToggled: (on) => card.editor.capturePrintScreen = on
                }
            }

            Item {
                width: parent.width
                height: Math.max(copyTexts.implicitHeight, copyToggle.implicitHeight)

                Column {
                    id: copyTexts
                    anchors.left: parent.left
                    anchors.right: copyToggle.left
                    anchors.rightMargin: 12
                    spacing: 4

                    Text {
                        width: parent.width
                        text: qsTr("选好就复制")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        width: parent.width
                        text: qsTr("松开鼠标或点选窗口后马上复制并关闭，不再调整。关着时：按 Enter 或双击选区复制并关闭，Ctrl+S 保存到“图片 › 屏幕截图”，Esc 或右键先取消选区、再退出")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
                ToggleSwitch {
                    id: copyToggle
                    anchors.right: parent.right
                    checked: card.editor.captureCopyOnSelect
                    onToggled: (on) => card.editor.captureCopyOnSelect = on
                }
            }
        }
    }
}
