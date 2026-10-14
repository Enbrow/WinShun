// The words in pictures (PaddleOcr): the models and onnxruntime.dll the
// build put next to this test, on text drawn as a screenshot shows it.

#include "PaddleOcr.h"

#include <QCoreApplication>
#include <QFont>
#include <QPainter>
#include <QTest>

using namespace Qt::StringLiterals;

class TestOcr : public QObject {
    Q_OBJECT

    static QImage drawn(const QStringList& lines, int px, QColor ink, QColor paper)
    {
        QFont font(u"Microsoft YaHei UI"_s);
        font.setPixelSize(px);
        QImage image(px * 24, px * 2 * static_cast<int>(lines.size()) + px, QImage::Format_RGB32);
        image.fill(paper);
        QPainter painter(&image);
        painter.setFont(font);
        painter.setPen(ink);
        for (qsizetype i = 0; i < lines.size(); ++i)
            painter.drawText(px, static_cast<int>(px * 2 * (i + 1)), lines[i]);
        return image;
    }

    static QString read(const QImage& image)
    {
        QString error;
        const std::unique_ptr<ws::PaddleOcr> ocr = ws::PaddleOcr::load(QCoreApplication::applicationDirPath(), 2, &error);
        if (!ocr) {
            qWarning().noquote() << error;
            return {};
        }
        QStringList lines;
        if (!ocr->read(image, &lines, &error))
            qWarning().noquote() << error;
        return lines.join(u'\n');
    }

private slots:
    void installed() { QVERIFY(ws::PaddleOcr::installed(QCoreApplication::applicationDirPath())); }

    // Small UI text, Chinese with English and decimals, one line each.
    void screenshotText()
    {
        const QString words = read(drawn({u"机器人暂停中"_s, u"起压 12.35 终止 18.402"_s, u"Press Ctrl twice"_s}, 14, Qt::black, Qt::white));
        const QStringList lines = words.split(u'\n');
        QCOMPARE(lines.size(), 3);
        QCOMPARE(lines[0], u"机器人暂停中"_s);
        QVERIFY2(lines[1].contains(u"12.35"_s) && lines[1].contains(u"18.402"_s), qPrintable(lines[1]));
        QVERIFY2(lines[2].contains(u"Ctrl"_s), qPrintable(lines[2]));
    }

    // Light text on dark, as in a dark theme.
    void darkTheme()
    {
        QCOMPARE(read(drawn({u"剪贴板历史"_s}, 16, QColor(230, 230, 230), QColor(32, 32, 32))), u"剪贴板历史"_s);
    }

    // A long screenshot, looked at in strips: every line once.
    void tallPicture()
    {
        QStringList lines;
        for (int i = 0; i < 120; ++i)
            lines.append(u"第%1行 文字识别"_s.arg(i));
        const QString words = read(drawn(lines, 14, Qt::black, Qt::white));
        for (int i : {0, 59, 60, 61, 119})
            QVERIFY2(words.count(u"第%1行"_s.arg(i)) == 1, qPrintable(u"line %1"_s.arg(i)));
    }

    void nothing() { QCOMPARE(read(drawn({}, 14, Qt::black, Qt::white)), QString()); }
};

QTEST_MAIN(TestOcr)
#include "tst_ocr.moc"
