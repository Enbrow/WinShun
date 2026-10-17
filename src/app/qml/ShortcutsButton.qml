import QtQuick
import WinShun

// The info button at the right of a footer: the shortcuts the footer has no
// room for, while the mouse rests on it, or at once on a click (ShortcutTip).
// `host` is what the tip's window is placed and styled by (the launcher, the
// clipboard); `frame` the window's, which drags by the footer around it.
Rectangle {
    id: info

    required property QtObject host
    required property WindowFrame frame
    // [[keys, what they do], …]
    property var shortcuts: []

    width: 32
    height: 32
    radius: Theme.controlRadius
    color: infoArea.containsMouse ? Theme.controlHover : "transparent"
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("快捷键")
    Component.onCompleted: info.frame.addControl(info) // a button, not part of the drag area

    Glyph {
        anchors.centerIn: parent
        glyph: "" // Info
        size: 16
        color: infoArea.containsMouse ? Theme.text : Theme.subtext
    }

    MouseArea {
        id: infoArea
        anchors.fill: parent
        hoverEnabled: true
        onContainsMouseChanged: {
            if (containsMouse) {
                tipDelay.restart()
            } else {
                tipDelay.stop()
                if (tipLoader.item)
                    (tipLoader.item as ShortcutTip).visible = false
            }
        }
        onClicked: tipDelay.triggered() // at once
    }

    Timer {
        id: tipDelay
        interval: 250 // as HoverTip
        onTriggered: {
            if (!infoArea.containsMouse)
                return
            tipLoader.active = true
            const tip = tipLoader.item as ShortcutTip
            tip.rows = info.shortcuts
            tip.popup(info)
        }
    }

    Loader { // made when first wanted; gone with the window (see Main.qml's menu)
        id: tipLoader
        active: false
        sourceComponent: ShortcutTip {
            host: info.host
        }
    }

    Connections { // the window hidden: the tip goes too
        target: info.Window.window
        function onVisibleChanged() {
            if (tipLoader.item && !info.Window.window.visible)
                (tipLoader.item as ShortcutTip).visible = false
        }
    }
}
