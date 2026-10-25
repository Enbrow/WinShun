// The drop pie's folder names (PieLabels): shortened in the middle, the end
// kept; paths shortened keeping the drive and the last folder; a parent's
// name for names shown twice. Measured with a real font, so the checks are
// on widths and kept parts rather than exact strings.
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
    void veryLongKeepsItsEnd();
    void surrogatesNotSplit();
    void middleElision();
    void paths();
    void duplicates();
    void lastNames();
};

void TestPieLabels::veryLongKeepsItsEnd()
{
    const QFontMetricsF metrics(labelFont());
    const QString name = u"非常非常非常非常非常非常非常非常非常非常长的名字终版"_s;
    const QString shown = elideMiddle(name, metrics, kWidth);
    QVERIFY2(metrics.horizontalAdvance(shown) <= kWidth, qPrintable(shown));
    QVERIFY(shown.contains(u"…"_s));
    QVERIFY(shown.endsWith(u"终版"_s));
    QVERIFY(shown.startsWith(u"非常"_s));
    QCOMPARE(elideMiddle(u"下载"_s, metrics, kWidth), u"下载"_s);
}

void TestPieLabels::surrogatesNotSplit()
{
    const QFontMetricsF metrics(labelFont());
    QString name;
    for (int i = 0; i < 30; ++i)
        name += u"😀"_s;
    const QString shown = elideMiddle(name, metrics, kWidth);
    for (qsizetype i = 0; i < shown.size(); ++i) {
        if (shown.at(i).isHighSurrogate())
            QVERIFY(i + 1 < shown.size() && shown.at(i + 1).isLowSurrogate());
        if (shown.at(i).isLowSurrogate())
            QVERIFY(i > 0 && shown.at(i - 1).isHighSurrogate());
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
