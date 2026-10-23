#include "ClipFiles.h"
#include "platform/DragOut.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <shellapi.h>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

qint64 addImage(ClipStore& store, QRgb color)
{
    QImage image(12, 10, QImage::Format_ARGB32);
    image.fill(color);
    ClipCapture capture;
    capture.kind = ClipKind::Image;
    capture.width = image.width();
    capture.height = image.height();
    QBuffer buffer(&capture.png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    capture.hash = QByteArray::number(color);
    return store.add(capture, 1'790'000'000'000); // both copied in the same second
}

} // namespace

class ClipFilesTest : public QObject {
    Q_OBJECT

private slots:
    void allSelectedImagesReachTheDropTarget()
    {
        QTemporaryDir dir;
        ClipStore store(dir.filePath(u"history"_s));
        QVERIFY(store.open(1'790'000'000'000));
        const auto red = addImage(store, qRgb(255, 0, 0));
        const auto blue = addImage(store, qRgb(0, 0, 255));
        QVERIFY(red && blue && red != blue);
        const std::vector<const Clip*> selected {store.clip(blue), store.clip(red)};
        const auto exported = clipfiles::images(store, selected, dir.filePath(u"exports"_s), u"剪贴板图片"_s);
        QCOMPARE(exported.error, clipfiles::Error::None);
        QCOMPARE(exported.paths.size(), 2);
        QVERIFY(QFileInfo(exported.paths[0]).fileName() != QFileInfo(exported.paths[1]).fileName());
        QCOMPARE(QImage(exported.paths[0]).pixel(0, 0), qRgb(0, 0, 255));
        QCOMPARE(QImage(exported.paths[1]).pixel(0, 0), qRgb(255, 0, 0));
        QCOMPARE(clipfiles::images(store, selected, dir.filePath(u"exports"_s), u"剪贴板图片"_s).paths, exported.paths);

        QVERIFY(SUCCEEDED(::OleInitialize(nullptr)));
        {
            const auto data = dragout::dataObject(exported.paths);
            QVERIFY(data);
            FORMATETC format {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium {};
            QVERIFY(SUCCEEDED(data->GetData(&format, &medium)));
            const auto drop = static_cast<HDROP>(medium.hGlobal);
            QCOMPARE(::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0), 2u);
            for (UINT i = 0; i < 2; ++i) {
                wchar_t path[32768] {};
                ::DragQueryFileW(drop, i, path, 32768);
                QCOMPARE(QString::fromWCharArray(path), exported.paths[i]);
            }
            ::ReleaseStgMedium(&medium);
        }
        ::OleUninitialize();
        // Removing an export never changes the history's original.
        QVERIFY(QFile::remove(exported.paths[0]));
        QVERIFY(QFile::exists(store.payload(blue).imagePath));
        QCOMPARE(QImage(store.payload(blue).imagePath).pixel(0, 0), qRgb(0, 0, 255));
    }

    void failedBatchDoesNotOfferPartialSelection()
    {
        QTemporaryDir dir;
        ClipStore store(dir.filePath(u"history"_s));
        QVERIFY(store.open(1'790'000'000'000));
        const auto first = addImage(store, qRgb(255, 0, 0));
        const auto missing = addImage(store, qRgb(0, 0, 255));
        QVERIFY(QFile::remove(store.payload(missing).imagePath));
        const auto result = clipfiles::images(
            store, {store.clip(first), store.clip(missing)}, dir.filePath(u"exports"_s), u"Image"_s);
        QCOMPARE(result.error, clipfiles::Error::WriteFailed);
        QVERIFY(result.paths.isEmpty());
        Clip text;
        text.kind = ClipKind::Text;
        QCOMPARE(clipfiles::images(store, {store.clip(first), &text}, dir.filePath(u"mixed"_s), u"Image"_s).error,
            clipfiles::Error::NotImages);
        QVERIFY(!QFile::exists(dir.filePath(u"mixed"_s)));
    }

    void uploadFileNames()
    {
        QCOMPARE(clipfiles::fileNames({u"C:/报告 甲.txt"_s}), u"\"C:\\报告 甲.txt\""_s);
        QCOMPARE(clipfiles::fileNames({u"C:/甲/a.txt"_s, u"\\\\nas\\共享\\b 乙.txt"_s}),
            u"\"C:\\甲\\a.txt\" \"\\\\nas\\共享\\b 乙.txt\""_s);
        QVERIFY(clipfiles::fileNames({u"C:/ok.txt"_s, u"bad\"name"_s}).isEmpty());
        QVERIFY(clipfiles::fileNames({u"C:/ok.txt\nD:/other.txt"_s}).isEmpty());
        QVERIFY(clipfiles::fileNames({}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(ClipFilesTest)
#include "tst_clipfiles.moc"
