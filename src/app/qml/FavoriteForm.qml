import QtQuick
import WinShun

// Changing a favorite in the settings (收藏夹): its name and its keyword,
// and its place in the list for the keyboard (上移, 下移; the mouse drags the
// grip). FavoritesSettings checks and saves it, or says what is wrong. Enter
// saves, Esc puts it away.
Item {
    id: form

    required property FavoritesSettings favorites
    required property int index
    required property var favorite // as FavoritesSettings.items has it
    required property int count // of favorites
    // What was typed and not saved yet, {name, keyword}, when the form is
    // made again (moved with 上移, the list redone): null for none.
    property var draft: null
    property string error
    property bool ready: false // the fields filled in

    signal finished()
    signal edited(string name, string keyword) // typed: kept as the draft
    signal moveRequested(int to) // 上移 or 下移: the form goes with it

    function save() {
        form.error = form.favorites.save(form.index, nameField.text, keywordField.text)
        if (form.error.length === 0)
            form.finished()
    }

    implicitHeight: fields.implicitHeight + 28
    Keys.onEscapePressed: form.finished()
    Component.onCompleted: {
        nameField.text = form.draft ? form.draft.name : form.favorite.name === form.favorite.ownName ? "" : form.favorite.name
        keywordField.text = form.draft ? form.draft.keyword : form.favorite.keyword
        form.ready = true
        nameField.takeFocus()
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
        radius: Theme.smallControlRadius
        color: Theme.navHover
    }

    Column {
        id: fields
        x: 14
        y: 14
        width: parent.width - 28
        spacing: 14

        Text {
            width: parent.width
            text: form.favorite.shownPath
            textFormat: Text.PlainText
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideMiddle
        }

        Row {
            width: parent.width
            spacing: 12

            Field {
                id: nameField
                width: parent.width - keywordField.width - parent.spacing
                label: qsTr("名称")
                placeholder: form.favorite.ownName
                hint: qsTr("不填就用它自己的名字")
                onTextChanged: if (form.ready) form.edited(nameField.text, keywordField.text)
                onAccepted: form.save()
            }
            Field {
                id: keywordField
                width: 200
                label: qsTr("关键词")
                placeholder: qsTr("可选，例如 gz")
                hint: qsTr("只输入它，按 Enter 就打开")
                onTextChanged: if (form.ready) form.edited(nameField.text, keywordField.text)
                onAccepted: form.save()
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

        Item {
            width: parent.width
            height: buttons.height

            Row {
                id: buttons
                spacing: 8

                FlatButton {
                    highlighted: true
                    text: qsTr("保存")
                    onClicked: form.save()
                }
                FlatButton {
                    text: qsTr("取消")
                    onClicked: form.finished()
                }
            }
            Row {
                anchors.right: parent.right
                spacing: 2

                FlatButton {
                    flat: true
                    enabled: form.index > 0
                    text: qsTr("上移")
                    glyph: "\uE74A" // Up
                    onClicked: form.moveRequested(form.index - 1)
                }
                FlatButton {
                    flat: true
                    enabled: form.index < form.count - 1
                    text: qsTr("下移")
                    glyph: "\uE74B" // Down
                    onClicked: form.moveRequested(form.index + 1)
                }
            }
        }
    }
}
