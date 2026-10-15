// Sites' icons as Win顺 reads them (SiteIconImage): PNG, ICO of PNGs and of
// Windows bitmaps, and whatever else a site may send, damaged or not.

#include "SiteIconImage.h"
#include "SvgIcon.h"

#include <QBuffer>
#include <QPainter>
#include <QRandomGenerator>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

void put16(QByteArray& b, int v)
{
    b.append(char(v & 0xFF)).append(char((v >> 8) & 0xFF));
}

void put32(QByteArray& b, qint64 v)
{
    put16(b, int(v & 0xFFFF));
    put16(b, int((v >> 16) & 0xFFFF));
}

QByteArray png(const QImage& image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

// A Windows bitmap as an ICO holds it. `color(x, y)` with y from the top;
// `palette` for 8 bits and fewer; the mask: `transparent(x, y)`.
QByteArray dib(int w, int h, int bits, const std::function<quint32(int, int)>& color,
    const std::function<bool(int, int)>& transparent, const QList<QRgb>& palette = {})
{
    QByteArray b;
    put32(b, 40);
    put32(b, w);
    put32(b, h * 2);
    put16(b, 1);
    put16(b, bits);
    put32(b, 0); // BI_RGB
    put32(b, 0);
    put32(b, 0);
    put32(b, 0);
    put32(b, palette.size());
    put32(b, 0);
    for (QRgb c : palette) {
        b.append(char(qBlue(c))).append(char(qGreen(c))).append(char(qRed(c))).append('\0');
    }
    const int stride = (w * bits + 31) / 32 * 4;
    for (int y = h - 1; y >= 0; --y) {
        QByteArray row(stride, '\0');
        for (int x = 0; x < w; ++x) {
            const quint32 v = color(x, y);
            if (bits == 32) {
                row[x * 4] = char(qBlue(v));
                row[x * 4 + 1] = char(qGreen(v));
                row[x * 4 + 2] = char(qRed(v));
                row[x * 4 + 3] = char(qAlpha(v));
            } else if (bits == 24) {
                row[x * 3] = char(qBlue(v));
                row[x * 3 + 1] = char(qGreen(v));
                row[x * 3 + 2] = char(qRed(v));
            } else {
                const int bit = x * bits;
                row[bit / 8] = char(row[bit / 8] | (v << (8 - bits - bit % 8)));
            }
        }
        b.append(row);
    }
    const int maskStride = (w + 31) / 32 * 4;
    for (int y = h - 1; y >= 0; --y) {
        QByteArray row(maskStride, '\0');
        for (int x = 0; x < w; ++x) {
            if (transparent(x, y))
                row[x / 8] = char(row[x / 8] | (0x80 >> (x % 8)));
        }
        b.append(row);
    }
    return b;
}

struct Image {
    int side; // as the directory says it
    int bits;
    QByteArray data;
};

QByteArray ico(const QList<Image>& images)
{
    QByteArray b;
    put16(b, 0);
    put16(b, 1);
    put16(b, images.size());
    qint64 at = 6 + 16 * images.size();
    for (const Image& i : images) {
        b.append(char(i.side >= 256 ? 0 : i.side)).append(char(i.side >= 256 ? 0 : i.side)).append('\0').append('\0');
        put16(b, 1);
        put16(b, i.bits);
        put32(b, i.data.size());
        put32(b, at);
        at += i.data.size();
    }
    for (const Image& i : images)
        b.append(i.data);
    return b;
}

QImage solid(int w, int h, QColor c)
{
    QImage image(w, h, QImage::Format_ARGB32);
    image.fill(c);
    return image;
}

} // namespace

