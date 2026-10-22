#include "ClipImage.h"

#include <QCryptographicHash>

#include <windows.h>

#include <cstdlib>
#include <cstring>

namespace ws {

QByteArray imagePixelHash(const QImage& image)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    QCryptographicHash hash(QCryptographicHash::Sha1);
    const qint32 size[] {argb.width(), argb.height()};
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(size), sizeof size));
    for (int y = 0; y < argb.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(argb.constScanLine(y)), argb.width() * 4));
    return hash.result();
}

// A DIB (CF_DIB, CF_DIBV5) is a BMP file without its 14-byte file header:
// give it one and Qt reads it, top-down, bit fields, alpha and all.
QImage imageFromDib(const QByteArray& dib)
{
    if (dib.size() < static_cast<qsizetype>(sizeof(BITMAPINFOHEADER)))
        return {};
    BITMAPINFOHEADER info;
    std::memcpy(&info, dib.constData(), sizeof info);
    if (info.biSize < sizeof(BITMAPINFOHEADER) || info.biWidth <= 0 || info.biHeight == 0)
        return {};
    qint64 colors = info.biClrUsed;
    if (colors == 0 && info.biBitCount <= 8)
        colors = 1ll << info.biBitCount;
    qint64 offset = info.biSize + colors * 4;
    // Masks after a plain header with BI_BITFIELDS; some programs leave them
    // after a V5 header too, where they are already inside it.
    const qint64 stride = ((static_cast<qint64>(info.biWidth) * info.biBitCount + 31) / 32) * 4;
    const qint64 pixels = stride * std::abs(static_cast<qint64>(info.biHeight));
    if (info.biCompression == BI_BITFIELDS && (info.biSize == sizeof(BITMAPINFOHEADER) || dib.size() - offset == pixels + 12))
        offset += 12;
    BITMAPFILEHEADER file {};
    file.bfType = 0x4D42; // "BM"
    file.bfSize = static_cast<DWORD>(sizeof file + dib.size());
    file.bfOffBits = static_cast<DWORD>(sizeof file + offset);
    QByteArray bmp(reinterpret_cast<const char*>(&file), sizeof file);
    bmp += dib;
    return QImage::fromData(bmp, "BMP");
}

} // namespace ws
