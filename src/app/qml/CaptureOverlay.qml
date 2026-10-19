pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The screen capture over one monitor (src/capture, Capture): the frozen
// picture shown pixel for pixel, the window or control under the mouse
// outlined, the selection with its size, handles and toolbar, the
// magnifier. All the state is CaptureScreen's, in the monitor's physical
// pixels; px() and py() turn them into this window's logical units with
// Qt's real buffer scale, so edges land on whole screen pixels. No
// animation: it is there at once and gone at once.
Window {
    id: overlay

    required property CaptureSession session
    required property CaptureScreen screenModel

    readonly property real sx: screenModel.scaleX
    readonly property real sy: screenModel.scaleY
    readonly property real dpr: screenModel.dpr
    // Lines one logical pixel thick, as a whole number of screen pixels.
    readonly property int line: Math.max(1, Math.round(dpr))
    readonly property rect selection: screenModel.selection
    readonly property bool selectedHere: selection.width > 0
    readonly property rect hover: screenModel.hover
    readonly property bool hoverHere: hover.width > 0 && !selectedHere
    readonly property rect shown: selectedHere ? selection : hover

    function px(v) { return v * sx }
    function py(v) { return v * sy }

    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
    color: "black"
    title: qsTr("截图")

    CaptureImage { // 1:1: the picture's pixels on the screen's
        x: 0
        y: 0
        width: overlay.px(overlay.screenModel.pixels.width)
        height: overlay.py(overlay.screenModel.pixels.height)
        image: overlay.screenModel.image
    }

    Item { // outside the selection, darker; all of this monitor when the selection is on another
        id: shade

        readonly property color tint: Qt.rgba(0, 0, 0, 0.45)
        readonly property real edgeLeft: overlay.px(overlay.selection.x)
        readonly property real edgeTop: overlay.py(overlay.selection.y)
        readonly property real edgeRight: overlay.px(overlay.selection.x + overlay.selection.width)
        readonly property real edgeBottom: overlay.py(overlay.selection.y + overlay.selection.height)

        anchors.fill: parent
        visible: overlay.screenModel.dimmed

        Rectangle {
            width: parent.width
            height: overlay.selectedHere ? shade.edgeTop : parent.height
            color: shade.tint
        }
        Rectangle {
            visible: overlay.selectedHere
            y: shade.edgeBottom
            width: parent.width
            height: parent.height - shade.edgeBottom
            color: shade.tint
        }
        Rectangle {
            visible: overlay.selectedHere
            y: shade.edgeTop
            width: shade.edgeLeft
            height: shade.edgeBottom - shade.edgeTop
            color: shade.tint
        }
        Rectangle {
            visible: overlay.selectedHere
            x: shade.edgeRight
            y: shade.edgeTop
            width: parent.width - shade.edgeRight
            height: shade.edgeBottom - shade.edgeTop
            color: shade.tint
        }
    }

    Item { // the window or control a click picks: an outline just inside it
        visible: overlay.hoverHere
        x: overlay.px(overlay.hover.x)
        y: overlay.py(overlay.hover.y)
        width: overlay.px(overlay.hover.width)
        height: overlay.py(overlay.hover.height)

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.width: overlay.px(overlay.line)
            border.color: Theme.accent
        }
    }

    Item { // the selection's frame, just outside it, and its eight handles
        id: frame

        readonly property int handle: Math.round(7 * overlay.dpr) | 1 // odd: centred on the edge line

        visible: overlay.selectedHere
        x: overlay.px(overlay.selection.x)
        y: overlay.py(overlay.selection.y)
        width: overlay.px(overlay.selection.width)
        height: overlay.py(overlay.selection.height)

        Rectangle {
            x: -overlay.px(overlay.line)
            y: -overlay.py(overlay.line)
            width: parent.width + 2 * overlay.px(overlay.line)
            height: parent.height + 2 * overlay.py(overlay.line)
            color: "transparent"
            border.width: overlay.px(overlay.line)
            border.color: Theme.accent
        }

        Repeater {
            // Corners and edge middles, as fractions of the selection.
            model: [[0, 0], [0.5, 0], [1, 0], [0, 0.5], [1, 0.5], [0, 1], [0.5, 1], [1, 1]]

            delegate: Rectangle {
                required property var modelData

                // Centred on the edge line (between two screen pixels), on whole pixels.
                readonly property int lineX: Math.round(overlay.selection.width * modelData[0])
                readonly property int lineY: Math.round(overlay.selection.height * modelData[1])

                x: overlay.px(lineX - Math.floor(frame.handle / 2))
                y: overlay.py(lineY - Math.floor(frame.handle / 2))
                width: overlay.px(frame.handle)
                height: overlay.py(frame.handle)
                color: Theme.accentFill
                border.width: overlay.px(1)
                border.color: Theme.accentText
            }
        }
    }

    Rectangle { // the size of what is picked, in screen pixels
        id: sizeLabel

        readonly property point place: overlay.screenModel.placeSizeLabel(overlay.shown, width, height)

        visible: overlay.selectedHere || overlay.hoverHere
        x: Math.round(place.x * overlay.dpr) / overlay.dpr
        y: Math.round(place.y * overlay.dpr) / overlay.dpr
        width: sizeText.implicitWidth + 12
        height: sizeText.implicitHeight + 4
        radius: Theme.smallControlRadius
        color: Theme.accentFill

        Text {
            id: sizeText
            anchors.centerIn: parent
            text: qsTr("%1 × %2").arg(overlay.shown.width).arg(overlay.shown.height)
            color: Theme.accentText
            font.pixelSize: Theme.fontCaption
        }
    }

    Rectangle { // what can be done with the selection
        id: toolbar

        readonly property point place: overlay.screenModel.placeToolbar(overlay.selection, width, height)

        visible: overlay.selectedHere && !overlay.screenModel.adjusting
        x: Math.round(place.x * overlay.dpr) / overlay.dpr
        y: Math.round(place.y * overlay.dpr) / overlay.dpr
        width: tools.implicitWidth + 8
        height: tools.implicitHeight + 8
        radius: Theme.controlRadius
        color: Theme.background
        border.width: 1
        border.color: Theme.windowEdge

        Row {
            id: tools
            anchors.centerIn: parent
            spacing: 2

            FlatButton {
                flat: true
                glyph: "" // Copy
                text: qsTr("复制")
                onClicked: overlay.session.act(CaptureSession.Copy)
            }
            FlatButton {
                flat: true
                glyph: "" // Save
                text: qsTr("保存")
                onClicked: overlay.session.act(CaptureSession.Save)
            }
            FlatButton {
                id: pinButton
                flat: true
                glyph: "" // Pin
                text: qsTr("贴图")
                onClicked: overlay.session.act(CaptureSession.Pin)

                HoverHandler { id: pinHover }
            }
            FlatButton {
                id: ocrButton
                flat: true
                // Without the models it still says why, in its tip and the OCR window.
                opacity: overlay.session.ocrAvailable ? 1 : 0.45
                glyph: "" // Font
                text: qsTr("识别文字")
                onClicked: overlay.session.act(CaptureSession.Ocr)

                HoverHandler { id: ocrHover }
            }
            FlatButton {
                flat: true
                glyph: "" // Cancel
                text: qsTr("取消")
                onClicked: overlay.session.act(CaptureSession.Cancel)
            }
        }
    }

    HoverTip {
        target: !toolbar.visible ? null : pinHover.hovered ? pinButton : ocrHover.hovered ? ocrButton : null
        text: pinHover.hovered ? qsTr("留在屏幕上，可拖动、缩放")
            : overlay.session.ocrAvailable ? qsTr("识别图里的文字，可复制或拿去搜索")
            : qsTr("没有安装文字识别模型（Win顺 文件夹里的 ocr 文件夹），无法识别文字")
        shortcut: pinHover.hovered ? "F3" : ""
    }

    Rectangle { // the magnifier: the pixels round the cursor, where it is, its colour
        id: magnifier

        readonly property int cells: 15 // CaptureScreen::kLoupeCells
        readonly property int cell: Math.max(1, Math.round(8 * overlay.dpr)) // screen pixels per picture pixel
        readonly property point place: overlay.screenModel.placeMagnifier(overlay.screenModel.cursor, width, height)

        visible: overlay.screenModel.cursorHere && overlay.screenModel.hasImage
        x: Math.round(place.x * overlay.dpr) / overlay.dpr
        y: Math.round(place.y * overlay.dpr) / overlay.dpr
        width: loupe.width + 2
        height: loupe.height + 2 + info.implicitHeight + 10
        color: Theme.background
        border.width: 1
        border.color: Theme.windowEdge

        CaptureImage {
            id: loupe
            x: 1
            y: 1
            width: overlay.px(magnifier.cells * magnifier.cell)
            height: overlay.py(magnifier.cells * magnifier.cell)
            image: overlay.screenModel.loupe

            Rectangle { // the cross through the middle row and column
                x: 0
                y: overlay.py((magnifier.cells - 1) / 2 * magnifier.cell)
                width: parent.width
                height: overlay.py(magnifier.cell)
                color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.25)
            }
            Rectangle {
                x: overlay.px((magnifier.cells - 1) / 2 * magnifier.cell)
                y: 0
                width: overlay.px(magnifier.cell)
                height: parent.height
                color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.25)
            }
            Rectangle { // the pixel under the cursor
                x: overlay.px((magnifier.cells - 1) / 2 * magnifier.cell)
                y: overlay.py((magnifier.cells - 1) / 2 * magnifier.cell)
                width: overlay.px(magnifier.cell)
                height: overlay.py(magnifier.cell)
                color: "transparent"
                border.width: overlay.px(1)
                border.color: Theme.accentText
            }
        }

        Column {
            id: info
            x: 8
            anchors.top: loupe.bottom
            anchors.topMargin: 5
            width: parent.width - 16
            spacing: 2

            Text { // the screen pixel, in the virtual desktop's coordinates
                text: qsTr("位置 %1, %2").arg(overlay.screenModel.cursor.x + overlay.screenModel.origin.x)
                                          .arg(overlay.screenModel.cursor.y + overlay.screenModel.origin.y)
                color: Theme.text
                font.pixelSize: Theme.fontCaption
            }
            Row {
                spacing: 6

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12
                    height: 12
                    color: overlay.screenModel.color.length > 0 ? overlay.screenModel.color : "transparent"
                    border.width: 1
                    border.color: Theme.windowEdge
                }
                Text {
                    text: overlay.screenModel.color
                    color: Theme.text
                    font.pixelSize: Theme.fontCaption
                }
            }
            Text {
                width: parent.width
                text: overlay.screenModel.flash.length > 0 ? overlay.screenModel.flash : qsTr("按 C 复制颜色")
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
        }
    }

    MouseArea { // under the toolbar: its buttons take their own clicks
        anchors.fill: parent
        z: -1
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: overlay.screenModel.cursorShape
        onPositionChanged: (mouse) => overlay.session.mouseMoved(overlay.screenModel.index, mouse.x, mouse.y)
        onPressed: (mouse) => overlay.session.mousePressed(overlay.screenModel.index, mouse.x, mouse.y, mouse.button)
        onReleased: (mouse) => overlay.session.mouseReleased(overlay.screenModel.index, mouse.x, mouse.y, mouse.button)
        onDoubleClicked: (mouse) => overlay.session.mouseDoubleClicked(overlay.screenModel.index, mouse.x, mouse.y)
        onWheel: (wheel) => overlay.session.wheeled(overlay.screenModel.index, wheel.x, wheel.y, wheel.angleDelta.y)
    }

    Item {
        focus: true
        Keys.onPressed: (event) => { event.accepted = overlay.session.keyPressed(event.key, event.modifiers) }
    }
}
