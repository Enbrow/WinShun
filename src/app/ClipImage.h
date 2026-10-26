#pragma once

#include "ClipStore.h"

#include <QByteArray>
#include <QImage>

#include <span>

namespace ws {

inline constexpr qint64 kClipMaxImageBytes = 256ll * 1024 * 1024;
inline constexpr qint64 kClipMaxPixels = 100'000'000;

struct ClipImageOffer {
    QStringView name;
    ClipImageFormat format;
};

// Richest representation first: preserve animation when a program also
// supplies a flattened PNG or DIB. Shared by capture, import and copying.
std::span<const ClipImageOffer> clipImageOffers();

struct DecodedClipImage {
    QImage firstFrame;
    QByteArray data;
    QByteArray hash;
    ClipImageFormat format = ClipImageFormat::Png;
    bool animated = false;
    bool tooBig = false;
};

// Validates dimensions before decoding. Stills deduplicate by pixels;
// animations by their whole encoded content, never just their first frame.
DecodedClipImage decodeClipImage(QByteArray data);
DecodedClipImage encodeClipImage(const QImage& image); // bitmap fallback as PNG

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
