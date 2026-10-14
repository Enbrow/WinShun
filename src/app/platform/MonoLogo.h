#pragma once

#include <QColor>
#include <QImage>

#include <windows.h>

namespace ws {

// Win顺's logo in one colour, for the taskbar (a setting): the outlines of
// its four panes and the leaf's vein, as the icons Windows draws there are,
// in the colour of the taskbar's text. Drawn at the size asked for, so it
// stays sharp at any scaling; resources/logo.svg is the coloured original.
namespace monologo {

QImage image(int pixels, const QColor& color); // premultiplied ARGB: B, G, R, A in memory, as Direct2D takes it
HICON icon(int pixels, const QColor& color); // the caller destroys it

bool lightTaskbar(); // Windows' own colour mode (the taskbar's), not the apps'
QColor taskbarInk(); // what the icon is drawn in: the taskbar's text colour, or high contrast's

} // namespace monologo

} // namespace ws
