#include "SiteIconImage.h"

#include <QBuffer>
#include <QImageReader>
#include <QBrush>
#include <QPainter>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ws::siteicon {

namespace {

constexpr qsizetype kMaxBytes = 1 << 20; // an icon; a site sending more sends no icon
constexpr int kMaxSide = 1024;

std::uint32_t u16(QByteArrayView b, qsizetype at)
{
    return static_cast<std::uint8_t>(b[at]) | static_cast<std::uint8_t>(b[at + 1]) << 8;
}

std::uint32_t u32(QByteArrayView b, qsizetype at)
{
    return u16(b, at) | u16(b, at + 2) << 16;
}

bool isPng(QByteArrayView b)
{
    return b.size() >= 8 && b.first(8) == QByteArrayView("\x89PNG\r\n\x1a\n", 8);
}

QImage readPng(QByteArrayView bytes)
{
    QByteArray data = bytes.toByteArray();
    QBuffer buffer(&data);
    QImageReader reader(&buffer, "png");
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > kMaxSide || size.height() > kMaxSide)
        return {};
    QImage image = reader.read();
    return image.isNull() ? QImage() : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// A Windows bitmap as an ICO holds it: a BITMAPINFOHEADER of twice the
// height, the colours (bottom row first), then a 1-bit mask of what is
// transparent. Uncompressed, 1, 4, 8, 24 or 32 bits a pixel.
QImage readDib(QByteArrayView b)
{
    if (b.size() < 40)
        return {};
    const std::uint32_t headerSize = u32(b, 0);
    const auto width = static_cast<std::int32_t>(u32(b, 4));
    const auto doubled = static_cast<std::int32_t>(u32(b, 8));
    const std::uint32_t bits = u16(b, 14);
    const std::uint32_t compression = u32(b, 16);
    const std::uint32_t colorsUsed = u32(b, 32);
    const int height = doubled / 2;
    if (headerSize < 40 || headerSize > 1024 || width <= 0 || width > 256 || height <= 0 || height > 256
        || compression != 0 || (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32))
        return {};
    const qsizetype paletteSize = bits <= 8 ? (colorsUsed != 0 ? colorsUsed : 1u << bits) : 0;
    if (paletteSize > 256)
        return {};
    const qsizetype paletteAt = headerSize;
    const qsizetype pixelsAt = paletteAt + paletteSize * 4;
    const qsizetype stride = (static_cast<qsizetype>(width) * bits + 31) / 32 * 4;
    const qsizetype maskAt = pixelsAt + stride * height;
    const qsizetype maskStride = (static_cast<qsizetype>(width) + 31) / 32 * 4;
    const bool hasMask = maskAt + maskStride * height <= b.size();
    if (maskAt > b.size())
        return {};

    QImage image(width, height, QImage::Format_ARGB32);
    bool anyAlpha = false;
    for (int y = 0; y < height; ++y) {
        const qsizetype row = pixelsAt + stride * (height - 1 - y);
        auto* out = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            QRgb pixel = 0;
            if (bits == 32) {
                const qsizetype at = row + x * 4;
                pixel = qRgba(static_cast<std::uint8_t>(b[at + 2]), static_cast<std::uint8_t>(b[at + 1]),
                    static_cast<std::uint8_t>(b[at]), static_cast<std::uint8_t>(b[at + 3]));
                anyAlpha = anyAlpha || qAlpha(pixel) != 0;
            } else if (bits == 24) {
                const qsizetype at = row + x * 3;
                pixel = qRgb(static_cast<std::uint8_t>(b[at + 2]), static_cast<std::uint8_t>(b[at + 1]),
                    static_cast<std::uint8_t>(b[at]));
            } else {
                const qsizetype bit = static_cast<qsizetype>(x) * bits;
                const auto byte = static_cast<std::uint8_t>(b[row + bit / 8]);
                const std::uint32_t index = (byte >> (8 - bits - bit % 8)) & ((1u << bits) - 1);
                if (index >= paletteSize)
                    return {};
                const qsizetype at = paletteAt + index * 4;
                pixel = qRgb(static_cast<std::uint8_t>(b[at + 2]), static_cast<std::uint8_t>(b[at + 1]),
                    static_cast<std::uint8_t>(b[at]));
            }
            out[x] = pixel;
        }
    }
    // The mask, where the colours have no alpha of their own (32 bits all 0).
    if (bits != 32 || !anyAlpha) {
        for (int y = 0; y < height; ++y) {
            auto* out = reinterpret_cast<QRgb*>(image.scanLine(y));
            const qsizetype row = maskAt + maskStride * (height - 1 - y);
            for (int x = 0; x < width; ++x) {
                const bool transparent = hasMask && (static_cast<std::uint8_t>(b[row + x / 8]) >> (7 - x % 8)) & 1;
                out[x] = transparent ? 0 : (out[x] | 0xFF000000u);
            }
        }
    }
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// An ICO's images, the largest (then the most colours) first; the first
// that reads.
QImage readIco(QByteArrayView b)
{
    if (b.size() < 6 || u16(b, 0) != 0 || (u16(b, 2) != 1 && u16(b, 2) != 2))
        return {};
    const std::uint32_t count = u16(b, 4);
    if (count == 0 || count > 64 || 6 + 16 * static_cast<qsizetype>(count) > b.size())
        return {};
    struct Entry {
        int side;
        std::uint32_t bits;
        qsizetype at;
        qsizetype size;
    };
    std::vector<Entry> entries;
    for (std::uint32_t i = 0; i < count; ++i) {
        const qsizetype e = 6 + 16 * static_cast<qsizetype>(i);
        const int side = static_cast<std::uint8_t>(b[e]) == 0 ? 256 : static_cast<std::uint8_t>(b[e]);
        const qsizetype size = u32(b, e + 8);
        const qsizetype at = u32(b, e + 12);
        if (at < 0 || size <= 0 || at > b.size() || size > b.size() - at)
            continue;
        entries.push_back({side, u16(b, e + 6), at, size});
    }
    std::stable_sort(entries.begin(), entries.end(),
        [](const Entry& x, const Entry& y) { return x.side != y.side ? x.side > y.side : x.bits > y.bits; });
    for (const Entry& e : entries) {
        const QByteArrayView data = b.sliced(e.at, e.size);
        const QImage image = isPng(data) ? readPng(data) : readDib(data);
        if (!image.isNull())
            return image;
    }
    return {};
}

} // namespace

