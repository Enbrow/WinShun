pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The launcher window: search bar, up to eight results, status footer.
// Keyboard first: everything works without touching the mouse. The clipboard
// history (Win+V) has a window of its own (ClipboardWindow.qml); opened from
// the search box, it drops down under it, and the rows and the footer fold
// away until it is gone: the box shows what a paste goes into.
Window {
    id: window

    required property Launcher launcher
    required property Clipboard clipboard
    required property Placement placement
    required property WindowFrame frame

    // The clipboard dropped down under the search box (see above).
    readonly property bool clipboardBelow: clipboard.active && clipboard.field === searchBar.field

    readonly property int rowHeight: Theme.rowHeight // 界面大小
    readonly property int maxRows: 8
    readonly property int minRows: 3 // the window settles no lower than these fit below
    // Everything but the rows: header, the strip of kinds of file, divider,
    // the list's margins, footer. Room is kept for the strip in every scope
    // (so opening it never moves the window), but at the taskbar, where the
    // list area takes its room from the rows instead.
    readonly property int chromeHeight: searchBar.implicitHeight + (fullHeight ? 0 : typeStrip.fullHeight) + 1 + 8
                                        + footer.implicitHeight
    // The strip opens in steps (TypeStrip): the window takes its full height
    // at once as it opens, so it is resized once, not on every frame (a
    // native resize per frame shows torn and stretched frames). Meanwhile the
    // rows below glide, the room it does not fill yet left above the footer;
    // at the taskbar the window keeps its height and the list area gives the
    // strip its room. It closes at once, the room given back with it: given
    // back after a closing animation, it was a second jump of the window
    // after the click (文件 → 内容: the rows went at the click, the strip's
    // room 160 ms later).
    // Closed, it still keeps its room for a frame with the footer drawn up
    // where it ends (stripLeaving): a window made smaller shows its last
    // frame cut at the bottom until it draws the next, and cut there that
    // frame would have no footer.
    readonly property bool stripOut: typeStrip.targetHeight > 0 || typeStrip.height > 0
    property bool stripLeaving: false
    readonly property int stripRoom: stripOut || stripLeaving ? typeStrip.fullHeight : 0
    readonly property real stripSlack: stripRoom - typeStrip.height
    onStripOutChanged: {
        stripLeaving = !stripOut && !fullHeight && visible
        if (stripLeaving) {
            frame.afterNextFrame()
            stripLeft.restart()
        }
    }
    Connections {
        target: window.frame
        function onFrameShown() { window.stripLeaving = false }
    }
    Timer { // no frame comes (hidden meanwhile)
        id: stripLeft
        interval: 150
        onTriggered: window.stripLeaving = false
    }
    // As many rows as fit between the window's top and the bottom of the screen.
    readonly property int fitRows: Math.max(minRows, Math.min(maxRows,
                                            Math.floor((placement.room - chromeHeight) / rowHeight)))
    readonly property bool contentMode: launcher.scope === Launcher.Content
    readonly property string trimmedQuery: launcher.query.trim()
    // What was typed, on one line to show it (with a line break in it, it
    // would spread over the window).
    readonly property string shownQuery: trimmedQuery.replace(/\s+/g, " ")
    // After the list area has closed (see settleRows), not while it is held
    // open. Set in settleEmpty, with what it says (emptyShown*): it stays as
    // it is while the next search runs, as the rows do (Launcher.stale), and
    // goes when that one has rows or has found nothing too. Taken away as the
    // search started (Tab from 内容 back to 全部), the window shrank to the
    // search box and grew again for the rows a frame later.
    property bool showEmptyState: false
    property string emptyShownTitle
    property string emptyShownHint
    property var emptyShownHints: []
    property bool dimEmpty: false // held long enough to notice (as dimRows): it is the last query's
    function settleEmpty() {
        if (trimmedQuery.length > 0 && list.count === 0 && !launcher.searching && shownRows === 0) {
            emptyShownTitle = emptyTitle
            emptyShownHint = emptyHint
            emptyShownHints = hints
            showEmptyState = true
            emptyDimDelay.stop()
            dimEmpty = false
        } else if (showEmptyState && trimmedQuery.length > 0 && list.count === 0 && launcher.searching) {
            if (!dimEmpty && !emptyDimDelay.running)
                emptyDimDelay.start()
        } else {
            showEmptyState = false
            emptyDimDelay.stop()
            dimEmpty = false
        }
    }
    Timer {
        id: emptyDimDelay
        interval: 250 // as dimDelay
        onTriggered: window.dimEmpty = true
    }
    onTrimmedQueryChanged: Qt.callLater(settleRows)
    onEmptyTitleChanged: Qt.callLater(settleRows)
    onEmptyHintChanged: Qt.callLater(settleRows)

    // What the search behind the rows passed over (Launcher.leftOutHints:
    // files over the size limit, system folders, other kinds of text, a USB
    // drive not indexed): said in rows after the last result, each with what
    // a click does (searched this once), or with Enter once ↓ past the last
    // row has put the keyboard on one (hintIndex; as homeIndex for the apps
    // above), ↓ ↑ going along them. With no result at all, the empty state
    // lists them instead, and ↓ ↑ Enter work there too. The one running
    // shows how far it is; Enter or a click on it stops it.
    readonly property var hints: launcher.leftOutHints
    readonly property bool showHints: hints.length > 0 && list.count > 0
    readonly property int hintHeight: Theme.hintRowHeight
    // Room for the rows after the last result: when every result is shown
    // without scrolling (else they come into view at the end), and the window
    // keeps its own height (at the taskbar the list area is as tall as it gets).
    readonly property int hintSpace: showHints && !fullHeight && shownRows < fitRows ? hints.length * hintHeight : 0
    property int hintIndex: -1 // the hint the keyboard is on; -1: none
    property int rowsBeforeHints: 0 // list.count when the keyboard went onto the hints: where what one brings starts
    function enterHints() {
        rowsBeforeHints = list.count
        hintIndex = 0
        list.currentIndex = -1
        list.positionViewAtEnd()
    }
    function takeHint(index) {
        if (index < 0 || index >= hints.length)
            return
        if (hints[index].running)
            launcher.stopLeftOut()
        else
            launcher.searchLeftOut(hints[index].kind)
    }
    onHintsChanged: {
        Qt.callLater(settleRows) // what the empty state lists
        // Changed under the keyboard (one taken up: it alone, running; done:
        // the rest, or none): the hint in the same place, or, none left, the
        // first row the one taken up brought in (they go after the last
        // row), else the last row.
        if (hintIndex < 0)
            return
        if (hints.length > 0) {
            hintIndex = Math.min(hintIndex, hints.length - 1)
        } else {
            hintIndex = -1
            if (list.count > 0)
                list.currentIndex = Math.min(rowsBeforeHints, list.count - 1)
        }
    }
    // Nothing is named as typed, and these rows sound like it: a heading over
    // them (Launcher.approximateHeading), above the list so the rows stay
    // where the number keys count them.
    readonly property int headingHeight: launcher.approximateHeading.length > 0 && list.count > 0 ? Theme.labelRowHeight : 0
    // Opened from the taskbar (its button, Win+S), the window stands on the
    // taskbar: the list area keeps the height of all the rows that fit, so
    // the search box stays where it is as results come and go.
    readonly property bool fullHeight: placement.atTaskbar && !clipboardBelow
    readonly property int listRows: fullHeight ? fitRows : shownRows
    // There, with nothing typed: the apps opened most in a row over the
    // recent ones, as Windows' own search starts out.
    readonly property bool showHome: fullHeight && trimmedQuery.length === 0 && launcher.frequentApps.length > 0
    // The app the keyboard is on (↑ from the first row); -1: in the list.
    property int homeIndex: -1
    onShowHomeChanged: if (!showHome) homeIndex = -1
    function enterHome() {
        homeIndex = 0
        list.currentIndex = -1
    }
    function leaveHome() {
        homeIndex = -1
        if (list.count > 0)
            list.currentIndex = 0
    }

    // Nothing found: under the search box, or in the list area at full height.
    readonly property string emptyTitle: contentMode && launcher.whereMissing.length > 0
        ? qsTr("%1 不在索引里，没法在里面搜").arg(launcher.whereMissing)
        : contentMode && launcher.contentWhere.length > 0 && launcher.contentWords.length === 0
        ? qsTr("在 %1 里搜：接着输入要找的文字").arg(launcher.contentWhere)
        : contentMode && launcher.contentWhere.length > 0
        ? qsTr("%1 里没有 %2包含“%3”").arg(launcher.contentWhere).arg(launcher.contentFilesLabel)
                                      .arg(launcher.contentWords.replace(/\s+/g, " "))
        : contentMode
        ? qsTr("没有 %1包含“%2”").arg(launcher.contentFilesLabel).arg(shownQuery)
        : launcher.pluginKeyword.length > 0 ? qsTr("%1 没有找到“%2”").arg(launcher.pluginLabel).arg(shownQuery)
        : qsTr("没有找到“%1”").arg(shownQuery)
    readonly property string emptyHint: contentMode && launcher.contentWhere.length > 0
        ? qsTr("按 Shift+Tab 换个文件夹，Tab 切换范围")
        : contentMode && launcher.folderChips.length > 1 ? qsTr("按 Shift+Tab 只搜某个文件夹，Tab 切换范围")
        : contentMode ? qsTr("按 Tab 回到文件名搜索")
        : typeStrip.shown ? qsTr("按 Shift+Tab 换个类型，Tab 切换范围")
        : launcher.pluginKeyword.length > 0 ? qsTr("按 Esc 清空关键字后面的文字")
        : launcher.searchesContent ? qsTr("文件名和 %1的内容里都没有").arg(launcher.contentFilesLabel)
        : launcher.contentEnabled ? qsTr("按 Tab 切换范围，或搜索文件内容")
        : qsTr("按 Tab 切换范围")
    component EmptyState: Column {
        property string title
        property string hint
        spacing: 4

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            // A long query keeps its start and its end, and the quotes.
            elide: Text.ElideMiddle
            text: parent.title
            textFormat: Text.PlainText // holds what was typed
            color: Theme.subtext
            font.pixelSize: Theme.uiTitle
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: parent.hint
            color: Theme.faint
            font.pixelSize: Theme.uiBody
        }
        Repeater { // what the search passed over, each with its action (as after the last row; ↓ ↑ and Enter too)
            model: window.emptyShownHints

            delegate: Item {
                id: emptyHint

                required property int index
                required property var modelData
                // The first is current until ↓ ↑ say otherwise: it is what Enter takes.
                readonly property bool current: window.hintIndex === index || (window.hintIndex < 0 && index === 0)

                anchors.horizontalCenter: parent.horizontalCenter
                width: hintLine.width + 24
                height: Theme.labelRowHeight
                Accessible.role: Accessible.Button
                Accessible.name: modelData.text + " " + modelData.action

                Rectangle { // raised with the keyboard on it, or hovered, as a row
                    anchors.fill: parent
                    radius: 6
                    color: emptyHint.current ? Theme.selection : emptyHintArea.containsMouse ? Theme.hover : "transparent"
                }
                Row {
                    id: hintLine
                    anchors.centerIn: parent

                    Text {
                        text: emptyHint.modelData.text
                        color: Theme.faint
                        font.pixelSize: Theme.uiBody
                    }
                    Text {
                        text: " · "
                        color: Theme.faint
                        font.pixelSize: Theme.uiBody
                    }
                    Text { // the action: accent words on the plain background, as a link
                        text: emptyHint.modelData.action
                        color: Theme.accent
                        font.pixelSize: Theme.uiBody
                    }
                }
                MouseArea {
                    id: emptyHintArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: window.takeHint(emptyHint.index)
                }
            }
        }
    }

    // Rows the list area is sized for. It grows at once but shrinks only
    // once the search has settled (or after a grace period), so a list that
    // is about to fill up again does not collapse and spring back.
    property int shownRows: 0
    function settleRows() {
        const wanted = Math.min(launcher.results.count, fitRows)
        if (wanted >= shownRows || !launcher.searching) {
            shownRows = wanted
            shrinkGrace.stop()
        } else if (!shrinkGrace.running) {
            shrinkGrace.start()
        }
        settleEmpty() // in the same go: no frame with both the rows and the empty state
    }
    Timer {
        id: shrinkGrace
        interval: 2000 // a content scan's first matches often take over a second
        onTriggered: {
            window.shownRows = Math.min(window.launcher.results.count, window.fitRows)
            window.settleEmpty()
        }
    }
    onFitRowsChanged: { // the window was put somewhere else
        shownRows = Math.min(shownRows, fitRows) // no room for them: at once
        settleRows()
    }

    // The previous query's rows while a slow search (内容) has found nothing
    // yet: dimmed, but only once that lasts long enough to notice.
    property bool dimRows: false
    readonly property bool stale: launcher.stale
    onStaleChanged: {
        if (stale) {
            dimDelay.start()
        } else {
            dimDelay.stop()
            dimRows = false
        }
    }
    Timer {
        id: dimDelay
        interval: 250
        onTriggered: window.dimRows = window.stale
    }

    width: 760
    height: layout.implicitHeight
    // See-through to Mica, as the settings window (see there).
    color: Theme.backdrop && SystemTheme.materials && window.active ? "transparent" : Theme.background
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    title: qsTr("Win顺")

    Binding {
        target: window.launcher.results
        property: "highlightColor"
        value: Theme.accent
    }
    Binding {
        target: window.launcher
        property: "currentRow"
        // On a hint after the last row, that row: what the search brings
        // goes after it (never above the row the keyboard is on).
        value: window.hintIndex >= 0 ? list.count - 1 : list.currentIndex
    }
    Binding { // with the fewest rows: kept room for below the window
        target: window.placement
        property: "roomNeeded"
        value: window.chromeHeight + window.minRows * window.rowHeight
    }

    onClipboardBelowChanged: closeContextMenu()

    function moveSelection(delta) {
        if (hintIndex >= 0) { // on the hints after the last row: ↓ ↑ along them, ↑ from the first back to the last row
            if (delta > 0)
                hintIndex = Math.min(hints.length - 1, hintIndex + 1)
            else if (hintIndex > 0)
                hintIndex -= 1
            else if (list.count > 0)
                list.currentIndex = list.count - 1 // onCurrentIndexChanged leaves the hints
        } else if (list.count > 0) {
            const next = list.currentIndex + delta
            if (delta === 1 && next >= list.count && showHints)
                enterHints() // ↓ past the last row
            else
                list.currentIndex = Math.max(0, Math.min(list.count - 1, next))
        } else if (hints.length > 0) { // nothing found: along the hints under 没有找到, the first already shown as current
            hintIndex = Math.max(0, Math.min(hints.length - 1, Math.max(hintIndex, 0) + delta))
        }
        selectionAnchor = -1
    }

    // Several rows: Ctrl+click toggles one, Shift+click or Shift+arrows take
    // the rows from the anchor (where the range started) to here.
    property int selectionAnchor: -1
    readonly property int selectedCount: launcher.results.selectedCount
    function toggleRow(index) {
        window.launcher.toggleSelected(index)
        list.currentIndex = index
        selectionAnchor = index
    }
    function selectTo(index, add) {
        if (list.count === 0 || index < 0)
            return
        if (selectionAnchor < 0 || selectionAnchor >= list.count)
            selectionAnchor = Math.max(0, list.currentIndex)
        window.launcher.selectRange(selectionAnchor, index, add)
        list.currentIndex = index
    }
    function extendSelection(delta) {
        if (list.count > 0)
            selectTo(Math.max(0, Math.min(list.count - 1, list.currentIndex + delta)), false)
    }

    // Ctrl+1… (or Alt+1…, Launcher.numberKeys) opens the nth row shown,
    // counted from the first one at least half in view. The chords are on
    // the rows' icons all along: shown only while the modifier was held,
    // they came late (after a wait, so that Ctrl+C and the like would not
    // flash them) and swapped the right of every row. The keys come here as
    // the window's own (no hook: GitHub #10); a digit another program holds
    // as a global hotkey stays its (numberkeys::heldMask).
    readonly property int numberKey: launcher.numberKeys === "ctrl" ? Qt.Key_Control
                                   : launcher.numberKeys === "alt" ? Qt.Key_Alt : -1
    readonly property int firstShownRow: Math.max(0, Math.floor((list.contentY + rowHeight / 2) / rowHeight))
    readonly property int lastShownRow: Math.min(list.count - 1, firstShownRow + 8,
                                                 Math.floor((list.contentY + list.height - rowHeight / 2) / rowHeight))
    readonly property string numberModifier: numberKey === Qt.Key_Alt ? "Alt" : "Ctrl"
    // A digit another program holds as a global hotkey is its (Launcher.numberKeysLeft):
    // the row still shows the chord, struck through (ResultRow.hintTaken).
    function numberUsable(number) {
        return number >= 1 && (launcher.numberKeysLeft & (1 << number)) === 0
    }
    function numberOf(index) {
        return numberKey >= 0 && index >= firstShownRow && index <= lastShownRow ? index - firstShownRow + 1 : 0
    }
    function openNumber(number) {
        const row = firstShownRow + number - 1
        if (!numberUsable(number) || row > lastShownRow)
            return
        window.launcher.clearSelection() // that row alone
        list.currentIndex = row
        window.launcher.trigger(row, Launcher.Open)
    }
    // Exactly the modifier of the number keys (the number pad counts with Ctrl only: Alt+0169 types ©).
    function isNumberChord(event) {
        if (event.key < Qt.Key_1 || event.key > Qt.Key_9)
            return false
        const keypad = (event.modifiers & Qt.KeypadModifier) !== 0
        const modifiers = event.modifiers & ~Qt.KeypadModifier
        return numberKey === Qt.Key_Control ? modifiers === Qt.ControlModifier
             : numberKey === Qt.Key_Alt ? modifiers === Qt.AltModifier && !keypad : false
    }

    // On the selection when there is one, else on the current row.
    function act(action) {
        if (selectedCount > 0)
            window.launcher.triggerSelection(action)
        else if (list.count > 0 && list.currentIndex >= 0)
            window.launcher.trigger(list.currentIndex, action)
    }

    readonly property ContextMenu contextMenu: menuLoader.item as ContextMenu
    readonly property bool menuOpen: contextMenu !== null && contextMenu.visible

    // `globalPos` is where the mouse was; without it (Menu key, Shift+F10)
    // the menu opens under the current row.
    function openContextMenu(index, globalPos) {
        if (index < 0 || index >= list.count)
            return
        if (!window.launcher.results.isSelected(index))
            window.launcher.clearSelection() // as in Explorer: the menu is for this row alone
        list.currentIndex = index
        const fromKeyboard = globalPos === undefined
        if (fromKeyboard) {
            list.positionViewAtIndex(index, ListView.Contain)
            globalPos = list.currentItem.mapToGlobal(56, rowHeight - 6)
        }
        menuLoader.active = true
        // Not a line starting with "(": it would continue the line above, "true(...)".
        const menu = menuLoader.item as ContextMenu
        menu.popup(index, launcher.results.pathAt(index), launcher.menuItems(index), globalPos, fromKeyboard)
    }

    function closeContextMenu() {
        if (contextMenu)
            contextMenu.dismiss()
    }

    // Hidden: also free the menu window until it is needed again.
    onVisibleChanged: {
        if (!visible) {
            closeContextMenu()
            menuLoader.active = false
        }
    }

    function handleKey(event) {
        if (menuOpen && contextMenu.handleKey(event)) {
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Alt && numberKey === Qt.Key_Alt) {
            event.accepted = true // alone, it would open the window menu
            return
        }
        if (isNumberChord(event) && searchBar.composition.length === 0) {
            window.launcher.pressNumber(event.key - Qt.Key_0)
            event.accepted = true
            return
        }
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        if (homeIndex >= 0) { // on the row of apps (nothing typed, so ← → move nothing else)
            switch (event.key) {
            case Qt.Key_Left: homeIndex = Math.max(0, homeIndex - 1); break
            case Qt.Key_Right: homeIndex = Math.min(launcher.frequentApps.length - 1, homeIndex + 1); break
            case Qt.Key_Down: leaveHome(); break
            case Qt.Key_Up: break
            case Qt.Key_Return:
            case Qt.Key_Enter: launcher.openFrequentApp(homeIndex); break
            case Qt.Key_Escape: launcher.dismiss(); break
            default: return
            }
            event.accepted = true
            return
        }
        switch (event.key) {
        case Qt.Key_Down: shift ? extendSelection(1) : moveSelection(1); break
        case Qt.Key_Up:
            if (!shift && showHome && list.currentIndex <= 0)
                enterHome()
            else
                shift ? extendSelection(-1) : moveSelection(-1)
            break
        case Qt.Key_PageDown: shift ? extendSelection(fitRows) : moveSelection(fitRows); break
        case Qt.Key_PageUp: shift ? extendSelection(-fitRows) : moveSelection(-fitRows); break
        case Qt.Key_Return:
        case Qt.Key_Enter: {
            // On a hint after the last row, or in the empty state with some:
            // that part is searched too (the first, with none chosen there).
            if (hintIndex >= 0 || (list.count === 0 && hints.length > 0)) {
                takeHint(Math.max(hintIndex, 0))
                break
            }
            // Shift+Enter: a plugin row's third action (Open on any other row).
            const action = ctrl && shift ? Launcher.RunAsAdmin : ctrl ? Launcher.Reveal
                         : shift ? Launcher.Alternate : Launcher.Open
            if (!window.launcher.holdUntilShown(action, 0)) // typed a moment ago: on the rows for it
                act(action)
            break
        }
        case Qt.Key_Escape:
            // In a plugin's keyword mode, first the words after the keyword go;
            // in 内容 in a folder chosen, the words (the folder stays); then the window.
            if (selectedCount > 0)
                window.launcher.clearSelection()
            else if (!window.launcher.clearToKeyword() && !window.launcher.clearContentWords())
                window.launcher.dismiss()
            break
        case Qt.Key_Tab:
            // In keyword mode the scopes mean nothing: Tab takes the row's
            // autocomplete instead. Outside it the hint row of a keyword
            // ("kill" typed alone, or "kil" that it begins with) takes it
            // too, from whichever row the keyboard is on (Launcher.completionRow).
            if (window.launcher.pluginKeyword.length > 0)
                window.launcher.autocomplete(list.currentIndex)
            else if (!window.launcher.autocomplete(list.currentIndex)) {
                // Always the scopes; the strip under the box is Shift+Tab's (GitHub #16).
                window.launcher.cycleScope(1)
            }
            break
        case Qt.Key_S: // the footer's switch: the start list, or the order
            if (!ctrl || shift)
                return
            window.launcher.switchOrder()
            break
        case Qt.Key_Backtab:
            // Along the strip under the box: in 文件 the kinds, in 内容 the folders.
            if (window.launcher.pluginKeyword.length > 0 || !typeStrip.shown)
                break
            if (typeStrip.folders)
                window.launcher.cycleFolderChip(1)
            else
                window.launcher.cycleFileType(1)
            break
        case Qt.Key_C:
            // Ctrl+C copies selected text in the box; otherwise the file itself
            // (always the files, once rows have been picked).
            if (!ctrl || (!shift && searchBar.hasSelection && selectedCount === 0))
                return
            act(shift ? Launcher.CopyPath : Launcher.CopyItem)
            break
        default:
            return
        }
        event.accepted = true
    }

    function selectFirst() {
        hintIndex = -1
        if (list.count > 0 && homeIndex < 0)
            list.currentIndex = 0
        list.positionViewAtBeginning()
    }

    Connections {
        target: window.launcher
        function onNumberPressed(number) { window.openNumber(number) }
        function onHeldKey(action, number) {
            if (number > 0)
                window.openNumber(number)
            else
                window.act(action)
        }
        function onShown() {
            // Still cloaked here (App::showLauncher): nobody sees the jump.
            window.homeIndex = -1
            window.selectFirst()
            searchBar.focusAndSelect()
        }
        function onResultsReplaced() {
            window.selectionAnchor = -1
            window.closeContextMenu() // its row now holds another result
            window.selectFirst()
        }
        function onStatusChanged() { Qt.callLater(window.settleRows) }
        function onBetterFirstRow() {
            if (window.homeIndex < 0 && list.count > 0)
                list.currentIndex = 0
        }
        function onContextMenuKeyPressed() { window.openContextMenu(list.currentIndex) } // Menu key, Shift+F10
        function onQueryChanged() {
            if (searchBar.text !== window.launcher.query)
                searchBar.text = window.launcher.query
        }
    }

    Connections {
        target: window.launcher.results
        function onCountChanged() { Qt.callLater(window.settleRows) }
        // A refresh rewrites rows in place: the menu stays open only while
        // its row still holds the same file.
        function onDataChanged() { window.checkContextMenu() }
        function onRowsRemoved() { window.checkContextMenu() }
        function onRowsInserted() { window.checkContextMenu() } // a better content match above it
    }

    Connections {
        target: window.frame
        function onDoubleClicked() { window.placement.moveHome() } // on the header or footer
    }
    Connections {
        target: window.placement
        function onMovingChanged() { // the menu would stay behind
            if (window.placement.moving)
                window.closeContextMenu()
        }
    }

    function checkContextMenu() {
        if (menuOpen && launcher.results.pathAt(contextMenu.row) !== contextMenu.key)
            closeContextMenu()
    }

    Column {
        id: layout
        width: window.width
        // Opening, the content comes in a little behind the window (Placement.slideIn).
        transform: Translate {
            x: window.placement.contentShift.x
            y: window.placement.contentShift.y
        }

        SearchBar {
            id: searchBar
            width: parent.width
            launcher: window.launcher
            placement: window.placement
            frame: window.frame
            onKeyPressed: (event) => window.handleKey(event)
        }

        TypeStrip {
            id: typeStrip
            width: parent.width
            launcher: window.launcher
            frame: window.frame
            shown: (window.launcher.scope === Launcher.Files || window.launcher.scope === Launcher.Folders
                    || window.launcher.scope === Launcher.Content)
                   && !window.clipboardBelow
        }

        Rectangle {
            width: parent.width
            height: 1
            color: Theme.divider
            visible: !window.clipboardBelow && (window.listRows > 0 || window.showEmptyState)
        }

        Item {
            id: listArea
            width: parent.width
            height: window.listRows > 0 ? window.listRows * window.rowHeight + 8 + window.hintSpace
                                          + (window.fullHeight ? -typeStrip.height : window.headingHeight) : 0
            visible: window.listRows > 0 && !window.clipboardBelow

            Text { // nothing is named as typed: these rows sound like it (window.headingHeight)
                id: readingHeading
                x: 20
                width: parent.width - 40
                height: window.headingHeight
                visible: height > 0
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideMiddle // a long query keeps its start and its end
                text: window.launcher.approximateHeading
                textFormat: Text.PlainText // holds what was typed
                color: Theme.subtext
                font.pixelSize: Theme.uiCaption
            }

            Column { // the apps opened most (showHome), over the recent ones
                id: home
                width: parent.width
                visible: window.showHome
                topPadding: 4

                Text {
                    x: 20
                    height: Theme.labelRowHeight
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("常用应用")
                    color: Theme.subtext
                    font.pixelSize: Theme.uiCaption
                }

                Row {
                    id: tiles
                    x: 20 // tiles, icons and text on multiples of 4 logical pixels: whole device pixels at 125–200 %

                    Repeater {
                        model: window.launcher.frequentApps

                        delegate: Item {
                            id: tile

                            required property int index
                            required property var modelData
                            readonly property bool current: window.homeIndex === index

                            width: 120
                            height: 96
                            // Vertically on whole device pixels too (see the rows').
                            transform: Translate {
                                y: {
                                    const dpr = Screen.devicePixelRatio
                                    const deviceY = (layout.y + listArea.y + home.y + tiles.y) * dpr
                                    return (Math.round(deviceY) - deviceY) / dpr
                                }
                            }
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData.name

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 2
                                radius: 6
                                color: tile.current ? Theme.selection : tileArea.containsMouse ? Theme.hover : "transparent"
                            }
                            Rectangle { // accent pill on the current one, as on the current row
                                visible: tile.current
                                anchors.horizontalCenter: parent.horizontalCenter
                                y: parent.height - 6
                                width: 18
                                height: 3
                                radius: 1.5
                                color: Theme.accent
                            }
                            Image { // as a row's icon (ResultRow.qml): its own pixels, unsmoothed
                                readonly property real dpr: Screen.devicePixelRatio
                                readonly property int pixels: Math.round(32 * dpr)

                                x: 44 + 1 / 64
                                y: 16 + 1 / 64
                                width: pixels / dpr
                                height: pixels / dpr
                                source: tile.modelData.icon
                                sourceSize.width: pixels / dpr
                                sourceSize.height: pixels / dpr
                                smooth: false
                                asynchronous: true
                                fillMode: Image.PreserveAspectFit
                            }
                            Text {
                                x: 6
                                y: 56
                                width: parent.width - 12
                                horizontalAlignment: Text.AlignHCenter
                                text: tile.modelData.name
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.uiCaption
                                elide: Text.ElideRight
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                lineHeight: 0.95
                            }
                            MouseArea {
                                id: tileArea
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: window.launcher.openFrequentApp(tile.index)
                            }
                        }
                    }
                }

                Text {
                    x: 20
                    height: Theme.labelRowHeight
                    verticalAlignment: Text.AlignVCenter
                    visible: list.count > 0
                    text: window.launcher.hasFavorites
                          ? (window.launcher.startList === Launcher.RecentlyModified ? qsTr("收藏和最近修改") : qsTr("收藏和最近"))
                          : (window.launcher.startList === Launcher.RecentlyModified ? qsTr("最近修改") : qsTr("最近"))
                    color: Theme.subtext
                    font.pixelSize: Theme.uiCaption
                }
            }

            ListView {
                id: list
                anchors.fill: parent
                anchors.topMargin: (window.showHome ? home.height : 0) + window.headingHeight
                topMargin: 4
                bottomMargin: 4
                clip: true
                opacity: window.dimRows ? 0.45 : 1
                Behavior on opacity { NumberAnimation { duration: 150 } }
                model: window.launcher.results
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                reuseItems: true
                onCurrentIndexChanged: {
                    positionViewAtIndex(currentIndex, ListView.Contain)
                    if (currentIndex >= 0)
                        window.hintIndex = -1 // back on a row, however it got there
                }

                // After the last row: what the search passed over, a row each
                // with the offer to search it (window.hints). The keyboard
                // reaches them with ↓ past the last row (window.hintIndex).
                footer: Column {
                    id: hintRows
                    width: list.width
                    visible: window.showHints
                    transform: Translate { // on whole device pixels, as the rows are (see the delegate)
                        y: {
                            const dpr = Screen.devicePixelRatio
                            const listTop = layout.y + listArea.y + list.y
                            const deviceY = (listTop + list.contentItem.y + hintRows.y) * dpr
                            return (Math.round(deviceY) - deviceY) / dpr
                        }
                    }

                    Repeater {
                        model: window.showHints ? window.hints : []

                        delegate: Item {
                            id: hintRow

                            required property int index
                            required property var modelData
                            readonly property bool current: window.hintIndex === index

                            width: hintRows.width
                            height: window.hintHeight
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData.text + " " + modelData.action

                            Rectangle { // as a row's: raised with the keyboard on it, or hovered
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                anchors.topMargin: 1
                                anchors.bottomMargin: 1
                                radius: 6
                                color: hintRow.current ? Theme.selection : hintArea.containsMouse ? Theme.hover : "transparent"
                            }
                            Rectangle { // accent pill, as on the current row
                                visible: hintRow.current
                                x: 7
                                anchors.verticalCenter: parent.verticalCenter
                                width: 3
                                height: 18
                                radius: 1.5
                                color: Theme.accent
                            }
                            Text {
                                x: 60 // under the rows' text
                                width: hintAction.x - 16 - x
                                anchors.verticalCenter: parent.verticalCenter
                                text: hintRow.modelData.text
                                color: Theme.subtext
                                font.pixelSize: Theme.uiBody
                                elide: Text.ElideRight
                            }
                            Text { // the action: accent words on the plain background, as a link; right, as the rows' labels
                                id: hintAction
                                anchors.right: parent.right
                                anchors.rightMargin: 18
                                anchors.verticalCenter: parent.verticalCenter
                                text: hintRow.modelData.action
                                color: Theme.accent
                                font.pixelSize: Theme.uiBody
                            }
                            MouseArea {
                                id: hintArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: window.takeHint(hintRow.index)
                            }
                        }
                    }
                }

                // Scrolled, or moved with the keys, to the end: a page more if
                // there is one (Launcher.loadMore), added after the rows. Asked
                // again once a search ends, in case it brought nothing new.
                function loadMoreAtEnd() {
                    if (atYEnd && count > 0 && window.launcher.moreAvailable && !window.launcher.searching)
                        window.launcher.loadMore()
                }
                // Later, not from within the launcher's own signal (a search inside a search).
                onAtYEndChanged: Qt.callLater(loadMoreAtEnd)
                Connections {
                    target: window.launcher
                    function onStatusChanged() { Qt.callLater(list.loadMoreAtEnd) }
                }

                delegate: ResultRow {
                    id: resultRow
                    width: list.width
                    height: window.rowHeight
                    showModified: window.launcher.showModified
                    hint: window.numberOf(index)
                    hintModifier: window.numberModifier
                    hintTaken: resultRow.hint > 0 && !window.numberUsable(resultRow.hint)
                    // Drawn on whole device pixels, wherever the list has
                    // scrolled to (ResultRow puts its text on whole device
                    // pixels within). Between them (at 150 % most scroll
                    // positions are), text and icons round to the pixel grid
                    // each its own way, so while the list glides they take
                    // turns moving by a pixel and the rows shake. Only the
                    // picture moves, by under a pixel: contentY stays as
                    // Flickable has it, so wheel, drag and flick are its own.
                    transform: Translate {
                        y: {
                            const dpr = Screen.devicePixelRatio
                            // Not mapToItem(): it would not be asked again once the layout places the list.
                            const listTop = layout.y + listArea.y + list.y
                            const deviceY = (listTop + list.contentItem.y + resultRow.y) * dpr
                            return (Math.round(deviceY) - deviceY) / dpr
                        }
                    }
                    onClicked: (index, modifiers) => {
                        const ctrl = (modifiers & Qt.ControlModifier) !== 0
                        if (modifiers & Qt.ShiftModifier) {
                            window.selectTo(index, ctrl)
                        } else if (ctrl) {
                            window.toggleRow(index)
                        } else {
                            list.currentIndex = index
                            window.launcher.trigger(index, Launcher.Open) // a selected row opens the selection
                        }
                    }
                    onActionRequested: (index, action) => {
                        list.currentIndex = index
                        window.launcher.trigger(index, action)
                    }
                    onMenuRequested: (index, globalPos) => window.openContextMenu(index, globalPos)
                    onDragRequested: (index) => window.launcher.dragOut(index)
                }
            }

            ListScrollBar {
                flickable: list
                anchors.right: parent.right
                anchors.top: list.top
                anchors.bottom: parent.bottom
            }

            Text { // held open while file contents are still being searched, or a plugin answers
                readonly property bool keywordMode: window.launcher.pluginKeyword.length > 0
                anchors.centerIn: parent
                // A plugin's wait shows only once it has lasted a moment
                // (pluginWaiting is empty before that), fading in.
                visible: list.count === 0 && window.launcher.searching
                         && (!keywordMode || window.launcher.pluginWaiting.length > 0)
                opacity: visible ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 100 } }
                text: keywordMode ? window.launcher.pluginWaiting : qsTr("正在搜索文件内容…")
                color: Theme.faint
                font.pixelSize: Theme.uiBody
            }

            EmptyState { // at full height, in the list area
                anchors.centerIn: parent
                width: parent.width - 80
                visible: window.fullHeight && window.showEmptyState
                title: window.emptyShownTitle
                hint: window.emptyShownHint
                opacity: window.dimEmpty ? 0.45 : 1
            }

            Text { // at full height, nothing typed and nothing opened yet (under the apps, if any)
                anchors.horizontalCenter: parent.horizontalCenter
                y: list.y + Math.round((list.height - height) / 2)
                width: parent.width - 80
                horizontalAlignment: Text.AlignHCenter
                visible: window.fullHeight && window.trimmedQuery.length === 0 && list.count === 0
                text: qsTr("输入名称或拼音，搜索文件、应用和设置")
                color: Theme.faint
                font.pixelSize: Theme.uiBody
                wrapMode: Text.Wrap
            }
        }

        Item {
            width: parent.width
            height: Math.max(80, emptyBelow.implicitHeight + 24) // taller with hints under it
            visible: window.showEmptyState && !window.clipboardBelow && !window.fullHeight

            EmptyState {
                id: emptyBelow
                anchors.centerIn: parent
                width: parent.width - 80
                title: window.emptyShownTitle
                hint: window.emptyShownHint
                opacity: window.dimEmpty ? 0.45 : 1
            }
        }

        Item { // the strip's room it does not fill yet, as it opens or closes (stripSlack)
            width: parent.width
            height: window.fullHeight ? 0 : window.stripSlack // at the taskbar, in the list area
        }

        Footer {
            id: footer
            width: parent.width
            visible: !window.clipboardBelow
            launcher: window.launcher
            frame: window.frame
            transform: Translate { y: window.stripLeaving ? -window.stripSlack : 0 } // up where it ends
        }
    }

    // While the menu is open, a click anywhere in the launcher closes it
    // (a right click on a row then opens it again there).
    MouseArea {
        anchors.fill: parent
        enabled: window.menuOpen
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => {
            window.closeContextMenu()
            mouse.accepted = mouse.button !== Qt.RightButton
        }
        onWheel: (wheel) => window.closeContextMenu()
    }

    Loader {
        id: menuLoader
        active: false
        sourceComponent: ContextMenu {
            host: window.launcher
            onTriggered: (row, action) => window.launcher.trigger(row, action)
        }
    }

    WindowEdge {}
}
