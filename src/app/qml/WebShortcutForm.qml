import QtQuick
import WinShun

// Adding or changing a web shortcut in the settings (网页搜索): its keyword,
// its name, the address it searches with %s where the words go, and the page
// the keyword alone opens, and a picture of the user's for its icon.
// SettingsEditor checks and saves it, or says what is wrong. Enter saves,
// Esc puts it away.
Item {
    id: form

    required property SettingsEditor editor
    property int index: -1 // the one being changed; -1: a new one
    property var shortcut: ({ keyword: "", name: "", url: "", home: "", icon: "", homeUrl: "" })
    property string error
    property string icon: form.shortcut.icon || "" // a picture chosen (SettingsEditor::pickWebIcon); empty: the site's

    signal finished()

    function save() {
        form.error = form.editor.saveWebShortcut(form.index, keywordField.text, nameField.text, urlField.text,
                                                 homeField.text, form.icon)
        if (form.error.length === 0)
            form.finished()
    }

    implicitHeight: fields.implicitHeight + 28
    Keys.onEscapePressed: form.finished()
    Component.onCompleted: {
        const first = form.index < 0 ? keywordField : urlField
        first.takeFocus()
    }

    // A box with its label above and, if any, a hint below.
    component Field: Column {
        id: field

        property string label
        property string hint
        property alias text: box.text
        property alias placeholder: box.placeholder

        signal accepted()

        function takeFocus() { box.takeFocus() }

        spacing: 6

        Text {
            text: field.label
            color: Theme.text
            font.pixelSize: Theme.fontCaption
        }
        InputBox {
            id: box
            width: field.width
            onAccepted: field.accepted()
        }
        Text {
            visible: field.hint.length > 0
            width: field.width
            text: field.hint
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
            lineHeight: 1.15
        }
    }

    Rectangle { // sets the form apart from the rows around it
        anchors.fill: parent
        anchors.margins: 1
        radius: 3
        color: Theme.navHover
    }

    Column {
        id: fields
        x: 14
        y: 14
        width: parent.width - 28
        spacing: 14

        Row {
            width: parent.width
            spacing: 12

            Field {
                id: keywordField
                width: 140
                label: qsTr("关键词")
                placeholder: qsTr("例如 gh")
                text: form.shortcut.keyword
                onAccepted: form.save()
            }
            Field {
                id: nameField
                width: parent.width - keywordField.width - parent.spacing
                label: qsTr("名称")
                placeholder: qsTr("例如 GitHub；不填就用网站的域名")
                text: form.shortcut.name
                onAccepted: form.save()
            }
        }

        Field {
            id: urlField
            width: parent.width
            label: qsTr("搜索网址")
            placeholder: "https://github.com/search?q=%s"
            hint: qsTr("要搜的文字用 %s 代替。最简单的办法：在这个网站上搜索 %s，再把浏览器地址栏里的网址整个粘贴过来。没有 %s 的网址只能打开，不能搜索")
            text: form.shortcut.url
            onAccepted: form.save()
        }

        Field {
            id: homeField
            width: parent.width
            label: qsTr("主页（可不填）")
            placeholder: qsTr("只输入关键词时打开的网址；不填就打开搜索网址所在网站的首页")
            text: form.shortcut.home
            onAccepted: form.save()
        }

        Column {
            width: parent.width
            spacing: 6

            Text {
                text: qsTr("图标")
                color: Theme.text
                font.pixelSize: Theme.fontCaption
            }
            Row {
                spacing: 10

                Item { // as its rows will show it
                    id: iconPreview
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24
                    height: 24

                    Image {
                        readonly property real dpr: Screen.devicePixelRatio
                        readonly property int pixels: Math.round(iconPreview.width * dpr)

                        // Exactly its device pixels, unsmoothed (as in ResultRow).
                        x: 1 / 64
                        y: 1 / 64
                        width: pixels / dpr
                        height: pixels / dpr
                        sourceSize.width: pixels / dpr
                        sourceSize.height: pixels / dpr
                        smooth: false
                        asynchronous: true
                        // A new one has no site yet: the browser's icon ("https:" has no host).
                        source: form.editor.siteIconsArrived >= 0
                                ? form.editor.webIconUrl(form.icon.length > 0 ? "custom:" + form.icon
                                                                             : (form.shortcut.homeUrl || "https:"),
                                                         Theme.dark)
                                : ""
                    }
                }
                FlatButton {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("选择图片…")
                    onClicked: {
                        const picked = form.editor.pickWebIcon()
                        if (picked.error) {
                            form.error = picked.error
                        } else if (picked.icon) {
                            form.icon = picked.icon
                            form.error = ""
                        }
                    }
                }
                FlatButton {
                    visible: form.icon.length > 0
                    anchors.verticalCenter: parent.verticalCenter
                    flat: true
                    text: qsTr("用网站的图标")
                    onClicked: form.icon = ""
                }
            }
            Text {
                width: parent.width
                text: qsTr("PNG 或 ICO，方形最好；不选就用网站自己的图标")
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }

        Text {
            visible: form.error.length > 0
            width: parent.width
            text: form.error
            textFormat: Text.PlainText
            color: Theme.danger
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }

        Row {
            spacing: 8

            FlatButton {
                highlighted: true
                text: form.index < 0 ? qsTr("添加") : qsTr("保存")
                onClicked: form.save()
            }
            FlatButton {
                text: qsTr("取消")
                onClicked: form.finished()
            }
        }
    }
}
