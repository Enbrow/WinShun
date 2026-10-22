#include "ClipImage.h"

#include <QCryptographicHash>

#include <windows.h>

#include <algorithm>
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

QImage imageFromEmf(const QByteArray& bits)
{
    constexpr double kScale = 2 * 96 / 2540.0; // from hundredths of a millimetre (rclFrame)
    constexpr double kMaxSide = 4096; // pixels (64 MB drawn at most): a bigger frame is drawn smaller
    constexpr int kMargin = 8; // pixels of white kept around what it draws
    if (bits.size() < static_cast<qsizetype>(sizeof(ENHMETAHEADER)))
        return {};
    const HENHMETAFILE emf
        = ::SetEnhMetaFileBits(static_cast<UINT>(bits.size()), reinterpret_cast<const BYTE*>(bits.constData()));
    if (!emf)
        return {};
    ENHMETAHEADER header {};
    ::GetEnhMetaFileHeader(emf, sizeof header, &header);
    const double frameWidth = header.rclFrame.right - header.rclFrame.left;
    const double frameHeight = header.rclFrame.bottom - header.rclFrame.top;
    QImage image;
    if (frameWidth > 0 && frameHeight > 0) {
        const double scale = std::min(kScale, kMaxSide / std::max(frameWidth, frameHeight));
        const int width = std::max(1, static_cast<int>(frameWidth * scale + 0.5));
        const int height = std::max(1, static_cast<int>(frameHeight * scale + 0.5));
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // top-down, as QImage's rows
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        const HDC dc = ::CreateCompatibleDC(nullptr);
        const HBITMAP bitmap = dc ? ::CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
        if (bitmap && pixels) {
            const HGDIOBJ old = ::SelectObject(dc, bitmap);
            const RECT area {0, 0, width, height};
            // On white: an equation is black, and the dark theme shows
            // pictures on dark grey.
            ::FillRect(dc, &area, static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
            ::PlayEnhMetaFile(dc, emf, &area);
            ::GdiFlush();
            image = QImage(static_cast<const uchar*>(pixels), width, height, width * 4, QImage::Format_RGB32).copy();
            ::SelectObject(dc, old);
        }
        if (bitmap)
            ::DeleteObject(bitmap);
        if (dc)
            ::DeleteDC(dc);
    }
    ::DeleteEnhMetaFile(emf);
    if (image.isNull())
        return {};
    // GDI leaves the fourth byte 0; RGB32 wants it 0xFF. And what it draws
    // is all there is to show: a display equation's frame is as wide as the page.
    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            row[x] |= 0xFF000000;
            if ((row[x] & 0xFFFFFF) != 0xFFFFFF) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < 0)
        return {}; // nothing drawn
    return image.copy(QRect(QPoint(left, top), QPoint(right, bottom)).adjusted(-kMargin, -kMargin, kMargin, kMargin)
        & image.rect());
}

} // namespace ws
