#include "Settings.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;

namespace {

const QString kSizeKeys[] {u"Content/MaxFileSizeMB"_s, u"Content/MaxCodeFileSizeMB"_s,
    u"Content/MaxDataFileSizeMB"_s, u"Content/MaxDocumentFileSizeMB"_s};

} // namespace

class SettingsTest : public QObject {
    Q_OBJECT

private slots:
    // Limits still at the default of an older version take the new default;
    // ones the user set, and a list of extensions set by hand, stay.
    void contentDefaults_data()
    {
        QTest::addColumn<int>("version"); // 0: no DefaultsVersion
        QTest::addColumn<QList<double>>("written");
        QTest::addColumn<QString>("extensions");
        QTest::addColumn<QList<double>>("expected"); // -1: the new default
        QTest::addColumn<bool>("defaultExtensions");

        QTest::newRow("before 3, untouched") << 0 << QList<double> {64, 8, 16, 512} << u"txt"_s
                                             << QList<double> {-1, -1, -1, -1} << true;
        QTest::newRow("before 3, one set") << 2 << QList<double> {64, 3, 16, 512} << u"txt, md"_s
                                           << QList<double> {-1, 3, -1, -1} << false;
        QTest::newRow("3, untouched") << 3 << QList<double> {16, 2, 4, 100} << QString()
                                      << QList<double> {-1, -1, -1, -1} << true;
        QTest::newRow("3, text set to 1 KB") << 3 << QList<double> {1.0 / 1024, 2, 4, 100} << QString()
                                             << QList<double> {1.0 / 1024, -1, -1, -1} << true;
        QTest::newRow("3, txt set by hand") << 3 << QList<double> {16, 8, 4, 512} << u"txt"_s
                                            << QList<double> {-1, 8, -1, 512} << false;
        // The new defaults are what the user now sets, the old ones too.
        QTest::newRow("4, old values set") << 4 << QList<double> {16, 2, 4, 100} << QString()
                                           << QList<double> {16, 2, 4, 100} << true;
    }

    void contentDefaults()
    {
        QFETCH(int, version);
        QFETCH(QList<double>, written);
        QFETCH(QString, extensions);
        QFETCH(QList<double>, expected);
        QFETCH(bool, defaultExtensions);

        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            if (version)
                ini.setValue(u"Content/DefaultsVersion"_s, version);
            for (int k = 0; k < 4; ++k)
                ini.setValue(kSizeKeys[k], written[k]);
            if (!extensions.isEmpty())
                ini.setValue(u"Content/Extensions"_s, extensions.split(u", "_s));
        }
        const ws::Settings d = ws::Settings::defaults();
        ws::Settings settings;
        settings.load();
        for (std::size_t k = 0; k < 4; ++k) {
            const double want = expected[k] < 0 ? d.contentMaxSizeMB[k] : expected[k];
            QCOMPARE(settings.contentMaxSizeMB[k], want);
        }
        if (defaultExtensions)
            QCOMPARE(settings.contentExtensions, d.contentExtensions);
        else
            QCOMPARE(settings.contentExtensions, extensions.split(u", "_s));

