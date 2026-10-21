pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The capture toolbar's second row (docs/design-capture-roadmap.md, 3.5):
// the style of the tool that is on, or of the selected object, as
// AnnotationModel.panel describes it. What shows depends on the panel's kind
// and keys; a change goes back through setPanel() and the other calls. One
// row of fixed height, so the toolbar never moves when it appears. Shown
// with a short fade; hidden at once.
Rectangle {
    id: panel

    required property AnnotationModel annot
    property real dpr: 1
    // Which way it opens: +1 below the toolbar, -1 above (the slide's direction).
    property int direction: 1

    // ---- geometry, logical px ----
    readonly property int pad: 4 // card edge to the controls
    readonly property int controlHeight: 32
    readonly property int cellWidth: 28 // a swatch, a size dot, a fill or toggle button
    readonly property int swatchSize: 20
    readonly property int checkSize: 12 // the tick on the chosen swatch
    readonly property var dots: [4, 8, 12] // the three widths' dots, thin to thick
    readonly property int fillIcon: 14 // the fill buttons' square
    readonly property int boxIconWidth: 18 // the box button's frame
    readonly property int boxIconHeight: 16
    readonly property int boxLetter: 11 // its "A", pixel size
    readonly property int stepGlyph: 12 // the start value's minus and plus
    readonly property int layerGlyph: 14 // up, down, delete
    readonly property int boldGlyph: 16
    readonly property int startWidth: 28 // the start value's number
    readonly property int textPad: 8 // each side of a word button's text
    readonly property int separatorWidth: 12 // a 1 device px line in the middle
    readonly property int separatorHeight: 20
    readonly property int showMs: 120
    readonly property int slide: 4 // the fade's travel

    // ---- what it shows ----
    readonly property var p: annot ? annot.panel : ({})
    readonly property int kind: p.kind !== undefined ? p.kind : -1 // AnnotationStyles: Rect 0 … Redact 8
    readonly property bool redact: kind === 8
    readonly property bool showColors: p.color !== undefined && (!redact || p.mode === "solid")
    readonly property bool showWidths: p.widths !== undefined
    readonly property bool showFill: p.fill !== undefined
    readonly property bool showSizes: p.sizes !== undefined
    readonly property bool showText: p.bold !== undefined
    readonly property bool showCells: p.cells !== undefined && p.mode !== "solid"
    readonly property bool showNumber: p.start !== undefined
    readonly property bool showObject: p.selected === true

    property PanelButton tipItem: null

    function snap(v) { return Math.round(v * dpr) / dpr }
    function change(map) { if (annot) annot.setPanel(map) }
    // The words and marks on a swatch of the user's colour: dark on yellow and
    // white, where white would not show (annot::inkOn); white elsewhere.
    function inkOn(c) {
        const k = Qt.color(c)
        return 0.299 * k.r + 0.587 * k.g + 0.114 * k.b > 0.7 ? "#212121" : Theme.accentText
    }
    function same(a, b) { return String(a).toUpperCase() === String(b).toUpperCase() }

    implicitWidth: row.width + 2 * pad
    implicitHeight: controlHeight + 2 * pad
    radius: Theme.controlRadius
    color: Theme.background
    border.width: 1
    border.color: Theme.windowEdge
    transform: Translate { id: shift }

    onVisibleChanged: {
        showing.stop()
        if (visible) {
            showing.restart()
        } else {
            opacity = 1
            shift.y = 0
        }
    }

    ParallelAnimation {
        id: showing
        NumberAnimation { target: panel; property: "opacity"; from: 0; to: 1; duration: panel.showMs; easing.type: Easing.OutCubic }
        NumberAnimation { target: shift; property: "y"; from: -panel.direction * panel.slide; to: 0; duration: panel.showMs; easing.type: Easing.OutCubic }
    }

    MouseArea { // presses between the controls stay here, not on the picture beneath
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.ArrowCursor
        onWheel: (wheel) => { wheel.accepted = true }
    }

    Row {
        id: row
        x: panel.pad
        y: panel.pad
        height: panel.controlHeight

        // Redaction: its kind first, then the solid colour or the cell size.
        Row {
            visible: panel.redact
            Repeater {
                model: [["mosaic", qsTr("马赛克")], ["blur", qsTr("模糊")], ["solid", qsTr("纯色")]]
                delegate: WordButton {
                    required property var modelData
                    words: modelData[1]
                    checked: panel.p.mode === modelData[0]
                    onClicked: panel.change({mode: modelData[0]})
                }
            }
        }

        Separator { visible: panel.redact && panel.showColors }
        Row { // the eight colours
            visible: panel.showColors
            Repeater {
                model: panel.p.colors !== undefined ? panel.p.colors : []
                delegate: PanelButton {
                    id: swatch
                    required property string modelData
                    readonly property bool chosen: panel.same(panel.p.color, modelData)
                    tip: modelData
                    onClicked: panel.change({color: modelData})

                    Rectangle {
                        x: panel.snap((panel.cellWidth - panel.swatchSize) / 2)
                        y: panel.snap((panel.controlHeight - panel.swatchSize) / 2)
                        width: panel.swatchSize
                        height: panel.swatchSize
                        radius: Theme.smallControlRadius
                        color: swatch.modelData
                        border.width: 1
                        border.color: Theme.windowEdge

                        Glyph {
                            anchors.centerIn: parent
                            visible: swatch.chosen
                            glyph: String.fromCharCode(0xE73E) // CheckMark
                            size: panel.checkSize
                            color: panel.inkOn(swatch.modelData)
                        }
                    }
                }
            }
        }

        Separator { visible: panel.showWidths }
        Row { // line widths: dots growing with the width
            visible: panel.showWidths
            Repeater {
                model: panel.p.widths !== undefined ? panel.p.widths : []
                delegate: PanelButton {
                    id: widthButton
                    required property int index
                    required property int modelData
                    readonly property int dot: panel.dots[Math.min(index, panel.dots.length - 1)]
                    checked: panel.p.width === modelData
                    tip: qsTr("粗细 %1").arg(modelData)
                    tipKeys: "[  ]"
                    onClicked: panel.change({width: modelData})

                    Rectangle {
                        x: panel.snap((panel.cellWidth - widthButton.dot) / 2)
                        y: panel.snap((panel.controlHeight - widthButton.dot) / 2)
                        width: widthButton.dot
                        height: widthButton.dot
                        radius: widthButton.dot / 2
                        color: widthButton.foreground
                    }
                }
            }
            ValueLabel { // a width between the presets ([ and ], the wheel)
                value: panel.p.width !== undefined ? panel.p.width : 0
                presets: panel.p.widths !== undefined ? panel.p.widths : []
            }
        }

        Separator { visible: panel.showFill }
        Row { // fill: none, soft, solid
            visible: panel.showFill
            Repeater {
                model: [["none", qsTr("不填充")], ["soft", qsTr("浅色填充")], ["solid", qsTr("实心填充")]]
                delegate: PanelButton {
                    id: fillButton
                    required property int index
                    required property var modelData
                    checked: panel.p.fill === modelData[0]
                    tip: modelData[1]
                    onClicked: panel.change({fill: modelData[0]})

                    Rectangle {
                        x: panel.snap((panel.cellWidth - panel.fillIcon) / 2)
                        y: panel.snap((panel.controlHeight - panel.fillIcon) / 2)
                        width: panel.fillIcon
                        height: panel.fillIcon
                        radius: Theme.smallControlRadius
                        color: fillButton.index === 0 ? "transparent"
                             : fillButton.index === 1 ? Qt.rgba(fillButton.foreground.r, fillButton.foreground.g, fillButton.foreground.b, 0.3)
                             : fillButton.foreground
                        border.width: 1
                        border.color: fillButton.foreground
                    }
                }
            }
        }

        Separator { visible: panel.showSizes }
        Row { // font sizes (text, numbers)
            visible: panel.showSizes
            Repeater {
                model: panel.p.sizes !== undefined ? panel.p.sizes : []
                delegate: WordButton {
                    required property int modelData
                    words: String(modelData)
                    checked: panel.p.size === modelData
                    tip: qsTr("字号 %1").arg(modelData)
                    tipKeys: "[  ]"
                    onClicked: panel.change({size: modelData})
                }
            }
            ValueLabel {
                value: panel.p.size !== undefined ? panel.p.size : 0
                presets: panel.p.sizes !== undefined ? panel.p.sizes : []
            }
        }

        Separator { visible: panel.showText }
        Row { // bold, and the coloured box behind the words
            visible: panel.showText
            PanelButton {
                id: boldButton
                checked: panel.p.bold === true
                tip: qsTr("粗体")
                onClicked: panel.change({bold: !boldButton.checked})

                Glyph {
                    anchors.centerIn: parent
                    glyph: String.fromCharCode(0xE8DD) // Bold
                    size: panel.boldGlyph
                    color: boldButton.foreground
                }
            }
            PanelButton {
                id: boxButton
                checked: panel.p.boxed === true
                tip: qsTr("底框")
                onClicked: panel.change({boxed: !boxButton.checked})

                Rectangle {
                    x: panel.snap((panel.cellWidth - panel.boxIconWidth) / 2)
                    y: panel.snap((panel.controlHeight - panel.boxIconHeight) / 2)
                    width: panel.boxIconWidth
                    height: panel.boxIconHeight
                    radius: Theme.smallControlRadius
                    color: "transparent"
                    border.width: 1
                    border.color: boxButton.foreground

                    Text {
                        anchors.centerIn: parent
                        text: "A"
                        color: boxButton.foreground
                        font.pixelSize: panel.boxLetter
                        font.weight: Font.DemiBold
                    }
                }
            }
        }

        Separator { visible: panel.showCells }
        Row { // mosaic cells
            visible: panel.showCells
            Repeater {
                model: panel.p.cells !== undefined ? panel.p.cells : []
                delegate: WordButton {
                    required property int modelData
                    words: String(modelData)
                    checked: panel.p.cell === modelData
                    tip: qsTr("格子 %1").arg(modelData)
                    tipKeys: "[  ]"
                    onClicked: panel.change({cell: modelData})
                }
            }
            ValueLabel {
                value: panel.p.cell !== undefined ? panel.p.cell : 0
                presets: panel.p.cells !== undefined ? panel.p.cells : []
            }
        }

        Separator { visible: panel.showNumber }
        Row { // numbers: the group's first number, a new group
            visible: panel.showNumber
            WordLabel { words: qsTr("起始") }
            PanelButton {
                enabled: panel.p.start !== undefined && panel.p.start > 0
                tip: qsTr("起始值减 1")
                onClicked: panel.annot.setStart(panel.p.start - 1)

                Glyph {
                    anchors.centerIn: parent
                    glyph: String.fromCharCode(0xE738) // Remove
                    size: panel.stepGlyph
                    color: Theme.text
                }
            }
            WordLabel {
                words: panel.p.start !== undefined ? String(panel.p.start) : ""
                minWidth: panel.startWidth
                strong: true
            }
            PanelButton {
                enabled: panel.p.start !== undefined && panel.p.start < 99999
                tip: qsTr("起始值加 1")
                onClicked: panel.annot.setStart(panel.p.start + 1)

                Glyph {
                    anchors.centerIn: parent
                    glyph: String.fromCharCode(0xE710) // Add
                    size: panel.stepGlyph
                    color: Theme.text
                }
            }
            WordButton {
                words: qsTr("另起一组")
                tip: qsTr("之后的序号从 1 重新数")
                onClicked: panel.annot.newSeries()
            }
        }

        Separator { visible: panel.showObject }
        Row { // the selected object: a layer up, a layer down, away
            visible: panel.showObject
            PanelButton {
                id: raiseButton
                enabled: panel.p.canRaise === true
                tip: qsTr("上移一层")
                tipKeys: "Ctrl+]"
                onClicked: panel.annot.raise(false)

                Glyph {
                    anchors.centerIn: parent
                    glyph: String.fromCharCode(0xE74A) // Up
                    size: panel.layerGlyph
                    color: raiseButton.foreground
                }
            }
            PanelButton {
                id: lowerButton
                enabled: panel.p.canLower === true
                tip: qsTr("下移一层")
                tipKeys: "Ctrl+["
                onClicked: panel.annot.lower(false)

                Glyph {
                    anchors.centerIn: parent
                    glyph: String.fromCharCode(0xE74B) // Down
                    size: panel.layerGlyph
                    color: lowerButton.foreground
                }
            }
            PanelButton {
                id: deleteButton
                tip: qsTr("删除")
                tipKeys: "Delete"
                onClicked: panel.annot.deleteSelection()

                Glyph {
                    anchors.centerIn: parent
                    glyph: String.fromCharCode(0xE74D) // Delete
                    size: panel.layerGlyph
                    color: deleteButton.foreground
                }
            }
        }
    }

    HoverTip {
        target: panel.visible ? panel.tipItem : null
        text: panel.tipItem ? panel.tipItem.tip : ""
        shortcut: panel.tipItem ? panel.tipItem.tipKeys : ""
    }

    // ---- the parts ----

    // A square button: accentFill and white marks when checked. Its marks
    // are its children, coloured with `foreground`.
    component PanelButton: Rectangle {
        id: button

        property bool checked: false
        property string tip
        property string tipKeys
        readonly property color foreground: checked ? Theme.accentText : Theme.text

        signal clicked()

        width: panel.cellWidth
        height: panel.controlHeight
        radius: Theme.controlRadius
        opacity: enabled ? 1 : 0.45
        color: checked
               ? (area.pressed ? Qt.darker(Theme.accentFill, 1.15) : area.containsMouse ? Qt.lighter(Theme.accentFill, 1.08) : Theme.accentFill)
               : (area.pressed ? Theme.controlPressed : area.containsMouse ? Theme.controlHover : "transparent")
        Accessible.role: Accessible.Button
        Accessible.name: tip

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
            onContainsMouseChanged: {
                if (containsMouse)
                    panel.tipItem = button
                else if (panel.tipItem === button)
                    panel.tipItem = null
            }
        }
    }

    // A button with a word or a number on it, as wide as that needs (in
    // steps of 4 logical px, whole device pixels at 125 %, 150 %, 175 %).
    component WordButton: PanelButton {
        id: wordButton

        property string words

        width: Math.max(panel.cellWidth, Math.ceil((wordText.implicitWidth + 2 * panel.textPad) / 4) * 4)
        Accessible.name: words

        Text {
            id: wordText
            anchors.centerIn: parent
            text: wordButton.words
            color: wordButton.foreground
            font.pixelSize: Theme.fontCaption
        }
    }

    // Words that are not a button.
    component WordLabel: Item {
        id: label

        property string words
        property int minWidth: 0
        property bool strong: false

        width: Math.max(minWidth, Math.ceil((labelText.implicitWidth + 2 * panel.textPad) / 4) * 4)
        height: panel.controlHeight

        Text {
            id: labelText
            anchors.centerIn: parent
            text: label.words
            color: label.strong ? Theme.text : Theme.subtext
            font.pixelSize: Theme.fontCaption
        }
    }

    // A width, size or cell between the presets ([ and ] go through more
    // steps): shown beside them, so the panel never hides what is in use.
    component ValueLabel: WordLabel {
        property int value
        property var presets: []

        visible: value > 0 && presets.indexOf(value) < 0
        words: String(value)
        strong: true
    }

    // A thin upright line between groups, on a whole device pixel.
    component Separator: Item {
        width: panel.separatorWidth
        height: panel.controlHeight

        Rectangle {
            x: panel.snap(panel.separatorWidth / 2)
            y: panel.snap((panel.controlHeight - panel.separatorHeight) / 2)
            width: Math.max(1, Math.round(panel.dpr)) / panel.dpr
            height: panel.separatorHeight
            color: Theme.controlBorder
        }
    }
}
