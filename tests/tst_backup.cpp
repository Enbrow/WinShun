#include "BackupFiles.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <miniz.h>
#include <windows.h>

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

QByteArray manifestFor(const QList<std::pair<QByteArray, QByteArray>>& entries, int format = 1)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto& [name, bytes] : entries) {
        hash.addData(name);
        hash.addData(QByteArrayView("\0", 1));
        hash.addData(bytes);
        hash.addData(QByteArrayView("\0", 1));
    }
    return QJsonDocument(QJsonObject {{u"app"_s, u"WinShun"_s}, {u"format"_s, format},
                             {u"hash"_s, QString::fromLatin1(hash.result().toHex())}})
        .toJson();
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
        QCOMPARE(apply(staged, m_ini, m_data), QString());
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
        // Only the payload written by our format is accepted.
        writeZip(zip, {{"backup.json", manifest}, {"data/favorites.json", "f"}, {"other/x.txt", "x"}});
        QVERIFY(!unpack(zip, staged).isEmpty());
        QVERIFY(!QFile::exists(staged + u"/other/x.txt"_s));
        writeFile(zip, "not a zip");
        QVERIFY(!unpack(zip, staged).isEmpty());
    }

    void exportsWithoutPruning()
    {
        const QString folder = m_tmp.filePath(u"exports"_s);
        const QDateTime when(QDate(2026, 10, 25), QTime(22, 5));
        for (int i = 0; i < kKept + 2; ++i)
            writeFile(QDir(folder).filePath(fileName(when.addSecs(i))), "user's archive");
        const QString path = QDir(folder).filePath(u"我的换机设置.zip"_s);
        const Packed packed = pack(collect(m_ini, m_data), u"0.13.0"_s, when, true);
        QCOMPARE(exportTo(path, packed), QString());
        QCOMPARE(list(folder).size(), kKept + 2); // exports do not rotate this folder
        QCOMPARE(hashOf(path), packed.hash);
        QCOMPARE(unpack(path, m_tmp.filePath(u"check-export"_s)), QString());
        const QByteArray before = readFile(path);
        QVERIFY(!exportTo(path, {}).isEmpty());
        QCOMPARE(readFile(path), before);
    }

    void preservesBackupsWrittenInTheSameSecond()
    {
        const QString folder = m_tmp.filePath(u"same-second"_s);
        const QDateTime when(QDate(2026, 10, 25), QTime(22, 5));
        const Packed first = pack(collect(m_ini, m_data), u"0.13.0"_s, when);
        QCOMPARE(write(folder, first, when), QString());
        writeFile(m_ini, "[Launcher]\nHotkey=Ctrl+Space\n");
        const Packed second = pack(collect(m_ini, m_data), u"0.13.0"_s, when);
        QCOMPARE(write(folder, second, when), QString());
        QCOMPARE(list(folder).size(), 2);
        QCOMPARE(hashOf(QDir(folder).filePath(fileName(when))), first.hash);
        QCOMPARE(hashOf(list(folder).first().path), second.hash);
    }

    void readsExistingVersionOneBackups()
    {
        const QString archive = m_tmp.filePath(u"old-format.zip"_s);
        const QByteArray original = readFile(m_ini);
        const QList<std::pair<QByteArray, QByteArray>> entries {{"WinShun.ini", original}};
        // Original v1 manifest: no portable flag, and no new fields needed.
        writeZip(archive, {{"backup.json", manifestFor(entries)}, entries.first()});
        const QString staged = m_tmp.filePath(u"old-stage"_s);
        QCOMPARE(unpack(archive, staged), QString());
        writeFile(m_ini, "[Launcher]\nHotkey=Changed\n");
        QCOMPARE(apply(staged, m_ini, m_data), QString());
        QCOMPARE(readFile(m_ini), original);
    }

    void migratesToAnotherProfile()
    {
        {
            QSettings source(m_ini, QSettings::IniFormat);
            source.setValue(u"Backup/Folder"_s, u"Z:/old-sync"_s);
            source.setValue(u"Index/Folder"_s, u"Z:/old-index"_s);
            source.setValue(u"Index/Drives"_s, QStringList {u"Z:"_s});
            source.setValue(u"Launcher/FolderMenuOpusBefore"_s, u"old-opus"_s);
            source.setValue(u"Capture/PrintScreenBefore"_s, u"old-windows"_s);
            source.setValue(u"Appearance/UiSize"_s, u"roomy"_s);
            source.setValue(u"Index/Folders"_s, QStringList {u"D:/Projects"_s});
            source.setValue(u"Plugin/me.notes/Setting.endpoint"_s, u"my-server"_s);
            source.sync();
        }
        const QByteArray sourceBefore = readFile(m_ini);
        const QString archive = m_tmp.filePath(u"transfer.zip"_s);
        const Packed packed = pack(collect(m_ini, m_data), u"0.13.0"_s, QDateTime::currentDateTime(), true);
        QCOMPARE(exportTo(archive, packed), QString());
        QCOMPARE(readFile(m_ini), sourceBefore); // exporting never edits the source
        const QString staged = m_tmp.filePath(u"import"_s);
        QCOMPARE(unpack(archive, staged), QString());
        {
            QSettings exported(staged + u"/WinShun.ini"_s, QSettings::IniFormat);
            QVERIFY(!exported.contains(u"Backup/Folder"_s));
            QVERIFY(!exported.contains(u"Index/Folder"_s));
            QVERIFY(!exported.contains(u"Index/Drives"_s));
            QCOMPARE(exported.value(u"Appearance/UiSize"_s).toString(), u"roomy"_s);
        }
        const QString target = m_tmp.filePath(u"new-pc/WinShun.ini"_s);
        const QString data = m_tmp.filePath(u"new-pc/data"_s);
        writeFile(target, "[Launcher]\nHotkey=Ctrl+Space\n");
        {
            QSettings current(target, QSettings::IniFormat);
            current.setValue(u"Backup/Folder"_s, u"C:/new-sync"_s);
            current.setValue(u"Index/Folder"_s, u"C:/new-index"_s);
            current.setValue(u"Index/Drives"_s, QStringList {u"C:"_s});
            current.setValue(u"Launcher/FolderMenuOpusBefore"_s, u"new-opus"_s);
            current.setValue(u"Capture/PrintScreenBefore"_s, u"new-windows"_s);
            current.sync();
        }
        QCOMPARE(apply(staged, target, data), QString());
        QSettings migrated(target, QSettings::IniFormat);
        QCOMPARE(migrated.value(u"Launcher/Hotkey"_s).toString(), u"Alt+Space"_s);
        QCOMPARE(migrated.value(u"Appearance/UiSize"_s).toString(), u"roomy"_s);
        QCOMPARE(migrated.value(u"Plugin/me.notes/Setting.endpoint"_s).toString(), u"my-server"_s);
        QCOMPARE(migrated.value(u"Index/Folders"_s).toStringList(), QStringList {u"D:/Projects"_s});
        QCOMPARE(migrated.value(u"Backup/Folder"_s).toString(), u"C:/new-sync"_s);
        QCOMPARE(migrated.value(u"Index/Folder"_s).toString(), u"C:/new-index"_s);
        QCOMPARE(migrated.value(u"Index/Drives"_s).toStringList(), QStringList {u"C:"_s});
        QCOMPARE(migrated.value(u"Launcher/FolderMenuOpusBefore"_s).toString(), u"new-opus"_s);
        QCOMPARE(migrated.value(u"Capture/PrintScreenBefore"_s).toString(), u"new-windows"_s);
        QCOMPARE(readFile(data + u"/favorites.json"_s), QByteArray("{\"favorites\":[]}"));
        QCOMPARE(readFile(data + u"/icons/web/custom/mine.png"_s), QByteArray("chosen"));
        QVERIFY(QFile::exists(data + u"/plugins/me.notes/plugin.json"_s));
        QVERIFY(!QFile::exists(data + u"/clipboard/clipboard.db"_s));
    }

    void rejectsInvalidPayloads()
    {
        const QString archive = m_tmp.filePath(u"invalid.zip"_s);
        const QString staged = m_tmp.filePath(u"invalid-stage"_s);
        const QByteArray original = readFile(m_ini);
        const QList<std::pair<QByteArray, QByteArray>> valid {{"WinShun.ini", original}};
        writeZip(archive, {{"backup.json", manifestFor(valid, 2)}, valid.first()});
        QVERIFY(unpack(archive, staged).contains(u"格式"_s));
        writeZip(archive, {{"backup.json", manifestFor(valid)}, {"WinShun.ini", "[Launcher]\nHotkey=Changed\n"}});
        QVERIFY(!unpack(archive, staged).isEmpty()); // valid ZIP CRC, wrong manifest hash
        for (const QByteArray& name : {QByteArray("data/../escape"), QByteArray("data/plugins/CON.txt"),
                 QByteArray("data/plugins/x/file. "), QByteArray("data/plugins/x/file:stream"),
                 QByteArray("data/clipboard/clipboard.db"), QByteArray("winshun.ini")}) {
            auto entries = valid;
            entries.append({name, "x"});
            const QByteArray manifest = manifestFor(entries);
            entries.prepend({"backup.json", manifest});
            writeZip(archive, entries);
            QVERIFY2(!unpack(archive, staged).isEmpty(), name.constData());
        }
        auto duplicate = valid;
        duplicate.append(valid.first());
        duplicate.prepend({"backup.json", manifestFor(duplicate)});
        writeZip(archive, duplicate);
        QVERIFY(!unpack(archive, staged).isEmpty());
        const QList<std::pair<QByteArray, QByteArray>> emptyIni {{"WinShun.ini", "not settings"}};
        writeZip(archive, {{"backup.json", manifestFor(emptyIni)}, emptyIni.first()});
        QVERIFY(!unpack(archive, staged).isEmpty());
        const QList<std::pair<QByteArray, QByteArray>> noIni {{"data/favorites.json", "{}"}};
        writeZip(archive, {{"backup.json", manifestFor(noIni)}, noIni.first()});
        QVERIFY(!unpack(archive, staged).isEmpty());
        QCOMPARE(readFile(m_ini), original);
        QVERIFY(pack({}, u"0.13.0"_s, QDateTime::currentDateTime()).zip.isEmpty());
    }

    void rollsBackIfAFileCannotBeReplaced()
    {
        const QString archive = m_tmp.filePath(u"restore.zip"_s);
        QCOMPARE(exportTo(archive, pack(collect(m_ini, m_data), u"0.13.0"_s, QDateTime::currentDateTime())), QString());
        const QString staged = m_tmp.filePath(u"staged"_s);
        QCOMPARE(unpack(archive, staged), QString());
        const QByteArray current = "[Launcher]\nHotkey=Ctrl+Space\n";
        writeFile(m_ini, current);
        const QString favorite = m_data + u"/favorites.json"_s;
        writeFile(favorite, "{\"favorites\":[1]}");
        QVERIFY(QFile::remove(m_data + u"/dialog-pins.txt"_s));
        // Readable for the safety copy, but held without FILE_SHARE_DELETE:
        // the earlier INI and pins are replaced before this commit fails.
        const HANDLE lock = ::CreateFileW(reinterpret_cast<LPCWSTR>(favorite.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);
        const QString problem = apply(staged, m_ini, m_data);
        ::CloseHandle(lock);
        QVERIFY(!problem.isEmpty());
        QCOMPARE(readFile(m_ini), current);
        QCOMPARE(readFile(favorite), QByteArray("{\"favorites\":[1]}"));
        QVERIFY(!QFile::exists(m_data + u"/dialog-pins.txt"_s));
        QVERIFY(QFileInfo(staged).isDir());
        QCOMPARE(apply(staged, m_ini, m_data), QString()); // retry succeeds once unlocked
        QCOMPARE(readFile(m_ini), QByteArray("[Launcher]\nHotkey=Alt+Space\n"));
    }

    void failedExportKeepsTheExistingFile()
    {
        const QString archive = m_tmp.filePath(u"existing.zip"_s);
        writeFile(archive, "original export");
        const HANDLE lock = ::CreateFileW(reinterpret_cast<LPCWSTR>(archive.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);
        const QString problem
            = exportTo(archive, pack(collect(m_ini, m_data), u"0.13.0"_s, QDateTime::currentDateTime()));
        ::CloseHandle(lock);
        QVERIFY(!problem.isEmpty());
        QCOMPARE(readFile(archive), QByteArray("original export"));
    }
};

QTEST_GUILESS_MAIN(TestBackup)
#include "tst_backup.moc"
