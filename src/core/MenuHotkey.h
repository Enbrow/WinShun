#pragma once

namespace ws::menuhotkey {

// The folder menu on a hotkey (GitHub #25): what one press means, by what
// the menu is doing. Pressed with the menu up, it closes it; pressed while
// the one it asked for is still on its way (the folder is being asked for
// on a worker), that one is forgotten, so that two quick presses leave the
// menu closed either way. The menu numbers its asks (FolderMenu::m_asks,
// from double clicks too) and drops the answers to all but the latest: an
// ask of ours that is no longer the latest is forgotten already. Pure
// logic, unit-tested; FolderMenu drives it.
class Toggle {
public:
    enum class Action {
        Open, // ask for the menu, and tell asked() the ask's number
        Close, // dismiss the one up
        Drop, // forget the one on its way: make the menu's asks move on
    };

    // `latestAsk`: the number of the menu's latest ask, whoever made it.
    Action pressed(bool menuOpen, int latestAsk) noexcept
    {
        if (menuOpen) {
            m_ask = 0;
            return Action::Close;
        }
        if (pending(latestAsk)) {
            m_ask = 0;
            return Action::Drop;
        }
        return Action::Open;
    }

    void asked(int ask) noexcept { m_ask = ask; }
    // The ask numbered `ask` was answered: the menu came up, or nothing did.
    void answered(int ask) noexcept
    {
        if (ask == m_ask)
            m_ask = 0;
    }
    // Our ask is on its way still: made, not answered, and still the latest.
    bool pending(int latestAsk) const noexcept { return m_ask != 0 && m_ask == latestAsk; }

private:
    int m_ask = 0; // 0: none of ours on its way
};

struct Point {
    int x = 0;
    int y = 0;
};

// Where the menu goes when a key, not a click, asked for it (physical
// pixels): under the pointer when that is over the window the menu is for,
// as a context menu would; else inside that window, `inset` from the
// top-left corner of what has the keyboard focus there (the folder view,
// an edit box), where a keyboard-opened context menu goes.
inline Point anchor(bool pointerOnWindow, Point pointer, Point focusedTopLeft, int inset) noexcept
{
    if (pointerOnWindow)
        return pointer;
    return {focusedTopLeft.x + inset, focusedTopLeft.y + inset};
}

} // namespace ws::menuhotkey
