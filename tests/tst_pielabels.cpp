// The drop pie's folder names (PieLabels): one line if it fits, else two
// broken at a separator or bracket, else the start and "…" with the end;
// paths shortened keeping the drive and the last folder; a parent's name
// for names shown twice. Measured with a real font, so the checks are on
// widths and kept parts rather than exact strings.
#include "PieLabels.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace ws::pielabels;

namespace {

QFont labelFont()
{
    QFont font(u"Microsoft YaHei UI"_s);
    font.setPixelSize(13);
    return font;
}

constexpr double kWidth = 96;

} // namespace

class TestPieLabels : public QObject {
    Q_OBJECT

private slots:
    void fits(); // every line within the width, nothing lost unless elided
    void fits_data();
    void shortStaysOneLine();
    void breaksAtSeparator();
    void breaksBeforeBracket();
    void veryLongKeepsItsEnd();
    void surrogatesNotSplit();
    void middleElision();
    void paths();
    void duplicates();
    void lastNames();
};

void TestPieLabels::fits_data()
{
    QTest::addColumn<QString>("name");
    QTest::newRow("cjk") << u"二〇二六年第三季度财务报告终版"_s;
    QTest::newRow("latin") << u"Quarterly Financial Reports Final"_s;
    QTest::newRow("mixed") << u"项目资料_ProjectAlpha_2026"_s;
    QTest::newRow("no breaks") << u"Supercalifragilisticexpialidocious"_s;
    QTest::newRow("very long") << u"这是一个非常非常非常非常非常非常非常非常长的文件夹名字终版v3"_s;
    QTest::newRow("brackets") << u"客户资料（2026 年归档）"_s;
    QTest::newRow("dots") << u"工作·资料·归档·二〇二六"_s;
}

void TestPieLabels::fits()
{
    QFETCH(QString, name);
    const QFontMetricsF metrics(labelFont());
    const Fitted f = fitTwoLines(name, metrics, kWidth);
    QVERIFY2(metrics.horizontalAdvance(f.line1) <= kWidth, qPrintable(f.line1));
    QVERIFY2(metrics.horizontalAdvance(f.line2) <= kWidth, qPrintable(f.line2));
    QVERIFY(!f.line1.isEmpty());
    if (f.elided) {
        QVERIFY(f.line2.startsWith(u"…"_s));
        QVERIFY(name.startsWith(f.line1));
        QVERIFY(name.endsWith(f.line2.mid(1)));
    } else {
        // Only spaces dropped at the break.
        QCOMPARE(QString(f.line1 + f.line2).remove(u' '), QString(name).remove(u' '));
    }
}

void TestPieLabels::shortStaysOneLine()
{
    const QFontMetricsF metrics(labelFont());
    const Fitted f = fitTwoLines(u"  下载  "_s, metrics, kWidth);
    QCOMPARE(f.line1, u"下载"_s);
    QVERIFY(!f.twoLines());
    QVERIFY(!f.elided);
}

void TestPieLabels::breaksAtSeparator()
{
    const QFontMetricsF metrics(labelFont());
    // The even break would be inside a word; the underscore wins.
    const Fitted f = fitTwoLines(u"季度报告_最终审定版本"_s, metrics, kWidth);
    QCOMPARE(f.line1, u"季度报告_"_s);
    QCOMPARE(f.line2, u"最终审定版本"_s);
    const Fitted spaced = fitTwoLines(u"Project Alpha Archive"_s, metrics, kWidth);
    QVERIFY(spaced.twoLines());
    QVERIFY(!spaced.line1.endsWith(u' ') && !spaced.line2.startsWith(u' '));
}

void TestPieLabels::breaksBeforeBracket()
{
    const QFontMetricsF metrics(labelFont());
    const Fitted f = fitTwoLines(u"客户资料（已归档）"_s, metrics, 80);
    QVERIFY(f.twoLines());
    QVERIFY2(f.line2.startsWith(u'（'), qPrintable(f.line2));
}

