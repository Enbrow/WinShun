// The index service (src/service) end to end, through its pipe client
// (ServiceVolumeAccess): `WinShunService.exe --console --pipe <random>` is
// started as a child (this test runs as administrator, as ctest does here)
// and asked for a real volume's MFT and change journal; what it gives is
// compared with reading the volume directly. The volume is the one holding
// the temporary folder (%TEMP%), or WINSHUN_TEST_VOLUME ("D:").
#include "Crawler.h"
#include "FileIndex.h"
#include "IndexFolder.h"
#include "IndexService.h"
#include "Ntfs.h"
#include "ServiceVolumeAccess.h"
#include "Snapshot.h"
#include "VolumeProtocol.h"
#include "Win32Util.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

bool elevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation {};
    DWORD size = 0;
    const bool ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size);
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

// Every record of an MFT, each reduced to a hash of what the index uses.
struct Digest {
    std::uint64_t recordCount = 0;
    std::uint64_t bytes = 0;
    std::uint64_t inUse = 0;
    std::uint64_t invalid = 0;
    std::uint64_t names = 0;
    std::uint64_t blocks = 0;
    std::vector<std::uint64_t> hashes; // by record number; 0: not handed out
    double ms = 0;
};

constexpr std::uint64_t kFnvBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void mix(std::uint64_t& h, const void* data, std::size_t n)
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

template <typename T> void mix(std::uint64_t& h, const T& v)
{
    mix(h, &v, sizeof v);
}

Digest digest(ntfs::RecordSource& source)
{
    Digest d;
    QElapsedTimer t;
    t.start();
    d.recordCount = source.recordCount();
    d.bytes = source.bytes();
    d.hashes.assign(static_cast<std::size_t>(d.recordCount), 0);
    ntfs::FileRecord record;
    while (source.next()) {
        ++d.blocks;
        for (std::size_t i = 0; i < source.blockRecords(); ++i) {
            const std::uint64_t number = std::uint64_t {source.firstRecord()} + i;
            std::uint64_t h = kFnvBasis;
            mix(h, number);
            if (!source.parse(i, record)) {
                ++d.invalid;
                mix(h, std::uint8_t {0xFF});
            } else if (!record.inUse) {
                mix(h, std::uint8_t {0xFE});
            } else {
                ++d.inUse;
                mix(h, record.baseRecord);
                mix(h, record.attributes);
                mix(h, record.modified);
                mix(h, record.directory);
                mix(h, record.hasAttributeList);
                for (const auto& n : record.names) {
                    ++d.names;
                    mix(h, n.parent);
                    mix(h, n.name.data(), n.name.size() * 2);
                }
            }
            if (number < d.hashes.size())
                d.hashes[static_cast<std::size_t>(number)] = h;
        }
    }
    d.ms = static_cast<double>(t.nsecsElapsed()) / 1e6;
    return d;
}

struct Change {
    std::int64_t usn;
    std::uint32_t reason;
    std::u16string name;
    bool operator==(const Change&) const = default;
};

std::vector<Change> changesOf(const std::vector<ntfs::UsnRecord>& records)
{
    std::vector<Change> out;
    for (const auto& r : records)
        out.push_back({r.usn, r.reason, std::u16string(r.name)});
    return out;
}

bool hasName(const std::vector<Change>& changes, std::u16string_view name, std::uint32_t reason)
{
    for (const auto& c : changes) {
        if (c.name == name && (c.reason & reason))
            return true;
    }
    return false;
}

// Reads on (waiting for something new each time) until a record names
// `name` with `reason`, or `limitMs` pass; `usn` moves on. The volume may
// be a busy one (C:), where every read completes with other programs'
// changes, so what we did may take a few batches to show up.
bool readUntil(ntfs::JournalSource& follow, std::int64_t& usn, std::u16string_view name, std::uint32_t reason,
    int limitMs = 5000)
{
    QElapsedTimer t;
    t.start();
    std::vector<ntfs::UsnRecord> records;
    std::int64_t next = 0;
    while (t.elapsed() < limitMs) {
        follow.start(usn, true);
        if (::WaitForSingleObject(follow.event(), static_cast<DWORD>(limitMs)) != WAIT_OBJECT_0)
            return false;
        if (!follow.finish(records, next))
            return false;
        usn = next;
        if (hasName(changesOf(records), name, reason))
            return true;
    }
    return false;
}

