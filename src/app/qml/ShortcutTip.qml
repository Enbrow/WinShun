pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The shortcuts a footer has no room for, shown while the mouse rests on its
// info button (ShortcutsButton): the keys, and beside them what they do.
// A window of its own (as ContextMenu), so it is not cut off by a launcher
// that is short (nothing typed: just the search box and the footer). It
// never takes the focus, nor the mouse. `host` is the launcher or the
// clipboard: the screen area and the native styling of the window (rounded,
// shadowed).
Window {
    id: tip

    required property QtObject host
    // [[keys, what they do], …]
    property var rows: []

    readonly property int padding: 12

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
           | Qt.WindowTransparentForInput
    color: Theme.menuBackground
    width: Math.ceil(grid.implicitWidth) + 2 * padding
    height: Math.ceil(grid.implicitHeight) + 2 * padding

    property Item button: null

    function onPixel(v) { return Math.round(v * Screen.devicePixelRatio) / Screen.devicePixelRatio }

    // Over `button` (under it when there is no room above), its right
    // edge at the button's, kept on the screen.
    function popup(button) {
        tip.button = button
        grid.forceLayout() // its size now, not after the next frame: placed by it
        place()
        host.prepareMenuWindow(tip)
        show()
    }

    function place() {
        if (!button)
            return
        const gap = 6
        const pos = button.mapToGlobal(0, 0)
        const area = host.screenArea(pos)
        let x = pos.x + button.width - width
        let y = pos.y - height - gap
        if (area.width > 0) {
            if (y < area.y)
                y = pos.y + button.height + gap
            x = Math.max(area.x, Math.min(x, area.x + area.width - width))
            y = Math.max(area.y, Math.min(y, area.y + area.height - height))
        }
        tip.x = x
        tip.y = y
    }
    // The rows changed while shown (another number key setting): still over the button.
    onWidthChanged: if (visible) place()
    onHeightChanged: if (visible) place()

    Grid {
        id: grid
        x: tip.padding
        y: tip.padding
        columns: 2
        columnSpacing: 14
        rowSpacing: 8
        verticalItemAlignment: Grid.AlignVCenter

        Repeater {
            // Keys, what they do, keys… (`rows` arrives as a list of lists,
            // which has no flat()).
            model: {
                const cells = []
                for (let i = 0; i < tip.rows.length; ++i)
                    cells.push(tip.rows[i][0], tip.rows[i][1])
                return cells
            }

            delegate: Loader {
                id: cell

                required property int index
                required property string modelData

                sourceComponent: index % 2 === 0 ? keysCell : labelCell

                Component {
                    id: keysCell

                    Rectangle { // a key cap, as the footer's (KeyHint)
                        id: cap
                        width: keyText.implicitWidth + 10
                        height: Theme.keycapHeight
                        radius: Theme.controlRadius
                        color: Theme.keycap

                        Text {
                            id: keyText
                            x: tip.onPixel((cap.width - width) / 2)
                            y: tip.onPixel((cap.height - height) / 2)
                            text: cell.modelData
                            color: Theme.subtext
                            font.pixelSize: Theme.uiCaption
                        }
                    }
                }

                Component {
                    id: labelCell

                    Text {
                        text: cell.modelData
                        color: Theme.text
                        font.pixelSize: Theme.fontCaption
                    }
                }
            }
        }
    }
}
