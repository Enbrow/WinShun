pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// The folder menu (FolderMenu, GitHub #22): favorite, recent and open
// folders, brought up by a double click on the blank space of a File
// Explorer folder or of the desktop. Drawn as ContextMenu is; it never takes
// the focus, File Explorer stays in front, and the keys come through
// FolderMenu (keyPressed).
Window {
    id: menu

    required property FolderMenu folderMenu
    property int highlighted: -1

    readonly property var entries: folderMenu.entries
    readonly property int padding: 4
    readonly property int itemHeight: 36

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: Theme.menuBackground
    height: content.implicitHeight + 2 * padding

    // `pos`: global point (logical pixels) for the top-left corner; the menu
    // flips left / up when it would leave the screen there.
    function popup(pos) {
        content.forceLayout() // the new rows' height, before it is placed by it
        let w = 0
        for (let i = 0; i < repeater.count; ++i)
            w = Math.max(w, repeater.itemAt(i).implicitWidth)
        width = Math.min(Math.max(280, w + 2 * padding), 520)
        highlighted = -1

        const area = folderMenu.screenArea(pos)
        let x = pos.x
        let y = pos.y
        if (area.width > 0) {
            if (x + width > area.x + area.width)
                x = x - width
            if (y + height > area.y + area.height)
                y = y - height
            x = Math.max(area.x, Math.min(x, area.x + area.width - width))
            y = Math.max(area.y, Math.min(y, area.y + area.height - height))
        }
        menu.x = x
        menu.y = y
        folderMenu.prepareMenuWindow(menu)
        show()
        raise()
    }

    function selectable(i) {
        const entry = entries[i]
        return entry !== undefined && entry.separator !== true && entry.disabled !== true
    }

    // Next selectable entry from `from` in direction `delta`, wrapping around.
    function step(from, delta) {
        const n = entries.length
        let i = from
        for (let k = 0; k < n; ++k) {
            i = ((i + delta) % n + n) % n
            if (selectable(i))
                return i
        }
        return -1
    }

    // The next entry after the highlighted one whose name starts with `text`.
    function jumpTo(text) {
        const wanted = text.toLowerCase()
        const n = entries.length
        for (let k = 1; k <= n; ++k) {
            const i = ((highlighted < 0 ? -1 : highlighted) + k) % n
            if (selectable(i) && (entries[i].text ?? "").toLowerCase().startsWith(wanted)) {
                highlighted = i
                return
            }
        }
    }

    function handleKey(key, text) {
        switch (key) {
        case Qt.Key_Down: highlighted = step(highlighted, 1); return
        case Qt.Key_Up: highlighted = step(highlighted < 0 ? 0 : highlighted, -1); return
        case Qt.Key_Home:
        case Qt.Key_PageUp: highlighted = step(-1, 1); return
        case Qt.Key_End:
        case Qt.Key_PageDown: highlighted = step(0, -1); return
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (highlighted >= 0)
                folderMenu.trigger(highlighted)
            else
                folderMenu.dismiss()
            return
        case Qt.Key_Escape:
        case Qt.Key_Menu:
            folderMenu.dismiss()
            return
        default:
            if (text.length > 0 && text.trim().length > 0)
                jumpTo(text)
        }
    }

    Connections {
        target: menu.folderMenu
        function onPopupRequested(pos) { menu.popup(pos) }
        function onHideRequested() {
            menu.visible = false
            menu.highlighted = -1
        }
        function onKeyPressed(key, text) { menu.handleKey(key, text) }
    }

    Column {
        id: content
        x: menu.padding
        y: menu.padding

        Repeater {
            id: repeater
            model: menu.entries

            delegate: Item {
                id: entry

                required property var modelData
                required property int index
                readonly property bool separator: modelData.separator === true
                readonly property bool disabled: modelData.disabled === true

                width: menu.width - 2 * menu.padding
                height: separator ? 9 : menu.itemHeight
                implicitWidth: separator ? 0 : 44 + label.implicitWidth + 32 + detail.implicitWidth + 14

                Rectangle {
                    visible: entry.separator
                    anchors.verticalCenter: parent.verticalCenter
                    x: -menu.padding
                    width: menu.width
                    height: 1
                    color: Theme.divider
                }

                Rectangle {
                    visible: !entry.separator && menu.highlighted === entry.index
                    anchors.fill: parent
                    radius: 4
                    color: Theme.menuHover
                }

                Glyph {
                    visible: !entry.separator
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: entry.modelData.glyph ?? ""
                    size: 16
                    color: entry.disabled ? Theme.faint : Theme.text
                }

                Text {
                    id: label
                    visible: !entry.separator
                    x: 44
                    width: Math.min(implicitWidth, entry.width - 44 - 32 - detail.implicitWidth - 14)
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.modelData.text ?? ""
                    elide: Text.ElideRight
                    color: entry.disabled ? Theme.faint : Theme.text
                    font.pixelSize: Theme.fontBody
                }

                Text {
                    id: detail
                    visible: !entry.separator
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.modelData.detail ?? ""
                    color: Theme.faint
                    font.pixelSize: Theme.fontCaption
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: !entry.separator && !entry.disabled
                    hoverEnabled: true
                    onEntered: menu.highlighted = entry.index
                    onExited: if (menu.highlighted === entry.index) menu.highlighted = -1
                    onClicked: menu.folderMenu.trigger(entry.index)
                }
            }
        }
    }

    WindowEdge {}
}
