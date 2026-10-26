#include "Favorites.h"
#include "FileIconProvider.h"
#include "SiteIconImage.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cstdlib>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

QImage load(FileIconProvider& provider, const QString& url, int px = 32)
{
    QSize size;
    return provider.requestImage(url.sliced(QStringView(u"image://fileicon/").size()), &size, QSize(px, px));
}

QString embedded(const QImage& image)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return QString::fromLatin1("data:image/png;base64," + png.toBase64());
}

} // namespace

class TestCommandIcons : public QObject {
    Q_OBJECT

private slots:
    void resolvesProgramsWithoutRunningThem()
    {
        const QString cmd = QStandardPaths::findExecutable(u"cmd.exe"_s);
        QVERIFY(!cmd.isEmpty());
        const QImage expected = FileIconProvider::fileIcon(cmd, 32);
        QVERIFY(!expected.isNull());
        FileIconProvider provider;
        for (const QString& command : {u'"' + cmd + u"\" /c echo test"_s,
                 u"\"%SystemRoot%\\System32\\cmd.exe\" /c echo test"_s, u"cmd /c echo test"_s}) {
            QCOMPARE(load(provider, FileIconProvider::commandIconUrl(command, {}, {}, false)), expected);
        }

        QTemporaryDir temporary(QDir::tempPath() + u"/命令图标 with spaces-XXXXXX"_s);
        QVERIFY(temporary.isValid());
        const QString executable = temporary.filePath(u"图标程序.exe"_s);
        QVERIFY(QFile::copy(cmd, executable));
        QCOMPARE(load(provider, FileIconProvider::commandIconUrl(u"图标程序.exe --argument"_s,
                     temporary.path(), {}, false)), FileIconProvider::fileIcon(executable, 32));
        const QString oldPath = qEnvironmentVariable("PATH");
        const auto restore = qScopeGuard([oldPath] { ::_wputenv_s(L"PATH", oldPath.toStdWString().c_str()); });
        QCOMPARE(::_wputenv_s(L"PATH", temporary.path().toStdWString().c_str()), 0);
        QCOMPARE(load(provider, FileIconProvider::commandIconUrl(u"图标程序 --argument"_s, {}, {}, false)),
            FileIconProvider::fileIcon(executable, 32));

        QFile script(temporary.filePath(u"never-run.cmd"_s));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("@echo off\r\necho executed > \"%~dp0executed.txt\"\r\n");
        script.close();
        QVERIFY(!load(provider, FileIconProvider::commandIconUrl(script.fileName(), {}, {}, false)).isNull());
        QVERIFY(!QFile::exists(temporary.filePath(u"executed.txt"_s)));
    }

    void customPictureAndTheme()
    {
        FileIconProvider provider;
        QImage picture(32, 32, QImage::Format_ARGB32_Premultiplied);
        picture.fill(Qt::transparent);
        {
            QPainter painter(&picture);
            painter.fillRect(QRect(6, 6, 20, 20), QColor(10, 10, 10));
        }
        const QString icon = embedded(picture);
        QVERIFY(!favoriteIconPng(icon).isEmpty());
        const QString light = FileIconProvider::commandIconUrl(u"nonexistent-program.exe"_s, {}, icon, false);
        const QString dark = FileIconProvider::commandIconUrl(u"nonexistent-program.exe"_s, {}, icon, true);
        QCOMPARE(load(provider, light), siteicon::drawn(picture, 32, false));
        QCOMPARE(load(provider, dark), siteicon::drawn(picture, 32, true));
        QVERIFY(load(provider, light) != load(provider, dark));
        picture.fill(QColor(210, 60, 80));
        const QString replaced = FileIconProvider::commandIconUrl(u"nonexistent-program.exe"_s, {}, embedded(picture), false);
        QVERIFY(replaced != light);
        QVERIFY(load(provider, replaced) != load(provider, light));
    }

    void missingAndOfflineProgramsKeepAnIcon()
    {
        FileIconProvider provider;
        const QImage fallback = load(provider, FileIconProvider::commandIconUrl({}, {}, {}, false));
        QVERIFY(!fallback.isNull());
        QCOMPARE(load(provider, FileIconProvider::commandIconUrl(u"winshun-nonexistent-92.exe"_s, {}, {}, false)), fallback);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(load(provider, FileIconProvider::commandIconUrl(u"\"\\\\winshun-offline-92\\share\\app.exe\" --arg"_s,
                     {}, {}, false)), fallback);
        QVERIFY2(timer.elapsed() < 2000, "An offline icon must not wait for the server");
        const QString cmd = QStandardPaths::findExecutable(u"cmd.exe"_s);
        QCOMPARE(load(provider, FileIconProvider::commandIconUrl(u'"' + cmd + u'"', {}, u"broken"_s, false)),
            FileIconProvider::fileIcon(cmd, 32));
    }
};

QTEST_MAIN(TestCommandIcons)
#include "tst_commandicons.moc"
