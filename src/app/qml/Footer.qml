pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Status on the left, keyboard hints on the right, and at the very right a
// button that clears the recent items (while there are some to clear).
// Before the hints, how the list is chosen (Ctrl+S goes to the next): with
// nothing typed, what was opened last or the files written last; in 文件 and
// 文件夹, best match first or by when last written, newest or oldest first.
// Dragging it moves the window.
Item {
    id: footer

    required property Launcher launcher
    required property WindowFrame frame

    implicitHeight: 42

    // The activity dot appears only once the work has lasted a moment, so
    // brief work does not make it (and the text beside it) blink.
    property bool showBusy: false
    readonly property bool busy: launcher.busy
    onBusyChanged: {
        if (busy) {
            busyDelay.start()
        } else {
            busyDelay.stop()
            showBusy = false
        }
    }
    Component.onCompleted: {
        showBusy = busy
        frame.addDragArea(footer)
    }
    Timer {
        id: busyDelay
        interval: 400
        onTriggered: footer.showBusy = footer.busy
    }

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.divider
    }

    Row {
        id: status
        anchors.left: parent.left
        anchors.leftMargin: 18
        anchors.right: order.visible ? order.left : hints.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        Rectangle { // activity dot while indexing or scanning file contents
            visible: footer.showBusy
            anchors.verticalCenter: parent.verticalCenter
            width: 6
            height: 6
            radius: 3
            color: Theme.accent

            SequentialAnimation on opacity {
                running: footer.showBusy && footer.Window.active
                loops: Animation.Infinite
                NumberAnimation { to: 0.25; duration: 600 }
                NumberAnimation { to: 1; duration: 600 }
            }
        }

        Text {
            width: status.width - (footer.showBusy ? 14 : 0)
            text: footer.launcher.statusText
            textFormat: Text.PlainText // may name a file
            color: Theme.subtext
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
    }

    // A few choices side by side, the current one raised (as ScopeTabs, smaller).
    component Choices: Row {
        id: choices

        property alias labels: options.model
        property int current: 0
        property string glyph
        property string tip
        readonly property bool tipWanted: hover.hovered

        signal activated(int index)

        spacing: 6

        HoverHandler { id: hover }

        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            glyph: choices.glyph
            size: 16
            color: Theme.faint
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: chips.implicitWidth + 4
            height: 28
            radius: 6
            color: Theme.track

            Row {
                id: chips
                x: 2
                y: 2
                spacing: 2

                Repeater {
                    id: options

                    delegate: Rectangle {
                        id: chip

                        required property int index
                        required property string modelData
                        readonly property bool active: index === choices.current

                        width: Math.ceil(label.implicitWidth) + 16 // whole pixels: the labels after it stay sharp
                        height: 24
                        radius: 4
                        color: active ? Theme.chipActive : area.containsMouse ? Theme.chipHover : "transparent"
                        border.width: active ? 1 : 0
                        border.color: Theme.chipBorder
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData
                        Accessible.checked: active

                        Text {
                            id: label
                            anchors.centerIn: parent
                            text: chip.modelData
                            color: chip.active || area.containsMouse ? Theme.text : Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            font.weight: chip.active ? Font.DemiBold : Font.Normal
                        }

                        MouseArea {
                            id: area
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: choices.activated(chip.index)
                        }
                    }
                }
            }
        }
    }

    // Nothing typed: which start list. Else, in 文件 and 文件夹: the order.
    readonly property bool atStart: launcher.query.trim().length === 0 && launcher.scope !== Launcher.Content
    readonly property bool sortable: launcher.scope === Launcher.Files || launcher.scope === Launcher.Folders

    Choices {
        id: order
        visible: footer.atStart || footer.sortable
        anchors.right: hints.left
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        glyph: footer.atStart ? "\uE81C" : "\uE8CB" // History, Sort
        labels: footer.atStart ? [qsTr("最近打开"), qsTr("最近修改")]
                                 : [qsTr("最匹配"), qsTr("最新"), qsTr("最早")]
        current: footer.atStart ? footer.launcher.startList : footer.launcher.sort
        tip: footer.atStart ? qsTr("不输入时列出最近打开的，或最近修改的文件")
                              : qsTr("最匹配的在前，或按修改时间：最新的在前、最早的在前")
        onActivated: (index) => {
            if (footer.atStart)
                footer.launcher.startList = index
            else
                footer.launcher.sort = index
        }
        Component.onCompleted: footer.frame.addControl(order) // buttons, not part of the drag area
    }

    Row {
        id: hints
        anchors.right: clearHistory.visible ? clearHistory.left : parent.right
        anchors.rightMargin: clearHistory.visible ? 10 : 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 14

        // What the keys do on the row the keyboard is on (Launcher.keyHints:
        // a plugin's row names its own actions, a row with no location has
        // no Ctrl+Enter, Tab completes the row that offers it). Plain words,
        // not "↵": a symbol missing from the UI font triggers system-wide
        // font fallback, which costs tens of MB.
        readonly property var keyHints: footer.launcher.keyHints

        // Hints give way to the status from the left, Enter's first (everyone
        // knows it), then Ctrl+Enter's; Tab's stays. Enter's always goes with
        // the switch in: the status keeps room for "123,456 个结果 · 158 毫秒"
        // (188 px; 135 were left, 89 in English). Worked out from the full
        // widths, not the laid-out ones, which depend on it.
        readonly property real statusNeeds: Math.ceil(statusMetrics.advanceWidth) + (footer.showBusy ? 14 : 0)
        readonly property real statusRoom: (clearHistory.visible ? footer.width - 6 - clearHistory.width - 10 : footer.width - 16)
                                           - (order.visible ? order.width + 14 : 0) - 12 - 18
        function roomWith(shown) { // the status's, with these hints shown
            const widths = shown.map(h => h.fullWidth).filter(w => w > 0)
            return statusRoom - widths.reduce((a, b) => a + b, 0) - hints.spacing * Math.max(0, widths.length - 1)
        }
        readonly property int given: !order.visible && statusNeeds <= roomWith([enterHint, secondHint, tabHint]) ? 0
                                     : statusNeeds <= roomWith([secondHint, tabHint]) ? 1 : 2

        TextMetrics {
            id: statusMetrics
            font.pixelSize: Theme.fontBody
            text: footer.launcher.statusText
        }

        KeyHint { id: enterHint; visible: hints.given < 1 && label.length > 0; keys: "Enter"; label: hints.keyHints.enter ?? "" }
        KeyHint { id: secondHint; visible: hints.given < 2 && label.length > 0; keys: hints.keyHints.secondKeys ?? ""; label: hints.keyHints.second ?? "" }
        KeyHint { id: tabHint; visible: label.length > 0; keys: "Tab"; label: hints.keyHints.tab ?? "" }
    }

    // Two clicks, as a row's delete button: the first arms it.
    RowAction {
        id: clearHistory

        property bool armed: false

        visible: footer.launcher.canClearHistory
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        glyph: "\uE74D" // Delete
        tip: armed ? "" : qsTr("清除最近使用记录")
        text: armed ? qsTr("确认清除") : ""
        danger: armed
        onClicked: {
            if (armed) {
                armed = false
                footer.launcher.clearHistory()
            } else {
                armed = true
                disarm.restart()
            }
        }
        onVisibleChanged: armed = false
        Component.onCompleted: footer.frame.addControl(clearHistory) // a button, not part of the drag area

        Timer {
            id: disarm
            interval: 3000
            onTriggered: clearHistory.armed = false
        }
    }

    HoverTip {
        target: clearHistory.tipWanted ? clearHistory : order.tipWanted && order.visible ? order : null
        text: clearHistory.tipWanted ? clearHistory.tip : order.tip
        shortcut: clearHistory.tipWanted ? "" : "Ctrl+S"
    }
}
