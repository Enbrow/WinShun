#pragma once

#include <algorithm>

namespace ws::boxside {

// Which side of a box a window attached to it opens on (Placement::attach):
// the clipboard by the text caret it pastes at, or by a search box of ours.
// On either side the window gets as many rows as fit between the box and the
// work area's edge there (Placement.room). Heights in logical pixels:
// `needed` the window with the fewest rows, `wanted` with the most, `step`
// what a row adds; `wanted` 0: just `needed`, `step` 0: any height between
// them. Pure logic, unit-tested (tst_placement).

// How tall the window is with `room` on a side: the most whole rows that fit
// there, within those bounds. Where even the fewest do not fit, the room
// itself: it shows those and is pushed onto the work area, over the box.
constexpr int shown(int room, int needed, int wanted, int step) noexcept
{
    if (room < needed)
        return room;
    const int most = std::min(room, std::max(wanted, needed));
    return step > 0 ? needed + (most - needed) / step * step : most;
}

// Over the box rather than under it: under it while all of the window fits
// there, else on the side where more of it shows (whole rows); under it when
// as much does. With neither `wanted` nor `step` that is over it where the
// fewest rows would not fit under it and there is more room over it.
constexpr bool above(int roomUnder, int roomOver, int needed, int wanted, int step) noexcept
{
    return shown(roomOver, needed, wanted, step) > shown(roomUnder, needed, wanted, step);
}

} // namespace ws::boxside
