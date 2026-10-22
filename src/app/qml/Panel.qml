pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The launcher grown into the search panel (Launcher.panel; the forum's
// #209, as Listary has it): the search box on top with the button that
// collapses it again at the right, filters down the left (范围, 类型, 修改
// 时间, 大小, 位置: the engine applies them, see NameSearchOptions::filter),
// and the rows in columns on the right (名称, 路径, 大小, 修改时间), each
// header sorting the whole list. The same window, query, scope and rows as
// the box: Main.qml hands the keys and the rows' events over as it does
// for its own list (`view` here is that list while the panel shows).
Item {
    id: panel

    required property Launcher launcher
    required property WindowFrame frame
    required property Placement placement
    property NameTip nameTip: null // the window's, for a name or folder cut short (Main.qml)
    readonly property alias view: list
    readonly property alias searchBar: bar
    readonly property int rowHeight: Theme.panelRowHeight
    // The rows are the last query's while a slow search has found nothing yet (Main.qml's dimRows).
    property bool dimmed: false

    signal keyPressed(var event)
    signal rowClicked(int index, int modifiers)
    signal rowDoubleClicked(int index, int modifiers)
    signal rowMenuRequested(int index, point globalPos, int modifiers)
    signal rowDragRequested(int index)

    readonly property int sidebarWidth: 290
    // The columns, from the list's left: the name has half of what the size
    // and the time leave, the path the other half.
    readonly property real nameX: 12
    readonly property real sizeWidth: 100
    readonly property real dateWidth: 152
    readonly property real dateX: list.width - 12 - dateWidth
    readonly property real sizeX: dateX - 10 - sizeWidth
    readonly property real pathX: nameX + 28 + Math.round((sizeX - 12 - nameX - 28) * 0.5)

    function onPixel(v) { return Math.round(v * Screen.devicePixelRatio) / Screen.devicePixelRatio }

    // Fades in as the window grows into it (the window is resized once;
    // the rows then come in over it).
    onVisibleChanged: if (visible) fadeIn.restart()
    NumberAnimation { id: fadeIn; target: panel; property: "opacity"; from: 0; to: 1; duration: 160; easing.type: Easing.OutCubic }

    SearchBar {
        id: bar
        width: parent.width
        launcher: panel.launcher
        placement: panel.placement
        frame: panel.frame
        panelMode: true
        onKeyPressed: (event) => panel.keyPressed(event)
    }

    Rectangle {
        id: topDivider
        anchors.top: bar.bottom
        width: parent.width
        height: 1
        color: Theme.divider
    }

    // ---- the filters -----------------------------------------------------------------

    component Choice: Item {
        id: choice

        property string text
        property string tip
        property bool active: false
        readonly property bool tipWanted: choiceArea.containsMouse && tip.length > 0

        signal clicked()

        width: parent ? parent.width : 200
        height: Theme.labelRowHeight + 6
        Accessible.role: Accessible.RadioButton
        Accessible.name: text
        Accessible.checked: active

        Rectangle {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            radius: Theme.controlRadius
            color: choice.active ? Theme.selection : choiceArea.containsMouse ? Theme.hover : "transparent"
        }
        Rectangle { // accent pill, as on the current row
            visible: choice.active
            x: 9
            anchors.verticalCenter: parent.verticalCenter
            width: 3
            height: 14
            radius: Theme.smallRadius
            color: Theme.accent
        }
        Text {
            x: 20
            width: parent.width - 32
            y: panel.onPixel((choice.height - height) / 2)
            text: choice.text
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            color: choice.active || choiceArea.containsMouse ? Theme.text : Theme.subtext
            font.pixelSize: Theme.uiBody
            font.weight: choice.active ? Font.DemiBold : Font.Normal
        }
        MouseArea {
            id: choiceArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: choice.clicked()
        }
    }

    component GroupTitle: Text {
        x: 16
        height: Theme.labelRowHeight
        verticalAlignment: Text.AlignVCenter
        color: Theme.faint
        font.pixelSize: Theme.uiBody
    }

    // A row of chips that wraps: one choice of a filter each, as the strip
    // under the box shows the kinds of file (TypeStrip).
    component Chips: Flow {
        x: 16
        width: parent ? parent.width - 32 : 200
        spacing: 6
        bottomPadding: 6
    }
    component Chip: Rectangle {
        id: chip

        property string text
        property string tip
        property bool active: false
        readonly property bool tipWanted: chipArea.containsMouse && tip.length > 0

        signal clicked()

        width: Math.ceil(chipLabel.implicitWidth) + 20 // whole pixels: the labels after it stay sharp
        height: Theme.chipHeight + 4
        radius: Theme.pillRadius(height)
        color: active ? Theme.chipActive : chipArea.containsMouse ? Theme.chipHover : "transparent"
        border.width: active ? 1 : 0
        border.color: Theme.chipBorder
        Accessible.role: Accessible.RadioButton
        Accessible.name: text
        Accessible.checked: active

        Text {
            id: chipLabel
            x: 10
            y: panel.onPixel((chip.height - height) / 2)
            text: chip.text
            color: chip.active || chipArea.containsMouse ? Theme.text : Theme.subtext
            font.pixelSize: Theme.uiBody
            font.weight: chip.active ? Font.DemiBold : Font.Normal
        }
        MouseArea {
            id: chipArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.clicked()
        }
    }

    // A small field for a bound typed by hand (a date, a size in MB). Enter
    // and Esc give the keyboard back to the search field (what it typed
    // is taken on either); Tab goes to the box beside it.
    component BoundBox: InputBox {
        id: bound
        property Item next
        implicitHeight: Theme.chipHeight + 4
        implicitWidth: 110
        onAccepted: panel.searchBar.focusAtEnd()
        Keys.onEscapePressed: panel.searchBar.focusAtEnd()
        Keys.onTabPressed: if (next) next.takeFocus(); else panel.searchBar.focusAtEnd()
    }

    Item {
        id: sidebar
        anchors.top: topDivider.bottom
        anchors.bottom: footer.top
        width: panel.sidebarWidth

        Flickable {
            id: filters
            anchors.fill: parent
            contentWidth: width
            contentHeight: filterColumn.height + 12
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick

            Column {
                id: filterColumn
                width: filters.width
                topPadding: 6

                Item { // "筛选", and the link that clears them all
                    width: parent.width
                    height: Theme.labelRowHeight

                    Text {
                        x: 20
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("筛选")
                        color: Theme.subtext
                        font.pixelSize: Theme.uiBody
                        font.weight: Font.DemiBold
                    }
                    Text {
                        id: clearAll
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        visible: panel.launcher.filtersActive
                        text: qsTr("清除全部")
                        color: Theme.accent
                        font.pixelSize: Theme.uiBody
                        font.underline: clearArea.containsMouse
                        Accessible.role: Accessible.Button
                        Accessible.name: text

                        MouseArea {
                            id: clearArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: panel.launcher.clearFilters()
                        }
                    }
                }

                GroupTitle { text: qsTr("范围") }
                Chips {
                    Chip {
                        text: qsTr("全部")
                        active: panel.launcher.scope === Launcher.All
                        onClicked: panel.launcher.scope = Launcher.All
                    }
                    Chip {
                        text: qsTr("文件")
                        active: panel.launcher.scope === Launcher.Files
                        onClicked: panel.launcher.scope = Launcher.Files
                    }
                    Chip {
                        text: qsTr("文件夹")
                        active: panel.launcher.scope === Launcher.Folders
                        onClicked: panel.launcher.chooseFileType(-2) // Launcher.kFoldersChip
                    }
                    Chip {
                        visible: panel.launcher.contentEnabled
                        text: qsTr("内容")
                        active: panel.launcher.scope === Launcher.Content
                        onClicked: panel.launcher.scope = Launcher.Content
                    }
                }

                GroupTitle { text: qsTr("类型") }
                Chips {
                    Chip {
                        text: qsTr("不限")
                        active: panel.launcher.scope !== Launcher.Folders && panel.launcher.fileType < 0
                        onClicked: panel.launcher.chooseFileType(-1)
                    }
                    Repeater {
                        model: panel.launcher.fileTypes

                        delegate: Chip {
                            required property int index
                            required property var modelData
                            text: modelData.name
                            tip: modelData.extensions.split(" ").slice(0, 12).map(e => "." + e).join(" ")
                            active: panel.launcher.scope !== Launcher.Folders && panel.launcher.fileType === index
                            onClicked: panel.launcher.chooseFileType(index)
                        }
                    }
                }

                GroupTitle { text: qsTr("修改时间") }
                Chips {
                    Repeater {
                        model: [qsTr("不限"), qsTr("今天"), qsTr("最近 7 天"), qsTr("最近 30 天"), qsTr("今年"), qsTr("自定义")]

                        delegate: Chip {
                            required property int index
                            required property string modelData
                            text: modelData
                            active: panel.launcher.dateChoice === index
                            onClicked: {
                                panel.launcher.setDateChoice(index)
                                if (index === 5)
                                    dateFrom.takeFocus()
                            }
                        }
                    }
                }
                Row { // 自定义: from and to, "yyyy-MM-dd"
                    x: 16
                    visible: panel.launcher.dateChoice === 5
                    spacing: 6
                    topPadding: 4
                    bottomPadding: 4

                    BoundBox {
                        id: dateFrom
                        implicitWidth: 126 // the placeholder fits; 16 + 126 + 6 + 126 leaves 16 on the right too
                        next: dateTo
                        placeholder: qsTr("起 2026-01-01")
                        text: panel.launcher.dateFrom
                        onEditingFinished: panel.launcher.setDateRange(text, dateTo.text)
                        Component.onCompleted: panel.frame.addControl(dateFrom)
                    }
                    BoundBox {
                        id: dateTo
                        implicitWidth: 126
                        placeholder: qsTr("止")
                        text: panel.launcher.dateTo
                        onEditingFinished: panel.launcher.setDateRange(dateFrom.text, text)
                        Component.onCompleted: panel.frame.addControl(dateTo)
                    }
                }

                GroupTitle { text: qsTr("大小") }
                Chips {
                    Repeater {
                        model: [qsTr("不限"), qsTr("小于 1 MB"), qsTr("1 MB – 100 MB"), qsTr("100 MB – 1 GB"), qsTr("大于 1 GB"), qsTr("自定义")]

                        delegate: Chip {
                            required property int index
                            required property string modelData
                            text: modelData
                            active: panel.launcher.sizeChoice === index
                            onClicked: {
                                panel.launcher.setSizeChoice(index)
                                if (index === 5)
                                    sizeMin.takeFocus()
                            }
                        }
                    }
                }
                Row { // 自定义: the least and the most, in MB
                    x: 16
                    visible: panel.launcher.sizeChoice === 5
                    spacing: 6
                    topPadding: 4
                    bottomPadding: 4

                    BoundBox {
                        id: sizeMin
                        next: sizeMax
                        placeholder: qsTr("最小 MB")
                        text: panel.launcher.sizeMin
                        onEditingFinished: panel.launcher.setSizeRange(text, sizeMax.text)
                        Component.onCompleted: panel.frame.addControl(sizeMin)
                    }
                    BoundBox {
                        id: sizeMax
                        placeholder: qsTr("最大 MB")
                        text: panel.launcher.sizeMax
                        onEditingFinished: panel.launcher.setSizeRange(sizeMin.text, text)
                        Component.onCompleted: panel.frame.addControl(sizeMax)
                    }
                }

                GroupTitle { text: qsTr("位置") }
                Choice {
                    text: qsTr("不限")
                    active: panel.launcher.folderFilter.length === 0
                    onClicked: panel.launcher.setFolderFilter("")
                }
                Repeater { // the favorite folders (the settings' 收藏夹)
                    model: panel.launcher.favoriteFolders

                    delegate: Choice {
                        required property var modelData
                        text: modelData.name
                        tip: modelData.path
                        active: panel.launcher.folderFilter.toLowerCase() === modelData.path.toLowerCase()
                        onClicked: panel.launcher.setFolderFilter(modelData.path)
                    }
                }
                Choice { // one picked with the folder dialog, while it is not a favorite
                    readonly property bool isFavorite: panel.launcher.favoriteFolders.some(
                        f => f.path.toLowerCase() === panel.launcher.folderFilter.toLowerCase())
                    visible: panel.launcher.folderFilter.length > 0 && !isFavorite
                    text: panel.launcher.folderFilter
                    tip: panel.launcher.folderFilter
                    active: true
                    onClicked: panel.launcher.pickFolderFilter()
                }
                Choice {
                    text: qsTr("选择文件夹…")
                    onClicked: panel.launcher.pickFolderFilter()
                }
            }
        }

        ListScrollBar {
            flickable: filters
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
        }

        HoverTip {
            readonly property Item hovered: {
                for (let i = 0; i < filterColumn.children.length; ++i) {
                    const c = filterColumn.children[i]
                    if (c.tipWanted)
                        return c
                    if (c instanceof Flow) {
                        for (let k = 0; k < c.children.length; ++k) {
                            if (c.children[k].tipWanted)
                                return c.children[k]
                        }
                    }
                }
                return null
            }
            target: hovered
            text: hovered ? hovered.tip : ""
        }
    }

    Rectangle { // between the filters and the rows
        anchors.left: sidebar.right
        anchors.top: topDivider.bottom
        anchors.bottom: footer.top
        width: 1
        color: Theme.divider
    }

    // ---- the rows --------------------------------------------------------------------

    // A column's header: sorts by it, the arrow on the one in charge.
    component Header: Item {
        id: header

        property string text
        property int key // ws::SortKey
        property bool alignRight: false
        readonly property bool active: panel.launcher.panelSortKey === key

        height: parent.height
        Accessible.role: Accessible.Button
        Accessible.name: text

        Rectangle {
            anchors.fill: parent
            anchors.topMargin: 2
            anchors.bottomMargin: 2
            anchors.leftMargin: -4 // the text sits on the column line, as the rows' text; the hover reaches past it
            anchors.rightMargin: -4
            radius: Theme.controlRadius
            color: headerArea.containsMouse ? Theme.hover : "transparent"
        }
        Text {
            id: headerText
            x: header.alignRight ? Math.max(0, header.width - width - (arrow.visible ? 14 : 0)) : 0
            y: panel.onPixel((header.height - height) / 2)
            width: Math.min(implicitWidth, header.width - (arrow.visible ? 14 : 0))
            text: header.text
            elide: Text.ElideRight
            color: header.active ? Theme.text : Theme.subtext
            font.pixelSize: Theme.uiBody
            font.weight: header.active ? Font.DemiBold : Font.Normal
        }
        Glyph {
            id: arrow
            visible: header.active
            x: headerText.x + headerText.width + 4
            y: panel.onPixel((header.height - height) / 2)
            glyph: panel.launcher.panelSortDescending ? "" : "" // ChevronDown, ChevronUp
            size: 8
            color: Theme.subtext
        }
        MouseArea {
            id: headerArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: panel.launcher.sortBy(header.key)
        }
    }

    Item {
        id: columns
        anchors.left: sidebar.right
        anchors.leftMargin: 1
        anchors.right: parent.right
        anchors.top: topDivider.bottom
        height: Theme.labelRowHeight + 8

        Header { x: panel.nameX; width: panel.pathX - 12 - x; text: qsTr("名称"); key: 1 }
        Header { x: panel.pathX; width: panel.sizeX - 12 - x; text: qsTr("路径"); key: 2 }
        Header { x: panel.sizeX; width: panel.sizeWidth; text: qsTr("大小"); key: 3; alignRight: true }
        Header { x: panel.dateX; width: panel.dateWidth; text: qsTr("修改时间"); key: 4 }

        Rectangle {
            anchors.bottom: parent.bottom
            x: 8
            width: parent.width - 16
            height: 1
            color: Theme.divider
        }
    }

    ListView {
        id: list
        anchors.left: sidebar.right
        anchors.leftMargin: 1
        anchors.right: parent.right
        anchors.top: columns.bottom
        anchors.bottom: footer.top
        topMargin: 4
        bottomMargin: 4
        clip: true
        opacity: panel.dimmed ? 0.45 : 1
        Behavior on opacity { NumberAnimation { duration: 150 } }
        model: panel.launcher.results
        boundsBehavior: Flickable.StopAtBounds
        highlightMoveDuration: 0
        reuseItems: true
        function stopScrolling() {
            wheelScroll.cancel()
            cancelFlick()
        }
        ListWheelScroll {
            id: wheelScroll
            view: list
            animate: SystemTheme.animations
        }
        onCurrentIndexChanged: {
            positionViewAtIndex(currentIndex, ListView.Contain)
            if (currentIndex === 0)
                contentY = originY - topMargin
        }
        onContentYChanged: if (panel.nameTip) panel.nameTip.dismiss() // scrolled: another row under the pointer

        // Scrolled to the end: a page more (Launcher.loadMore), as the box's list.
        function loadMoreAtEnd() {
            if (atYEnd && count > 0 && panel.launcher.moreAvailable && !panel.launcher.searching)
                panel.launcher.loadMore()
        }
        onAtYEndChanged: Qt.callLater(loadMoreAtEnd)
        Connections {
            target: panel.launcher
            function onStatusChanged() { Qt.callLater(list.loadMoreAtEnd) }
        }

        delegate: PanelRow {
            id: panelRow
            width: list.width
            height: panel.rowHeight
            nameX: panel.nameX
            pathX: panel.pathX
            sizeX: panel.sizeX
            dateX: panel.dateX
            sizeWidth: panel.sizeWidth
            dateWidth: panel.dateWidth
            nameTip: panel.nameTip
            // On whole device pixels, as the box's rows (see Main.qml).
            transform: ScrollPixelAlignment {
                view: list
                viewY: list.y
                itemY: panelRow.y
            }
            onClicked: (index, modifiers) => panel.rowClicked(index, modifiers)
            onDoubleClicked: (index, modifiers) => panel.rowDoubleClicked(index, modifiers)
            onMenuRequested: (index, globalPos, modifiers) => panel.rowMenuRequested(index, globalPos, modifiers)
            onDragRequested: (index) => panel.rowDragRequested(index)
        }
    }

    ListScrollBar {
        id: listScrollBar
        flickable: list
        anchors.right: parent.right
        anchors.top: list.top
        anchors.bottom: list.bottom
        // Its own at the window's right edge, not the edge that resizes the window.
        Component.onCompleted: panel.frame.addControl(listScrollBar)
    }

    Text { // nothing to list, or not yet
        readonly property string query: panel.launcher.query.trim().replace(/\s+/g, " ")
        anchors.centerIn: list
        width: list.width - 80
        horizontalAlignment: Text.AlignHCenter
        visible: list.count === 0
        text: panel.launcher.searching ? (panel.launcher.scope === Launcher.Content ? qsTr("正在搜索文件内容…") : qsTr("正在搜索…"))
            : query.length > 0 ? qsTr("没有找到“%1”").arg(query)
            : panel.launcher.filtersActive ? qsTr("没有符合筛选条件的文件")
            : qsTr("输入名称或拼音，或在左侧加筛选条件")
        textFormat: Text.PlainText
        elide: Text.ElideMiddle
        color: Theme.faint
        font.pixelSize: Theme.uiBody
    }

    Footer {
        id: footer
        anchors.bottom: parent.bottom
        width: parent.width
        launcher: panel.launcher
        frame: panel.frame
    }
}