QImage decode(QByteArrayView bytes)
{
    if (bytes.isEmpty() || bytes.size() > kMaxBytes)
        return {};
    return isPng(bytes) ? readPng(bytes) : readIco(bytes);
}

QImage normalized(const QImage& image, int maxSize)
{
    if (image.isNull())
        return {};
    QImage scaled = image;
    if (std::max(image.width(), image.height()) > maxSize)
        scaled = image.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled = scaled.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (scaled.width() == scaled.height())
        return scaled;
    const int side = std::max(scaled.width(), scaled.height());
    QImage square(side, side, QImage::Format_ARGB32_Premultiplied);
    square.fill(Qt::transparent);
    QPainter painter(&square);
    painter.drawImage((side - scaled.width()) / 2, (side - scaled.height()) / 2, scaled);
    painter.end();
    return square;
}

bool isTile(const QImage& image)
{
    if (image.isNull() || image.width() < 8 || image.width() != image.height())
        return false;
    const int last = image.width() - 1;
    for (const QPoint p : {QPoint(0, 0), QPoint(last, 0), QPoint(0, last), QPoint(last, last), QPoint(last / 2, 0),
             QPoint(0, last / 2)}) {
        if (qAlpha(image.pixel(p)) < 250)
            return false;
    }
    return true;
}

bool fadesInto(const QImage& image, bool dark)
{
    if (image.isNull() || isTile(image))
        return false;
    const QImage straight = image.convertToFormat(QImage::Format_ARGB32);
    const QRgb row = dark ? qRgb(0x20, 0x20, 0x20) : qRgb(0xF9, 0xF9, 0xF9); // Theme.background
    qint64 shown = 0;
    qint64 faded = 0;
    for (int y = 0; y < straight.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(straight.constScanLine(y));
        for (int x = 0; x < straight.width(); ++x) {
            const int alpha = qAlpha(line[x]);
            if (alpha < 96) // edges' smoothing, shadows
                continue;
            const int dr = qRed(line[x]) - qRed(row);
            const int dg = qGreen(line[x]) - qGreen(row);
            const int db = qBlue(line[x]) - qBlue(row);
            shown += alpha;
            if (dr * dr + dg * dg + db * db < 64 * 64) // a saturated colour stays apart: 百度's blue on dark
                faded += alpha;
        }
    }
    // Nearly all of it: 抖音's black square (68 %) and 百度's white one
    // (63 %) show by what is on them; GitHub's cat (100 %) by nothing.
    return shown > 0 && faded * 10 > shown * 9;
}

QImage drawn(const QImage& image, int px, bool dark)
{
    QImage out(px, px, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    if (image.isNull())
        return out;
    const int inset = std::max(1, px / 16);
    const int side = px - 2 * inset;
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    if (fadesInto(image, dark)) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(dark ? QColor(0xF3, 0xF3, 0xF3) : QColor(0x2B, 0x2B, 0x2B));
        painter.drawRoundedRect(QRectF(inset, inset, side, side), side / 5.0, side / 5.0);
        const int inner = side * 3 / 4;
        const QImage scaled = image.scaled(inner, inner, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        painter.drawImage(QPointF((px - scaled.width()) / 2, (px - scaled.height()) / 2), scaled);
        painter.end();
        return out;
    }
    const QImage scaled = image.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QRectF box(inset + (side - scaled.width()) / 2, inset + (side - scaled.height()) / 2, scaled.width(),
        scaled.height());
    if (isTile(image)) { // filled with it rather than clipped: a clip's edge is not smoothed
        painter.setPen(Qt::NoPen);
        painter.setBrushOrigin(box.topLeft());
        painter.setBrush(QBrush(scaled));
        painter.drawRoundedRect(box, side / 5.0, side / 5.0);
    } else {
        painter.drawImage(box.topLeft(), scaled);
    }
    painter.end();
    return out;
}

} // namespace ws::siteicon
