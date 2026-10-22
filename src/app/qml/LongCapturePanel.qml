pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The long screenshot's panel (LongCapture; docs/design-capture-roadmap.md,
// 4.2): beside the region, made with the first long screenshot and hidden
// after. It never takes the focus (the program under the region keeps it,
// with the wheel and the keys) and is left out of every capture. The end of
// the picture so far, what is going on, the height, auto-scroll, and 完成 /
// 取消 / 复制诊断信息.
Window {
    id: panel

    required property LongCapture capture
    // The border round the region (drawn natively by LongCapture) takes this grey.
    readonly property color edgeColor: Theme.windowEdge
    readonly property int padding: 16
    readonly property real dpr: Screen.devicePixelRatio
    readonly property var statusWords: [qsTr("等待"), qsTr("拼接中"), qsTr("往回滚了"), qsTr("跟丢了"), qsTr("到上限")]

    function onPixel(v) { return Math.round(v * dpr) / dpr }

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.background
    title: qsTr("长截图") // Qt adds " - Win顺"
    width: capture.previewBox.width + 2 * padding
    height: column.implicitHeight + 2 * padding

    FontMetrics { id: captionMetrics; font.pixelSize: Theme.fontCaption }

    Column {
        id: column
        x: panel.padding
        y: panel.padding
        width: panel.width - 2 * panel.padding
        spacing: 8

        Item {
            width: parent.width
            height: title.implicitHeight

            Text {
                id: title
                text: qsTr("长截图")
                color: Theme.text
                font.pixelSize: Theme.uiTitle
                font.weight: Font.DemiBold
            }
            Text {
                anchors.right: parent.right
                anchors.baseline: title.baseline
                text: panel.statusWords[panel.capture.status] ?? ""
                color: panel.capture.status === LongCapture.Lost ? Theme.danger : Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
        }

        Item { // the end of the picture so far, 1:1 on the screen's pixels, framed
            width: parent.width
            height: panel.capture.previewBox.height + 2

            CaptureImage {
                id: shot
                x: panel.onPixel((parent.width - width) / 2)
                y: panel.onPixel(1)
                width: panel.capture.previewSize.width / panel.dpr
                height: panel.capture.previewSize.height / panel.dpr
                image: panel.capture.preview
            }
            Rectangle {
                visible: panel.capture.hasPreview
                x: shot.x - 1
                y: shot.y - 1
                width: shot.width + 2
                height: shot.height + 2
                color: "transparent"
                border.width: 1
                border.color: Theme.windowEdge
            }
            Text {
                anchors.centerIn: parent
                visible: !panel.capture.hasPreview
                text: qsTr("等第一帧…")
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
            }
        }

        Text {
            text: panel.capture.partial ? qsTr("高 %1 像素，有一段是推测着接上的").arg(panel.capture.pictureHeight)
                                        : qsTr("高 %1 像素").arg(panel.capture.pictureHeight)
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
        }

        Item {
            width: parent.width
            height: autoSwitch.implicitHeight

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("自动滚动")
                color: panel.capture.canAutoScroll ? Theme.text : Theme.subtext
                font.pixelSize: Theme.fontBody
            }
            ToggleSwitch {
                id: autoSwitch
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                word: false
                enabled: panel.capture.canAutoScroll
                opacity: enabled ? 1 : 0.45
                checked: panel.capture.autoScroll && panel.capture.canAutoScroll
                onToggled: (on) => panel.capture.autoScroll = on
            }
        }

        Text { // two lines' room, so that the panel keeps its size
            width: parent.width
            height: 2 * Math.ceil(captionMetrics.height)
            text: panel.capture.hint
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }

        Item {
            width: parent.width
            height: doneButton.implicitHeight

            FlatButton {
                flat: true
                keyboardActivation: false
                text: qsTr("复制诊断信息")
                onClicked: panel.capture.copyDiagnostics()
            }
            Row {
                anchors.right: parent.right
                spacing: 8

                FlatButton {
                    keyboardActivation: false
                    text: qsTr("取消")
                    onClicked: panel.capture.cancel()
                }
                FlatButton {
                    id: doneButton
                    highlighted: true
                    keyboardActivation: false
                    text: qsTr("完成")
                    onClicked: panel.capture.finish()
                }
            }
        }
    }

    WindowEdge {}
}
