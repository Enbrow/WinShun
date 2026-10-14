pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Under the search box in 文件: the kinds of file it narrows to, as chips
// (不限, 文件夹, then 文档, 图片… as the settings have them), and + for the
// settings. Shift+Tab goes along them (Launcher.cycleFileType), a chip's tooltip
// lists its extensions. In 内容 the folders it looks in instead
// (Launcher.folderChips: 不限 and the favorite folders, or those a folder
// named among the words stands for), + for the favorites, and after them a
// few words on naming one (Launcher.folderHint). It opens and closes by its height (`shown`), the
// chips coming down from under the search box; the window keeps room for
// all of it meanwhile (see Main.qml), so only the rows below it move. Too
// many for the width, they scroll, the current one kept in view and + after them. The rest
// of it drags the window.
Item {
    id: strip

    required property Launcher launcher
    required property WindowFrame frame
    property bool shown: false

    readonly property int fullHeight: Theme.stripHeight // 界面大小
    readonly property int targetHeight: shown ? fullHeight : 0
    readonly property bool folders: launcher.scope === Launcher.Content
    // The chip that is current, as Launcher.chooseFileType (or chooseFolderChip) takes it.
    readonly property var current: folders ? launcher.folderChip
                                   : launcher.scope === Launcher.Folders ? -2 : launcher.fileType
    // A dot for each, by place (any number of kinds of the user's own).
    readonly property var dotColors: ["#4F9DFF", "#3FBF7F", "#A57CFF", "#FF8A5C", "#C9953A", "#7B8CFF", "#2FBFB4", "#E07BD0"]
    readonly property var chips: folders ? launcher.folderChips : fileChips
    readonly property var fileChips: {
        const list = [{ type: -1, name: qsTr("不限"), tip: qsTr("所有文件和文件夹"), dot: "" },
                      { type: -2, name: qsTr("文件夹"), tip: qsTr("只找文件夹"), dot: "#E8B84A" }]
        const types = launcher.fileTypes
        for (let i = 0; i < types.length; ++i) {
            const exts = types[i].extensions.split(" ")
            const listed = exts.slice(0, 10).map(e => "." + e).join(" ")
            list.push({ type: i, name: types[i].name, tip: exts.length > 10 ? listed + " …" : listed,
                        dot: dotColors[i % dotColors.length] })
        }
        return list
    }

    // The chip the mouse rests on, for its tooltip.
    property Item hoveredChip: null
    property string hoveredTip

    function onPixel(v) { return Math.round(v * Screen.devicePixelRatio) / Screen.devicePixelRatio }

    // Whole logical pixels, also while it moves: the window's height is the
    // sum of this and the room it does not fill yet (Main.qml), and with a
    // fraction in each the sum could come out a pixel short, a native resize
    // on that frame. The rows below glide on whole pixels too.
    // It opens with an animation but closes at once: the window gives its
    // room back in the same go as the click, not in a second jump once it
    // has closed (often the rows below go in that same click, and nothing
    // would be left to glide). Written here, not bound, so that `animated`
    // is set before the Behavior sees the new value.
    property real opening: 0
    property bool animated: false
    onTargetHeightChanged: {
        animated = targetHeight > opening
        opening = targetHeight
    }
    Behavior on opening {
        enabled: strip.animated
        NumberAnimation { duration: 200; easing.type: Easing.OutCubic } // as fast as Windows' own
    }
    height: Math.round(opening)
    clip: true

    Component.onCompleted: {
        opening = targetHeight
        frame.addDragArea(strip)
        frame.addControl(view)
        frame.addControl(edit)
    }

    // Keeps the current chip in view (Tab past the right edge).
    function reveal() {
        const chip = chipRepeater.itemAt(chips.findIndex(c => c.type === current))
        if (!chip || view.contentWidth <= view.width)
            return
        const left = chip.x - 8
        const right = chip.x + chip.width + 8
        let to = view.contentX
        if (left < view.contentX)
            to = Math.max(0, left)
        else if (right > view.contentX + view.width)
            to = Math.min(view.contentWidth - view.width, right - view.width)
        if (to !== view.contentX) {
            slide.to = to
            slide.restart()
        }
    }
    NumberAnimation { id: slide; target: view; property: "contentX"; duration: 150; easing.type: Easing.OutCubic }
    onCurrentChanged: Qt.callLater(reveal)
    onChipsChanged: Qt.callLater(reveal)

    Flickable {
        id: view
        x: 12
        // Comes down from under the search box as the strip opens.
        y: strip.height - strip.fullHeight
        width: Math.min(contentWidth, strip.width - 24 - 4 - edit.width - hint.room) // + stays in view
        height: strip.fullHeight - 6
        contentWidth: row.width
        contentHeight: height
        interactive: contentWidth > width
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        opacity: strip.shown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Row {
            id: row
            y: strip.onPixel((view.height - height) / 2)
            spacing: 4

            Repeater {
                id: chipRepeater
                model: strip.chips

                delegate: Rectangle {
                    id: chip

                    required property var modelData
                    readonly property bool active: modelData.type === strip.current
                    readonly property bool tipWanted: area.containsMouse
                    onTipWantedChanged: {
                        if (tipWanted) {
                            strip.hoveredChip = chip
                            strip.hoveredTip = modelData.tip
                        } else if (strip.hoveredChip === chip) {
                            strip.hoveredChip = null
                        }
                    }

                    // Whole pixels: a half one would leave the labels after it blurred.
                    width: (modelData.dot.length > 0 ? 10 + 8 + 6 : 12) + Math.ceil(label.implicitWidth) + 12
                    height: Theme.chipHeight
                    radius: height / 2
                    color: active ? Theme.chipActive : area.containsMouse ? Theme.chipHover : "transparent"
                    border.width: active ? 1 : 0
                    border.color: Theme.chipBorder
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.name
                    Accessible.checked: active

                    Rectangle {
                        visible: chip.modelData.dot.length > 0
                        x: 10
                        y: strip.onPixel((chip.height - height) / 2)
                        width: 8
                        height: 8
                        radius: 2
                        color: chip.modelData.dot
                    }

                    Text {
                        id: label
                        x: chip.modelData.dot.length > 0 ? 24 : 12
                        y: strip.onPixel((chip.height - height) / 2)
                        text: chip.modelData.name
                        color: chip.active || area.containsMouse ? Theme.text : Theme.subtext
                        font.pixelSize: Theme.uiCaption
                        font.weight: chip.active ? Font.DemiBold : Font.Normal
                    }

                    MouseArea {
                        id: area
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: strip.folders ? strip.launcher.chooseFolderChip(chip.modelData.type)
                                                 : strip.launcher.chooseFileType(chip.modelData.type)
                    }
                }
            }
        }
    }

    Rectangle { // the kinds and their extensions, in the settings
        id: edit

        readonly property string tip: strip.folders ? qsTr("收藏常搜的文件夹，它们会出现在这里") : qsTr("添加或修改类型")
        readonly property bool tipWanted: editArea.containsMouse

        x: view.x + view.width + 4
        y: view.y + row.y
        width: Theme.chipHeight
        height: Theme.chipHeight
        radius: height / 2
        opacity: view.opacity
        color: editArea.pressed ? Theme.controlPressed : editArea.containsMouse ? Theme.chipHover : "transparent"
        Accessible.role: Accessible.Button
        Accessible.name: tip

        Glyph {
            anchors.centerIn: parent
            glyph: "\uE710" // Add
            size: 12
            color: editArea.containsMouse ? Theme.text : Theme.subtext
        }

        MouseArea {
            id: editArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: strip.folders ? strip.launcher.editFolderChips() : strip.launcher.editFileTypes()
        }
    }

    Text { // 内容: how to name a folder, where there is room for it after +
        id: hint
        // Its room, whole pixels; none when it would push the chips into scrolling.
        readonly property int room: shown ? Math.ceil(implicitWidth) + 8 : 0
        readonly property bool shown: strip.folders && text.length > 0
                                      && view.contentWidth + Math.ceil(implicitWidth) + 8 <= strip.width - 24 - 4 - edit.width
        x: edit.x + edit.width + 8
        y: strip.onPixel(edit.y + (edit.height - height) / 2)
        visible: shown
        opacity: view.opacity
        text: strip.launcher.folderHint
        color: Theme.subtext
        font.pixelSize: Theme.uiCaption
    }

    HoverTip {
        target: !strip.shown ? null : edit.tipWanted ? edit : strip.hoveredChip
        text: edit.tipWanted ? edit.tip : strip.hoveredTip
    }
}
