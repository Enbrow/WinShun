import QtQuick
import WinShun

// A row's shortcut while its modifier is held: Ctrl+1 (or Alt+1) acts on the
// row that shows "Ctrl+1". On the rows of the bar by file dialogs (the
// launcher's and the clipboard's rows have theirs on the icon, IconNumber).
// `taken`: another program holds the chord as a global hotkey, so pressing it
// goes there: faint and struck through.
Rectangle {
    id: cap

    property string keys
    property bool taken: false

    width: 26
    height: 26
    radius: 6
    color: Theme.track
    border.width: 1
    border.color: Theme.chipBorder

    CenteredNumber {
        text: cap.number
        color: Theme.text
        font.pixelSize: Theme.fontBody
        font.weight: Font.DemiBold
        font.strikeout: cap.taken
    }
}
