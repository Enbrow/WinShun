pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// One row of the search panel (Panel.qml): a line in columns, as a file
// manager lists files: icon and name, the folder it is in (an app: where it
// comes from; a content match: the matching line), its size, when it was
// last written. The columns' places come from the panel (its header shows
// them). Clicks, the menu, dragging out and the selection work as on a
// ResultRow; the buttons a ResultRow shows are the menu's here.
Item {
    id: row

    required property int index
    required property string name
    required property string nameHtml
    required property var nameMatches
    required property string folder
    required property var folderMatches
    required property string path
    required property bool isDir
    required property string icon
    required property string snippetHtml
    required property int line
    required property string location
    required property bool isApp
    required property bool place
    required property bool web
    required property bool plugin
    required property string source
    required property bool selected
    required property string size // "1.2 MB"; empty for a folder, an app, or not known
    required property string modifiedDate // "2026-10-14 14:32"; empty when not known
    required property bool offline
    required property bool missing
    required property bool favorite
    required property bool recent
    // Where the columns start and how wide they are (Panel.qml's header).
    property real nameX: 12
    property real pathX: 300
    property real sizeX: 600
    property real dateX: 700
    property real sizeWidth: 90
    property real dateWidth: 140

    readonly property bool contentMode: line > 0
    readonly property bool current: ListView.isCurrentItem
    readonly property int selectedCount: ListView.view ? ListView.view.model.selectedCount : 0
    readonly property var results: ListView.view ? ListView.view.model : null

    signal clicked(int index, int modifiers)
    signal menuRequested(int index, point globalPos, int modifiers)
    signal dragRequested(int index)

    function onPixel(y) { return Math.round(y * Screen.devicePixelRatio) / Screen.devicePixelRatio }

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        radius: 4
        color: row.selected || (row.current && row.selectedCount === 0) ? Theme.selection
               : area.containsMouse ? Theme.hover : "transparent"
    }

    Rectangle { // accent pill on the current row, as in Windows 11 lists
        visible: row.current
        x: 7
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: Math.round(row.height * 0.5)
        radius: 1.5
        color: Theme.accent
    }

    Image { // 20 logical pixels: 30 device pixels at 150 %, its own pixels unsmoothed as the box's icons
        id: iconImage

        readonly property real dpr: Screen.devicePixelRatio
        readonly property int pixels: Math.round(20 * dpr)

        x: row.nameX + 1 / 64
        y: row.onPixel((row.height - height) / 2) + 1 / 64
        width: pixels / dpr
        height: pixels / dpr
        opacity: row.offline || row.missing ? 0.6 : 1
        source: row.icon
        sourceSize.width: pixels / dpr
        sourceSize.height: pixels / dpr
        smooth: false
        asynchronous: true
        fillMode: Image.PreserveAspectFit
    }

    Text { // the name, shortened around its matches
        id: title
        x: row.nameX + 28
        width: Math.max(0, row.pathX - 12 - x)
        y: row.onPixel((row.height - height) / 2)
        opacity: row.offline || row.missing ? 0.6 : 1
        text: row.results && row.nameMatches.length > 0
              ? row.results.elidedHtml(row.name, row.nameMatches, width, font, row.results.highlightColor)
              : row.nameHtml
        textFormat: Text.StyledText
        elide: Text.ElideRight
        maximumLineCount: 1
        color: Theme.text
        font.pixelSize: Theme.uiTitle
    }

    Text { // the folder (or the matching line; an app's origin), its matches in sight
        readonly property bool marked: !row.contentMode && row.folderMatches.length > 0 && row.results !== null
        x: row.pathX
        width: Math.max(0, row.sizeX - 12 - x)
        y: row.onPixel((row.height - height) / 2)
        text: row.contentMode ? row.snippetHtml
            : marked ? row.results.elidedHtml(row.folder, row.folderMatches, width, font, row.results.highlightColor)
            : row.folder
        textFormat: row.contentMode || marked ? Text.StyledText : Text.PlainText
        elide: row.contentMode || marked ? Text.ElideRight : Text.ElideMiddle
        maximumLineCount: 1
        color: Theme.subtext
        font.pixelSize: Theme.uiBody
    }

    Text { // the size, right-aligned under its header; a tag instead for an app, a place, a plugin's row
        x: row.sizeX
        width: row.sizeWidth
        y: row.onPixel((row.height - height) / 2)
        horizontalAlignment: Text.AlignRight
        text: row.contentMode ? row.location
            : row.isApp ? (row.plugin ? row.source : row.web ? qsTr("网页") : row.place ? qsTr("系统") : qsTr("应用"))
            : row.offline ? qsTr("离线") : row.missing ? qsTr("已不存在") : row.size
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: row.isApp && !row.contentMode ? Theme.accent : Theme.subtext
        font.pixelSize: Theme.uiBody
    }

    Text { // when last written
        x: row.dateX
        width: row.dateWidth
        y: row.onPixel((row.height - height) / 2)
        text: row.modifiedDate
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: Theme.subtext
        font.pixelSize: Theme.uiBody
    }

    MouseArea {
        id: area

        // As ResultRow: held and moved past the drag distance, the file is
        // dragged out of the window.
        property point pressedAt: Qt.point(0, 0)
        property bool draggedOut: false

        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: (mouse) => {
            draggedOut = false
            pressedAt = Qt.point(mouse.x, mouse.y)
            preventStealing = mouse.button === Qt.LeftButton && !row.isApp && !row.offline && !row.missing
                              && mouse.source === Qt.MouseEventNotSynthesized
        }
        onPositionChanged: (mouse) => {
            if (!pressed || !preventStealing || draggedOut || !(mouse.buttons & Qt.LeftButton))
                return
            if (Math.hypot(mouse.x - pressedAt.x, mouse.y - pressedAt.y) < Application.styleHints.startDragDistance)
                return
            draggedOut = true
            row.dragRequested(row.index) // returns once it was let go
        }
        onClicked: (mouse) => {
            if (draggedOut)
                return
            if (mouse.button === Qt.RightButton)
                row.menuRequested(row.index, area.mapToGlobal(mouse.x, mouse.y), mouse.modifiers)
            else
                row.clicked(row.index, mouse.modifiers)
        }
    }

    HoverTip { // the whole path, and the time in full
        target: area.containsMouse && !row.isApp && !row.contentMode ? row : null
        text: row.path
        delay: 700
    }
}
