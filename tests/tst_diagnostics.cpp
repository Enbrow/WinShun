#include "DiagnosticLog.h"
#include "Diagnostics.h"
#include "ContentIndexer.h"
#include "Log.h"
#include "Wtf8.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QScopeGuard>

#include <atomic>
#include <thread>

#include <miniz.h>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {
void write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(bytes), bytes.size());
}
QList<QJsonObject> events(const std::vector<diagnostics::Log::File>& files)
{
    QList<QJsonObject> out;
    for (const auto& file : files) {
        for (const QByteArray& line : file.bytes.split('\n')) {
            if (!line.isEmpty())
                out.append(QJsonDocument::fromJson(line).object());
        }
    }
    return out;
}
QMap<QString, QByteArray> unpack(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = file.readAll();
    mz_zip_archive zip {};
    if (!mz_zip_reader_init_mem(&zip, bytes.constData(), static_cast<size_t>(bytes.size()), 0))
        return {};
    QMap<QString, QByteArray> files;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat {};
        if (!mz_zip_reader_file_stat(&zip, i, &stat))
            break;
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (data) {
            files[QString::fromUtf8(stat.m_filename)]
                = QByteArray(static_cast<const char*>(data), static_cast<qsizetype>(size));
            mz_free(data);
        }
    }
    mz_zip_reader_end(&zip);
    return files;
}
} // namespace

