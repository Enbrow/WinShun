#include "Settings.h"

#include <QCoreApplication>
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
};

QTEST_GUILESS_MAIN(SettingsTest)
#include "tst_settings.moc"