void TestPieLabels::veryLongKeepsItsEnd()
{
    const QFontMetricsF metrics(labelFont());
    const QString name = u"非常非常非常非常非常非常非常非常非常非常长的名字终版"_s;
    const Fitted f = fitTwoLines(name, metrics, kWidth);
    QVERIFY(f.elided);
    QVERIFY(f.line2.endsWith(u"终版"_s));
    QVERIFY(f.line1.startsWith(u"非常"_s));
}

void TestPieLabels::surrogatesNotSplit()
{
    const QFontMetricsF metrics(labelFont());
    QString name;
    for (int i = 0; i < 30; ++i)
        name += u"😀"_s;
    const Fitted f = fitTwoLines(name, metrics, kWidth);
    for (const QString& line : {f.line1, f.line2}) {
        QVERIFY(line.isEmpty() || !line.front().isLowSurrogate());
        QVERIFY(line.isEmpty() || !line.back().isHighSurrogate());
    }
}

void TestPieLabels::middleElision()
{
    const QFontMetricsF metrics(labelFont());
    const QString text = u"Quarterly financial reports for the board, final version"_s;
    QCOMPARE(elideMiddle(u"Short"_s, metrics, 300), u"Short"_s);
    const QString shown = elideMiddle(text, metrics, 160);
    QVERIFY(shown.contains(u"…"_s));
    QVERIFY(metrics.horizontalAdvance(shown) <= 160);
    QVERIFY(shown.startsWith(u"Quar"_s));
    QVERIFY(shown.endsWith(u"version"_s));
}

void TestPieLabels::paths()
{
    const QFontMetricsF metrics(labelFont());
    const QString path = uR"(F:\工作\客户\二〇二六\季度报告\终版)"_s;
    QCOMPARE(elidePath(path, metrics, 1000), path);
    const QString shown = elidePath(path, metrics, 140);
    QVERIFY2(metrics.horizontalAdvance(shown) <= 140, qPrintable(shown));
    QVERIFY2(shown.startsWith(uR"(F:\…\)"_s), qPrintable(shown));
    QVERIFY(shown.endsWith(uR"(\终版)"_s));
    const QString unc = elidePath(uR"(\\nas\share\部门\财务\二〇二六\归档)"_s, metrics, 170);
    QVERIFY2(unc.startsWith(uR"(\\nas\share\…\)"_s), qPrintable(unc));
    QVERIFY(unc.endsWith(u"归档"_s));
    // Even the last folder too wide: still within the width.
    const QString tight = elidePath(uR"(D:\a\这是一个非常非常非常长的最后一个文件夹)"_s, metrics, 120);
    QVERIFY2(metrics.horizontalAdvance(tight) <= 120, qPrintable(tight));
}

void TestPieLabels::duplicates()
{
    const QStringList names {u"资料"_s, u"下载"_s, u"资料"_s};
    const QStringList paths {uR"(D:\工作\资料)"_s, uR"(C:\Users\me\Downloads)"_s, uR"(E:\家里\资料)"_s};
    const QStringList hints = parentHints(names, paths);
    QCOMPARE(hints, (QStringList {u"工作"_s, QString(), u"家里"_s}));
    // Parents with the same name: their whole paths.
    const QStringList same = parentHints({u"资料"_s, u"资料"_s}, {uR"(D:\工作\资料)"_s, uR"(E:\工作\资料)"_s});
    QCOMPARE(same, (QStringList {uR"(D:\工作)"_s, uR"(E:\工作)"_s}));
    // At a drive's root.
    const QStringList roots = parentHints({u"资料"_s, u"资料"_s}, {uR"(D:\资料)"_s, uR"(E:\资料)"_s});
    QCOMPARE(roots, (QStringList {uR"(D:\)"_s, uR"(E:\)"_s}));
}

void TestPieLabels::lastNames()
{
    QCOMPARE(lastName(uR"(D:\Work\资料)"_s), u"资料"_s);
    QCOMPARE(lastName(uR"(D:\)"_s), uR"(D:\)"_s);
}

QTEST_MAIN(TestPieLabels)
#include "tst_pielabels.moc"
