#pragma once

#include <QByteArrayView>
#include <QImage>

// The pictures of sites' icons (see SiteIcons): read from what a site sent,
// kept on disk, drawn in a row. Apart from the fetching so it can be tested.
namespace ws::siteicon {

// The picture in an icon a site sent: a PNG, or an ICO (each of its images a
// PNG or a Windows bitmap) for which the largest it holds. Null for anything
// else (JPEG, GIF, WebP, SVG: few sites' only icon) and anything damaged.
// Read by Qt's own PNG reader and the ICO reader here, never by Windows'
// codecs: those load whatever codecs are installed, and Win顺 runs as
// administrator with bytes from the internet.
QImage decode(QByteArrayView bytes);

// As kept on disk: square (a narrow one centred), at most `maxSize` pixels.
QImage normalized(const QImage& image, int maxSize = 128);

// A full-bleed tile (an apple-touch-icon: opaque to its corners), which gets
// rounded corners when drawn; most favicons are a shape on transparency.
bool isTile(const QImage& image);

// Whether nearly all of a shape on transparency is close to the colour of
// the rows it sits on (GitHub's black cat on the dark theme).
bool fadesInto(const QImage& image, bool dark);

// `image` drawn in a square of `px`, a sixteenth in from each side so it
// weighs like an app's icon; a tile with its corners rounded. One that fades
// into the theme's rows sits a little smaller on a rounded plate of the
// other theme's colour, as browsers show such icons in their dark tabs.
QImage drawn(const QImage& image, int px, bool dark);

} // namespace ws::siteicon
