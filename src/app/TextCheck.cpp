#include "TextCheck.h"

#include <QRawFont>
#include <QTextCharFormat>
#include <QTextLayout>

#include <algorithm>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::textcheck {
namespace {
constexpr qsizetype kMostMismatches = 8; // per case: enough to read the swap off

QString describeFont(const QFont& font)
{
    const QString size = font.pixelSize() > 0 ? u"%1px"_s.arg(font.pixelSize()) : u"%1pt"_s.arg(font.pointSizeF());
    return u"%1, %2, weight %3"_s.arg(font.family(), size).arg(font.weight());
}
} // namespace

QString sample()
{
    // Checked with fontTools against the GSUB tables of the five fonts named
    // in the header (Regular and Bold where there are two).
    return u"色长城板文件图片资料 Report 2026"_s;
}

QList<QGlyphRun> layOut(const QString& text, const QFont& font, qsizetype from, qsizetype length)
{
    QTextLayout layout(text, font);
    if (length > 0) {
        QTextLayout::FormatRange bold;
        bold.start = static_cast<int>(from);
        bold.length = static_cast<int>(length);
        bold.format.setFontWeight(QFont::Bold);
        layout.setFormats({bold});
    }
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid())
        line.setLineWidth(100000); // one line, as a row's Text has
    layout.endLayout();
    return layout.glyphRuns(-1, -1, QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveGlyphPositions);
}

QJsonArray mismatches(const QString& text, const QList<QGlyphRun>& runs)
{
    // The n-th glyph from the left is the n-th character. Not by
    // QGlyphRun::stringIndexes: in Qt 6.12 the part of a text item that falls
    // back to another font gets the item's first index for all its glyphs
    // (glyphRunWithInfo), and glyphRuns() merges a font's parts out of order.
    struct Placed {
        qreal x;
        quint32 glyph;
        qsizetype run;
    };
    std::vector<Placed> placed;
    std::vector<QRawFont> fonts;
    for (const QGlyphRun& run : runs) {
        fonts.push_back(run.rawFont());
        const QList<quint32> glyphs = run.glyphIndexes();
        const QList<QPointF> at = run.positions();
        for (qsizetype i = 0; i < std::min(glyphs.size(), at.size()); ++i)
            placed.push_back({at[i].x(), glyphs[i], static_cast<qsizetype>(fonts.size()) - 1});
    }
    if (static_cast<qsizetype>(placed.size()) != text.size())
        return {QJsonObject {{u"note"_s, u"%1 glyphs for %2 characters"_s.arg(placed.size()).arg(text.size())}}};
    std::ranges::stable_sort(placed, {}, &Placed::x);

    QJsonArray found;
    for (qsizetype pos = 0; pos < text.size(); ++pos) {
        if (text[pos].isSpace())
            continue;
        const QRawFont& font = fonts[placed[pos].run];
        const quint32 glyph = placed[pos].glyph;
        const quint32 expected = font.glyphIndexesForString(QString(text[pos])).value(0);
        if (expected != 0 && expected == glyph)
            continue;
        found.append(QJsonObject {{u"char"_s, QString(text[pos])},
            {u"code"_s, u"U+%1"_s.arg(static_cast<uint>(text[pos].unicode()), 4, 16, QChar(u'0')).toUpper()},
            {u"glyph"_s, static_cast<qint64>(glyph)}, {u"expected"_s, static_cast<qint64>(expected)},
            {u"font"_s, font.familyName()}, {u"style"_s, font.styleName()}, {u"weight"_s, font.weight()},
            {u"pixel_size"_s, font.pixelSize()}});
    }
    return found;
}

QJsonObject check(const QFont& font)
{
    const QString text = sample();
    struct Case {
        QString name;
        qsizetype from;
        qsizetype length;
    };
    // The bold part starts and ends inside the Chinese, as a match does.
    const Case cases[] {{u"highlight"_s, 2, 4}, {u"bold"_s, 0, text.size()}, {u"plain"_s, 0, 0}};
    bool ok = true;
    QJsonArray results;
    for (const Case& c : cases) {
        const QList<QGlyphRun> runs = layOut(text, font, c.from, c.length);
        QJsonArray wrong = mismatches(text, runs);
        while (wrong.size() > kMostMismatches)
            wrong.removeLast();
        QJsonArray fonts; // what each run is drawn with
        for (const QGlyphRun& run : runs) {
            const QRawFont raw = run.rawFont();
            fonts.append(u"%1 %2 (%3), %4 glyphs"_s.arg(raw.familyName(), raw.styleName()).arg(raw.weight())
                    .arg(run.glyphIndexes().size()));
        }
        ok = ok && wrong.isEmpty();
        results.append(QJsonObject {{u"case"_s, c.name}, {u"ok"_s, wrong.isEmpty()}, {u"runs"_s, fonts},
            {u"mismatches"_s, wrong}});
    }
    return {{u"ok"_s, ok}, {u"font"_s, describeFont(font)}, {u"cases"_s, results}};
}

} // namespace ws::textcheck
