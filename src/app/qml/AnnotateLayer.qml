pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// What lies over the annotation canvas in a capture overlay (docs/design-
// capture-roadmap.md, 3.5-3.9): the selected object's outline and handles,
// the text box while a text is typed, the small box that renumbers a
// number, and the short words by the selection (the first tool's hint, "H
// shows the toolbar", the session's notices). Positions from the model are
// image px (the monitor's physical pixels); px() and py() make them this
// window's logical units, as CaptureOverlay does. Nothing here takes the
// mouse except the text box, the number box and the word bars: the rest
// passes to the overlay's MouseArea beneath.
Item {
    id: over

    required property CaptureSession session
    required property CaptureScreen screenModel
    required property AnnotationModel annot
    // The overlay's key handler: focus goes back to it when typing ends.
    required property Item keyTarget
    // The selection is on this monitor.
    property bool here: false
    // The toolbar's first row and its rows on the screen now (with the
    // style panel when it shows), logical px in this window, and where it
    // sits: "below", "above" or "inside" the selection. Shown:
    // CaptureOverlay shows it now.
    property rect barRect
    property rect blockRect
    property string toolbarSide: "below"
    property bool toolbarShown: false

    // ---- geometry, logical px unless named image px ----
    readonly property int hintGap: 8 // a word bar off the toolbar or the selection
    readonly property int margin: 4 // a word bar off the screen's edge
    readonly property int noticeTop: 16 // a notice with no selection here: from the top
    readonly property int barHeight: 28 // a word bar
    readonly property int barPad: 10 // each side of a word bar's words
    readonly property int numberWidth: 64 // the renumbering box
    readonly property int numberHeight: 32
    readonly property int numberPad: 6 // inside it, each side
    readonly property int hintMs: 6000 // the first tool's hint stays this long
    readonly property int fadeInMs: 120
    readonly property int fadeOutMs: 200
    // Handles: an odd number of device pixels, centred on the point, as the
    // selection's own (CaptureOverlay's frame).
    readonly property int handle: Math.round(7 * dpr) | 1

    readonly property real sx: screenModel.scaleX
    readonly property real sy: screenModel.scaleY
    readonly property real dpr: screenModel.dpr
    readonly property rect selection: screenModel.selection

    function px(v) { return v * sx }
    function py(v) { return v * sy }
    function snap(v) { return Math.round(v * dpr) / dpr }
    function clampX(x, w) { return Math.max(margin, Math.min(x, width - w - margin)) }
    function clampY(y, h) { return Math.max(margin, Math.min(y, height - h - margin)) }

    // ---- the selected object ----

    Repeater {
        model: over.here && over.annot ? over.annot.selection : []

        delegate: Item {
            id: picked

            required property var modelData
            // The outline's pixels (image px), just outside the object's bounds.
            readonly property int edgeLeft: Math.floor(modelData.x)
            readonly property int edgeTop: Math.floor(modelData.y)
            readonly property int edgeRight: Math.ceil(modelData.x + modelData.width)
            readonly property int edgeBottom: Math.ceil(modelData.y + modelData.height)

            Rectangle { // a line has only its two end handles
                visible: picked.modelData.handles.length !== 2
                x: over.px(picked.edgeLeft - 1)
                y: over.py(picked.edgeTop - 1)
                width: over.px(picked.edgeRight - picked.edgeLeft + 2)
                height: over.py(picked.edgeBottom - picked.edgeTop + 2)
                color: "transparent"
                border.width: over.px(1)
                border.color: Theme.accent
            }

            Repeater {
                model: picked.modelData.handles

                delegate: Rectangle {
                    required property var modelData

                    // Centred on the point: a box's edge (a whole pixel line)
                    // or a line's end (a pixel's centre), on whole pixels.
                    x: over.px(Math.round(modelData.x - over.handle / 2))
                    y: over.py(Math.round(modelData.y - over.handle / 2))
                    width: over.px(over.handle)
                    height: over.py(over.handle)
                    color: Theme.accentFill
                    border.width: over.px(1)
                    border.color: Theme.accentText
                }
            }
        }
    }

    // ---- the text being typed ----

    Item {
        id: textBox

        readonly property var ed: over.annot ? over.annot.editing : ({})
        readonly property bool active: over.here && ed.fontPx !== undefined
        // A new edit: another text, or the same one again after it ended.
        readonly property string editKey: active ? ed.id + "," + ed.x + "," + ed.y : ""
        // Image px, as the compositor measures the text (annot::layoutText).
        readonly property real descent: ed.descent !== undefined ? ed.descent : (ed.pitch ?? 0) - (ed.ascent ?? 0)
        readonly property int lines: Math.max(1, edit.lineCount)
        readonly property int inkWidth: Math.ceil(edit.contentWidth / over.sx - 0.01)

        function start() {
            // Set once: the model's map changes again while typing (a style
            // change on the panel), and setting the words again would move
            // the caret and drop what the input method is composing.
            edit.text = ed.text ?? ""
            Qt.callLater(textBox.focusCaret) // once the font and the layout follow the new map
        }

        // The window active, the box focused, the caret where the press was
        // (at the end for a new text).
        function focusCaret() {
            if (!active)
                return
            const w = over.Window.window
            if (w)
                w.requestActivate()
            edit.forceActiveFocus()
            if (ed.clickX < 0)
                edit.cursorPosition = edit.length
            else
                edit.cursorPosition = edit.positionAt(over.px(ed.x + ed.clickX) - edit.x, over.py(ed.y + ed.clickY) - edit.y)
        }

        function end() {
            edit.clear()
            over.keyTarget.forceActiveFocus()
        }

        anchors.fill: parent
        visible: active
        onEditKeyChanged: editKey.length > 0 ? start() : end()

        Rectangle { // the box behind boxed words, as the compositor will draw it
            visible: textBox.ed.boxed === true
            x: over.px(textBox.ed.x - textBox.ed.pad)
            y: over.py(textBox.ed.y - textBox.ed.pad)
            width: over.px(textBox.inkWidth + 2 * textBox.ed.pad)
            height: over.py(textBox.ed.ascent + textBox.descent + (textBox.lines - 1) * textBox.ed.pitch + 2 * textBox.ed.pad)
            radius: over.px(textBox.ed.radius)
            color: textBox.ed.fill ?? "transparent"
        }

        TextEdit {
            id: edit

            // The first baseline where the compositor puts it (top + ascent,
            // image px). Later lines keep Qt's own spacing, which may differ
            // from the model's pitch by a fraction of a pixel per line.
            x: over.px(textBox.ed.x ?? 0)
            y: over.py((textBox.ed.y ?? 0) + (textBox.ed.ascent ?? 0)) - metrics.ascent
            textFormat: TextEdit.PlainText
            renderType: TextEdit.NativeRendering
            wrapMode: TextEdit.NoWrap
            font.family: over.annot ? over.annot.fontFamily : ""
            // In points: font.pixelSize takes whole logical px only (3.8).
            font.pointSize: Math.max(1, (textBox.ed.fontPx ?? 18) * over.sx * 0.75)
            font.bold: textBox.ed.bold === true
            font.hintingPreference: Font.PreferVerticalHinting // as annot::annotationFont
            color: textBox.ed.color ?? "#000000"
            selectionColor: Theme.textSelection
            selectedTextColor: color

            // The model reads the words from here when the session commits
            // them (a press outside, another tool, Ctrl+S, F3, the hotkey).
            onTextChanged: if (textBox.active && over.annot) over.annot.setEditingText(text)

            Keys.onPressed: (event) => {
                const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
                if (ctrl && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                    over.annot.commitText(edit.text)
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    if (edit.inputMethodComposing)
                        return // the input method's: it drops the pinyin
                    over.annot.commitText(edit.text)
                    event.accepted = true
                } else if ((ctrl && event.key === Qt.Key_S) || event.key === Qt.Key_F3) {
                    const key = event.key
                    const modifiers = event.modifiers
                    over.annot.commitText(edit.text)
                    over.session.keyPressed(key, modifiers)
                    event.accepted = true
                }
            }
        }

        FontMetrics {
            id: metrics
            font: edit.font
        }
    }

    // ---- a number being renumbered (double click) ----

    Rectangle {
        id: numberBox

        readonly property var ne: over.annot ? over.annot.numberEdit : ({})
        readonly property bool active: over.here && ne.id !== undefined
        readonly property string editKey: active ? String(ne.id) : ""

        visible: active
        x: over.snap(over.clampX(over.px(ne.x ?? 0) - width / 2, width))
        y: over.snap(over.clampY(over.py(ne.y ?? 0) - height / 2, height))
        width: over.numberWidth
        height: over.numberHeight
        radius: Theme.controlRadius
        color: Theme.background
        border.width: 1
        border.color: Theme.windowEdge
        onEditKeyChanged: {
            if (editKey.length > 0) {
                numberInput.text = String(ne.shown ?? "")
                Qt.callLater(numberBox.focusInput)
            } else if (numberInput.activeFocus) {
                over.keyTarget.forceActiveFocus()
            }
        }

        function focusInput() {
            if (!active)
                return
            const w = over.Window.window
            if (w)
                w.requestActivate()
            numberInput.forceActiveFocus()
            numberInput.selectAll()
        }

        MouseArea { // presses on the box stay with it
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.IBeamCursor
            onPressed: numberInput.forceActiveFocus()
        }

        TextInput {
            id: numberInput
            anchors.fill: parent
            leftPadding: over.numberPad
            rightPadding: over.numberPad
            horizontalAlignment: TextInput.AlignHCenter
            verticalAlignment: TextInput.AlignVCenter
            validator: IntValidator { bottom: 0; top: 99999 }
            inputMethodHints: Qt.ImhDigitsOnly
            selectByMouse: true
            color: Theme.text
            selectionColor: Theme.textSelection
            selectedTextColor: Theme.text
            font.pixelSize: Theme.fontBody

            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    // Empty: counted again (setNumber takes "").
                    const t = numberInput.text.trim()
                    over.annot.setNumber(numberBox.ne.id, t.length > 0 ? parseInt(t, 10) : "")
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    over.annot.closeNumberEdit()
                    event.accepted = true
                }
            }
            // Focus gone elsewhere (another window, a press the model did
            // not see): the box closes, as a press on the picture closes it.
            onActiveFocusChanged: if (!activeFocus && numberBox.active) over.annot.closeNumberEdit()
        }
    }

    // ---- words by the toolbar and the selection ----

    // The first time a tool is chosen (once ever: session.toolHintSeen):
    // left of the toolbar, or past it on the side away from the selection.
    FadeBar {
        id: toolHint

        readonly property bool leftRoom: over.barRect.x - over.hintGap - width >= over.margin

        allowed: over.here && over.toolbarShown
        words: qsTr("再按一次关掉 · [ ] 改粗细 · H 隐藏工具条")
        x: over.snap(leftRoom ? over.barRect.x - over.hintGap - width
                               : over.clampX(over.barRect.x + over.barRect.width - width, width))
        y: over.snap(leftRoom ? over.barRect.y + (over.barRect.height - height) / 2
                               : over.clampY(over.toolbarSide === "below" ? over.blockRect.y + over.blockRect.height + over.hintGap
                                                                            : over.blockRect.y - over.hintGap - height, height))
        onClicked: shown = false
    }

    Timer {
        id: hintTimer
        interval: over.hintMs
        onTriggered: toolHint.shown = false
    }

    Connections {
        target: over.annot
        function onToolChanged() {
            if (!over.here)
                return
            if (over.annot.tool < 0) {
                hintTimer.stop()
                toolHint.shown = false
            } else if (!over.session.toolHintSeen && over.toolbarShown) {
                over.session.toolHintSeen = true
                toolHint.shown = true
                hintTimer.restart()
            }
        }
    }

    // H hid the toolbar: where it was, a word on how to have it back.
    FadeBar {
        allowed: over.here
        shown: over.here && over.session.toolbarHidden && !over.screenModel.adjusting
        words: qsTr("按 H 显示工具条")
        x: over.snap(over.clampX(over.barRect.x + over.barRect.width - width, width))
        y: over.snap(over.clampY(over.toolbarSide === "below" ? over.barRect.y
                                                                 : over.barRect.y + over.barRect.height - height, height))
    }

    // The session's word for the user (screenModel.notice), on the side of
    // the selection away from the toolbar; with no selection here, at the
    // top of the screen.
    FadeBar {
        id: noticeBar

        readonly property string notice: over.screenModel.notice
        readonly property real selTop: over.py(over.selection.y)
        readonly property real selBottom: over.py(over.selection.y + over.selection.height)
        readonly property bool under: over.toolbarSide === "above"

        shown: notice.length > 0
        x: over.snap(over.here ? over.clampX(over.px(over.selection.x + over.selection.width / 2) - width / 2, width)
                                 : (over.width - width) / 2)
        y: over.snap(!over.here ? over.noticeTop
                      : under ? (selBottom + over.hintGap + height <= over.height - over.margin ? selBottom + over.hintGap
                                                                                                   : selBottom - over.hintGap - height)
                      : (selTop - over.hintGap - height >= over.margin ? selTop - over.hintGap - height
                                                                         : selTop + over.hintGap))
        // The words stay while it fades out.
        onNoticeChanged: if (notice.length > 0) words = notice
        Component.onCompleted: if (notice.length > 0) words = notice
    }

    // A short line of words in a small card (Theme.background, a 1 px grey
    // edge): fades in over fadeInMs, out over fadeOutMs, the fade-out at once.
    component FadeBar: Rectangle {
        id: fade

        property string words
        property bool shown: false
        property bool allowed: true

        signal clicked()

        width: Math.ceil((label.implicitWidth + 2 * over.barPad) / 4) * 4
        height: over.barHeight
        radius: Theme.controlRadius
        color: Theme.background
        border.width: 1
        border.color: Theme.windowEdge
        opacity: 0
        visible: opacity > 0 && allowed
        Component.onCompleted: if (shown) opacity = 1
        onShownChanged: {
            if (shown) {
                fadeOut.stop()
                fadeIn.restart()
            } else {
                fadeIn.stop()
                fadeOut.restart()
            }
        }

        NumberAnimation { id: fadeIn; target: fade; property: "opacity"; to: 1; duration: over.fadeInMs }
        NumberAnimation { id: fadeOut; target: fade; property: "opacity"; to: 0; duration: over.fadeOutMs }

        Text {
            id: label
            anchors.centerIn: parent
            text: fade.words
            color: Theme.text
            font.pixelSize: Theme.fontCaption
        }

        MouseArea { // its presses stay with it, not on the picture beneath
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.ArrowCursor
            onClicked: fade.clicked()
        }
    }
}