class DiagnosticsTest : public QObject {
    Q_OBJECT
private slots:
    void cleanup() { diagnostics::initialize({}); }
    void boundedRotationAndUnicode()
    {
        QTemporaryDir dir;
        diagnostics::Log log(dir.path(), {1024, 512, 7});
        for (int i = 0; i < 100; ++i)
            QVERIFY(log.append(u"progress"_s, {{u"number"_s, i}, {u"message"_s, u"中文内容"_s.repeated(300)}}));
        const auto files = log.snapshot();
        QCOMPARE(files.size(), std::size_t(3));
        qint64 bytes = 0;
        for (const auto& file : files) {
            QVERIFY(file.bytes.size() <= 1024);
            bytes += file.bytes.size();
            for (const QByteArray& line : file.bytes.split('\n')) {
                if (line.isEmpty())
                    continue;
                QVERIFY(line.size() + 1 <= 512);
                QJsonParseError error;
                QVERIFY(QJsonDocument::fromJson(line, &error).isObject());
                QCOMPARE(error.error, QJsonParseError::NoError);
            }
        }
        QVERIFY(bytes <= 3072);
        QVERIFY(events(files).last().value(u"details"_s).toObject().value(u"truncated"_s).toBool());
        diagnostics::Log tiny(dir.filePath(u"tiny"_s), {256, 256, 7});
        QVERIFY(tiny.append(QString(500, u'\n'), {{u"message"_s, QString(1000, u'\t')}}));
        for (const auto& file : tiny.snapshot()) {
            QVERIFY(file.bytes.size() <= 256);
            QVERIFY(QJsonDocument::fromJson(file.bytes.trimmed()).isObject());
        }
    }
    void concurrentWritersKeepCompleteRecords()
    {
        QTemporaryDir dir;
        diagnostics::Log log(dir.path(), {1 << 20, 8192, 7});
        std::atomic<int> failures = 0;
        {
            std::vector<std::jthread> threads;
            for (int t = 0; t < 6; ++t)
                threads.emplace_back([&, t] {
                    for (int i = 0; i < 100; ++i) {
                        if (!log.append(u"writer"_s, {{u"thread"_s, t}, {u"number"_s, i}}))
                            ++failures;
                    }
                });
        }
        QCOMPARE(failures.load(), 0);
        const auto records = events(log.snapshot());
        QCOMPARE(records.size(), 600);
        QSet<QString> pairs;
        for (const auto& record : records) {
            const auto details = record.value(u"details"_s).toObject();
            pairs.insert(QString::number(details.value(u"thread"_s).toInt()) + u':'
                + QString::number(details.value(u"number"_s).toInt()));
        }
        QCOMPARE(pairs.size(), 600);
    }
    void failedRotationDoesNotGrowTheLog()
    {
        QTemporaryDir dir;
        diagnostics::Log log(dir.path(), {256, 256, 7});
        QVERIFY(log.append(u"first"_s));
        QVERIFY(QDir().mkpath(dir.filePath(u"events.1.jsonl"_s)));
        bool refused = false;
        for (int i = 0; i < 10; ++i)
            refused |= !log.append(u"more"_s);
        QVERIFY(refused);
        QVERIFY(QFileInfo(dir.filePath(u"events.jsonl"_s)).size() <= 256);
        QVERIFY(QFileInfo(dir.filePath(u"events.1.jsonl"_s)).isDir());
    }
    void expiryOnlyRemovesOwnedFiles()
    {
        QTemporaryDir dir;
        diagnostics::Log log(dir.path());
        QVERIFY(log.append(u"old"_s));
        write(log.archivePath(), "old archive");
        const QString unrelated = QDir(dir.path()).filePath(u"user-notes.log"_s);
        write(unrelated, "keep");
        for (const QString& path : {QDir(dir.path()).filePath(u"events.jsonl"_s), log.archivePath(), unrelated}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-8), QFileDevice::FileModificationTime));
        }
        log.prune();
        QVERIFY(log.snapshot().empty());
        QVERIFY(!QFileInfo::exists(log.archivePath()));
        QVERIFY(QFileInfo::exists(unrelated));
        QVERIFY(log.append(u"new"_s));
        QCOMPARE(events(log.snapshot()).last().value(u"event"_s).toString(), u"new"_s);
    }
    void exportWhitelistsAndOverwrites()
    {
        QTemporaryDir dir;
        const QString data = dir.filePath(u"portable/data"_s);
        const QString service = dir.filePath(u"service"_s);
        diagnostics::Log log(data + u"/diagnostics"_s);
        QVERIFY(log.append(u"content-progress"_s, {{u"done"_s, 12}}));
        write(data + u"/WinShun.log"_s, "app warning");
        write(service + u"/service.log"_s, "service warning");
        for (const QString& name : {u"WinShun.ini"_s, u"clipboard/clipboard.db"_s, u"history.txt"_s, u"mcp.log"_s,
                 u"plugins.log"_s, u"content/document.texts"_s})
            write(data + u'/' + name, "PRIVATE_DATA_DO_NOT_EXPORT");
        const auto first = diagnostics::exportArchive(log, data, service, {{u"run"_s, 1}});
        QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
        auto files = unpack(first.path);
        QCOMPARE(files.size(), 4);
        QVERIFY(files.contains(u"summary.json"_s));
        QVERIFY(files.contains(u"events.jsonl"_s));
        QVERIFY(files.contains(u"app/WinShun.log"_s));
        QVERIFY(files.contains(u"service/service.log"_s));
        for (const auto& bytes : files)
            QVERIFY(!bytes.contains("PRIVATE_DATA_DO_NOT_EXPORT"));
        const auto second = diagnostics::exportArchive(log, data, service, {{u"run"_s, 2}});
        QCOMPARE(second.path, first.path);
        QVERIFY(second.error.isEmpty());
        files = unpack(second.path);
        QCOMPARE(QJsonDocument::fromJson(files[u"summary.json"_s]).object().value(u"run"_s).toInt(), 2);
        QCOMPARE(QDir(log.folder()).entryList({u"*.zip"_s}, QDir::Files).size(), 1);
    }
    void serviceLogRotationAndLockedFileStayBounded()
    {
        QTemporaryDir dir;
        const QString previous = qEnvironmentVariable("ProgramData");
        const auto restore = qScopeGuard([&] {
            ::SetEnvironmentVariableW(
                L"ProgramData", previous.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(previous.utf16()));
        });
        QVERIFY(::SetEnvironmentVariableW(L"ProgramData", reinterpret_cast<const wchar_t*>(dir.path().utf16())));
        const QString path = dir.filePath(u"WinShun/service.log"_s);
        write(path, QByteArray(1 << 20, 'x'));
        service::log::write(L"diagnostic test: %ls", L"中文");
        QCOMPARE(QFileInfo(dir.filePath(u"WinShun/service.old.log"_s)).size(), qint64(1 << 20));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains(u"中文"_s.toUtf8()));
        file.close();
        write(path, QByteArray(1 << 20, 'x'));
        const win32::UniqueHandle locked(::CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        QVERIFY(locked.valid());
        for (int i = 0; i < 20; ++i)
            service::log::write(L"must not grow while rotation is blocked");
        QCOMPARE(QFileInfo(path).size(), qint64(1 << 20));
    }

    void exportBoundsOldGiantLogsAndReportsFailure()
    {
        QTemporaryDir dir;
        diagnostics::Log log(dir.filePath(u"diagnostics"_s));
        write(dir.filePath(u"WinShun.log"_s), QByteArray(2 << 20, 'x') + "newest error");
        const auto result = diagnostics::exportArchive(log, dir.path(), {}, {});
        QVERIFY(result.error.isEmpty());
        const auto files = unpack(result.path);
        QCOMPARE(files[u"app/WinShun.log"_s].size(), 1 << 20);
        QVERIFY(files[u"app/WinShun.log"_s].endsWith("newest error"));
        QFile previousArchive(result.path);
        QVERIFY(previousArchive.open(QIODevice::ReadOnly));
        const QByteArray previousBytes = previousArchive.readAll();
        previousArchive.close();
        {
            const win32::UniqueHandle locked(::CreateFileW(reinterpret_cast<const wchar_t*>(result.path.utf16()),
                GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
            QVERIFY(locked.valid());
            const auto failedReplace = diagnostics::exportArchive(log, dir.path(), {}, {{u"changed"_s, true}});
            QVERIFY(!failedReplace.error.isEmpty());
            QVERIFY(previousArchive.open(QIODevice::ReadOnly));
            QCOMPARE(previousArchive.readAll(), previousBytes);
            previousArchive.close();
        }
        diagnostics::Log blocked(dir.filePath(u"blocked"_s));
        QVERIFY(QDir().mkpath(blocked.archivePath()));
        const auto failed = diagnostics::exportArchive(blocked, dir.path(), {}, {});
        QVERIFY(failed.path.isEmpty());
        QVERIFY(!failed.error.isEmpty());
        QVERIFY(QFileInfo(blocked.archivePath()).isDir());
    }
    void realIndexerRecordsPauseResumeAndCompletion()
    {
        QTemporaryDir dir;
        diagnostics::initialize(dir.filePath(u"profile"_s));
        const QString root = dir.filePath(u"files"_s);
        write(root + u"/private-name.txt"_s, "PRIVATE_DOCUMENT_BODY");
        const std::string rootName = wtf8::fromUtf16(wtf8::view(QDir::toNativeSeparators(root)));
        auto index = std::make_shared<FileIndex>();
        {
            auto lock = index->writeLock();
            const auto id = index->addRoot(rootName);
            index->add(id, "private-name.txt", 0);
        }
        auto content = std::make_shared<ContentIndex>(dir.filePath(u"content"_s));
        std::atomic<bool> ready = false;
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] { return std::vector<ContentIndexer::Volume> {{rootName, false}}; };
        source.ready = [&] { return ready.load(); };
        ContentIndexer::Options options;
        options.extensions = {u"txt"_s};
        using namespace std::chrono_literals;
        ContentIndexer indexer(content, source, options, {0ms, 0ms, 1h, 0ms, 100ms});
        indexer.setSearchBoxOpen(true);
        ready = true;
        indexer.wake();
        QTRY_VERIFY_WITH_TIMEOUT(indexer.reading(), 3000);
        QCOMPARE(indexer.diagnosticStatus().value(u"phase"_s).toString(), u"paused-search-window"_s);
        QTest::qWait(650);
        QCOMPARE(indexer.progress().done, std::size_t(0));
        indexer.setSearchBoxOpen(false);
        QTRY_VERIFY_WITH_TIMEOUT(content->stats().documents == 1 && !indexer.reading(), 8000);
        indexer.stop();
        const auto logs = diagnostics::current()->snapshot();
        QStringList kinds;
        bool pause = false;
        bool heartbeat = false;
        for (const auto& record : events(logs)) {
            kinds.append(record.value(u"event"_s).toString());
            pause |= record.value(u"details"_s).toObject().value(u"phase"_s).toString() == u"paused-search-window"_s;
            heartbeat |= record.value(u"event"_s).toString() == u"content-progress"_s
                && record.value(u"details"_s).toObject().value(u"done"_s).toInt() == 0;
        }
        QVERIFY(kinds.contains(u"content-start"_s));
        QVERIFY(kinds.contains(u"content-finished"_s));
        QVERIFY(pause);
        QVERIFY(heartbeat);
        for (const auto& file : logs) {
            QVERIFY(!file.bytes.contains("PRIVATE_DOCUMENT_BODY"));
            QVERIFY(!file.bytes.contains("private-name.txt"));
        }
    }
};
QTEST_GUILESS_MAIN(DiagnosticsTest)
#include "tst_diagnostics.moc"