class TestSiteIcon : public QObject {
    Q_OBJECT

private slots:
    void pngAlone()
    {
        QImage image = solid(48, 40, QColor(0x12, 0x34, 0x56));
        image.setPixel(0, 0, qRgba(0, 0, 0, 0));
        const QImage read = siteicon::decode(png(image));
        QCOMPARE(read.size(), QSize(48, 40));
        QCOMPARE(read.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(qAlpha(read.pixel(0, 0)), 0);
        QCOMPARE(read.pixel(5, 5), qRgb(0x12, 0x34, 0x56));
    }

    void icoTakesTheLargest()
    {
        // 16 px bitmap and a 64 px PNG (as many favicons have): the PNG.
        const QByteArray small = dib(16, 16, 32, [](int, int) { return qRgba(255, 0, 0, 255); }, [](int, int) { return false; });
        const QByteArray large = png(solid(64, 64, Qt::blue));
        const QImage read = siteicon::decode(ico({{16, 32, small}, {64, 32, large}}));
        QCOMPARE(read.size(), QSize(64, 64));
        QCOMPARE(read.pixel(10, 10), qRgb(0, 0, 255));
        // 256 is written as 0 in the directory.
        QCOMPARE(siteicon::decode(ico({{16, 32, small}, {256, 32, png(solid(256, 256, Qt::green))}})).size(), QSize(256, 256));
        // The largest damaged: the next.
        const QImage fallback = siteicon::decode(ico({{16, 32, small}, {64, 32, QByteArray("\x89PNG\r\n\x1a\nbroken", 14)}}));
        QCOMPARE(fallback.size(), QSize(16, 16));
    }

    void icoBitmaps()
    {
        // 32 bits with alpha, rows bottom first: the top row red, the rest half-transparent green.
        const QByteArray b32 = dib(4, 3, 32, [](int, int y) { return y == 0 ? qRgba(255, 0, 0, 255) : qRgba(0, 255, 0, 128); },
            [](int, int) { return false; });
        QImage read = siteicon::decode(ico({{4, 32, b32}})).convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(read.size(), QSize(4, 3));
        QCOMPARE(read.pixel(1, 0), qRgba(255, 0, 0, 255));
        QCOMPARE(qAlpha(read.pixel(1, 2)), 128);
        QCOMPARE(qGreen(read.pixel(1, 2)), 255);

        // 32 bits with no alpha at all (old icons): the mask says.
        const QByteArray noAlpha = dib(8, 8, 32, [](int, int) { return qRgba(10, 20, 30, 0); }, [](int x, int) { return x < 4; });
        read = siteicon::decode(ico({{8, 32, noAlpha}})).convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(qAlpha(read.pixel(1, 1)), 0);
        QCOMPARE(read.pixel(6, 1), qRgba(10, 20, 30, 255));

        // 24 bits and the mask.
        const QByteArray b24 = dib(5, 5, 24, [](int, int) { return qRgb(1, 2, 3); }, [](int x, int y) { return x == y; });
        read = siteicon::decode(ico({{5, 24, b24}})).convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(qAlpha(read.pixel(2, 2)), 0);
        QCOMPARE(read.pixel(3, 2), qRgb(1, 2, 3));

        // 8, 4 and 1 bits: a palette.
        const QList<QRgb> palette {qRgb(0, 0, 0), qRgb(255, 255, 255), qRgb(200, 100, 50)};
        for (int bits : {8, 4}) {
            const QByteArray b = dib(9, 7, bits, [](int x, int) { return quint32(x % 3); }, [](int, int y) { return y == 6; }, palette);
            read = siteicon::decode(ico({{9, bits, b}})).convertToFormat(QImage::Format_ARGB32);
            QCOMPARE(read.size(), QSize(9, 7));
            QCOMPARE(read.pixel(2, 0), qRgb(200, 100, 50));
            QCOMPARE(read.pixel(4, 3), qRgb(255, 255, 255));
            QCOMPARE(qAlpha(read.pixel(4, 6)), 0);
        }
        const QByteArray b1 = dib(10, 2, 1, [](int x, int) { return quint32(x % 2); }, [](int, int) { return false; },
            {qRgb(0, 0, 0), qRgb(255, 255, 255)});
        read = siteicon::decode(ico({{10, 1, b1}})).convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(read.pixel(0, 0), qRgb(0, 0, 0));
        QCOMPARE(read.pixel(9, 1), qRgb(255, 255, 255));
        // A colour the palette has not: not read.
        const QByteArray outside = dib(4, 4, 8, [](int, int) { return quint32(7); }, [](int, int) { return false; }, palette);
        QVERIFY(siteicon::decode(ico({{4, 8, outside}})).isNull());
    }

    void damaged()
    {
        const QByteArray good = ico({{16, 32, dib(16, 16, 32, [](int, int) { return qRgba(1, 2, 3, 255); }, [](int, int) { return false; })},
            {48, 32, png(solid(48, 48, Qt::red))}});
        QVERIFY(!siteicon::decode(good).isNull());
        // Cut short anywhere, each byte changed in turn, random bytes: never a
        // crash, and nothing larger than what the directory allows.
        for (qsizetype n = 0; n < good.size(); n += 7) {
            const QImage image = siteicon::decode(good.first(n));
            QVERIFY(image.isNull() || image.width() <= 256);
        }
        QRandomGenerator random(20261011);
        for (int round = 0; round < 3000; ++round) {
            QByteArray bytes = good;
            for (int k = 0; k < 4; ++k)
                bytes[random.bounded(bytes.size())] = char(random.bounded(256));
            const QImage image = siteicon::decode(bytes);
            QVERIFY(image.isNull() || (image.width() <= 1024 && image.height() <= 1024));
        }
        for (int round = 0; round < 200; ++round) {
            QByteArray bytes(random.bounded(4096), '\0');
            for (char& c : bytes)
                c = char(random.bounded(256));
            if (round % 2)
                bytes.replace(0, 4, QByteArray("\0\0\1\0", 4)); // as an ICO starts
            siteicon::decode(bytes);
        }
        // What is no icon Win顺 reads.
        QVERIFY(siteicon::decode({}).isNull());
        QVERIFY(siteicon::decode("<html><body>Not found</body></html>").isNull());
        QVERIFY(siteicon::decode(QByteArray("\xFF\xD8\xFF\xE0", 4) + QByteArray(100, 'x')).isNull()); // JPEG
        QVERIFY(siteicon::decode(QByteArray(2 << 20, '\0')).isNull()); // too much
        // A bitmap wider than an ICO allows.
        QVERIFY(siteicon::decode(ico({{0, 32, dib(300, 2, 32, [](int, int) { return 0xFF000000u; }, [](int, int) { return false; })}})).isNull());
    }

    void keptAndDrawn()
    {
        // Kept square, 128 px at most.
        QImage kept = siteicon::normalized(solid(300, 150, Qt::red));
        QCOMPARE(kept.size(), QSize(128, 128));
        QCOMPARE(qAlpha(kept.pixel(64, 5)), 0); // above the narrow picture
        QCOMPARE(kept.pixel(64, 64), qRgb(255, 0, 0));
        QCOMPARE(siteicon::normalized(solid(32, 32, Qt::red)).size(), QSize(32, 32)); // never scaled up

        // A tile (opaque to its corners) gets rounded corners; a shape on
        // transparency is drawn as it is. Both a sixteenth in.
        const QImage tile = solid(152, 152, QColor(0x05, 0x6D, 0xE8));
        QVERIFY(siteicon::isTile(tile));
        QImage glyph = solid(32, 32, Qt::transparent);
        QPainter(&glyph).fillRect(4, 4, 24, 24, Qt::black);
        QVERIFY(!siteicon::isTile(glyph));
        for (int px : {32, 40, 48, 64}) {
            const int inset = std::max(1, px / 16);
            for (bool dark : {false, true}) {
                const QImage drawnTile = siteicon::drawn(tile, px, dark);
                QCOMPARE(drawnTile.size(), QSize(px, px));
                QCOMPARE(qAlpha(drawnTile.pixel(inset - 1, px / 2)), 0); // the margin
                QCOMPARE(qAlpha(drawnTile.pixel(inset, inset)), 0); // a rounded corner
                QCOMPARE(qAlpha(drawnTile.pixel(px / 2, px / 2)), 255);
                QCOMPARE(qAlpha(drawnTile.pixel(px / 2, inset)), 255); // the edge, mid-way
            }
            // Black on the light theme: as it is.
            const QImage onLight = siteicon::drawn(glyph, px, false);
            QCOMPARE(onLight.size(), QSize(px, px));
            QCOMPARE(onLight.pixel(px / 2, px / 2), qRgb(0, 0, 0));
            QCOMPARE(qAlpha(onLight.pixel(px / 2, inset)), 0);
            // On the dark theme: on a light plate, the glyph a little smaller.
            const QImage onDark = siteicon::drawn(glyph, px, true);
            QCOMPARE(onDark.pixel(px / 2, px / 2), qRgb(0, 0, 0));
            QCOMPARE(onDark.pixel(px / 2, inset), qRgb(0xF3, 0xF3, 0xF3)); // the plate's edge, mid-way
            QCOMPARE(qAlpha(onDark.pixel(inset, inset)), 0); // its rounded corner
        }
        QCOMPARE(siteicon::drawn({}, 32, false).size(), QSize(32, 32));
    }

    void plate()
    {
        const auto shape = [](QColor color) {
            QImage image = solid(32, 32, Qt::transparent);
            QPainter(&image).fillRect(4, 4, 24, 24, color);
            return image;
        };
        QVERIFY(siteicon::fadesInto(shape(Qt::black), true)); // GitHub's cat
        QVERIFY(siteicon::fadesInto(shape(QColor(0x24, 0x29, 0x2F)), true));
        QVERIFY(!siteicon::fadesInto(shape(Qt::black), false));
        QVERIFY(siteicon::fadesInto(shape(Qt::white), false));
        QVERIFY(!siteicon::fadesInto(shape(Qt::white), true));
        QVERIFY(!siteicon::fadesInto(shape(QColor(0x29, 0x32, 0xE1)), true)); // 百度's blue
        QVERIFY(!siteicon::fadesInto(shape(QColor(0xFB, 0x72, 0x99)), false)); // bilibili's pink
        // Black on a coloured part that shows: no plate.
        QImage mixed = shape(QColor(0x00, 0xA1, 0xD6));
        QPainter(&mixed).fillRect(10, 10, 6, 6, Qt::black);
        QVERIFY(!siteicon::fadesInto(mixed, true));
        // A black square that shows by what is on it (抖音's: two thirds black).
        QImage square = shape(Qt::black);
        QPainter(&square).fillRect(9, 9, 14, 14, QColor(0x25, 0xF4, 0xEE));
        QVERIFY(!siteicon::fadesInto(square, true));
        // A tile brings its own background; nothing at all shows no plate.
        QVERIFY(!siteicon::fadesInto(solid(64, 64, Qt::black), true));
        QVERIFY(!siteicon::fadesInto(solid(32, 32, Qt::transparent), true));
    }

    // SVG icons (SvgIcon): iconfont's code as copied, drawn with its colours;
    // anything naming what is outside it turned away.
    void svgDrawn()
    {
        // As iconfont's "复制 SVG 代码" gives it (not a raw string: moc reads this file).
        const QByteArray iconfont = "<svg t=\"1697000000000\" class=\"icon\" viewBox=\"0 0 1024 1024\" version=\"1.1\" "
                                    "xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" p-id=\"4012\" "
                                    "width=\"200\" height=\"200\"><path d=\"M512 64L960 512 512 960 64 512z\" fill=\"#D4237A\" "
                                    "p-id=\"4013\"></path><path d=\"M512 384a128 128 0 1 0 0 256 128 128 0 1 0 0-256z\" "
                                    "fill=\"#FFFFFF\"></path></svg>";
        QVERIFY(svgicon::looksLikeSvg(iconfont));
        svgicon::Problem problem = svgicon::Problem::TooLarge;
        const QImage image = svgicon::render(iconfont, 256, &problem);
        QCOMPARE(problem, svgicon::Problem::None);
        QCOMPARE(image.size(), QSize(256, 256));
        QCOMPARE(image.pixel(128, 40), qRgb(0xD4, 0x23, 0x7A)); // its own colour
        QCOMPARE(image.pixel(128, 128), qRgb(0xFF, 0xFF, 0xFF));
        QCOMPARE(qAlpha(image.pixel(10, 10)), 0); // outside the diamond

        // A file as iconfont downloads it: a BOM, the XML declaration, a DOCTYPE.
        const QByteArray file = "\xEF\xBB\xBF<?xml version=\"1.0\" standalone=\"no\"?><!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" "
                                "\"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">" + iconfont;
        QVERIFY(svgicon::looksLikeSvg(file));
        QCOMPARE(svgicon::render(file, 64).pixel(32, 8), qRgb(0xD4, 0x23, 0x7A));

        // Wider than high: centred, its aspect kept.
        const QImage wide = svgicon::render("<svg viewBox=\"0 0 200 100\"><rect width=\"200\" height=\"100\" fill=\"#00f\"/></svg>", 100);
        QCOMPARE(qAlpha(wide.pixel(50, 10)), 0);
        QCOMPARE(wide.pixel(50, 50), qRgb(0, 0, 0xFF));
        // currentColor: black, as a browser shows it with no colour given (the dark theme puts it on a plate).
        QCOMPARE(svgicon::render("<svg viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\" fill=\"currentColor\"/></svg>", 20)
                     .pixel(10, 10), qRgb(0, 0, 0));
        // A gradient and a <use> within it, a picture in a data: address: all its own.
        QCOMPARE(svgicon::render("<svg viewBox=\"0 0 10 10\" xmlns:xlink=\"http://www.w3.org/1999/xlink\"><defs><linearGradient id=\"g\">"
                                 "<stop offset=\"0\" stop-color=\"#f00\"/></linearGradient><rect id=\"r\" width=\"10\" height=\"10\"/></defs>"
                                 "<use xlink:href=\"#r\" fill=\"url(#g)\"/><image href=\"data:image/png;base64,iVBORw0KGgo=\"/></svg>", 20, &problem)
                     .pixel(10, 10), qRgb(0xFF, 0, 0));
        // A script is left as it is: QtSvg runs none, and only the picture is kept.
        QVERIFY(!svgicon::render("<svg viewBox=\"0 0 10 10\"><script>alert(1)</script><rect width=\"10\" height=\"10\"/></svg>", 16).isNull());

        // Drawn large and made smaller (as kept, then as a row draws it) is as
        // sharp as drawn at the row's size: each pixel the average of those it covers.
        for (int px : {20, 30, 45}) {
            const QImage direct = svgicon::render(iconfont, px);
            const QImage scaled = svgicon::render(iconfont, 256).scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            double alphaDiff = 0;
            for (int y = 0; y < px; ++y) {
                for (int x = 0; x < px; ++x)
                    alphaDiff += std::abs(qAlpha(direct.pixel(x, y)) - qAlpha(scaled.pixel(x, y)));
            }
            const double mean = alphaDiff / (px * px);
            qInfo("SVG at %d px: drawn there vs from 256 px, mean alpha difference %.2f of 255", px, mean);
            QVERIFY(mean < 6);
        }
    }

    void svgRefused()
    {
        svgicon::Problem problem = svgicon::Problem::None;
        const auto refused = [&](const QByteArray& svg) {
            problem = svgicon::Problem::None;
            return svgicon::render(svg, 32, &problem).isNull() ? problem : svgicon::Problem::None;
        };
        const QByteArray shape = "<rect width=\"10\" height=\"10\"/>";
        const auto svg = [&](const QByteArray& inside) { return "<svg viewBox=\"0 0 10 10\">" + inside + shape + "</svg>"; };
        // What would be read from outside: QtSvg reads a local file for <image href>.
        QCOMPARE(refused(svg("<image href=\"C:/Windows/Web/Wallpaper/Windows/img0.jpg\" width=\"10\" height=\"10\"/>")),
            svgicon::Problem::External);
        QCOMPARE(refused(svg("<image xlink:href='file:///C:/secret.png'/>")), svgicon::Problem::External);
        QCOMPARE(refused(svg("<use href=\"http://example.com/a.svg#x\"/>")), svgicon::Problem::External);
        QCOMPARE(refused(svg("<image href=\"data:image/svg+xml;base64,PHN2Zz4=\"/>")), svgicon::Problem::External);
        QCOMPARE(refused(svg("<rect width=\"1\" height=\"1\" style=\"fill:url(http://example.com/x)\"/>")), svgicon::Problem::External);
        QCOMPARE(refused(svg("<style>@import url(\"https://example.com/a.css\");</style>")), svgicon::Problem::External);
        QCOMPARE(refused("<?xml version=\"1.0\"?><!DOCTYPE svg [<!ENTITY x SYSTEM \"file:///C:/secret.txt\">]><svg>&x;</svg>"),
            svgicon::Problem::External);
        // Not SVG, too large, nothing to see.
        QCOMPARE(refused("<html><body>Not found</body></html>"), svgicon::Problem::NotSvg);
        QCOMPARE(refused("<?xml version=\"1.0\"?><html/>"), svgicon::Problem::NotSvg);
        QCOMPARE(refused("{\"svg\": 1}"), svgicon::Problem::NotSvg);
        QCOMPARE(refused(svg(QByteArray(svgicon::kMaxBytes, ' '))), svgicon::Problem::TooLarge);
        QCOMPARE(refused("<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>"), svgicon::Problem::Empty);
        QCOMPARE(refused("<svg viewBox=\"0 0 10 10\"></svg>"), svgicon::Problem::Empty);
        QCOMPARE(refused("<svg viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\" fill=\"none\"/></svg>"), svgicon::Problem::Empty);
        QVERIFY(!svgicon::looksLikeSvg("\x89PNG\r\n\x1a\n"));
    }
};

QTEST_MAIN(TestSiteIcon)
#include "tst_siteicon.moc"
