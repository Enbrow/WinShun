#include "BackupFiles.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <miniz.h>

using namespace Qt::StringLiterals;
using namespace ws::backupfiles;

namespace {

void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QStringList names(const QList<File>& files)
{
    QStringList out;
    for (const File& f : files)
        out << f.name;
    return out;
}

// A zip of these entries, written as given.
void writeZip(const QString& path, const QList<std::pair<QByteArray, QByteArray>>& entries)
{
    mz_zip_archive zip {};
    QVERIFY(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, bytes] : entries)
        QVERIFY(mz_zip_writer_add_mem(&zip, name.constData(), bytes.constData(), static_cast<size_t>(bytes.size()), 0));
    void* buffer = nullptr;
    size_t size = 0;
    QVERIFY(mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size));
    mz_zip_writer_end(&zip);
    writeFile(path, QByteArray(static_cast<const char*>(buffer), static_cast<qsizetype>(size)));
    mz_free(buffer);
}

} // namespace

class TestBackup : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_ini;
    QString m_data;

private slots:
    void init()
    {
        QVERIFY(m_tmp.isValid());
        m_ini = m_tmp.filePath(u"roaming/WinShun.ini"_s);
        m_data = m_tmp.filePath(u"local"_s);
        QDir(m_tmp.path()).removeRecursively();
        QDir().mkpath(m_tmp.path());
        writeFile(m_ini, "[Launcher]\nHotkey=Alt+Space\n");
        writeFile(m_data + u"/favorites.json"_s, "{\"favorites\":[]}");
        writeFile(m_data + u"/dialog-pins.txt"_s, "D:\\work\n");
        writeFile(m_data + u"/history.txt"_s, "private");
        writeFile(m_data + u"/index.bin"_s, "index");
        writeFile(m_data + u"/clipboard/clipboard.db"_s, "clips");
        writeFile(m_data + u"/icons/web/github.com.png"_s, "fetched");
        writeFile(m_data + u"/icons/web/custom/mine.png"_s, "chosen");
        writeFile(m_data + u"/plugins/me.notes/plugin.json"_s, "{}");
        writeFile(m_data + u"/plugins/me.notes/run.log"_s, "log");
        writeFile(m_data + u"/plugins/winshun.semantic.data/seg-1.bin"_s, "cache");
        QFile big(m_data + u"/plugins/me.notes/huge.bin"_s);
        QVERIFY(big.open(QIODevice::WriteOnly));
        QVERIFY(big.resize(kMaxFileBytes + 1));
    }

    // Settings, favorites, pins, chosen pictures and added plugins; no
    // indexes, histories, fetched icons, logs, caches or huge files.
    void collectsTheSettings()
    {
        QCOMPARE(names(collect(m_ini, m_data)),
            (QStringList {u"WinShun.ini"_s, u"data/dialog-pins.txt"_s, u"data/favorites.json"_s,
                u"data/icons/web/custom/mine.png"_s, u"data/plugins/me.notes/plugin.json"_s}));
    }

    void sameFilesSameHash()
    {
        const QDateTime t1(QDate(2026, 10, 13), QTime(19, 47, 0));
        const Packed a = pack(collect(m_ini, m_data), u"0.7.3"_s, t1);
        const Packed b = pack(collect(m_ini, m_data), u"0.7.4"_s, t1.addSecs(60));
        QVERIFY(!a.zip.isEmpty());
        QCOMPARE(a.hash, b.hash);
        writeFile(m_data + u"/history.txt"_s, "changed: not in a backup");
        QCOMPARE(pack(collect(m_ini, m_data), u"0.7.3"_s, t1).hash, a.hash);
        writeFile(m_data + u"/favorites.json"_s, "{\"favorites\":[1]}");
        QVERIFY(pack(collect(m_ini, m_data), u"0.7.3"_s, t1).hash != a.hash);
    }

    void keepsTheNewest()
    {
        const QString folder = m_tmp.filePath(u"backups"_s);
        const QDateTime start(QDate(2026, 10, 13), QTime(8, 0, 0));
        Packed packed = pack(collect(m_ini, m_data), u"0.7.3"_s, start);
        for (int i = 0; i < kKept + 2; ++i)
            QCOMPARE(write(folder, packed, start.addSecs(i * 3600)), QString());
        writeFile(folder + u"/WinShun-2026-10-13-070000.zip.part"_s, "cut short");
        writeFile(folder + u"/notes.txt"_s, "the user's");
        QCOMPARE(write(folder, packed, start.addSecs((kKept + 2) * 3600)), QString());
        const QList<Found> found = list(folder);
        QCOMPARE(found.size(), kKept);
        QCOMPARE(found.first().when, start.addSecs((kKept + 2) * 3600));
        QCOMPARE(found.last().when, start.addSecs(3 * 3600));
        QCOMPARE(QFileInfo(found.first().path).fileName(), u"WinShun-2026-10-13-200000.zip"_s);
        QCOMPARE(hashOf(found.first().path), packed.hash);
        QVERIFY(!QFile::exists(folder + u"/WinShun-2026-10-13-070000.zip.part"_s));
        QVERIFY(QFile::exists(folder + u"/notes.txt"_s));
    }

    void restores()
    {
        const QString folder = m_tmp.filePath(u"backups"_s);
        const QDateTime when(QDate(2026, 10, 13), QTime(19, 47, 0));
        QCOMPARE(write(folder, pack(collect(m_ini, m_data), u"0.7.3"_s, when), when), QString());
        // Changed since, and something new.
        writeFile(m_ini, "[Launcher]\nHotkey=Ctrl+Space\n");
        writeFile(m_data + u"/favorites.json"_s, "{\"favorites\":[1]}");
        writeFile(m_data + u"/plugins/me.later/plugin.json"_s, "{}");

        const QString staged = m_data + u"/restore"_s;
        QCOMPARE(unpack(list(folder).first().path, staged), QString());
        apply(staged, m_ini, m_data);
        QCOMPARE(readFile(m_ini), QByteArray("[Launcher]\nHotkey=Alt+Space\n"));
        QCOMPARE(readFile(m_data + u"/favorites.json"_s), QByteArray("{\"favorites\":[]}"));
        QCOMPARE(readFile(m_data + u"/icons/web/custom/mine.png"_s), QByteArray("chosen"));
        QVERIFY(QFile::exists(m_data + u"/plugins/me.later/plugin.json"_s)); // what the backup lacks stays
        QCOMPARE(readFile(m_data + u"/index.bin"_s), QByteArray("index"));
        QVERIFY(!QFileInfo::exists(staged));
    }

    void refusesOthers()
    {
        const QByteArray manifest = "{\"app\":\"WinShun\",\"format\":1}";
        const QString staged = m_tmp.filePath(u"staged"_s);
        const QString zip = m_tmp.filePath(u"x.zip"_s);
        writeZip(zip, {{"WinShun.ini", "x"}});
        QVERIFY(!unpack(zip, staged).isEmpty()); // no backup.json: not ours
        writeZip(zip, {{"backup.json", manifest}, {"data/../../evil.txt", "x"}});
        QVERIFY(!unpack(zip, staged).isEmpty());
        writeZip(zip, {{"backup.json", manifest}, {"data/C:/evil.txt", "x"}});
        QVERIFY(!unpack(zip, staged).isEmpty());
        QVERIFY(!QFile::exists(m_tmp.filePath(u"evil.txt"_s)));
        // Other names are passed over.
        writeZip(zip, {{"backup.json", manifest}, {"data/favorites.json", "f"}, {"other/x.txt", "x"}});
        QCOMPARE(unpack(zip, staged), QString());
        QVERIFY(QFile::exists(staged + u"/data/favorites.json"_s));
        QVERIFY(!QFile::exists(staged + u"/other/x.txt"_s));
        writeFile(zip, "not a zip");
        QVERIFY(!unpack(zip, staged).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestBackup)
#include "tst_backup.moc"
