#pragma once

#include <windows.h>

// Where a double click landed, for the folder menu (GitHub #22): on the blank
// space of a File Explorer folder view or of the desktop, or elsewhere.
namespace ws::folderview {

enum class Surface {
    None,
    Explorer, // a folder view in a File Explorer window
    Desktop, // the desktop's icons
    Opus, // a Directory Opus file display: blank space told by Opus itself (FolderMenu::requested)
};

struct Hit {
    Surface surface = Surface::None;
    HWND view = nullptr; // the window clicked on: DirectUIHWND, or the desktop's SysListView32
    HWND tab = nullptr; // the Explorer tab (ShellTabWindowClass; the window itself on Windows 10)
    HWND root = nullptr; // the top-level window
    POINT pt {}; // physical pixels
};

// Which surface `window` (under the pointer, at `pt`) belongs to, by window
// classes alone: quick, sends no messages, for any thread. Everything else
// (the title bar, the navigation pane, the address bar, a name being edited,
// another file manager, whatever covers the desktop) is None.
Hit classify(HWND window, POINT pt);

// Whether the point is on blank space there: no item or its cells, no group
// header, column header or scroll bar. Asks the view through Microsoft Active
// Accessibility (accHitTest; one to a few milliseconds), so it waits on
// Explorer: on a COM thread of its own, never the GUI's. Unsure is not blank.
bool isBlank(const Hit& hit);

} // namespace ws::folderview