        // Migrated once: loading again changes nothing, and the file holds it.
        ws::Settings again;
        again.load();
        QCOMPARE(again.contentMaxSizeMB, settings.contentMaxSizeMB);
        QCOMPARE(again.contentExtensions, settings.contentExtensions);
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        QCOMPARE(ini.value(u"Content/DefaultsVersion"_s).toInt(), 4);
    }

    // The folder menu's groups (GitHub #51): all on by default, kept when
    // saved, and a file with none on reads back as all on.
    void folderMenuGroups()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QVERIFY(fresh.folderMenuFavorites && fresh.folderMenuRecent && fresh.folderMenuWindows);

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Launcher/FolderMenuFavorites"_s, false);
            ini.setValue(u"Launcher/FolderMenuRecent"_s, true);
            ini.setValue(u"Launcher/FolderMenuWindows"_s, false);
        }
        ws::Settings some;
        some.load();
        QVERIFY(!some.folderMenuFavorites && some.folderMenuRecent && !some.folderMenuWindows);

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Launcher/FolderMenuRecent"_s, false);
        }
        ws::Settings none;
        none.load();
        QVERIFY(none.folderMenuFavorites && none.folderMenuRecent && none.folderMenuWindows);
    }

    // GitHub #44: the other sorts and the double-click setting. A click opens
    // by default; every sort name is kept, and a stranger reads as the best match.
    // The capture's mouse buttons (#46): the old behaviour by default, kept
    // across a save, and a name not known (or "back" on a button that cannot
    // step back) falls to the default.
    void captureMouseActions()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QCOMPARE(fresh.captureMouseDoubleLeft, u"copy"_s);
        QCOMPARE(fresh.captureMouseRight, u"back"_s);
        QCOMPARE(fresh.captureMouseMiddle, u"none"_s);
        QCOMPARE(fresh.captureMouseBack, u"none"_s);
        QCOMPARE(fresh.captureMouseForward, u"none"_s);

        ws::Settings s;
        s.load();
        s.captureMouseDoubleLeft = u"save"_s;
        s.captureMouseRight = u"cancel"_s;
        s.captureMouseMiddle = u"pin"_s;
        s.captureMouseBack = u"ocr"_s;
        s.captureMouseForward = u"copy"_s;
        s.save();
        ws::Settings back;
        back.load();
        QCOMPARE(back.captureMouseDoubleLeft, u"save"_s);
        QCOMPARE(back.captureMouseRight, u"cancel"_s);
        QCOMPARE(back.captureMouseMiddle, u"pin"_s);
        QCOMPARE(back.captureMouseBack, u"ocr"_s);
        QCOMPARE(back.captureMouseForward, u"copy"_s);

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Capture/MouseDoubleLeft"_s, u"back"_s);
            ini.setValue(u"Capture/MouseRight"_s, u"Back"_s);
            ini.setValue(u"Capture/MouseMiddle"_s, u"nonsense"_s);
        }
        ws::Settings odd;
        odd.load();
        QCOMPARE(odd.captureMouseDoubleLeft, u"copy"_s);
        QCOMPARE(odd.captureMouseRight, u"back"_s);
        QCOMPARE(odd.captureMouseMiddle, u"none"_s);
    }

    void sortAndDoubleClick()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QCOMPARE(fresh.sort, u"match"_s);
        QVERIFY(!fresh.doubleClickToOpen);

        for (const QStringView name : ws::kSortNames) {
            ws::Settings s;
            s.load();
            s.sort = name.toString();
            s.doubleClickToOpen = true;
            s.save();
            ws::Settings back;
            back.load();
            QCOMPARE(back.sort, name.toString());
            QVERIFY(back.doubleClickToOpen);
        }
        QCOMPARE(ws::sortIndex(u"newest"), 1);
        QCOMPARE(ws::sortIndex(u"smallest"), 7);
        QCOMPARE(ws::sortIndex(u"nonsense"), 0);

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Results/Sort"_s, u"alphabetical"_s);
        }
        ws::Settings odd;
        odd.load();
        QCOMPARE(odd.sort, u"match"_s);
    }

    // GitHub #52: a new install starts with auto and the known folders; an
    // older file, which indexed whole disks, goes on so (and says so in the
    // file). The folders stay as written: an emptied list is not filled again.
    void indexScope()
    {
        QTemporaryDir fresh;
        QVERIFY(fresh.isValid());
        ws::Settings::useProfile(fresh.path());
        ws::Settings installed;
        installed.load();
        QCOMPARE(installed.indexScope, u"auto"_s);
        QCOMPARE(installed.indexFolders, ws::Settings::defaultIndexFolders());
        QCOMPARE(installed.folderVerifyHours, 24);
        QCOMPARE(QSettings(ws::Settings::filePath(), QSettings::IniFormat).value(u"Index/Scope"_s).toString(), u"auto"_s);

        QTemporaryDir old;
        QVERIFY(old.isValid());
        ws::Settings::useProfile(old.path());
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Index/RescanOnStartup"_s, true);
        }
        ws::Settings upgraded;
        upgraded.load();
        QCOMPARE(upgraded.indexScope, u"drives"_s);
        QCOMPARE(QSettings(ws::Settings::filePath(), QSettings::IniFormat).value(u"Index/Scope"_s).toString(), u"drives"_s);
        ws::Settings again;
        again.load();
        QCOMPARE(again.indexScope, u"drives"_s);

        // Kept as written, nested ones and variables too; an empty list stays empty.
        const QStringList written {u"D:\\Work"_s, u"D:\\Work\\Sub"_s, u"%USERPROFILE%\\Desktop"_s, u"E:\\"_s};
        again.indexScope = u"folders"_s;
        again.indexFolders = written;
        again.folderVerifyHours = 4;
        again.save();
        ws::Settings back;
        back.load();
        QCOMPARE(back.indexScope, u"folders"_s);
        QCOMPARE(back.indexFolders, written);
        QCOMPARE(back.folderVerifyHours, 4);
        back.indexFolders.clear();
        back.save();
        ws::Settings emptied;
        emptied.load();
        QVERIFY(emptied.indexFolders.isEmpty());

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Index/Scope"_s, u"Everything"_s);
            ini.setValue(u"Index/FolderVerifyHours"_s, 1000);
        }
        ws::Settings odd;
        odd.load();
        QCOMPARE(odd.indexScope, u"auto"_s);
        QCOMPARE(odd.folderVerifyHours, 24 * 7);
    }

    // As IndexService takes them: full, the letter upper case, "D:" for a
    // drive; network places and what is no full path left out.
    void indexFolderPaths()
    {
        ws::Settings s;
        s.indexFolders = {u"d:\\Work\\"_s, u"E:\\"_s, u"e:"_s, u"\\\\nas\\docs"_s, u"Documents"_s, u"F:Work"_s, u"  "_s};
        const std::vector<std::wstring> expected {L"D:\\Work", L"E:", L"E:"};
        QCOMPARE(s.indexFolderPaths(), expected);
        QVERIFY(ws::Settings::isNetworkPath(u"\\\\nas\\docs"_s));
        QVERIFY(!ws::Settings::isNetworkPath(u"C:\\Windows"_s));

        // None inside another, none on a network, every one there.
        const QStringList defaults = ws::Settings::defaultIndexFolders();
        for (const QString& f : defaults) {
            QVERIFY2(QFileInfo(f).isDir(), qPrintable(f));
            QVERIFY(!ws::Settings::isNetworkPath(f));
            for (const QString& o : defaults)
                QVERIFY2(f == o || !f.startsWith(o + u'\\', Qt::CaseInsensitive), qPrintable(f + u" in "_s + o));
        }
    }
};

QTEST_GUILESS_MAIN(SettingsTest)
#include "tst_settings.moc"
