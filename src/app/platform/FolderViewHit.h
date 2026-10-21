#pragma once

#include <windows.h>

// Where a double click landed, for the folder menu (GitHub #22): on the blank
// space of a File Explorer folder view or of the desktop, or elsewhere. And
// which window the menu's hotkey (GitHub #25) found in front.
namespace ws::folderview {

enum class Surface {
    None,
    Explorer, // a folder view in a File Explorer window
    Desktop, // the desktop's icons
    Opus, // a Directory Opus file display: blank space told by Opus itself (FolderMenu::requested), or its lister in front
    // The hotkey only (FolderMenu::hotkeyPressed):
    TotalCommander, // its window in front: the panel with the focus
    XYplorer, // its window in front: the tab with the focus
    Pulse, // its window in front: the pane with the focus in the tab on top (GitHub #41)
    Dialog, // an Open or Save dialog in front: chosen, it goes there
    Other, // any other window: chosen, the file manager opens it
};

struct Hit {
    Surface surface = Surface::None;
    HWND view = nullptr; // the window clicked on: DirectUIHWND, or the desktop's SysListView32
    HWND tab = nullptr; // the Explorer tab (ShellTabWindowClass; the window itself on Windows 10)
    HWND root = nullptr; // the top-level window
    POINT pt {}; // physical pixels: where the menu goes
};

// Which surface `window` (under the pointer, at `pt`) belongs to, by window
// classes alone: quick, sends no messages, for any thread. Everything else
// (the title bar, the navigation pane, the address bar, a name being edited,
// another file manager, whatever covers the desktop) is None.
Hit classify(HWND window, POINT pt);

// Whether the top-level window `root` is a File Explorer folder window (the
// tab on top is the Hit's) or the desktop, by window classes alone; None
// for anything else. For the hotkey, which has no point: `pt` is left zero.
Hit classifyWindow(HWND root);

// Whether the point is on blank space there: no item or its cells, no group
// header, column header or scroll bar. Asks the view through Microsoft Active
// Accessibility (accHitTest; one to a few milliseconds), so it waits on
// Explorer: on a COM thread of its own, never the GUI's. Unsure is not blank.
bool isBlank(const Hit& hit);

} // namespace ws::folderview