void touch(const QString& path)
{
    const HANDLE h = ::CreateFileW(path.toStdWString().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    QVERIFY(h != INVALID_HANDLE_VALUE);
    ::CloseHandle(h);
}

// The service's access, counting the MFT reads asked of it: whether the
// index caught up through the journal or read the volume again.
struct CountingAccess final : ntfs::VolumeAccess {
    ntfs::ServiceVolumeAccess inner;
    std::atomic<int> mftOpens {0};

    explicit CountingAccess(std::wstring pipe) : inner(std::move(pipe)) {}
    Kind kind() const noexcept override { return Kind::Service; }
    std::unique_ptr<ntfs::RecordSource> openMft(std::wstring_view root, std::uint32_t serial, bool lowPriority) override
    {
        ++mftOpens;
        return inner.openMft(root, serial, lowPriority);
    }
    std::optional<ntfs::JournalInfo> queryJournal(std::wstring_view root, std::uint32_t serial) override
    {
        return inner.queryJournal(root, serial);
    }
    std::unique_ptr<ntfs::JournalSource> openJournal(std::wstring_view root, std::uint32_t serial,
        std::uint64_t journalId) override
    {
        return inner.openJournal(root, serial, journalId);
    }
    void closeVolume(std::wstring_view root) override { inner.closeVolume(root); }
};

// Whether `path` is in the index (with its read lock, as the app looks).
bool indexed(const IndexService& service, const QString& path)
{
    const std::shared_ptr<FileIndex> index = service.index();
    const auto lock = index->readLock();
    return index->findPath(path.toStdWString()) != kNoEntry;
}

// A connection of our own, for frames the client would never send.
struct RawClient {
    volproto::Channel channel;

    bool connect(const QString& pipe)
    {
        const std::wstring path = volproto::fullPipePath(pipe.toStdWString());
        for (int attempt = 0; attempt < 3; ++attempt) {
            const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                channel = volproto::Channel(h);
                return true;
            }
            // Between one client accepted and the next instance made: wait for it.
            if (::GetLastError() != ERROR_PIPE_BUSY || !::WaitNamedPipeW(path.c_str(), 2000))
                return false;
        }
        return false;
    }
    bool hello(std::uint16_t major, volproto::Type& reply, std::vector<std::byte>& payload)
    {
        volproto::Hello hello;
        hello.major = major;
        volproto::Writer w;
        encode(w, hello);
        return channel.write(w.finish(volproto::Type::Hello), 5000) && channel.read(reply, payload, 5000);
    }
};

} // namespace

class ServiceTest : public QObject {
    Q_OBJECT

    QString m_exe = QString::fromUtf8(WINSHUN_SERVICE_EXE);
    QString m_pipe;
    bool m_external = false; // WINSHUN_TEST_PIPE: a server someone else started (--no-privileges, say)
    std::unique_ptr<QProcess> m_service;
    std::wstring m_root; // "C:"
    std::uint32_t m_serial = 0;
    QString m_scratch; // a folder on that volume

    void startService()
    {
        if (m_external)
            return;
        m_service = std::make_unique<QProcess>();
        m_service->setProgram(m_exe);
        m_service->setArguments({u"--console"_s, u"--pipe"_s, m_pipe});
        m_service->setProcessChannelMode(QProcess::SeparateChannels);
        m_service->start();
        QVERIFY2(m_service->waitForStarted(5000), qPrintable(m_service->errorString()));
        QByteArray out;
        QElapsedTimer t;
        t.start();
        while (!out.contains("ready") && t.elapsed() < 10000 && m_service->state() == QProcess::Running) {
            m_service->waitForReadyRead(200);
            out += m_service->readAllStandardOutput();
        }
        QVERIFY2(out.contains("ready"), qPrintable(u"service did not come up: "_s + QString::fromUtf8(out)
            + QString::fromUtf8(m_service->readAllStandardError())));
    }

