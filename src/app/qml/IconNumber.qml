import QtQuick
import WinShun

// A row's number on its icon's corner: the modifier of the number keys and
// this digit act on the row (ResultRow: Ctrl+1 opens it; ClipRow: Alt+1
// pastes it). The caller places it (on whole device pixels: the halo is a
// layer, so no blur); not dimmed with the icon. 0: none.
Loader {
    id: root

    property int number: 0

    active: number > 0
    width: 24
    height: 24

    sourceComponent: Item {
        id: digit

        readonly property font face: Qt.font({ family: "Segoe UI", weight: Font.DemiBold, pixelSize: 20 })

        Item { // a halo of the window's dark (light), so it reads over any icon and on the selected row alike
            anchors.fill: parent
            opacity: Theme.dark ? 0.6 : 0.9
            layer.enabled: true // the copies made one shape first, then faded as one

            Repeater {
                model: 17 // itself, then a ring of 16 around it

                CenteredNumber {
                    id: copy

                    required property int index

                    text: root.number
                    font: digit.face
                    color: Theme.dark ? "black" : "white"
                    transform: Translate {
                        x: copy.index > 0 ? 1.5 * Math.cos(copy.index * Math.PI / 8) : 0
                        y: copy.index > 0 ? 1.5 * Math.sin(copy.index * Math.PI / 8) : 0
                    }
                }
            }
        }
        CenteredNumber {
            text: root.number
            font: digit.face
            color: Theme.text
        }
    }
}
