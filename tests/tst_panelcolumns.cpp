#include "PanelColumns.h"
#include "ResultColumns.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace ws;
using namespace Qt::StringLiterals;

class TestPanelColumns : public QObject {
    Q_OBJECT
private slots:
    void persistenceAndRepair()
    {
        QTemporaryDir profile;
        const QString file = profile.filePath(u"WinShun.ini"_s);
        QSettings(file, QSettings::IniFormat).setValue(u"Other/Keep"_s, 42);
        PanelColumns columns;
        columns.load(file);
        QCOMPARE(columns.visibleColumns().size(), 4);
        columns.toggle(u"name"_s);
        QCOMPARE(columns.visibleColumns().size(), 4);
        columns.toggle(u"created"_s);
        columns.move(4, 1);
        columns.resize(u"created"_s, 210);
        PanelColumns loaded;
        loaded.load(file);
        QCOMPARE(loaded.visibleColumns(), columns.visibleColumns());
        QCOMPARE(loaded.visibleColumns()[1].toMap().value(u"id"_s).toString(), u"created"_s);
        QCOMPARE(loaded.visibleColumns()[1].toMap().value(u"width"_s).toInt(), 210);
        QSettings(file, QSettings::IniFormat).setValue(u"Panel/Columns"_s, u"bogus:1,size:-4,size:500,created:99999"_s);
        PanelColumns repaired;
        repaired.load(file);
        const auto visible = repaired.visibleColumns();
        QCOMPARE(visible.size(), 3);
        QCOMPARE(visible[0].toMap().value(u"id"_s).toString(), u"name"_s);
        QCOMPARE(visible[1].toMap().value(u"width"_s).toInt(), 72);
        QCOMPARE(visible[2].toMap().value(u"width"_s).toInt(), 2000);
        repaired.reset();
        QCOMPARE(repaired.visibleColumns().size(), 4);
        QCOMPARE(QSettings(file, QSettings::IniFormat).value(u"Other/Keep"_s).toInt(), 42);
    }

    void metadataSort()
    {
        SearchResult unknown {.name = u"unknown"_s};
        SearchResult older {.name = u"older.txt"_s,
            .path = u"C:\\Work\\older.txt"_s,
            .modified = 1700000000,
            .created = 1600000000,
            .size = 0};
        SearchResult newer {.name = u"newer.pdf"_s,
            .path = u"C:\\Docs\\newer.pdf"_s,
            .modified = 1800000000,
            .created = 1750000000,
            .size = 100};
        for (const auto key : {SortKey::Size, SortKey::Modified, SortKey::Created}) {
            for (const bool down : {false, true}) {
                QVERIFY(resultColumnLess(older, unknown, {key, down}));
                QVERIFY(!resultColumnLess(unknown, older, {key, down}));
                QCOMPARE(resultColumnLess(older, newer, {key, down}), !down);
                QVERIFY(!resultColumnLess(older, older, {key, down}));
            }
        }
        QCOMPARE(resultFolderName(older), u"Work"_s);
        QCOMPARE(resultType(newer), u"PDF"_s);
        QVERIFY(resultColumnLess(newer, older, {SortKey::Folder, false}));
    }
};

QTEST_GUILESS_MAIN(TestPanelColumns)
#include "tst_panelcolumns.moc"
