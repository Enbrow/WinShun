pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The drop pie (DropPie): a ring around the pointer while files are dragged
// and the mouse is shaken. The middle cancels, the inner ring is 移动 (left)
// or 复制 (right), the outer ring the favorite folders; a tip beside the
// pointer and a line under the ring say what letting go would do. The
// window is see-through and never takes the focus; DropPie places it, is
// its drop target, and hides it the moment the pointer leaves the ring.
// With `hint` set it is only that note, beside the pointer.
Window {
    id: pie

    required property DropPie dropPie

    readonly property real dpr: Screen.devicePixelRatio
    readonly property real k: dropPie.ringScale
    readonly property real outerRadius: 226 * k
    readonly property real innerLabelRadius: (58 + 112) / 2 * k
    // The centre on a whole device pixel, so the ring's edges are crisp.
    readonly property real cx: Theme.snap(dropPie.centre.x, dpr)
    readonly property real cy: Theme.snap(dropPie.centre.y, dpr)
    readonly property int labelSize: dropPie.labelPixelSize
    readonly property int smallSize: Math.max(12, labelSize - 1)
    readonly property bool noteOnly: dropPie.hint.length > 0
    property real reveal: 0 // 0 → 1 as it appears; 0 at once when it goes

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: "transparent"

    Connections {
        target: pie.dropPie
        function onRevealedChanged() {
            if (pie.dropPie.revealed) {
                revealAnimation.restart()
            } else {
                revealAnimation.stop()
                pie.reveal = 0
            }
        }
    }

    NumberAnimation {
        id: revealAnimation
        target: pie
        property: "reveal"
        from: 0
        to: 1 // exactly: the ring is drawn unscaled once it is in
        duration: SystemTheme.animations ? 120 : 0
        easing.type: Easing.OutCubic
    }

    // A note by the pointer and nothing else (the drag comes from a program
    // run as administrator).
    Rectangle {
        visible: pie.noteOnly
        x: Theme.snap(4, pie.dpr)
        y: Theme.snap(4, pie.dpr)
        width: Theme.snapUp(noteText.implicitWidth + 24, pie.dpr)
        height: Theme.snapUp(noteText.implicitHeight + 14, pie.dpr)
        radius: Theme.controlRadius
        color: Theme.menuBackground
        border.width: Theme.edge(pie.dpr)
        border.color: Theme.windowEdge

        Text {
            id: noteText
            anchors.centerIn: parent
            text: pie.dropPie.hint
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.fontCaption
        }
    }

    Item {
        id: content

        anchors.fill: parent
        visible: !pie.noteOnly
        opacity: pie.reveal
        transform: Scale {
            origin.x: pie.cx
            origin.y: pie.cy
            xScale: 0.94 + 0.06 * pie.reveal
            yScale: 0.94 + 0.06 * pie.reveal
        }

        PieRing {
            id: ring

            // An even number of device pixels across, so its middle is the
            // centre's whole pixel.
            readonly property int pixels: 2 * Math.ceil((pie.outerRadius + shadow + 2) * pie.dpr)

            x: pie.cx - width / 2
            y: pie.cy - height / 2
            width: pixels / pie.dpr
            height: pixels / pie.dpr
            ringScale: pie.k
            count: Math.max(1, pie.dropPie.sectors.length)
            hovered: pie.dropPie.hovered
            picked: pie.dropPie.picked
            mode: pie.dropPie.mode
            innerHover: pie.dropPie.innerHover
            moveAllowed: pie.dropPie.moveAllowed
            copyAllowed: pie.dropPie.copyAllowed
            disabled: pie.dropPie.sectors.map(s => s.disabled)
            edgeWidth: Theme.edge(pie.dpr)
            shadow: 16
            fill: Theme.menuBackground
            innerFill: Theme.ringInner
            hoverFill: Theme.ringHover
            accentFill: Theme.accentFill
            edge: Theme.windowEdge
            shadowColor: Qt.rgba(0, 0, 0, Theme.dark ? 0.4 : 0.16)
        }

        // The folders' names
        Repeater {
            model: pie.dropPie.sectors

            delegate: Column {
                id: label

                required property var modelData
                required property int index
                readonly property bool picked: index === pie.dropPie.picked
                readonly property color ink: picked ? Theme.accentText : modelData.disabled ? Theme.faint : Theme.text

                x: Theme.snap(pie.cx + modelData.x - width / 2, pie.dpr)
                y: Theme.snap(pie.cy + modelData.y - height / 2, pie.dpr)
                spacing: 2

                Glyph {
                    anchors.horizontalCenter: parent.horizontalCenter
                    glyph: label.modelData.disabled && pie.dropPie.noFolders ? "\uE734" : "\uE8B7" // FavoriteStar, Folder
                    size: 16
                    color: label.ink
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: label.modelData.line1
                    textFormat: Text.PlainText
                    color: label.ink
                    font.pixelSize: pie.labelSize
                }
                Text {
                    visible: label.modelData.line2.length > 0
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: label.modelData.line2
                    textFormat: Text.PlainText
                    color: label.ink
                    font.pixelSize: pie.labelSize
                }
                Text {
                    visible: label.modelData.sub.length > 0
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: label.modelData.sub
                    textFormat: Text.PlainText
                    color: label.picked ? Theme.accentText : Theme.subtext
                    font.pixelSize: pie.smallSize
                }
            }
        }

        // 移动 and 复制
        Repeater {
            model: [
                { name: "move", text: qsTr("移动"), glyph: "\uE72A", side: -1 }, // Forward
                { name: "copy", text: qsTr("复制"), glyph: "\uE8C8", side: 1 } // Copy
            ]

            delegate: Column {
                id: half

                required property var modelData
                readonly property bool allowed: modelData.name === "move" ? pie.dropPie.moveAllowed : pie.dropPie.copyAllowed
                readonly property bool active: allowed && pie.dropPie.mode === modelData.name
                readonly property color ink: active ? Theme.accentText : allowed ? Theme.text : Theme.faint

                x: Theme.snap(pie.cx + modelData.side * pie.innerLabelRadius - width / 2, pie.dpr)
                y: Theme.snap(pie.cy - height / 2, pie.dpr)
                spacing: 2

                Glyph {
                    anchors.horizontalCenter: parent.horizontalCenter
                    glyph: half.modelData.glyph
                    size: 16
                    color: half.ink
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: half.modelData.text
                    color: half.ink
                    font.pixelSize: pie.labelSize + 1
                    font.weight: Font.DemiBold
                }
            }
        }

        // The middle: let go here and nothing happens; or what letting go
        // over the folder picked would do.
        Column {
            readonly property bool picked: pie.dropPie.picked >= 0
            readonly property var sector: picked ? pie.dropPie.sectors[pie.dropPie.picked] : null

            x: Theme.snap(pie.cx - width / 2, pie.dpr)
            y: Theme.snap(pie.cy - height / 2, pie.dpr)
            width: 2 * 50 * pie.k - 16
            spacing: 1

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: parent.picked ? pie.dropPie.tipVerb : qsTr("松开")
                color: Theme.subtext
                font.pixelSize: pie.smallSize
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: parent.sector ? parent.sector.name : qsTr("取消")
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: Theme.text
                font.pixelSize: pie.labelSize + 1
                font.weight: Font.DemiBold
            }
        }

        // What letting go would do, under the ring (above it when the
        // screen ends below).
        Rectangle {
            id: status

            readonly property real below: pie.cy + pie.outerRadius + 14
            readonly property real above: pie.cy - pie.outerRadius - 14 - height

            width: Theme.snap(pie.dropPie.statusWidth, pie.dpr)
            height: Theme.snapUp(statusColumn.implicitHeight + 20, pie.dpr)
            x: Theme.snap(Math.max(4, Math.min(pie.width - width - 4, pie.cx - width / 2)), pie.dpr)
            y: Theme.snap(below + height <= pie.height - 4 ? below : Math.max(4, above), pie.dpr)
            radius: Theme.largeControlRadius
            color: Theme.menuBackground
            border.width: Theme.edge(pie.dpr)
            border.color: Theme.windowEdge

            Column {
                id: statusColumn

                x: 16
                y: Theme.snap((parent.height - height) / 2, pie.dpr)
                width: parent.width - 32
                spacing: 3

                Row {
                    width: parent.width
                    spacing: 8

                    Text {
                        id: statusVerb
                        visible: text.length > 0
                        text: pie.dropPie.statusVerb
                        color: Theme.subtext
                        font.pixelSize: pie.labelSize + 1
                    }
                    Text {
                        width: Math.min(implicitWidth, parent.width - (statusVerb.visible ? statusVerb.width + parent.spacing : 0))
                        text: pie.dropPie.statusName
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        color: Theme.text
                        font.pixelSize: pie.labelSize + 1
                        font.weight: pie.dropPie.hovered >= 0 ? Font.DemiBold : Font.Normal
                    }
                }
                Text {
                    visible: text.length > 0
                    width: parent.width
                    text: pie.dropPie.statusDetail
                    textFormat: Text.PlainText
                    color: Theme.subtext
                    font.pixelSize: pie.smallSize
                }
            }
        }

        // Beside the pointer, on the side away from the ring's centre, kept
        // inside the window.
        Rectangle {
            id: tip

            readonly property real maxWidth: 300 * pie.k
            readonly property real towardsX: pie.dropPie.pointer.x - pie.cx
            readonly property real towardsY: pie.dropPie.pointer.y - pie.cy
            readonly property real wantedX: towardsX >= 0 ? pie.dropPie.pointer.x + 16 : pie.dropPie.pointer.x - 16 - width
            readonly property real wantedY: towardsY >= 0 ? pie.dropPie.pointer.y + 20 : pie.dropPie.pointer.y - 12 - height

            visible: pie.dropPie.tip.length > 0
            width: Theme.snapUp(tipRow.width + 20, pie.dpr)
            height: Theme.snapUp(tipName.implicitHeight + 10, pie.dpr)
            x: Theme.snap(Math.max(4, Math.min(pie.width - width - 4, wantedX)), pie.dpr)
            y: Theme.snap(Math.max(4, Math.min(pie.height - height - 4, wantedY)), pie.dpr)
            radius: Theme.controlRadius
            color: Theme.menuBackground
            border.width: Theme.edge(pie.dpr)
            border.color: Theme.windowEdge

            Row {
                id: tipRow

                x: 10
                y: Theme.snap((parent.height - height) / 2, pie.dpr)
                spacing: 6

                Text {
                    id: tipVerb
                    visible: text.length > 0
                    text: pie.dropPie.tipVerb
                    color: Theme.subtext
                    font.pixelSize: pie.smallSize
                }
                Text {
                    id: tipName
                    width: Math.min(implicitWidth, tip.maxWidth - (tipVerb.visible ? tipVerb.width + tipRow.spacing : 0))
                    text: pie.dropPie.tip
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: Theme.text
                    font.pixelSize: pie.smallSize
                    font.weight: tipVerb.visible ? Font.DemiBold : Font.Normal
                }
            }
        }
    }
}