    void stopService()
    {
        if (!m_service)
            return;
        m_service->kill();
        m_service->waitForFinished(5000);
        const QByteArray log = m_service->readAllStandardError();
        if (!log.isEmpty())
            qInfo().noquote() << "service log:\n" << QString::fromUtf8(log).remove(u'\r').trimmed();
        m_service.reset();
    }

private slots:
    void initTestCase()
    {
        if (!elevated())
            QSKIP("runs as administrator only (the service reads the volumes)");
        QVERIFY2(QFile::exists(m_exe), qPrintable(m_exe));
        m_pipe = qEnvironmentVariable("WINSHUN_TEST_PIPE");
        m_external = !m_pipe.isEmpty();
        if (!m_external)
            m_pipe = u"WinShun.Volumes.test."_s + QUuid::createUuid().toString(QUuid::Id128).left(12);

        // The volume: WINSHUN_TEST_VOLUME, else the temporary folder's.
        QString root = qEnvironmentVariable("WINSHUN_TEST_VOLUME").left(2).toUpper();
        if (root.size() != 2)
            root = QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).left(2).toUpper();
        m_root = root.toStdWString();
        bool ntfs = false;
        for (const VolumeInfo& v : listLocalVolumes(true)) {
            if (win32::equalsIgnoreCase(v.root, m_root)) {
                m_serial = v.serial;
                ntfs = v.ntfs;
            }
        }
        QVERIFY2(ntfs && m_serial != 0, qPrintable(root + u" is not a local NTFS volume"_s));
        // A scratch folder on that volume: under TEMP when it is TEMP's, else at its root.
        const QString temp = QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::TempLocation));
        m_scratch = (temp.left(2).compare(root, Qt::CaseInsensitive) == 0 ? temp : root)
            + u"\\wsservice-test-"_s + QString::number(::GetCurrentProcessId());
        QVERIFY(QDir().mkpath(m_scratch));
        qInfo("volume %ls (serial %08X), scratch %ls", m_root.c_str(), m_serial, qUtf16Printable(m_scratch));
        startService();
    }

    void cleanupTestCase()
    {
        stopService();
        QDir(m_scratch).removeRecursively();
    }

    void handshake()
    {
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        QVERIFY2(access.probe(), qPrintable(QString::fromStdWString(access.lastError())));
        QCOMPARE(QString::fromStdWString(access.serviceVersion()), QString::fromUtf8(WINSHUN_VERSION));
        QVERIFY(access.lastError().empty());
        // Not there at all: told so, no hang.
        ntfs::ServiceVolumeAccess nothing(L"WinShun.Volumes.nowhere");
        QVERIFY(!nothing.probe());
        QVERIFY(QString::fromStdWString(nothing.lastError()).contains(u"not running"));
    }

    void wrongProtocolRefused()
    {
        RawClient raw;
        QVERIFY(raw.connect(m_pipe));
        volproto::Type reply {};
        std::vector<std::byte> payload;
        QVERIFY(raw.hello(volproto::kMajor + 1, reply, payload));
        QCOMPARE(reply, volproto::Type::HelloRefused);
        volproto::HelloRefused refused;
        QVERIFY(decode(payload, refused));
        QCOMPARE(refused.reason, static_cast<std::uint32_t>(volproto::Refusal::Protocol));
        // The connection is over: nothing more comes.
        QVERIFY(!raw.channel.read(reply, payload, 2000));
    }

    void malformedFramesDropped()
    {
        // A frame longer than any the service reads: dropped unread.
        RawClient raw;
        QVERIFY(raw.connect(m_pipe));
        volproto::Type reply {};
        std::vector<std::byte> payload;
        QVERIFY(raw.hello(volproto::kMajor, reply, payload));
        QCOMPARE(reply, volproto::Type::HelloOk);
        std::byte header[8] {};
        const std::uint32_t length = volproto::kMaxPayloadBytes + 1;
        const std::uint16_t type = static_cast<std::uint16_t>(volproto::Type::OpenMft);
        std::memcpy(header, &length, 4);
        std::memcpy(header + 4, &type, 2);
        QVERIFY(raw.channel.write(header, 2000));
        QVERIFY(!raw.channel.read(reply, payload, 3000)); // gone
        QVERIFY(raw.channel.lastError() == ERROR_BROKEN_PIPE || raw.channel.lastError() == ERROR_PIPE_NOT_CONNECTED
            || raw.channel.lastError() == ERROR_NO_DATA);

        // A request whose root is not a drive: refused with Error.
        RawClient bad;
        QVERIFY(bad.connect(m_pipe));
        QVERIFY(bad.hello(volproto::kMajor, reply, payload));
        volproto::Writer w;
        encode(w, volproto::OpenMft {L"C:\\Users", 0, false});
        QVERIFY(bad.channel.write(w.finish(volproto::Type::OpenMft), 2000));
        QVERIFY(bad.channel.read(reply, payload, 5000));
        QCOMPARE(reply, volproto::Type::Error);
        volproto::Error error;
        QVERIFY(decode(payload, error));
        QCOMPARE(error.code, std::uint32_t {ERROR_INVALID_DATA});

        // An OpenMft cut short.
        RawClient cut;
        QVERIFY(cut.connect(m_pipe));
        QVERIFY(cut.hello(volproto::kMajor, reply, payload));
        volproto::Writer w2;
        w2.u16(2);
        w2.u16(u'C'); // the name says two units, one follows
        QVERIFY(cut.channel.write(w2.finish(volproto::Type::OpenMft), 2000));
        QVERIFY(cut.channel.read(reply, payload, 5000));
        QCOMPARE(reply, volproto::Type::Error);
    }

    void mftMatchesDirect()
    {
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        ntfs::DirectVolumeAccess direct;
        // The service's read first, the direct one right after: the volume
        // changes meanwhile (other programs write), so a few records may differ.
        const std::unique_ptr<ntfs::RecordSource> viaService = access.openMft(m_root, m_serial, false);
        QVERIFY(viaService);
        QVERIFY2(viaService->valid(), qPrintable(QString::fromStdWString(viaService->error())));
        const Digest s = digest(*viaService);
        QVERIFY2(viaService->valid(), qPrintable(QString::fromStdWString(viaService->error())));
        QCOMPARE(viaService->bytesDone(), viaService->bytes());
        const std::unique_ptr<ntfs::RecordSource> viaVolume = direct.openMft(m_root, m_serial, false);
        QVERIFY(viaVolume && viaVolume->valid());
        const Digest d = digest(*viaVolume);
        QVERIFY(viaVolume->valid());

        QCOMPARE(s.recordCount, d.recordCount);
        QCOMPARE(s.bytes, d.bytes);
        std::uint64_t same = 0, differ = 0, handedOut = 0;
        for (std::size_t i = 0; i < d.hashes.size(); ++i) {
            if (d.hashes[i] == 0 && s.hashes[i] == 0)
                continue; // left out as free by both
            ++handedOut;
            if (d.hashes[i] == s.hashes[i])
                ++same;
            else
                ++differ;
        }
        qInfo("MFT %ls: %llu records, %llu handed out; service %llu in use, %llu names, %llu blocks, %.0f ms; "
              "direct %llu in use, %llu names, %llu blocks, %.0f ms; %llu records differ (changed meanwhile)",
            m_root.c_str(), static_cast<unsigned long long>(d.recordCount), static_cast<unsigned long long>(handedOut),
            static_cast<unsigned long long>(s.inUse), static_cast<unsigned long long>(s.names),
            static_cast<unsigned long long>(s.blocks), s.ms, static_cast<unsigned long long>(d.inUse),
            static_cast<unsigned long long>(d.names), static_cast<unsigned long long>(d.blocks), d.ms,
            static_cast<unsigned long long>(differ));
        QVERIFY(handedOut > 1000);
        QVERIFY2(differ * 10'000 <= handedOut, "more than 0.01% of the records differ");
        QVERIFY(same + differ == handedOut);
        QCOMPARE(s.invalid, d.invalid);
    }

    void mftRefusals()
    {
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        // Another serial: the drive letter holds another disk, as far as the service knows.
        const std::unique_ptr<ntfs::RecordSource> wrong = access.openMft(m_root, m_serial ^ 0x5A5A, false);
        QVERIFY(wrong && !wrong->valid());
        QVERIFY2(QString::fromStdWString(wrong->error()).contains(u"another volume"),
            qPrintable(QString::fromStdWString(wrong->error())));
        // One MFT read per process and volume at a time.
        const std::unique_ptr<ntfs::RecordSource> first = access.openMft(m_root, m_serial, false);
        QVERIFY(first && first->valid());
        const std::unique_ptr<ntfs::RecordSource> second = access.openMft(m_root, m_serial, false);
        QVERIFY(second && !second->valid());
        QVERIFY2(QString::fromStdWString(second->error()).contains(u"already"),
            qPrintable(QString::fromStdWString(second->error())));
    }

    void journalMatchesDirect()
    {
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        const std::optional<ntfs::JournalInfo> viaService = access.queryJournal(m_root, m_serial);
        const std::optional<ntfs::JournalInfo> viaVolume = ntfs::queryJournal(m_root);
        QVERIFY(viaService && viaVolume);
        QCOMPARE(viaService->id, viaVolume->id);
        QVERIFY(viaService->nextUsn <= viaVolume->nextUsn);
        QVERIFY(!access.queryJournal(m_root, m_serial ^ 1));

        // Changes in the scratch folder: the service's records are those the volume gives.
        const std::int64_t from = viaVolume->nextUsn;
        touch(m_scratch + u"\\one.txt"_s);
        QVERIFY(QFile::rename(m_scratch + u"\\one.txt"_s, m_scratch + u"\\two.txt"_s));
        QVERIFY(QFile::remove(m_scratch + u"\\two.txt"_s));
        const std::unique_ptr<ntfs::JournalSource> follow = access.openJournal(m_root, m_serial, viaService->id);
        QVERIFY(follow && follow->valid());
        std::vector<ntfs::UsnRecord> records;
        std::int64_t next = 0;
        follow->start(from, false);
        QCOMPARE(::WaitForSingleObject(follow->event(), 5000), DWORD {WAIT_OBJECT_0});
        QVERIFY(follow->finish(records, next));
        QVERIFY(next > from);
        const std::vector<Change> got = changesOf(records);
        QVERIFY(hasName(got, u"one.txt", USN_REASON_FILE_CREATE));
        QVERIFY(hasName(got, u"one.txt", USN_REASON_RENAME_OLD_NAME));
        QVERIFY(hasName(got, u"two.txt", USN_REASON_RENAME_NEW_NAME));
        QVERIFY(hasName(got, u"two.txt", USN_REASON_FILE_DELETE));
        ntfs::JournalReader reader(m_root, viaVolume->id);
        QVERIFY(reader.valid());
        reader.start(from, false);
        std::vector<ntfs::UsnRecord> direct;
        std::int64_t directNext = 0;
        QVERIFY(reader.finish(direct, directNext));
        const std::vector<Change> want = changesOf(direct);
        QVERIFY(want.size() >= got.size()); // more may have come since
        QVERIFY(std::equal(got.begin(), got.end(), want.begin()));
        if (want.size() == got.size())
            QCOMPARE(next, directNext);

        // Waiting for something new, then a cancel, then a read that still lines up.
        std::int64_t usn = next;
        follow->start(usn, true);
        touch(m_scratch + u"\\three.txt"_s);
        QCOMPARE(::WaitForSingleObject(follow->event(), 5000), DWORD {WAIT_OBJECT_0});
        QVERIFY(follow->finish(records, next));
        usn = next;
        if (!hasName(changesOf(records), u"three.txt", USN_REASON_FILE_CREATE))
            QVERIFY(readUntil(*follow, usn, u"three.txt", USN_REASON_FILE_CREATE));
        follow->start(usn, true);
        follow->cancel();
        touch(m_scratch + u"\\four.txt"_s);
        QVERIFY(readUntil(*follow, usn, u"four.txt", USN_REASON_FILE_CREATE));
        // A cancel that races the read: the batch is dropped, the next read
        // starts from the same place and finds the file anyway.
        follow->start(usn, true);
        touch(m_scratch + u"\\five.txt"_s);
        ::WaitForSingleObject(follow->event(), 2000);
        follow->cancel();
        QVERIFY(readUntil(*follow, usn, u"five.txt", USN_REASON_FILE_CREATE));
    }

    void closeVolumeAcknowledged()
    {
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        const std::optional<ntfs::JournalInfo> journal = access.queryJournal(m_root, m_serial);
        QVERIFY(journal);
        const std::unique_ptr<ntfs::JournalSource> follow = access.openJournal(m_root, m_serial, journal->id);
        QVERIFY(follow && follow->valid());
        std::int64_t usn = journal->nextUsn;
        follow->start(usn, true); // the service waits on the volume (or a busy volume answers at once)
        QElapsedTimer t;
        t.start();
        access.closeVolume(m_root);
        const qint64 ms = t.elapsed();
        qInfo("CloseVolume acknowledged in %lld ms", static_cast<long long>(ms));
        QVERIFY(ms < 2000);
        // The follower was told (after any batch that completed first): its
        // journal is gone to it.
        std::vector<ntfs::UsnRecord> records;
        std::int64_t next = 0;
        bool lost = false;
        for (int i = 0; i < 20 && !lost; ++i) {
            QCOMPARE(::WaitForSingleObject(follow->event(), 2000), DWORD {WAIT_OBJECT_0});
            lost = !follow->finish(records, next);
            if (!lost) {
                usn = next;
                follow->start(usn, true);
            }
        }
        QVERIFY(lost);
        // Nothing held: a CloseVolume now is a no-op, and quick.
        t.restart();
        access.closeVolume(m_root);
        QVERIFY(t.elapsed() < 1000);
    }

    void serviceGoneAndBack()
    {
        if (m_external)
            QSKIP("the server is not ours to stop");
        ntfs::ServiceVolumeAccess::setReconnectWindow(std::chrono::seconds(6));
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        const std::optional<ntfs::JournalInfo> journal = access.queryJournal(m_root, m_serial);
        QVERIFY(journal);
        const std::unique_ptr<ntfs::JournalSource> follow = access.openJournal(m_root, m_serial, journal->id);
        QVERIFY(follow && follow->valid());
        std::int64_t usn = journal->nextUsn;
        std::vector<ntfs::UsnRecord> records;
        std::int64_t next = 0;

        // The service dies while a read waits: batches that completed before
        // still come; then no "journal lost", just nothing new, and the next
        // reads try again.
        follow->start(usn, true);
        stopService();
        bool quiet = false;
        for (int i = 0; i < 20 && !quiet; ++i) {
            QCOMPARE(::WaitForSingleObject(follow->event(), 5000), DWORD {WAIT_OBJECT_0});
            QVERIFY(follow->finish(records, next));
            quiet = records.empty() && next == usn;
            if (!quiet) {
                usn = next;
                follow->start(usn, true);
            }
        }
        QVERIFY(quiet);
        follow->start(usn, true); // cannot connect: completes at once, with nothing
        QCOMPARE(::WaitForSingleObject(follow->event(), 1000), DWORD {WAIT_OBJECT_0});
        QVERIFY(follow->finish(records, next));
        QVERIFY(records.empty());
        QCOMPARE(next, usn);

        // Back: the next read connects again and goes on from the same USN.
        touch(m_scratch + u"\\while-away.txt"_s);
        startService();
        const std::int64_t before = usn;
        QVERIFY(readUntil(*follow, usn, u"while-away.txt", USN_REASON_FILE_CREATE));
        QVERIFY(usn > before);

        // Away for longer than the window: given up as lost.
        follow->start(usn, true);
        stopService();
        QElapsedTimer t;
        t.start();
        bool lost = false;
        while (!lost && t.elapsed() < 20000) {
            QCOMPARE(::WaitForSingleObject(follow->event(), 5000), DWORD {WAIT_OBJECT_0});
            lost = !follow->finish(records, next);
            if (!lost) {
                usn = next;
                ::Sleep(500); // as followJournals pauses between reads
                follow->start(usn, true);
            }
        }
        qInfo("given up after %lld ms", static_cast<long long>(t.elapsed()));
        QVERIFY(lost);
        QVERIFY(t.elapsed() >= 6000 && t.elapsed() < 12000);
        ntfs::ServiceVolumeAccess::setReconnectWindow(std::chrono::seconds(30));
        startService(); // for the tests after this one
    }

    // ---- IndexService with the service coming up after Win顺 (GitHub #21) ----------

    // Rules that leave out every other volume and, on ours, every folder but
    // the scratch folder's branch: an index small enough to read or walk in a
    // moment, but a real one, from the real MFT and journal.
    CrawlRules scratchRules() const
    {
        CrawlRules rules;
        for (const VolumeInfo& v : listLocalVolumes(true)) {
            if (!win32::equalsIgnoreCase(v.root, m_root))
                rules.excludedPaths.push_back(v.root); // "E:": the whole drive
        }
        QString ancestor = QString::fromStdWString(m_root);
        for (const QString& next : m_scratch.mid(3).split(u'\\', Qt::SkipEmptyParts)) {
            const QFileInfoList siblings
                = QDir(ancestor + u'\\').entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
            for (const QFileInfo& sub : siblings) {
                if (sub.fileName().compare(next, Qt::CaseInsensitive) != 0)
                    rules.excludedPaths.push_back(QDir::toNativeSeparators(sub.absoluteFilePath()).toStdWString());
            }
            ancestor += u'\\' + next;
        }
        return rules;
    }

    IndexService::Options scratchOptions(std::shared_ptr<ntfs::VolumeAccess> access) const
    {
        IndexService::Options options;
        options.rules = scratchRules();
        options.includeRemovable = true; // whatever kind of disk the scratch folder is on
        options.content.enabled = false;
        options.volumeAccess = std::move(access);
        return options;
    }

    // Writes `folder`'s snapshot: the scratch branch read through the service
    // and followed from the journal's position of now, plus a folder that is
    // not on disk, which only a walk or a read of the MFT would drop.
    QString fakeFolder() const { return m_scratch + u"\\not-on-disk"_s; }
    void makeSnapshot(const QString& folder)
    {
        {
            IndexService first(folder, scratchOptions(std::make_shared<ntfs::ServiceVolumeAccess>(m_pipe.toStdWString())));
            first.start();
            QTRY_COMPARE_WITH_TIMEOUT(first.state(), IndexService::State::Ready, 180'000);
            QVERIFY(indexed(first, m_scratch));
            first.shutdown(); // saves the snapshot with the volume's journal position
        }
        const QString path = indexfolder::snapshotPath(folder);
        std::optional<snapshot::Contents> contents = snapshot::load(path);
        QVERIFY(contents && contents->rules);
        QCOMPARE(contents->volumes.size(), std::size_t {1});
        QVERIFY(contents->journals.at(0).journalId != 0);
        const EntryId scratch = contents->index->findPath(m_scratch.toStdWString());
        QVERIFY(scratch != kNoEntry);
        {
            const auto lock = contents->index->writeLock();
            contents->index->add(scratch, "not-on-disk", EntryFlag::Directory);
        }
        const auto lock = contents->index->readLock();
        QVERIFY(snapshot::save(*contents->index, contents->volumes, contents->journals, *contents->rules, path));
    }

    void indexServiceComesLate()
    {
        QTemporaryDir dir;
        makeSnapshot(dir.path());
        // Started with no way to the journals, as at login before the service
        // is up: searchable from the snapshot, and waiting for the service.
        IndexService::Options options = scratchOptions(std::make_shared<ntfs::NoVolumeAccess>());
        options.accessWaitMs = 20'000;
        IndexService late(dir.path(), options);
        late.start();
        QTRY_COMPARE_WITH_TIMEOUT(late.state(), IndexService::State::Ready, 10'000);
        QTest::qWait(500);
        QCOMPARE(late.volumeAccessKind(), ntfs::VolumeAccess::Kind::None);
        QVERIFY(indexed(late, fakeFolder()));
        // The service: caught up from the snapshot's position (a file made
        // now comes through the journal), no MFT read, no walk.
        const auto service = std::make_shared<CountingAccess>(m_pipe.toStdWString());
        late.setVolumeAccess(service);
        QCOMPARE(late.volumeAccessKind(), ntfs::VolumeAccess::Kind::Service);
        const QString made = m_scratch + u"\\came-late.txt"_s;
        touch(made);
        QTRY_VERIFY_WITH_TIMEOUT(indexed(late, made), 15'000);
        QCOMPARE(service->mftOpens.load(), 0);
        QVERIFY(indexed(late, fakeFolder()));
        late.shutdown();
    }

    void indexServiceNeverComes()
    {
        QTemporaryDir dir;
        makeSnapshot(dir.path());
        // Waited for in vain: the volume is walked (the journal position is
        // lost, so whatever the rescan setting says), a little after start.
        IndexService::Options options = scratchOptions(std::make_shared<ntfs::NoVolumeAccess>());
        options.accessWaitMs = 300;
        options.rescanOnStartup = false;
        options.rescanDelayMs = 300;
        const QString made = m_scratch + u"\\walked-in.txt"_s;
        touch(made); // after the snapshot: only the walk can find it
        IndexService walked(dir.path(), options);
        walked.start();
        QTRY_COMPARE_WITH_TIMEOUT(walked.state(), IndexService::State::Ready, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(!indexed(walked, fakeFolder()), 60'000);
        QVERIFY(indexed(walked, made));
        QCOMPARE(walked.volumeAccessKind(), ntfs::VolumeAccess::Kind::None);
        // The service after all: the position is gone, so the MFT is read
        // once, and the volume followed from there.
        const auto service = std::make_shared<CountingAccess>(m_pipe.toStdWString());
        walked.setVolumeAccess(service);
        QTRY_COMPARE_WITH_TIMEOUT(service->mftOpens.load(), 1, 180'000);
        QTRY_VERIFY_WITH_TIMEOUT(walked.readingVolumes().empty() && walked.state() == IndexService::State::Ready, 180'000);
        QVERIFY(indexed(walked, made));
        const QString after = m_scratch + u"\\after-the-read.txt"_s;
        touch(after);
        QTRY_VERIFY_WITH_TIMEOUT(indexed(walked, after), 15'000);
        QCOMPARE(service->mftOpens.load(), 1);
        walked.shutdown();
    }

    void connectionLimit()
    {
        // 16 connections per process; the 17th is refused.
        std::vector<std::unique_ptr<RawClient>> held;
        volproto::Type reply {};
        std::vector<std::byte> payload;
        for (int i = 0; i < 16; ++i) {
            auto raw = std::make_unique<RawClient>();
            QVERIFY(raw->connect(m_pipe));
            QVERIFY(raw->hello(volproto::kMajor, reply, payload));
            QCOMPARE(reply, volproto::Type::HelloOk);
            held.push_back(std::move(raw));
        }
        RawClient extra;
        QVERIFY(extra.connect(m_pipe));
        QVERIFY(extra.hello(volproto::kMajor, reply, payload));
        QCOMPARE(reply, volproto::Type::HelloRefused);
        volproto::HelloRefused refused;
        QVERIFY(decode(payload, refused));
        QCOMPARE(refused.reason, static_cast<std::uint32_t>(volproto::Refusal::TooManyConnections));
        held.clear();
        // Let go: room again (the service notices the closed ones).
        QTest::qWait(300);
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        QVERIFY2(access.probe(), qPrintable(QString::fromStdWString(access.lastError())));
    }
};

QTEST_GUILESS_MAIN(ServiceTest)
#include "tst_service.moc"
