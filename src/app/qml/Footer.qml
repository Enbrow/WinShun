pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// On the left, how the list is chosen (Ctrl+S goes to the next): with nothing
// typed, what was opened last or the files written last; else best match
// first or by when last written, newest or oldest first. Beside it a word
// when there is something to say ("已复制路径"); keyboard hints on the
// right (the three used most; the info button after them lists the rest),
// and at the very right a button that clears the recent items (while there
// are some to clear). The divider above shows work going on.
// Dragging it moves the window.
Item {
    id: footer

    required property Launcher launcher
    required property WindowFrame frame

    implicitHeight: 42

    // The divider shows work only once it has lasted a moment, so brief
    // work does not make it blink.
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

    // The divider, and while work goes on (indexing, reading files for their
    // contents) the bar along it: filled as far as a content scan has come,
    // or a light running along it while how far is not known. Done, it fills
    // up and fades.
    Item {
        id: progressLine

        readonly property bool running: footer.showBusy
        readonly property real fraction: footer.launcher.progress
        property bool determinate: false
        property real shown: 0 // of the width, filled

        function update() {
            if (running && footer.launcher.busy) {
                determinate = fraction >= 0
                if (determinate)
                    shown = fraction
            } else if (determinate) {
                shown = 1
            }
        }
        onRunningChanged: update()
        onFractionChanged: update()

        width: parent.width
        height: 1
        clip: true

        Rectangle {
            anchors.fill: parent
            color: Theme.divider
        }

        Item {
            id: bar
            anchors.fill: parent
            opacity: progressLine.running ? 1 : 0
            Behavior on opacity {
                NumberAnimation { duration: progressLine.running ? 150 : 500; easing.type: Easing.OutCubic }
            }

            Rectangle {
                visible: progressLine.determinate
                width: Math.round(parent.width * progressLine.shown)
                height: parent.height
                color: Theme.accent
                Behavior on width {
                    enabled: bar.opacity > 0.99 // appearing: at once, not back from the last scan's end
                    NumberAnimation { duration: 200; easing.type: Easing.OutCubic }
                }
            }

            Rectangle {
                id: sweep
                visible: !progressLine.determinate
                width: Math.round(parent.width * 0.3)
                height: parent.height
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0) }
                    GradientStop { position: 0.5; color: Theme.accent }
                    GradientStop { position: 1; color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0) }
                }

                NumberAnimation on x {
                    from: -sweep.width
                    to: progressLine.width
                    duration: 1400
                    loops: Animation.Infinite
                    running: sweep.visible && bar.opacity > 0 && footer.Window.active
                }
            }
        }
    }

    Text {
        id: status
        anchors.left: order.visible ? order.right : parent.left
        anchors.leftMargin: order.visible ? 14 : 18
        anchors.right: hints.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: footer.launcher.statusText
        textFormat: Text.PlainText // may name a file
        color: Theme.subtext
        font.pixelSize: Theme.fontBody
        elide: Text.ElideRight
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

    // Nothing typed: which start list. Else the order; not for a plugin
    // command's list, which is in the plugin's.
    readonly property bool atStart: launcher.query.trim().length === 0 && launcher.scope !== Launcher.Content

    Choices {
        id: order
        visible: footer.launcher.pluginKeyword.length === 0
        anchors.left: parent.left
        anchors.leftMargin: 16
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
        anchors.right: info.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 14

        // What the keys do on the row the keyboard is on (Launcher.keyHints:
        // a plugin's row names its own actions, a row with no location has
        // no Ctrl+Enter, Tab completes the row that offers it). Plain words,
        // not "↵": a symbol missing from the UI font triggers system-wide
        // font fallback, which costs tens of MB.
        readonly property var keyHints: footer.launcher.keyHints

        // Hints give way to the status from the left, Enter's first (everyone
        // knows it), then Ctrl+Enter's; Tab's stays. Mostly there is none,
        // and all show. Worked out from the full widths, not the laid-out
        // ones, which depend on it.
        readonly property real statusNeeds: Math.ceil(statusMetrics.advanceWidth)
        readonly property real statusRoom: info.x - 8 - (order.visible ? 16 + order.width + 14 : 18) - 12
        function roomWith(shown) { // the status's, with these hints shown
            const widths = shown.map(h => h.fullWidth).filter(w => w > 0)
            return statusRoom - widths.reduce((a, b) => a + b, 0) - hints.spacing * Math.max(0, widths.length - 1)
        }
        readonly property int given: statusNeeds <= roomWith([enterHint, secondHint, tabHint]) ? 0
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

    // The launcher's other shortcuts, while the mouse rests on it.
    ShortcutsButton {
        id: info

        readonly property string modifier: footer.launcher.numberKeys === "alt" ? "Alt" : "Ctrl"
        shortcuts: [
            ["Shift+Tab", qsTr("切换范围：全部、文件、内容")],
            ["↑ ↓", qsTr("上下移动，PgUp PgDn 翻页")],
            ["Shift+↑ ↓", qsTr("选中多行，也可 Ctrl+单击、Shift+单击")],
            ["Ctrl+Shift+Enter", qsTr("以管理员身份运行")],
            ["Ctrl+C", qsTr("复制文件")],
            ["Ctrl+Shift+C", qsTr("复制路径")],
            ["Ctrl+S", qsTr("切换排序；不输入时切换最近打开、最近修改")]
        ].concat(footer.launcher.numberKeys === "off" ? []
                 : [[modifier + "+1…9", qsTr("打开第几行（图标上标着快捷键）")]])
         .concat([
            ["Shift+F10", qsTr("右键菜单")],
            ["Esc", qsTr("关闭；选中了多行时先取消选择")]
        ])

        host: footer.launcher
        frame: footer.frame
        anchors.right: clearHistory.visible ? clearHistory.left : parent.right
        anchors.rightMargin: clearHistory.visible ? 2 : 6
        anchors.verticalCenter: parent.verticalCenter
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
