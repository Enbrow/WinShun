import QtQuick
import WinShun

// A small tag beside a name, in the accent colour: 随包, AI (PluginCard).
Rectangle {
    id: tag

    property string text

    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
    width: label.implicitWidth + 12
    height: 20
    radius: 3
    color: Theme.badge

    Text {
        id: label
        anchors.centerIn: parent
        text: tag.text
        color: Theme.accent
        font.pixelSize: 12
    }
}
