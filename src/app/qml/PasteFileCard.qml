import QtQuick
import WinShun

// 粘贴为文件 ([PasteFile] in WinShun.ini; the file is made by
// WinShunSearch.exe, src/stub/PasteAsFile.cpp): the words or the picture
// on the clipboard saved as a file where the user is. Off until switched on;
// off, no key is watched for it and the right-click menu has no verb. Its
// two ways in fold under 设置, and one of them stays on (SettingsEditor).
BuiltinCard {
    id: card

    required property SettingsEditor editor
    // What the settings search finds it by, besides its name.
    readonly property var searchTerms: qsTr("粘贴为文件;PasteEx;Paste As File;剪贴板;新建文本文档;保存图片;另存为文件;txt;png;Ctrl+V;右键菜单").split(";")

    // One way in: its words on the left, its switch on the right.
    component WayRow: Item {
        id: way

        property string title
        property string words
        property bool checked
        property bool last: false // the other way is off: this one stays
        signal toggled(bool on)

        width: parent.width
        height: Math.max(wayTexts.implicitHeight, wayToggle.implicitHeight)

        Column {
            id: wayTexts
            anchors.left: parent.left
            anchors.right: wayToggle.left
            anchors.rightMargin: 12
            spacing: 4

            Text {
                width: parent.width
                text: way.title
                color: Theme.text
                font.pixelSize: Theme.fontBody
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                text: way.words
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }
        ToggleSwitch {
            id: wayToggle
            anchors.right: parent.right
            enabled: !way.last
            opacity: enabled ? 1 : 0.5
            checked: way.checked
            Accessible.name: way.title
            onToggled: (on) => way.toggled(on)
        }
    }

    objectName: "pasteFileSettingsCard"
    glyph: String.fromCharCode(0xE77F) // Paste
    title: qsTr("粘贴为文件")
    description: qsTr("把剪贴板里的文字或图片直接存成文件，不用先新建一个文本文档再粘贴")
    status: !editor.pasteFileEnabled ? qsTr("关着")
            : editor.pasteFileCtrlV && editor.pasteFileMenu ? qsTr("开着：在资源管理器或桌面按 Ctrl+V，或右键空白处选“粘贴为文件”")
            : editor.pasteFileCtrlV ? qsTr("开着：在资源管理器或桌面按 Ctrl+V")
            : qsTr("开着：在文件夹或桌面的空白处右键，选“粘贴为文件”")
    checked: editor.pasteFileEnabled
    onToggled: (on) => editor.pasteFileEnabled = on

    WayRow {
        title: qsTr("在资源管理器和桌面按 Ctrl+V")
        words: qsTr("剪贴板里是文字或图片时存成文件；复制的文件照旧由资源管理器粘贴，改名和地址栏里的 Ctrl+V 也不受影响")
        checked: card.editor.pasteFileCtrlV
        last: checked && !card.editor.pasteFileMenu
        onToggled: (on) => card.editor.pasteFileCtrlV = on
    }
    WayRow {
        title: qsTr("右键菜单里的“粘贴为文件”")
        words: qsTr("在文件夹或桌面的空白处右键；Windows 11 在“显示更多选项”里")
        checked: card.editor.pasteFileMenu
        last: checked && !card.editor.pasteFileCtrlV
        onToggled: (on) => card.editor.pasteFileMenu = on
    }
    Text {
        width: parent.width
        text: (card.editor.pasteFileCtrlV && card.editor.pasteFileMenu ? "" : qsTr("两种方式至少留一种；都不用时关掉上面的开关。"))
              + qsTr("文字按内容存成 .txt、.csv（表格）、.json、.html 等，图片存成 .png；文件名取第一行或当前时间，存好后直接改名。")
        color: Theme.subtext
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
    }
}
