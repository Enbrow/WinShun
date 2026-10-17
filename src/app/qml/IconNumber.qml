import QtQuick
import WinShun

// A row's shortcut on its icon's top left, running right: "Ctrl+1" opens
// (ResultRow) or pastes (ClipRow) the row. `taken`: another program holds
// that chord as a global hotkey, so pressing it goes there; it still shows,
// faint and struck through, so the gap in the numbers is not a mystery (the
// row's tip says why, from the row's own mouse area: under it, a handler
// here gets no hover). The caller places it (on whole device pixels: the
// halo is a layer, so no blur); not dimmed with the icon. "": none.
Loader {
    id: root

    property string keys
    property bool taken: false

    active: keys.length > 0
    visible: active && opacity > 0
    height: 16 // the width is the text's

    sourceComponent: Item {
        id: label

        readonly property font face: Qt.font({ family: "Segoe UI", weight: Font.Normal, pixelSize: 11,
                                               strikeout: root.taken })

        width: Math.ceil(ink.advanceWidth) + 4
        height: root.height

        TextMetrics {
            id: ink
            font: label.face
            text: root.keys
        }

        Item { // a halo of the window's dark (light), so it reads over any icon and on the selected row alike
            anchors.fill: parent
            opacity: Theme.dark ? 0.55 : 0.85
            layer.enabled: true // the copies made one shape first, then faded as one

            Repeater {
                model: 17 // itself, then a ring of 16 around it

                Text {
                    id: copy

                    required property int index

                    x: 2
                    y: 12 - baselineOffset // the baseline 12 px down
                    text: root.keys
                    textFormat: Text.PlainText
                    renderType: Text.CurveRendering
                    font: label.face
                    color: Theme.dark ? "black" : "white"
                    transform: Translate {
                        x: copy.index > 0 ? Math.cos(copy.index * Math.PI / 8) : 0
                        y: copy.index > 0 ? Math.sin(copy.index * Math.PI / 8) : 0
                    }
                }
            }
        }
        Text {
            x: 2
            y: 12 - baselineOffset
            text: root.keys
            textFormat: Text.PlainText
            renderType: Text.CurveRendering
            font: label.face
            color: root.taken ? Theme.faint : Theme.subtext
        }
    }
}
