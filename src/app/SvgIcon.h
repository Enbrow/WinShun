#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QImage>

// SVG pictures as icons: one pasted or picked for a web shortcut (iconfont's
// "复制 SVG 代码"), or a site's own SVG favicon. Drawn once by QtSvg into a
// square picture, which is what is kept (siteicons): the SVG text itself is
// never stored nor read again.
namespace ws::svgicon {

// An icon's SVG is a few KB (iconfont's run to 10); more is a drawing.
constexpr qsizetype kMaxBytes = 64 * 1024;

// Starts like SVG (after a BOM and spaces): "<svg", "<?xml", "<!--", "<!DOCTYPE svg".
bool looksLikeSvg(QByteArrayView bytes);

enum class Problem {
    None,
    TooLarge, // over kMaxBytes
    NotSvg, // no <svg> element, or QtSvg cannot read it
    External, // it names another file or address: an href or url() not within itself, @import, an entity
    Empty, // nothing to see (no size, or nothing drawn)
};

// Checked and drawn `side` × `side`, its aspect kept, centred, with its own
// colours (currentColor is black, as a browser shows an SVG with no colour
// given). Null, and `problem` set, for anything not drawn. Safe on any
// thread. Nothing outside the SVG is read: QtSvg would read a local file
// named by <image href>, so every href must be "#…" within it or a data:
// PNG, JPEG or GIF; scripts are left as they are (QtSvg runs none, and
// only the picture is kept).
QImage render(QByteArrayView bytes, int side, Problem* problem = nullptr);

} // namespace ws::svgicon
