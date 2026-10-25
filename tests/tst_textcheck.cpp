// The glyph self-check of the diagnostics (textcheck, GitHub #70): clean in
// the fonts Win顺 draws with, and the swap seen in #70 (SimSun's glyph
// numbers drawn with PuHuiTi, 色 as 柔) reported character by character.
#include "DisplayDiagnostics.h"
#include "TextCheck.h"

#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QRawFont>
#include <QTextLayout>
#include <QtTest>

#include <windows.h>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

QByteArray fontData(const QString& file)
{
    QFile data(QStringLiteral(WINSHUN_FONTS_DIR) + u'/' + file);
    return data.open(QIODevice::ReadOnly) ? data.readAll() : QByteArray();
}

QString failures(const QJsonObject& result)
{
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented));
}

} // namespace

class TestTextCheck : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // As the app loads it (main.cpp: applyUiFont), from data: the source
        // folder's path is not in the ANSI code page.
        for (const QString& file : {u"AlibabaPuHuiTi-3-55-Regular.ttf"_s, u"AlibabaPuHuiTi-3-85-Bold.ttf"_s}) {
            const QStringList families
                = QFontDatabase::applicationFontFamilies(QFontDatabase::addApplicationFontFromData(fontData(file)));
            QVERIFY2(!families.isEmpty(), qPrintable(file));
            m_puhuiti = families.first();
        }
    }

    void clean_data()
    {
        QTest::addColumn<QString>("family");
        QTest::addColumn<int>("pixelSize");
        QTest::newRow("puhuiti 15") << m_puhuiti << 15;
        QTest::newRow("puhuiti 13") << m_puhuiti << 13; // a row's folder line
        QTest::newRow("yahei ui") << u"Microsoft YaHei UI"_s << 15; // without fonts\ next to the exe
        QTest::newRow("simsun") << u"SimSun"_s << 15; // the first of Qt's Chinese fallbacks
    }

    // Every glyph its run's font's own for its character, highlighted, bold and plain.
    void clean()
    {
        QFETCH(QString, family);
        QFETCH(int, pixelSize);
        if (!QFontDatabase::hasFamily(family))
            QSKIP("font not installed");
        QFont font(family);
        font.setPixelSize(pixelSize);
        font.setHintingPreference(QFont::PreferVerticalHinting);
        const QJsonObject result = textcheck::check(font);
        QVERIFY2(result[u"ok"_s].toBool(), qPrintable(failures(result)));
        const QJsonArray cases = result[u"cases"_s].toArray();
        QCOMPARE(cases.size(), 3);
        for (const QJsonValue& c : cases)
            QVERIFY(!c[u"runs"_s].toArray().isEmpty());
    }

    // The highlight laid out in bold: the run of the bold part is the bold face.
    void highlightIsBold()
    {
        QFont font(m_puhuiti);
        font.setPixelSize(15);
        const QString text = textcheck::sample();
        const QList<QGlyphRun> runs = textcheck::layOut(text, font, 2, 4);
        bool bold = false;
        bool regular = false;
        for (const QGlyphRun& run : runs) {
            (run.rawFont().weight() >= QFont::Bold ? bold : regular) = true;
            QCOMPARE(run.positions().size(), run.glyphIndexes().size());
        }
        QVERIFY(bold);
        QVERIFY(regular);
        QVERIFY(textcheck::mismatches(text, runs).isEmpty());
    }

    // GitHub #70: the glyph numbers of SimSun drawn with PuHuiTi.
    void swappedFontReported()
    {
        if (!QFontDatabase::hasFamily(u"SimSun"_s))
            QSKIP("SimSun not installed");
        QFont simsun(u"SimSun"_s);
        simsun.setPixelSize(15);
        const QString text = textcheck::sample();
        QList<QGlyphRun> runs = textcheck::layOut(text, simsun, 0, 0);
        QVERIFY(!runs.isEmpty());
        const QRawFont puhuiti(fontData(u"AlibabaPuHuiTi-3-55-Regular.ttf"_s), 15);
        QVERIFY(puhuiti.isValid());
        for (QGlyphRun& run : runs)
            run.setRawFont(puhuiti);

        const QJsonArray found = textcheck::mismatches(text, runs);
        QStringList chars;
        for (const QJsonValue& m : found) {
            chars.append(m[u"char"_s].toString());
            if (m[u"char"_s].toString() == u"色"_s) {
                QCOMPARE(m[u"expected"_s].toInteger(), 21312); // PuHuiTi's 色; SimSun's number is its 柔
                QVERIFY(m[u"glyph"_s].toInteger() != 21312);
                QCOMPARE(m[u"code"_s].toString(), u"U+8272"_s);
                QCOMPARE(m[u"font"_s].toString(), puhuiti.familyName());
            }
        }
        for (const QString& c : {u"色"_s, u"长"_s, u"城"_s, u"板"_s})
            QVERIFY2(chars.contains(c), qPrintable(chars.join(u' ')));
        QVERIFY(!chars.contains(u" "_s));
    }

    // A Text keeps its layout while Qt empties its font caches (an
    // application font added or removed): its glyphs asked again afterwards
    // are still each character's own. 々 is not in PuHuiTi: a fallback run.
    void keptLayoutAfterFontCachesEmptied_data()
    {
        QTest::addColumn<int>("boldFrom");
        QTest::addColumn<int>("boldLength");
        QTest::newRow("highlight") << 1 << 3;
        QTest::newRow("plain") << 0 << 0;
    }

    void keptLayoutAfterFontCachesEmptied()
    {
        QFETCH(int, boldFrom);
        QFETCH(int, boldLength);
        QFont font(m_puhuiti);
        font.setPixelSize(15);
        font.setHintingPreference(QFont::PreferVerticalHinting);
        const QString text = u"色々长城板 文件 Report"_s;
        QTextLayout layout(text, font);
        if (boldLength > 0) {
            QTextLayout::FormatRange bold;
            bold.start = boldFrom;
            bold.length = boldLength;
            bold.format.setFontWeight(QFont::Bold);
            layout.setFormats({bold});
        }
        layout.beginLayout();
        layout.createLine().setLineWidth(100000);
        layout.endLayout();
        const auto flags = QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveGlyphPositions;
        const QList<QGlyphRun> before = layout.glyphRuns(-1, -1, flags);
        const QJsonArray wrongBefore = textcheck::mismatches(text, before);
        QVERIFY2(wrongBefore.isEmpty(), QJsonDocument(wrongBefore).toJson().constData());
        QStringList families;
        for (const QGlyphRun& run : before)
            families.append(run.rawFont().familyName());
        families.removeDuplicates();
        QVERIFY2(families.size() >= 2, qPrintable(families.join(u", "_s))); // 々 from another font

        const int id = QFontDatabase::addApplicationFontFromData(fontData(u"AlibabaPuHuiTi-3-85-Bold.ttf"_s));
        QVERIFY(id >= 0);
        QVERIFY(QFontDatabase::removeApplicationFont(id));

        const QList<QGlyphRun> after = layout.glyphRuns(-1, -1, flags);
        QStringList afterFamilies;
        for (const QGlyphRun& run : after)
            afterFamilies.append(u"%1 %2 (%3)"_s.arg(run.rawFont().familyName(), run.rawFont().styleName())
                    .arg(run.glyphIndexes().size()));
        const QJsonArray wrong = textcheck::mismatches(text, after);
        QVERIFY2(wrong.isEmpty(),
            qPrintable(afterFamilies.join(u", "_s) + u' ' + QString::fromUtf8(QJsonDocument(wrong).toJson())));
    }

    // What an export's summary.json holds of the display (diagnostics::display,
    // App's export on the GUI thread), with the fonts the rows were seen in.
    void displaySummary()
    {
        QFont ui(m_puhuiti);
        ui.setHintingPreference(QFont::PreferVerticalHinting);
        QGuiApplication::setFont(ui);
        diagnostics::setUiFont({{u"family"_s, m_puhuiti}});
        diagnostics::watchDisplay();
        QQuickWindow window;
        diagnostics::watchWindow(&window, u"launcher"_s);

        QFont name = ui;
        name.setPixelSize(15);
        QFont folder = ui;
        folder.setPixelSize(13);
        diagnostics::watchHighlightFont(name); // checked now: the first
        diagnostics::watchHighlightFont(folder); // within 10 minutes: only remembered
        diagnostics::watchHighlightFont(name);

        // Qt's font caches emptied, Windows' fonts changed (twice within 2 s: noted twice, recorded once).
        const int id = QFontDatabase::addApplicationFontFromData(fontData(u"AlibabaPuHuiTi-3-85-Bold.ttf"_s));
        QVERIFY(QFontDatabase::removeApplicationFont(id));
        diagnostics::noteSystemMessage(WM_FONTCHANGE, 0);
        diagnostics::noteSystemMessage(WM_FONTCHANGE, 0);
        diagnostics::noteSystemMessage(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC);

        const QJsonObject display = diagnostics::display(u"d3d11"_s, true);
        const QString json = QString::fromUtf8(QJsonDocument(display).toJson());
        QCOMPARE(display[u"renderer"_s].toString(), u"d3d11"_s);
        QCOMPARE(display[u"ui_font"_s][u"family"_s].toString(), m_puhuiti);
        QCOMPARE(display[u"font"_s][u"family"_s].toString(), m_puhuiti);
        QVERIFY2(display[u"font_bold"_s][u"weight"_s].toInt() >= QFont::Bold, qPrintable(json));
        QVERIFY(!display[u"screens"_s].toArray().isEmpty());
        QVERIFY(!display[u"adapters"_s].toArray().isEmpty());
        QVERIFY(!display[u"user_locale"_s].toString().isEmpty());
        QVERIFY(display[u"uptime_minutes"_s].toInteger(-1) >= 0);
        QCOMPARE(display[u"font_changes"_s].toInt(), 2);
        QVERIFY2(display[u"font_database_changes"_s].toInt() >= 2, qPrintable(json));
        QCOMPARE(display[u"resumes"_s].toInt(), 1);
        QCOMPARE(display[u"scene_graph_starts"_s][u"launcher"_s].toInt(-1), 0);
        QVERIFY2(display[u"text_check_last"_s][u"ok"_s].toBool(), qPrintable(json));

        // Checked again now: both row fonts and the UI font, all right.
        const QJsonObject check = display[u"text_check"_s].toObject();
        QVERIFY2(check[u"ok"_s].toBool(), qPrintable(json));
        QStringList fonts;
        for (const QJsonValue& f : check[u"fonts"_s].toArray())
            fonts.append(f[u"font"_s].toString());
        QCOMPARE(fonts.size(), 3);
        QVERIFY2(fonts.join(u';').contains(u"15px"_s) && fonts.join(u';').contains(u"13px"_s), qPrintable(fonts.join(u';')));
    }

private:
    QString m_puhuiti;
};

QTEST_MAIN(TestTextCheck)
#include "tst_textcheck.moc"
