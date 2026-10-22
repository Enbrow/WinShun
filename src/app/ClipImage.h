#pragma once

#include <QByteArray>
#include <QImage>

namespace ws {

// A picture's identity in the history (ClipCapture::hash): the same pixels
// copied again are the same entry, whatever format they came in.
QByteArray imagePixelHash(const QImage& image);

// A picture from a clipboard DIB (CF_DIB, CF_DIBV5); null if it is not one.
QImage imageFromDib(const QByteArray& dib);

// A picture of a metafile (CF_ENHMETAFILE's bits, as Word gives an
// equation): drawn at twice its size (at 96 dpi) on white, without the blank
// around it; null if it is not one or draws nothing.
QImage imageFromEmf(const QByteArray& bits);

} // namespace ws
