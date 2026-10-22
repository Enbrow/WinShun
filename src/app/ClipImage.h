#pragma once

#include <QByteArray>
#include <QImage>

namespace ws {

// A picture's identity in the history (ClipCapture::hash): the same pixels
// copied again are the same entry, whatever format they came in.
QByteArray imagePixelHash(const QImage& image);

// A picture from a clipboard DIB (CF_DIB, CF_DIBV5); null if it is not one.
QImage imageFromDib(const QByteArray& dib);

} // namespace ws
