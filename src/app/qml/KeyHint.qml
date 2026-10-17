import QtQuick
import WinShun

// "[Ctrl ↵] 打开位置" in the footer.
Row {
    id: hint

    property string keys
    property string label
    // Its width when shown, also while hidden (a hidden Row lays out nothing).
    readonly property real fullWidth: label.length > 0 ? keyText.implicitWidth + 10 + spacing + labelText.implicitWidth : 0

    spacing: 5

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: keyText.implicitWidth + 10
        height: Theme.keycapHeight
        radius: Theme.controlRadius
        color: Theme.keycap
        border.width: 1
        border.color: Theme.keycapBorder

        Text {
            id: keyText
            anchors.centerIn: parent
            text: hint.keys
            color: Theme.subtext
            font.pixelSize: Theme.uiCaption
        }
    }

    Text {
        id: labelText
        anchors.verticalCenter: parent.verticalCenter
        text: hint.label
        color: Theme.faint
        font.pixelSize: Theme.uiBody
    }
}
