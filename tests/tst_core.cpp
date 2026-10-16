#include "AppCatalog.h"
#include "AppLogo.h"
#include "ChangeWatcher.h"
#include "ContentIndex.h"
#include "ContentIndexer.h"
#include "ContentScanner.h"
#include "Crawler.h"
#include "DocExtractor.h"
#include "Documents.h"
#include "DoubleClickDetector.h"
#include "MenuHotkey.h"
#include "DoubleTapDetector.h"
#include "Extract.h"
#include "GameGuard.h"
#include "FileIndex.h"
#include "FileText.h"
#include "History.h"
#include "IndexService.h"
#include "NameSearch.h"
#include "Ntfs.h"
#include "NtfsBulk.h"
#include "NtfsIndexer.h"
#include "PathText.h"
#include "Pinyin.h"
#include "Query.h"
#include "Release.h"
#include "SearchEngine.h"
#include "SearchTyping.h"
#include "SettingsMatch.h"
#include "ShareIndexer.h"
#include "ShareLister.h"
#include "Snapshot.h"
#include "SystemCatalog.h"
#include "TextUtil.h"
#include "Unc.h"
#include "VolumeProtocol.h"
#include "WebShortcut.h"
#include "WebSuggest.h"
#include "Win32Util.h"
#include "Writer.h"
#include "Wtf8.h"
#include "Xml.h"
#include "miniz.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

// Feeds `data` to the scanner `step` bytes per read call.
ContentScanner::ReadFn reader(QByteArray data, std::size_t step = 1u << 20)
{
    auto offset = std::make_shared<qsizetype>(0);
    return [data = std::move(data), offset, step](char* buffer, std::size_t capacity) -> std::size_t {
        const qsizetype n = std::min<qsizetype>(
            {static_cast<qsizetype>(capacity), static_cast<qsizetype>(step), data.size() - *offset});
        if (n <= 0)
            return 0;
        std::memcpy(buffer, data.constData() + *offset, static_cast<std::size_t>(n));
        *offset += n;
        return static_cast<std::size_t>(n);
    };
}

QByteArray encode(const QString& text, unsigned codePage)
{
    const auto* wide = reinterpret_cast<const wchar_t*>(text.utf16());
    const int n = ::WideCharToMultiByte(codePage, 0, wide, static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    QByteArray out(n, Qt::Uninitialized);
    ::WideCharToMultiByte(codePage, 0, wide, static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

QByteArray utf16le(const QString& text, bool bom)
{
    QByteArray out;
    if (bom)
        out.append("\xFF\xFE", 2);
    for (QChar c : text) {
        out.append(static_cast<char>(c.unicode() & 0xFF));
        out.append(static_cast<char>(c.unicode() >> 8));
    }
    return out;
}

template <typename T> void put(std::vector<std::byte>& data, std::size_t offset, T value)
{
    std::memcpy(data.data() + offset, &value, sizeof value);
}

// The grams of `bytes`, fed `chunk` bytes at a time.
std::vector<grams::Key> gramsOf(const QByteArray& bytes, TextEncoding encoding, std::size_t chunk = 1u << 20)
{
    grams::Collector collector(encoding, 936);
    for (qsizetype i = 0; i < bytes.size(); i += static_cast<qsizetype>(chunk))
        collector.feed(bytes.constData() + i, std::min<std::size_t>(chunk, static_cast<std::size_t>(bytes.size() - i)));
    return collector.finish();
}

std::vector<grams::Key> gramsOf(const QString& text)
{
    return gramsOf(text.toUtf8(), TextEncoding::Utf8);
}

bool hasAll(const std::vector<grams::Key>& keys, const QString& phrase)
{
    const std::vector<grams::Key> wanted = grams::ofPhrase(phrase);
    return !wanted.empty() && std::includes(keys.begin(), keys.end(), wanted.begin(), wanted.end());
}

// The entries a lookup says contain the phrase, and all it knows.
std::vector<EntryId> matchesOf(const ContentIndex& content, const QString& phrase)
{
    std::vector<EntryId> out;
    for (const std::uint32_t k : content.lookup(phrase).known) {
        if (k & ContentIndex::Lookup::kMatch)
            out.push_back(k & ~ContentIndex::Lookup::kMatch);
    }
    return out;
}

std::vector<EntryId> knownOf(const ContentIndex& content, const QString& phrase)
{
    std::vector<EntryId> out;
    for (const std::uint32_t k : content.lookup(phrase).known)
        out.push_back(k & ~ContentIndex::Lookup::kMatch);
    return out;
}

// ---- documents ------------------------------------------------------------------

// A zip archive (stored, or deflated) of these entries.
QByteArray makeZip(const std::vector<std::pair<std::string, std::string>>& entries)
{
    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return {};
    for (const auto& [name, data] : entries)
        mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_LEVEL);
    void* buffer = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
    QByteArray out(static_cast<const char*>(buffer), static_cast<qsizetype>(size));
    mz_zip_writer_end(&zip);
    return out;
}

// A compound file (version 3) with these streams in its root: those under
// 4096 bytes in the mini stream, as Office writes them.
QByteArray makeCfb(const std::vector<std::pair<std::u16string, QByteArray>>& streams)
{
    constexpr quint32 kEnd = 0xFFFFFFFE;
    constexpr quint32 kFree = 0xFFFFFFFF;
    constexpr quint32 kFatSector = 0xFFFFFFFD;
    std::vector<QByteArray> sectors;
    std::vector<quint32> fat;
    const auto chain = [&](const QByteArray& data) -> quint32 {
        if (data.isEmpty())
            return kEnd;
        const auto start = static_cast<quint32>(sectors.size());
        for (qsizetype at = 0; at < data.size(); at += 512) {
            QByteArray s = data.mid(at, 512);
            s.resize(512, '\0');
            sectors.push_back(s);
            fat.push_back(static_cast<quint32>(sectors.size()));
        }
        fat.back() = kEnd;
        return start;
    };
    const auto u32 = [](QByteArray& b, qsizetype at, quint32 v) { std::memcpy(b.data() + at, &v, 4); };

    QByteArray mini;
    std::vector<quint32> miniFat;
    std::vector<quint32> starts(streams.size(), kEnd);
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const QByteArray& data = streams[i].second;
        if (data.size() >= 4096 || data.isEmpty())
            continue;
        const auto first = static_cast<quint32>(mini.size() / 64);
        QByteArray padded = data;
        padded.resize((data.size() + 63) / 64 * 64, '\0');
        mini += padded;
        for (qsizetype k = 0; k < padded.size() / 64; ++k)
            miniFat.push_back(first + static_cast<quint32>(k) + 1);
        miniFat.back() = kEnd;
        starts[i] = first;
    }
    for (std::size_t i = 0; i < streams.size(); ++i) {
        if (streams[i].second.size() >= 4096)
            starts[i] = chain(streams[i].second);
    }
    const quint32 miniStart = chain(mini);
    QByteArray miniFatBytes(static_cast<qsizetype>(miniFat.size() * 4), '\0');
    if (!miniFat.empty())
        std::memcpy(miniFatBytes.data(), miniFat.data(), miniFatBytes.size());
    const quint32 miniFatStart = chain(miniFatBytes);

    QByteArray directory;
    const auto entry = [&](std::u16string_view name, quint8 type, quint32 child, quint32 right, quint32 start,
                           quint32 size) {
        QByteArray e(128, '\0');
        std::memcpy(e.data(), name.data(), name.size() * 2);
        const auto length = static_cast<quint16>((name.size() + 1) * 2);
        std::memcpy(e.data() + 0x40, &length, 2);
        e[0x42] = static_cast<char>(type);
        e[0x43] = 1;
        u32(e, 0x44, kFree);
        u32(e, 0x48, right);
        u32(e, 0x4C, child);
        u32(e, 0x74, start);
        u32(e, 0x78, size);
        directory += e;
    };
    entry(u"Root Entry", 5, streams.empty() ? kFree : 1, kFree, miniStart, static_cast<quint32>(mini.size()));
    for (std::size_t i = 0; i < streams.size(); ++i)
        entry(streams[i].first, 2, kFree, i + 1 < streams.size() ? static_cast<quint32>(i + 2) : kFree, starts[i],
            static_cast<quint32>(streams[i].second.size()));
    const quint32 directoryStart = chain(directory);

    const auto dataSectors = static_cast<quint32>(sectors.size());
    quint32 fatSectors = 1;
    while (dataSectors + fatSectors > fatSectors * 128)
        ++fatSectors;
    for (quint32 k = 0; k < fatSectors; ++k)
        fat.push_back(kFatSector);
    fat.resize(fatSectors * 128, kFree);

    QByteArray header(512, '\0');
    std::memcpy(header.data(), "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
    const quint16 fields[] = {0x3E, 3, 0xFFFE, 9, 6};
    std::memcpy(header.data() + 0x18, fields, sizeof fields);
    u32(header, 0x2C, fatSectors);
    u32(header, 0x30, directoryStart);
    u32(header, 0x38, 4096);
    u32(header, 0x3C, miniFatStart);
    u32(header, 0x40, static_cast<quint32>((miniFatBytes.size() + 511) / 512));
    u32(header, 0x44, kEnd);
    for (quint32 k = 0; k < 109; ++k)
        u32(header, 0x4C + 4 * k, k < fatSectors ? dataSectors + k : kFree);
    QByteArray out = header;
    for (const QByteArray& s : sectors)
        out += s;
    out.append(reinterpret_cast<const char*>(fat.data()), static_cast<qsizetype>(fat.size() * 4));
    return out;
}

// A PDF of pages, each a list of lines (Helvetica, so ASCII only), with a
// cross-reference table that points right.
QByteArray makePdf(const std::vector<std::vector<std::string>>& pages)
{
    std::vector<std::string> objects;
    const auto pageCount = pages.size();
    std::string kids;
    for (std::size_t p = 0; p < pageCount; ++p)
        kids += std::to_string(3 + 2 * p) + " 0 R ";
    const std::size_t font = 3 + 2 * pageCount;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageCount) + " >>");
    for (std::size_t p = 0; p < pageCount; ++p) {
        std::string content = "BT /F1 12 Tf 20 180 Td";
        for (const std::string& line : pages[p])
            content += " (" + line + ") Tj 0 -20 Td";
        content += " ET";
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] /Contents " + std::to_string(4 + 2 * p)
            + " 0 R /Resources << /Font << /F1 " + std::to_string(font) + " 0 R >> >> >>");
        objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream");
    }
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t o : offsets) {
        char line[24];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
        pdf += line;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n"
        + std::to_string(xref) + "\n%%EOF\n";
    return QByteArray::fromStdString(pdf);
}

QByteArray le16(std::initializer_list<quint16> values)
{
    QByteArray out;
    for (const quint16 v : values)
        out.append(reinterpret_cast<const char*>(&v), 2);
    return out;
}

QByteArray le32(quint32 v)
{
    return QByteArray(reinterpret_cast<const char*>(&v), 4);
}

QByteArray utf16(std::u16string_view s)
{
    return QByteArray(reinterpret_cast<const char*>(s.data()), static_cast<qsizetype>(s.size() * 2));
}

// One BIFF record: type, length, data.
QByteArray biff(quint16 type, const QByteArray& data)
{
    return le16({type, static_cast<quint16>(data.size())}) + data;
}

// A PowerPoint record (ver/instance, type, length) around `body`.
QByteArray pptRecord(quint16 verInstance, quint16 type, const QByteArray& body)
{
    return le16({verInstance, type}) + le32(static_cast<quint32>(body.size())) + body;
}

struct Extracted {
    extract::Status status;
    doctext::DocText text;
};

Extracted extractBytes(const QByteArray& bytes, std::size_t maxText = 16u << 20)
{
    Extracted out;
    extract::MemorySource source({bytes.constData(), static_cast<std::size_t>(bytes.size())});
    extract::Options options;
    options.maxText = maxText;
    options.codePage = 936;
    out.status = extract::extract(source, options, out.text);
    return out;
}

// The lines of a text, with each one's place: "page 2|text".
QStringList placedLines(const doctext::DocText& doc)
{
    QStringList out;
    const QString text = QString::fromUtf8(doc.text);
    const QStringList lines = text.split(u'\n', Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const doctext::Location where = doctext::locate(doc, static_cast<std::uint32_t>(i + 1));
        QString place;
        switch (where.kind) {
        case doctext::PlaceKind::None:
            place = u"-"_s;
            break;
        case doctext::PlaceKind::Page:
            place = u"page %1"_s.arg(where.number);
            break;
        case doctext::PlaceKind::Slide:
            place = u"slide %1"_s.arg(where.number);
            break;
        case doctext::PlaceKind::Row:
            place = QString::fromUtf8(where.sheet) + u"!%1"_s.arg(where.number);
            break;
        }
        out.append(place + u'|' + lines[i]);
    }
    return out;
}

// A 1 KB MFT file record as NTFS writes it, update sequence included.
class RecordBuilder {
public:
    explicit RecordBuilder(std::uint16_t flags, std::uint64_t baseRecord = 0, std::size_t size = 1024)
        : m_data(size)
        , m_strides(size / 512)
        , m_pos((0x30 + 2 * (m_strides + 1) + 7) & ~std::size_t {7})
    {
        put<std::uint32_t>(m_data, 0, 0x454C4946); // "FILE"
        put<std::uint16_t>(m_data, 0x04, 0x30); // update sequence array
        put<std::uint16_t>(m_data, 0x06, static_cast<std::uint16_t>(m_strides + 1)); // check value + one per 512 bytes
        put<std::uint16_t>(m_data, 0x10, 1); // sequence number
        put<std::uint16_t>(m_data, 0x14, static_cast<std::uint16_t>(m_pos)); // first attribute
        put<std::uint16_t>(m_data, 0x16, flags);
        put<std::uint64_t>(m_data, 0x20, baseRecord);
    }

    void standardInformation(std::uint32_t attributes)
    {
        std::vector<std::byte> value(0x48);
        put(value, 0x20, attributes);
        attribute(0x10, value);
    }

    void fileName(std::uint64_t parent, std::u16string_view name, std::uint8_t nameSpace)
    {
        std::vector<std::byte> value(0x42 + name.size() * 2);
        put(value, 0, parent);
        value[0x40] = static_cast<std::byte>(name.size());
        value[0x41] = static_cast<std::byte>(nameSpace);
        std::memcpy(value.data() + 0x42, name.data(), name.size() * 2);
        attribute(0x30, value);
    }

    // An unnamed non-resident attribute of `size` bytes: where they are, as a run list.
    void nonResident(std::uint32_t type, const std::vector<std::byte>& runs, std::uint64_t lastVcn, std::uint64_t size = 0,
        std::uint64_t firstVcn = 0)
    {
        const std::size_t length = (0x40 + runs.size() + 7) & ~std::size_t {7};
        put(m_data, m_pos, type);
        put(m_data, m_pos + 4, static_cast<std::uint32_t>(length));
        m_data[m_pos + 8] = std::byte {1};
        put<std::uint64_t>(m_data, m_pos + 0x10, firstVcn);
        put<std::uint64_t>(m_data, m_pos + 0x18, lastVcn);
        put<std::uint16_t>(m_data, m_pos + 0x20, 0x40);
        put<std::uint64_t>(m_data, m_pos + 0x28, size); // allocated
        put<std::uint64_t>(m_data, m_pos + 0x30, size); // data
        put<std::uint64_t>(m_data, m_pos + 0x38, size); // initialized
        std::memcpy(m_data.data() + m_pos + 0x40, runs.data(), runs.size());
        m_pos += length;
    }

    void resident(std::uint32_t type, const std::vector<std::byte>& value) { attribute(type, value); }

    // An unnamed non-resident attribute with its sizes and flags given apart
    // (a file's $DATA: the valid data length short of the size, compressed...).
    void nonResidentData(const std::vector<std::byte>& runs, std::uint64_t lastVcn, std::uint64_t allocated,
        std::uint64_t size, std::uint64_t initialized, std::uint16_t flags)
    {
        const std::size_t at = m_pos;
        nonResident(0x80, runs, lastVcn, allocated);
        put<std::uint16_t>(m_data, at + 0x0C, flags);
        put<std::uint64_t>(m_data, at + 0x30, size);
        put<std::uint64_t>(m_data, at + 0x38, initialized);
    }

    std::vector<std::byte> finish()
    {
        put<std::uint32_t>(m_data, m_pos, 0xFFFF'FFFF);
        put<std::uint32_t>(m_data, 0x18, static_cast<std::uint32_t>(m_pos + 8)); // bytes in use
        put<std::uint32_t>(m_data, 0x1C, static_cast<std::uint32_t>(m_data.size()));
        // Each 512-byte stride ends in the check value; the real bytes move to the array.
        put<std::uint16_t>(m_data, 0x30, 0x0042);
        for (std::size_t i = 1; i <= m_strides; ++i) {
            std::memcpy(m_data.data() + 0x30 + i * 2, m_data.data() + i * 512 - 2, 2);
            put<std::uint16_t>(m_data, i * 512 - 2, 0x0042);
        }
        return m_data;
    }

private:
    void attribute(std::uint32_t type, const std::vector<std::byte>& value)
    {
        const std::size_t length = (0x18 + value.size() + 7) & ~std::size_t {7};
        put(m_data, m_pos, type);
        put(m_data, m_pos + 4, static_cast<std::uint32_t>(length));
        put(m_data, m_pos + 0x10, static_cast<std::uint32_t>(value.size()));
        put<std::uint16_t>(m_data, m_pos + 0x14, 0x18);
        std::memcpy(m_data.data() + m_pos + 0x18, value.data(), value.size());
        m_pos += length;
    }

    std::vector<std::byte> m_data;
    std::size_t m_strides;
    std::size_t m_pos;
};

// The run list (mapping pairs) of extents, as NTFS writes it.
std::vector<std::byte> runList(const std::vector<ntfs::Extent>& extents, std::uint64_t clusterSize)
{
    const auto append = [](std::vector<std::byte>& out, std::uint64_t value, unsigned bytes) {
        for (unsigned b = 0; b < bytes; ++b)
            out.push_back(static_cast<std::byte>(value >> (8 * b)));
    };
    std::vector<std::byte> out;
    std::int64_t lcn = 0;
    for (const ntfs::Extent& e : extents) {
        const std::uint64_t length = e.length / clusterSize;
        const std::int64_t delta = static_cast<std::int64_t>(e.offset / clusterSize) - lcn;
        lcn += delta;
        unsigned lengthBytes = 1;
        while (length >> (8 * lengthBytes))
            ++lengthBytes;
        unsigned deltaBytes = 1; // signed: the top bit of the last byte is the sign
        while (deltaBytes < 8 && (delta < -(std::int64_t {1} << (8 * deltaBytes - 1))
                   || delta >= (std::int64_t {1} << (8 * deltaBytes - 1))))
            ++deltaBytes;
        out.push_back(static_cast<std::byte>(deltaBytes << 4 | lengthBytes));
        append(out, length, lengthBytes);
        append(out, static_cast<std::uint64_t>(delta), deltaBytes);
    }
    out.push_back(std::byte {0});
    return out;
}

// Appends a USN_RECORD (version 2, or another version to be skipped).
void appendUsn(std::vector<std::byte>& buffer, std::uint64_t file, std::uint64_t parent, std::int64_t usn,
    std::uint32_t reason, std::uint32_t attributes, std::u16string_view name, std::uint16_t major = 2)
{
    const std::size_t offset = buffer.size();
    const std::size_t length = (60 + name.size() * 2 + 7) & ~std::size_t {7};
    buffer.resize(offset + length);
    put(buffer, offset, static_cast<std::uint32_t>(length));
    put(buffer, offset + 4, major);
    put(buffer, offset + 8, file);
    put(buffer, offset + 16, parent);
    put(buffer, offset + 24, usn);
    put(buffer, offset + 40, reason);
    put(buffer, offset + 52, attributes);
    put(buffer, offset + 56, static_cast<std::uint16_t>(name.size() * 2));
    put(buffer, offset + 58, std::uint16_t {60});
    std::memcpy(buffer.data() + offset + 60, name.data(), name.size() * 2);
}

// The NTFS record number of a folder on disk.
std::uint32_t recordOfPath(const QString& path)
{
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const HANDLE h = ::CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BY_HANDLE_FILE_INFORMATION info {};
    const bool ok = h != INVALID_HANDLE_VALUE && ::GetFileInformationByHandle(h, &info);
    if (h != INVALID_HANDLE_VALUE)
        ::CloseHandle(h);
    return ok ? ntfs::recordOf((std::uint64_t {info.nFileIndexHigh} << 32) | info.nFileIndexLow) : 0;
}

// C:\Users\me\Documents\report.docx etc.
struct SampleTree {
    FileIndex index;
    EntryId root, users, me, docs, report, notes, windows;

    SampleTree()
    {
        root = index.addRoot("C:");
        users = index.add(root, "Users", EntryFlag::Directory);
        me = index.add(users, "me", EntryFlag::Directory);
        docs = index.add(me, "Documents", EntryFlag::Directory);
        report = index.add(docs, "Report.docx", 0);
        notes = index.add(docs, "notes.txt", 0);
        windows = index.add(root, "Windows", EntryFlag::Directory | EntryFlag::LowPriority);
    }
};

} // namespace

class CoreTest : public QObject {
    Q_OBJECT

private slots:
    void pathFromText()
    {
        using pathtext::pathFromText;
        QCOMPARE(pathFromText(u"C:\\Users\\me\\file.txt"_s), u"C:\\Users\\me\\file.txt"_s);
        QCOMPARE(pathFromText(u"  \"C:\\Program Files\\App\"  \r\n"_s), u"C:\\Program Files\\App"_s); // Copy as path
        QCOMPARE(pathFromText(u"\n\nD:/work/docs/\nsecond line"_s), u"D:\\work\\docs"_s);
        QCOMPARE(pathFromText(u"C:\\\\Users\\\\me\\\\"_s), u"C:\\Users\\me"_s); // doubled, as in source code
        QCOMPARE(pathFromText(u"e:"_s), u"e:\\"_s);
        QCOMPARE(pathFromText(u"C:\\"_s), u"C:\\"_s);
        QCOMPARE(pathFromText(u"file:///C:/Users/me/a%20b.txt"_s), u"C:\\Users\\me\\a b.txt"_s);
        QCOMPARE(pathFromText(u"\\\\server\\share\\folder\\"_s), u"\\\\server\\share\\folder"_s);
        QCOMPARE(pathFromText(u"%SystemRoot%\\System32"_s), qEnvironmentVariable("SystemRoot") + u"\\System32"_s);
        QCOMPARE(pathFromText(u"C:\\100%NOSUCHVARIABLE%\\x"_s), u"C:\\100%NOSUCHVARIABLE%\\x"_s);
        QVERIFY(pathFromText(u"Users\\me"_s).isEmpty()); // relative
        QVERIFY(pathFromText(u"\\\\server"_s).isEmpty()); // no share
        QVERIFY(pathFromText(u"C:\\a?b"_s).isEmpty());
        QVERIFY(pathFromText(u"C:\\a:b"_s).isEmpty());
        QVERIFY(pathFromText(u"https://example.com/a"_s).isEmpty());
        QVERIFY(pathFromText(u"hello world"_s).isEmpty());
        QVERIFY(pathFromText({}).isEmpty());
    }

    void filterExtensions()
    {
        using pathtext::filterExtensions;
        QCOMPARE(filterExtensions(u"Text files (*.txt;*.LOG)"_s), (QStringList {u"txt"_s, u"log"_s}));
        QCOMPARE(filterExtensions(u"图片 (*.png, *.jpg; *.png)"_s), (QStringList {u"png"_s, u"jpg"_s}));
        QCOMPARE(filterExtensions(u"*.tar.gz"_s), (QStringList {u"tar.gz"_s}));
        QVERIFY(filterExtensions(u"所有文件 (*.*)"_s).isEmpty());
        QVERIFY(filterExtensions(u"All files (*)"_s).isEmpty());
        QVERIFY(filterExtensions(u"Web pages (*.htm*)"_s).isEmpty());
        QVERIFY(filterExtensions(u"Word 文档"_s).isEmpty());
    }

    // The preview's text (filetext::decodeHead): the encoding as the content
    // search finds it, and a cut start ends on a whole line or character.
    void previewTextHead()
    {
        using filetext::decodeHead;
        using filetext::HeadEncoding;
        const auto bytes = [](std::initializer_list<int> list) {
            std::string s;
            for (const int b : list)
                s.push_back(static_cast<char>(b));
            return s;
        };
        // Whole file, UTF-8 with CRLF.
        filetext::Head h = decodeHead("one\r\ntwo\r\n", 10);
        QCOMPARE(h.status, filetext::Status::Ok);
        QCOMPARE(h.text, u"one\ntwo\n"_s);
        QCOMPARE(h.encoding, HeadEncoding::Utf8);
        QVERIFY(!h.cut);
        // UTF-8 with a BOM.
        h = decodeHead(bytes({0xEF, 0xBB, 0xBF, 'h', 'i'}), 5);
        QCOMPARE(h.text, u"hi"_s);
        QCOMPARE(h.encoding, HeadEncoding::Utf8Bom);
        // Cut: back to the last line end.
        h = decodeHead("line 1\nline 2 cut he", 1000);
        QVERIFY(h.cut);
        QCOMPARE(h.text, u"line 1\n"_s);
        // One long line cut inside a character (好 is E5 A5 BD): the whole ones only.
        h = decodeHead(bytes({'a', 0xE4, 0xBD, 0xA0, 0xE5, 0xA5}), 100);
        QCOMPARE(h.encoding, HeadEncoding::Utf8);
        QCOMPARE(h.text, u"a你"_s);
        // UTF-16 LE with a BOM, cut after a line.
        h = decodeHead(bytes({0xFF, 0xFE, 'x', 0, '\n', 0, 'y', 0}), 100);
        QCOMPARE(h.encoding, HeadEncoding::Utf16LE);
        QCOMPARE(h.text, u"x\n"_s);
        // A NUL early on: not text.
        QCOMPARE(decodeHead(bytes({'M', 'Z', 0, 0, 3}), 5).status, filetext::Status::NotText);
        // GBK: 你好 is C4 E3 BA C3; cut inside the second. On a Chinese
        // Windows as its ANSI code page, on a Western one because every
        // character beyond ASCII comes out Chinese.
        const unsigned system = ContentScanner::legacyCodePage();
        if (system == 936 || system == 1252) {
            h = decodeHead(bytes({0xC4, 0xE3, 0xBA, 0xC3}), 4);
            QCOMPARE(h.encoding, HeadEncoding::Ansi);
            QCOMPARE(h.codePage, 936u);
            QCOMPARE(h.text, u"你好"_s);
            h = decodeHead(bytes({0xC4, 0xE3, 0xBA}), 50);
            QCOMPARE(h.text, u"你"_s);
            QCOMPARE(h.codePage, 936u);
        }
        if (system == 1252) {
            // Western text stays Western: "café." (E9 then a full stop is no GBK pair).
            h = decodeHead(bytes({'c', 'a', 'f', 0xE9, '.'}), 5);
            QCOMPARE(h.codePage, 1252u);
            QCOMPARE(h.text, u"café."_s);
        }
        // From the disk (Windows paths, as the preview gives them): a folder, a missing file.
        QCOMPARE(filetext::readHead(QDir::toNativeSeparators(QDir::tempPath()).toStdWString(), 1024).status, filetext::Status::Folder);
        QCOMPARE(filetext::readHead(L"C:\\no\\such\\file.txt", 1024).status, filetext::Status::Missing);
    }

    void folderFromAddress()
    {
        using pathtext::folderFromAddress;
        QCOMPARE(folderFromAddress(u"地址: C:\\Windows\\System32"_s), u"C:\\Windows\\System32"_s);
        QCOMPARE(folderFromAddress(u"Address: D:\\"_s), u"D:\\"_s);
        QCOMPARE(folderFromAddress(u"Adresse : \\\\nas\\photos\\2024"_s), u"\\\\nas\\photos\\2024"_s);
        QVERIFY(folderFromAddress(u"地址: 此电脑"_s).isEmpty());
        QVERIFY(folderFromAddress(u"Address: Libraries\\Documents"_s).isEmpty());
        QVERIFY(pathtext::sameFolder(u"c:\\users\\ME\\"_s, u"C:\\Users\\me"_s));
        QVERIFY(!pathtext::sameFolder(u"C:\\Users"_s, u"C:\\Users\\me"_s));
        QVERIFY(!pathtext::sameFolder({}, {}));
    }

    // What Total Commander and Directory Opus say they show (filemanager::).
    void fileManagerText()
    {
        using pathtext::folderFromTotalCommander;
        QCOMPARE(folderFromTotalCommander(u"c:\\Windows\\*.*"_s), u"C:\\Windows"_s);
        QCOMPARE(folderFromTotalCommander(u"d:\\*.*"_s), u"D:\\"_s);
        QCOMPARE(folderFromTotalCommander(u"c:\\文档\\报告 2024\\*.txt"_s), u"C:\\文档\\报告 2024"_s);
        QCOMPARE(folderFromTotalCommander(u"\\\\nas\\photos\\*.*"_s), u"\\\\nas\\photos"_s);
        QCOMPARE(folderFromTotalCommander(u"c:\\Windows"_s), u"C:\\Windows"_s); // no filter shown
        QVERIFY(folderFromTotalCommander(u"\\\\\\Uninstaller64\\*.*"_s).isEmpty()); // a plugin's
        QVERIFY(folderFromTotalCommander(u"ftp://example.com/pub/*.*"_s).isEmpty());
        QVERIFY(folderFromTotalCommander({}).isEmpty());

        // As dopusrt /info writes it (UTF-8 with a BOM once a name is not ASCII).
        const QByteArray xml = QByteArray("\xEF\xBB\xBF")
            + QString::fromUtf16(uR"(<?xml version="1.0" encoding="UTF-8"?>
<results command="paths" result="1">
	<path active_lister="1" display_path="C:\" lister="0x6185e" side="1" tab="0x3185a">C:\</path>
	<path active_lister="1" active_tab="1" display_path="C:\用户\me\A &amp; B" lister="0x6185e" side="1" tab="0x518c0" tab_state="1">C:\Users\me\A &amp; B</path>
	<path active_lister="1" active_tab="2" display_path="D:\" lister="0x6185e" side="2" tab="0x1187a" tab_state="2">D:\</path>
	<path display_path="FTP" lister="0x7f00a" side="1" tab="0x2" tab_state="1">ftp://example.com/</path>
	<path display_path="Windows" lister="0x7f00a" side="1" tab="0x3">C:\Windows</path>
</results>
)").toUtf8();
        const QList<pathtext::OpusTab> tabs = pathtext::opusTabs(xml);
        QCOMPARE(tabs.size(), 4); // not the FTP site
        QCOMPARE(tabs[0].path, u"C:\\"_s);
        QCOMPARE(tabs[0].lister, quintptr(0x6185e));
        QCOMPARE(tabs[0].state, 0);
        QCOMPARE(tabs[1].path, u"C:\\Users\\me\\A & B"_s); // the real path, not the display one
        QCOMPARE(tabs[1].state, 1);
        QCOMPARE(tabs[2].state, 2);
        QCOMPARE(tabs[3].lister, quintptr(0x7f00a));
        QVERIFY(pathtext::opusTabs("<results command=\"paths\" result=\"0\"/>").isEmpty());
        QVERIFY(pathtext::opusTabs("not xml").isEmpty());
    }

    // What XYplorer answers (filemanager::), and what goes into its scripts.
    void xyplorerText()
    {
        using pathtext::xyplorerTabs;
        // Both panes shown: the active pane's tab on top (state 1), the
        // other pane's (2), the rest (0). pathreal() gives "C:" for a drive,
        // special places stay as they are ("此电脑"), a quick search keeps
        // its "?pattern".
        QList<pathtext::XYplorerTab> tabs = xyplorerTabs(
            u"1\r\n|F:\\wsfmtest\\子文件夹 甲|此电脑|c:|C:\\Users\\me?*.bmp\r\n|D:\\Work|\\\\nas\\photos\\2024|"_s);
        QCOMPARE(tabs.size(), 5);
        QCOMPARE(tabs[0].path, u"F:\\wsfmtest\\子文件夹 甲"_s);
        QCOMPARE(tabs[0].state, 1);
        QCOMPARE(tabs[1].path, u"C:\\"_s); // the drive, as a folder
        QCOMPARE(tabs[1].state, 0);
        QCOMPARE(tabs[2].path, u"C:\\Users\\me"_s); // the folder searched
        QCOMPARE(tabs[3].path, u"D:\\Work"_s);
        QCOMPARE(tabs[3].state, 2);
        QCOMPARE(tabs[4].path, u"\\\\nas\\photos\\2024"_s);
        QCOMPARE(tabs[4].state, 0);
        // One pane shown: the other's tabs are not on view.
        tabs = xyplorerTabs(u"0\n|此电脑|C:\\Windows\n|D:\\|E:\\"_s);
        QCOMPARE(tabs.size(), 1);
        QCOMPARE(tabs[0].path, u"C:\\Windows"_s);
        QCOMPARE(tabs[0].state, 1);
        QVERIFY(xyplorerTabs(u"1\n|此电脑\n|"_s).isEmpty());
        QVERIFY(xyplorerTabs({}).isEmpty());
        QVERIFY(xyplorerTabs(u"garbage"_s).isEmpty());

        QCOMPARE(pathtext::xyplorerQuoted(u"C:\\Users\\me"_s), u"'C:\\Users\\me'"_s);
        QCOMPARE(pathtext::xyplorerQuoted(u"D:\\John's 100%"_s), u"'D:\\John''s 100%'"_s);
        QCOMPARE(pathtext::xyplorerQuoted({}), u"''"_s);
    }

    // A command of the user's own for another file manager, with {path}.
    void customFileManagerCommand()
    {
        using pathtext::commandArgument;
        using pathtext::customCommand;
        QCOMPARE(commandArgument(u"C:\\Tools\\x.exe"_s), u"C:\\Tools\\x.exe"_s);
        QCOMPARE(commandArgument(u"C:\\"_s), u"C:\\"_s); // no quotes: nothing to escape
        QCOMPARE(commandArgument(u"C:\\Program Files"_s), u"\"C:\\Program Files\""_s);
        QCOMPARE(commandArgument(u"D:\\A B\\"_s), u"\"D:\\A B\\\\\""_s); // \" would be a quote
        QCOMPARE(commandArgument(u"F:\\wsfmtest\\子文件夹 甲"_s), u"\"F:\\wsfmtest\\子文件夹 甲\""_s);
        QCOMPARE(commandArgument({}), u"\"\""_s);

        pathtext::CustomCommand c = customCommand(u"\"C:\\Tools\\My Manager\\fm.exe\" --open {path}"_s, u"D:\\A B"_s);
        QCOMPARE(c.program, u"C:\\Tools\\My Manager\\fm.exe"_s);
        QCOMPARE(c.arguments, u"--open \"D:\\A B\""_s);
        // Quoted by the user: not twice. Twice in the line: both.
        c = customCommand(u"fm.exe \"{path}\" /x={path}"_s, u"D:\\A B"_s);
        QCOMPARE(c.program, u"fm.exe"_s);
        QCOMPARE(c.arguments, u"\"D:\\A B\" /x=\"D:\\A B\""_s);
        // No {path}: the path goes last.
        c = customCommand(u"  C:\\fm.exe  "_s, u"C:\\"_s);
        QCOMPARE(c.program, u"C:\\fm.exe"_s);
        QCOMPARE(c.arguments, u"C:\\"_s);
        c = customCommand(u"C:\\fm.exe /n"_s, u"C:\\x y"_s);
        QCOMPARE(c.arguments, u"/n \"C:\\x y\""_s);
        // %VARIABLES% in the line, not in the path.
        qputenv("WS_TEST_FM", "C:\\Tools");
        c = customCommand(u"\"%WS_TEST_FM%\\fm.exe\" {path}"_s, u"%TEMP%"_s);
        QCOMPARE(c.program, u"C:\\Tools\\fm.exe"_s);
        QCOMPARE(c.arguments, u"%TEMP%"_s);
        qunsetenv("WS_TEST_FM");
        QVERIFY(customCommand({}, u"C:\\"_s).program.isEmpty());
        QVERIFY(customCommand(u"\"unclosed"_s, u"C:\\"_s).program.isEmpty());
        QVERIFY(customCommand(u"\"\" {path}"_s, u"C:\\"_s).program.isEmpty());
    }

    void opusBlankClick()
    {
        using pathtext::opusBlankClickEvent;
        using pathtext::withOpusBlankClick;
        // As Opus 13 writes prefs.oxc; another <bg_commands> outside <dblclk> stays out of it.
        const QByteArray before = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<prefs version=\"6\">\n"
                                  "\t<other><bg_commands><event command=\"x\" enable=\"yes\" id=\"0\" /></bg_commands></other>\n"
                                  "\t<dblclk flags=\"0x405\">\n\t\t<layout />\n\t\t<text />\n"
                                  "\t\t<bg_commands single_middle_click=\"no\">\n"
                                  "\t\t\t<event command=\"Go UP\" enable=\"yes\" id=\"1\" />\n"
                                  "\t\t\t<event command=\"Go UP BACK\" enable=\"no\" id=\"0\" />\n"
                                  "\t\t</bg_commands>\n\t\t<mouse_hook_delay>0</mouse_hook_delay>\n"
                                  "\t</dblclk>\n\t<名字>文档</名字>\n</prefs>\n";
        pathtext::OpusBlankClick click = pathtext::opusBlankClick(before);
        QVERIFY(click.hasPlace);
        QCOMPARE(click.element, "<event command=\"Go UP BACK\" enable=\"no\" id=\"0\" />");
        QCOMPARE(click.command, u"Go UP BACK"_s);
        QVERIFY(!click.enabled);

        // Quotes, & and > in the command; only the element changes.
        const QString ours = uR"("C:\A & B\WinShunSearch.exe" --folder-menu "{sourcepath$|noterm}" >x)"_s;
        const QByteArray event = opusBlankClickEvent(ours);
        QCOMPARE(event, R"(<event command="&quot;C:\A &amp; B\WinShunSearch.exe&quot; --folder-menu &quot;{sourcepath$|noterm}&quot; &gt;x" enable="yes" id="0" />)");
        const QByteArray set = withOpusBlankClick(before, event);
        QCOMPARE(QByteArray(set).replace(event, click.element), before);
        click = pathtext::opusBlankClick(set);
        QCOMPARE(click.command, ours);
        QVERIFY(click.enabled);
        QCOMPARE(withOpusBlankClick(set, "<event command=\"Go UP BACK\" enable=\"no\" id=\"0\" />"), before);
        // Taken away with its line; a later read finds none.
        const QByteArray removed = withOpusBlankClick(set, {});
        QCOMPARE(QByteArray(removed).replace("\t\t\t<event command=\"Go UP\" enable=\"yes\" id=\"1\" />\n",
                     "\t\t\t<event command=\"Go UP\" enable=\"yes\" id=\"1\" />\n\t\t\t" + click.element + '\n'),
            set);
        QVERIFY(pathtext::opusBlankClick(removed).element.isEmpty());
        QVERIFY(pathtext::opusBlankClick(removed).hasPlace);

        // No <bg_commands> yet: one before <mouse_hook_delay>; a <bg_commands/> opened up.
        const QByteArray bare = "<prefs><dblclk flags=\"0x405\"><text /><mouse_hook_delay>0</mouse_hook_delay></dblclk></prefs>";
        QCOMPARE(withOpusBlankClick(bare, event),
            "<prefs><dblclk flags=\"0x405\"><text /><bg_commands>" + event
                + "</bg_commands><mouse_hook_delay>0</mouse_hook_delay></dblclk></prefs>");
        QCOMPARE(pathtext::opusBlankClick(withOpusBlankClick(bare, event)).command, ours);
        QCOMPARE(withOpusBlankClick("<prefs><dblclk><bg_commands single_middle_click=\"no\"/></dblclk></prefs>", event),
            "<prefs><dblclk><bg_commands single_middle_click=\"no\">" + event + "</bg_commands></dblclk></prefs>");
        QCOMPARE(withOpusBlankClick("<prefs><dblclk></dblclk></prefs>", event),
            "<prefs><dblclk><bg_commands>" + event + "</bg_commands></dblclk></prefs>");
        QCOMPARE(withOpusBlankClick(bare, {}), bare); // nothing to take away

        // No place for it: no <dblclk>, an empty one, not XML.
        QVERIFY(withOpusBlankClick("<prefs><dblclkx></dblclkx></prefs>", event).isEmpty());
        QVERIFY(withOpusBlankClick("<prefs><dblclk /></prefs>", event).isEmpty());
        QVERIFY(withOpusBlankClick("<prefs><dblclk>", event).isEmpty());
        QVERIFY(!pathtext::opusBlankClick("<prefs/>").hasPlace);
        QVERIFY(withOpusBlankClick({}, event).isEmpty());
    }

    void splitTyped()
    {
        const auto split = [](const QString& text) {
            const pathtext::TypedPath typed = pathtext::splitTyped(text);
            return QStringList {typed.folder, typed.name};
        };
        QCOMPARE(split(u"D:\\Pro"_s), (QStringList {u"D:\\"_s, u"Pro"_s}));
        QCOMPARE(split(u"D:\\Projects\\"_s), (QStringList {u"D:\\Projects"_s, QString()}));
        QCOMPARE(split(u"D:/work/docs"_s), (QStringList {u"D:\\work"_s, u"docs"_s}));
        QCOMPARE(split(u"d:"_s), (QStringList {u"d:\\"_s, QString()}));
        QCOMPARE(split(u"\"C:\\Program Files\\Wi\""_s), (QStringList {u"C:\\Program Files"_s, u"Wi"_s}));
        QCOMPARE(split(u"\\\\server\\share\\"_s), (QStringList {u"\\\\server\\share"_s, QString()}));
        QCOMPARE(split(u"%SystemRoot%\\Sys"_s), (QStringList {qEnvironmentVariable("SystemRoot"), u"Sys"_s}));
        QVERIFY(pathtext::splitTyped(u"\\\\server\\sha"_s).folder.isEmpty()); // no share yet
        QVERIFY(pathtext::splitTyped(u"D:\\*.pdf"_s).folder.isEmpty()); // for the search
        QVERIFY(pathtext::splitTyped(u"report"_s).folder.isEmpty());
        QVERIFY(pathtext::splitTyped(u"docs\\report"_s).folder.isEmpty()); // a folder name in a search
    }

    void splitRelative()
    {
        const auto names = [](const QString& text) { return pathtext::splitRelative(text).names; };
        QCOMPARE(names(u"tools/screenshots/work/out/covers/"_s),
            (QStringList {u"tools"_s, u"screenshots"_s, u"work"_s, u"out"_s, u"covers"_s}));
        QVERIFY(pathtext::splitRelative(u"tools/out/"_s).folder);
        QCOMPARE(names(u" src\\app\\App.cpp "_s), (QStringList {u"src"_s, u"app"_s, u"App.cpp"_s}));
        QVERIFY(!pathtext::splitRelative(u"src\\app\\App.cpp"_s).folder);
        QCOMPARE(names(u"/Tencent Files//x\\"_s), (QStringList {u"Tencent Files"_s, u"x"_s}));
        QCOMPARE(names(u"covers/"_s), QStringList {u"covers"_s});
        QVERIFY(names(u"covers"_s).isEmpty()); // a name, not a path
        QVERIFY(names(u"D:\\work\\docs"_s).isEmpty()); // splitTyped's
        QVERIFY(names(u"\\\\server\\share"_s).isEmpty());
        QVERIFY(names(u"src/*.cpp"_s).isEmpty());
        QVERIFY(names(u"!node_modules\\"_s).isEmpty());
        QVERIFY(names(u"covers/ png"_s).isEmpty()); // two terms
        QVERIFY(names(u"../out"_s).isEmpty());
        QVERIFY(names(u"/"_s).isEmpty());
    }

    void listTyped()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString root = QDir::toNativeSeparators(tmp.path());
        for (const QString& name : {u"Alpha"_s, u"alpha10"_s, u"alpha2"_s, u"Beta"_s, u"项目资料"_s, u"secret"_s})
            QVERIFY(QDir(root).mkdir(name));
        for (const QString& name : {u"alpha.txt"_s, u"notes.md"_s}) {
            QFile file(root + u'\\' + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QVERIFY(::SetFileAttributesW(reinterpret_cast<const wchar_t*>((root + u"\\secret"_s).utf16()), FILE_ATTRIBUTE_HIDDEN));
        const auto names = [](const SearchResults& rows) {
            QStringList out;
            for (const SearchResult& r : rows)
                out.append(r.name + (r.isDir ? u"\\"_s : QString()));
            return out;
        };
        const QString self = QDir(root).dirName() + u'\\';

        // Nothing typed: the folder itself, its folders, its files; no hidden ones.
        const SearchResults all = pathtext::listTyped({root, {}}, false, 50);
        QCOMPARE(names(all).first(5), (QStringList {self, u"Alpha\\"_s, u"alpha2\\"_s, u"alpha10\\"_s, u"Beta\\"_s}));
        QCOMPARE(names(all).last(2), (QStringList {u"alpha.txt"_s, u"notes.md"_s}));
        QCOMPARE(all.size(), 8);
        QCOMPARE(all[0].path, root);
        QCOMPARE(all[1].path, root + u"\\Alpha"_s);

        QCOMPARE(names(pathtext::listTyped({root, u"alp"_s}, false, 50)),
            (QStringList {u"Alpha\\"_s, u"alpha2\\"_s, u"alpha10\\"_s, u"alpha.txt"_s}));
        QCOMPARE(names(pathtext::listTyped({root, u"alp"_s}, true, 50)),
            (QStringList {u"Alpha\\"_s, u"alpha2\\"_s, u"alpha10\\"_s}));
        QCOMPARE(names(pathtext::listTyped({root, u"xm"_s}, false, 50)), (QStringList {u"项目资料\\"_s})); // pinyin
        QCOMPARE(names(pathtext::listTyped({root, {}}, false, 2)), (QStringList {self, u"Alpha\\"_s}));
        QVERIFY(pathtext::listTyped({root + u"\\nothing here"_s, {}}, false, 50).isEmpty());
    }

    void webKeyword()
    {
        const WebShortcuts shortcuts {
            {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s"_s, {}},
            {u"blog"_s, u"Blog"_s, u"https://example.com/blog"_s, {}}, // a page: takes no words
        };
        const auto typed = [&](const QString& text) {
            const auto t = typedKeyword(shortcuts, text);
            return t ? QStringList {QString::number(t->index), t->words} : QStringList();
        };
        QCOMPARE(typed(u"gh"_s), (QStringList {u"0"_s, QString()}));
        QCOMPARE(typed(u"  GH  "_s), (QStringList {u"0"_s, QString()})); // any case, spaces around
        QCOMPARE(typed(u"gh  Win Shun "_s), (QStringList {u"0"_s, u"Win Shun"_s}));
        QCOMPARE(typed(u"gh \"exact words\""_s), (QStringList {u"0"_s, u"\"exact words\""_s})); // as typed
        QCOMPARE(typed(u"blog"_s), (QStringList {u"1"_s, QString()}));
        QVERIFY(typed(u"blog notes"_s).isEmpty()); // an ordinary query then
        QVERIFY(typed(u"ghost"_s).isEmpty());
        QVERIFY(typed(u"g"_s).isEmpty());
        QVERIFY(typed(u"report gh"_s).isEmpty());
        QVERIFY(typed({}).isEmpty());
        QVERIFY(!typedKeyword({}, u"gh"_s));
    }

    void webAddresses()
    {
        const WebShortcut github {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s&type=code"_s, {}};
        QVERIFY(github.searches());
        QCOMPARE(github.searchUrl(u"Win Shun/中文&x"_s),
            u"https://github.com/search?q=Win%20Shun%2F%E4%B8%AD%E6%96%87%26x&type=code"_s);
        QCOMPARE(github.homeUrl(), u"https://github.com/"_s); // its site
        const WebShortcut bilibili {u"bili"_s, u"哔哩哔哩"_s, u"https://search.bilibili.com/all?keyword=%s"_s,
            u"https://www.bilibili.com/"_s};
        QCOMPARE(bilibili.homeUrl(), u"https://www.bilibili.com/"_s);
        const WebShortcut page {u"blog"_s, u"Blog"_s, u"https://example.com/blog"_s, {}};
        QVERIFY(!page.searches());
        QCOMPARE(page.homeUrl(), u"https://example.com/blog"_s);
        const WebShortcut app {u"ob"_s, u"Obsidian"_s, u"obsidian://search?query=%s"_s, {}};
        QCOMPARE(app.homeUrl(), u"obsidian://search?query="_s); // an app's link, with no words

        QCOMPARE(normalizeWebUrl(u"  github.com/search?q=%s "_s), u"https://github.com/search?q=%s"_s);
        // Searched for "%s" itself, then copied from the address bar.
        QCOMPARE(normalizeWebUrl(u"https://www.baidu.com/s?wd=%25s&rsv_spt=1"_s), u"https://www.baidu.com/s?wd=%s&rsv_spt=1"_s);
        QCOMPARE(normalizeWebUrl(u"obsidian://search?query=%s"_s), u"obsidian://search?query=%s"_s);
        QCOMPARE(normalizeWebUrl(u"HTTP://Example.com"_s), u"HTTP://Example.com"_s);
        QCOMPARE(normalizeWebUrl(u"wiki.example.com:8080/find?q=%s"_s), u"https://wiki.example.com:8080/find?q=%s"_s);
        QVERIFY(normalizeWebUrl(u"file:///C:/notes/%s"_s).isEmpty()); // could start a program
        QVERIFY(normalizeWebUrl(u"C:\\Windows\\%s"_s).isEmpty());
        QVERIFY(normalizeWebUrl(u"https://"_s).isEmpty());
        QVERIFY(normalizeWebUrl(u"   "_s).isEmpty());

        QCOMPARE(displayWebUrl(u"https://www.baidu.com/s?wd=%E5%A4%A9%E6%B0%94%20%E9%A2%84%E6%8A%A5"_s),
            u"baidu.com/s?wd=天气 预报"_s);
        QCOMPARE(displayWebUrl(u"https://github.com/"_s), u"github.com"_s);
        QCOMPARE(displayWebUrl(u"https://github.com/search?q=%s&type=code"_s), u"github.com/search?q=%s&type=code"_s);
        QCOMPARE(displayWebUrl(u"obsidian://search?query=a%20b"_s), u"obsidian://search?query=a b"_s);

        QVERIFY(isValidWebKeyword(u"gh"_s));
        QVERIFY(isValidWebKeyword(u"知乎"_s));
        QVERIFY(!isValidWebKeyword(u"g h"_s));
        QVERIFY(!isValidWebKeyword(u"g\"h"_s));
        QVERIFY(!isValidWebKeyword(QString()));

        QVERIFY(isWebPath(webPath(u"gh"_s, u"WinShun"_s)));
        QVERIFY(webPath(u"gh"_s) != webPath(u"gh"_s, u"WinShun"_s)); // rows of their own
        QVERIFY(!isWebPath(u"C:\\gh"_s));
    }

    void webByName()
    {
        const WebShortcuts shortcuts {
            {u"bd"_s, u"百度"_s, u"https://www.baidu.com/s?wd=%s"_s, {}},
            {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s"_s, {}},
            {u"zh"_s, u"知乎"_s, u"https://www.zhihu.com/search?q=%s"_s, {}},
        };
        const auto found = [&](const QString& text) {
            QStringList names;
            for (const WebHit& hit : searchWebShortcuts(shortcuts, NameMatcher(ws::parseQuery(text))))
                names.append(shortcuts[hit.index].name);
            return names;
        };
        QCOMPARE(found(u"github"_s), (QStringList {u"GitHub"_s}));
        QCOMPARE(found(u"baidu"_s), (QStringList {u"百度"_s})); // pinyin
        QCOMPARE(found(u"bd"_s), (QStringList {u"百度"_s})); // initials
        QVERIFY(found(u"!github"_s).isEmpty()); // exclusions alone list nothing
        QVERIFY(found(u"report"_s).isEmpty());

    }

    void webDefault()
    {
        // Ten sites most used on the Chinese web, and "winshun" for the repository.
        const WebShortcuts shortcuts = defaultWebShortcuts();
        QCOMPARE(shortcuts.size(), 11);
        QStringList keywords;
        QStringList sites;
        for (const WebShortcut& s : shortcuts) {
            QCOMPARE(normalizeWebUrl(s.url), s.url); // as the settings window would keep them
            QVERIFY(isValidWebKeyword(s.keyword));
            // Whole words: two letters typed alone would put a site above the files.
            QVERIFY2(s.keyword.size() >= 3, qPrintable(s.keyword));
            // Not the winshun.web plugin's: its keyword mode would win.
            QVERIFY(!QStringList({u"bd"_s, u"gh"_s, u"yt"_s, u"wiki"_s, u"bing"_s, u"bili"_s}).contains(s.keyword));
            QVERIFY(!keywords.contains(s.keyword));
            keywords.append(s.keyword);
            if (s.searches()) {
                QVERIFY2(!s.home.isEmpty(), qPrintable(s.keyword)); // the site, for the keyword alone and its icon
                QVERIFY(!sites.contains(siteIconKey(s.homeUrl()))); // an icon each
                sites.append(siteIconKey(s.homeUrl()));
            }
        }
        QCOMPARE(sites.size(), 10);
        // Suggestions where the site answers a plain request (checked live 2026-10-15), none elsewhere.
        QStringList suggesting;
        for (const WebShortcut& s : shortcuts) {
            QCOMPARE(s.suggest, knownSuggestUrl(s.url));
            if (!s.suggest.isEmpty()) {
                QCOMPARE(normalizeSuggestUrl(s.suggest), s.suggest);
                suggesting.append(s.keyword);
            }
        }
        QCOMPARE(suggesting, (QStringList {u"baidu"_s, u"biying"_s, u"bilibili"_s, u"taobao"_s}));
        const auto typed = typedKeyword(shortcuts, u"baidu 天气 预报"_s);
        QVERIFY(typed);
        QCOMPARE(shortcuts[typed->index].searchUrl(typed->words), u"https://www.baidu.com/s?wd=%E5%A4%A9%E6%B0%94%20%E9%A2%84%E6%8A%A5"_s);
        QCOMPARE(shortcuts[typed->index].suggestUrl(typed->words),
            u"https://suggestion.baidu.com/su?wd=%E5%A4%A9%E6%B0%94%20%E9%A2%84%E6%8A%A5&action=opensearch"_s);
        QVERIFY(!typedKeyword(shortcuts, u"bd"_s)); // found by its name instead (pinyin initials)
        const auto byName = searchWebShortcuts(shortcuts, NameMatcher(ws::parseQuery(u"jd"_s)));
        QVERIFY(!byName.empty());
        QCOMPARE(shortcuts[byName.front().index].name, u"京东"_s);

        const auto winshun = typedKeyword(shortcuts, u"WinSHUN"_s);
        QVERIFY(winshun);
        QCOMPARE(shortcuts[winshun->index].homeUrl(), u"https://github.com/LingCore/WinShun"_s);
        QVERIFY(winshun->words.isEmpty());
        QVERIFY(!typedKeyword(shortcuts, u"winshun 设置"_s)); // a page: an ordinary query then
    }

    void webNewDefaults()
    {
        const WebShortcuts defaults = defaultWebShortcuts();
        // Written by 0.6.0: "winshun" alone. The ten come after it.
        const WebShortcuts old {defaults.last()};
        WebShortcuts merged = withNewDefaults(old, defaults);
        QCOMPARE(merged.size(), 11);
        QCOMPARE(merged.first().keyword, u"winshun"_s);
        // The user's own: a keyword taken (theirs stays), a site already there
        // under another keyword ("bd" for 百度), nothing twice.
        const WebShortcuts own {
            {u"zhihu"_s, u"我的知乎"_s, u"https://www.zhihu.com/search?q=%s"_s, {}},
            {u"bd"_s, u"百度"_s, u"https://www.baidu.com/s?wd=%s"_s, u"https://www.baidu.com/"_s},
        };
        merged = withNewDefaults(own, defaults);
        QCOMPARE(merged.size(), 2 + 11 - 2);
        QCOMPARE(merged[0].name, u"我的知乎"_s);
        QCOMPARE(std::count_if(merged.begin(), merged.end(), [](const WebShortcut& s) { return s.name == u"百度"; }),
            std::ptrdiff_t(1));
        QCOMPARE(withNewDefaults(merged, defaults), merged); // once is all
    }

    void webFile()
    {
        QVERIFY(isValidCustomIconName(u"0123456789abcdef.png"));
        QVERIFY(!isValidCustomIconName(u"0123456789ABCDEF.png")); // as Win顺 names them: lower case
        QVERIFY(!isValidCustomIconName(u"..\\..\\evil1234.png"));
        QVERIFY(!isValidCustomIconName(u"C:\\icons\\gh.png"));
        QVERIFY(!isValidCustomIconName(u""));

        WebShortcuts shortcuts {
            {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s"_s, {}, u"0123456789abcdef.png"_s,
                u"https://example.com/sug?q=%s"_s},
            {u"zhihu"_s, u"知乎"_s, u"https://www.zhihu.com/search?q=%s"_s, u"https://www.zhihu.com/"_s, {}},
            {u"gone"_s, u"图标没了"_s, u"https://example.com/"_s, {}, u"fedcba9876543210.png"_s},
        };
        const QByteArray png("\x89PNG fake", 9);
        const QByteArray json = writeWebShortcutsFile(shortcuts, [&](const QString& icon) {
            return icon == u"0123456789abcdef.png" ? png : QByteArray(); // the other's file is gone
        });
        const std::optional<WebShortcutFile> read = readWebShortcutsFile(json);
        QVERIFY(read);
        QCOMPARE(read->skipped, 0);
        QCOMPARE(read->entries.size(), 3);
        QCOMPARE(read->entries[0].iconData, png);
        QVERIFY(read->entries[0].shortcut.icon.isEmpty()); // a name of this computer's: the picture travels instead
        QCOMPARE(read->entries[1].shortcut.home, u"https://www.zhihu.com/"_s);
        QCOMPARE(read->entries[1].shortcut.name, u"知乎"_s);
        QVERIFY(read->entries[2].iconData.isEmpty());
        QCOMPARE(read->entries[0].shortcut.suggest, u"https://example.com/sug?q=%s"_s);
        QVERIFY(read->entries[1].shortcut.suggest.isEmpty()); // written empty: none, not the one known for the site

        // Tidied as the settings file is; the unusable skipped; the later of two keywords wins.
        // Not a raw string: moc takes the // of its addresses for a comment and loses the class (pitfalls).
        const QByteArray hand = "{\"format\": \"winshun-web-shortcuts\", \"version\": 1, \"shortcuts\": ["
                                "{\"keyword\": \"bd\", \"url\": \"baidu.com/s?wd=%s\"},"
                                "{\"keyword\": \"two words\", \"url\": \"https://example.com/\"},"
                                "{\"keyword\": \"x\", \"url\": \"file:///C:/Windows/\"},"
                                "{\"keyword\": \"BD\", \"name\": \"百度\", \"url\": \"https://www.baidu.com/s?wd=%s\","
                                " \"icon\": \"data:image/png;base64,iVBORw==\"},"
                                "{\"keyword\": \"y\", \"url\": \"https://y.example/\", \"icon\": \"C:/Windows/a.png\"}"
                                "]}";
        const std::optional<WebShortcutFile> tidied = readWebShortcutsFile(hand);
        QVERIFY(tidied);
        QCOMPARE(tidied->skipped, 2);
        QCOMPARE(tidied->entries.size(), 2);
        QCOMPARE(tidied->entries[0].shortcut.keyword, u"BD"_s);
        QCOMPARE(tidied->entries[0].shortcut.name, u"百度"_s);
        QCOMPARE(tidied->entries[0].iconData, QByteArray::fromBase64("iVBORw=="));
        QVERIFY(tidied->entries[1].iconData.isEmpty()); // a path is no picture
        QCOMPARE(tidied->entries[1].shortcut.name, u"y"_s);
        // A file from before suggestions: the one known for the site, if any.
        QCOMPARE(tidied->entries[0].shortcut.suggest, knownSuggestUrl(u"https://www.baidu.com/s?wd=%s"_s));
        QVERIFY(!tidied->entries[0].shortcut.suggest.isEmpty());
        QVERIFY(tidied->entries[1].shortcut.suggest.isEmpty());

        bool newer = false;
        QVERIFY(!readWebShortcutsFile("{}", &newer));
        QVERIFY(!newer);
        QVERIFY(!readWebShortcutsFile("not json"));
        QVERIFY(!readWebShortcutsFile("{\"format\": \"winshun-web-shortcuts\", \"version\": 2, \"shortcuts\": []}", &newer));
        QVERIFY(newer);
    }

    void webMerge()
    {
        WebShortcuts own {
            {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s"_s, {}, {}},
            {u"zhihu"_s, u"知乎"_s, u"https://www.zhihu.com/search?q=%s"_s, {}, {}},
        };
        const WebShortcuts file {
            {u"ZHIHU"_s, u"知乎"_s, u"https://www.zhihu.com/search?type=content&q=%s"_s, {}, {}},
            {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s"_s, {}, {}},
            {u"bd"_s, u"百度"_s, u"https://www.baidu.com/s?wd=%s"_s, {}, {}},
        };
        const WebMerge merge = mergeWebShortcuts(own, file);
        QCOMPARE(merge.added, 1);
        QCOMPARE(merge.updated, 1);
        QCOMPARE(merge.unchanged, 1);
        QCOMPARE(own.size(), 3);
        QCOMPARE(own[1].keyword, u"ZHIHU"_s); // in its place
        QCOMPARE(own[2].keyword, u"bd"_s);
        const WebShortcuts again = own;
        QCOMPARE(mergeWebShortcuts(own, file).unchanged, 3);
        QCOMPARE(own, again);
    }

    void siteIcons()
    {
        QCOMPARE(siteIconKey(u"https://Fanyi.Baidu.com/"_s), u"fanyi.baidu.com"_s);
        QCOMPARE(siteIconKey(u"http://example.com:8080/x"_s), u"example.com"_s);
        QVERIFY(siteIconKey(u"obsidian://search?query="_s).isEmpty());

        // As www.bilibili.com and www.zhihu.com have them (2026-10), and the
        // kinds left out: one colour, other relations, attributes alike.
        // (Not a raw string literal: moc reads this file and stumbles on one.)
        const QByteArray html = "<html><head>\n"
                                "<link rel=\"stylesheet\" href=\"/a.css\">\n"
                                "<link rel=\"shortcut icon\" href=\"//i0.hdslb.com/images/favicon.ico\" />\n"
                                "<link rel=\"mask-icon\" href=\"/pinned.svg\" color=\"#000\">\n"
                                "<link rel=\"icon\" type=\"image/svg+xml\" href=\"/favicon.svg\">\n"
                                "<LINK REL='apple-touch-icon' HREF='https://static.zhihu.com/touch-120.png' sizes='120x120'>\n"
                                "<link data-rh=\"true\" rel=\"apple-touch-icon\" href=\"https://static.zhihu.com/touch-152.png\" sizes=\"152x152\"/>\n"
                                "<link rel=\"icon\" sizes=\"16x16 196x196\" href=\"icons/big.png?v=1&amp;x=2\">\n"
                                "<link rel=\"apple-touch-icon-precomposed\" href=/touch.png>\n"
                                "<link rel=\"icon\" href=\"data:image/png;base64,AAAA\">\n"
                                "<link rel=icon mce_href=\"/wrong.ico\" href=\"/right.ico\">\n"
                                "</head></html>";
        const SiteIconCandidates found = siteIconCandidates(html, u"https://www.example.com/start/page"_s);
        QCOMPARE(found.icons,
            (QStringList {
                u"https://www.example.com/start/icons/big.png?v=1&x=2"_s, // declared large
                u"https://www.example.com/favicon.svg"_s, // SVG: any size, after the large ones (not mask-icon's)
                u"https://i0.hdslb.com/images/favicon.ico"_s, // the other icons, as the page has them
                u"https://www.example.com/right.ico"_s,
                u"https://www.example.com/favicon.ico"_s, // the site's, last
            }));
        QCOMPARE(found.touchIcons,
            (QStringList {
                u"https://www.example.com/touch.png"_s, // size unknown: taken as 180
                u"https://static.zhihu.com/touch-152.png"_s,
                u"https://static.zhihu.com/touch-120.png"_s,
            }));
        // No page (it could not be read): the site's favicon alone.
        QCOMPARE(siteIconCandidates({}, u"https://weibo.com/"_s).icons, (QStringList {u"https://weibo.com/favicon.ico"_s}));
        QVERIFY(siteIconCandidates({}, u"https://weibo.com/"_s).touchIcons.isEmpty());
        QVERIFY(siteIconCandidates({}, u"obsidian://x"_s).icons.isEmpty());
    }

    void webSuggestAddresses()
    {
        QCOMPARE(normalizeSuggestUrl(u" api.bing.com/osjson.aspx?query=%s "_s), u"https://api.bing.com/osjson.aspx?query=%s"_s);
        QCOMPARE(normalizeSuggestUrl(u"https://example.com/sug?q={query}"_s), u"https://example.com/sug?q=%s"_s); // Listary's
        QCOMPARE(normalizeSuggestUrl(u"https://example.com/sug?q=%25s"_s), u"https://example.com/sug?q=%s"_s);
        QVERIFY(normalizeSuggestUrl(u"https://example.com/sug"_s).isEmpty()); // no words: nothing to suggest for
        QVERIFY(normalizeSuggestUrl(u"obsidian://search?query=%s"_s).isEmpty()); // read by Win顺: http(s) only
        QVERIFY(normalizeSuggestUrl(u"file:///C:/x?%s"_s).isEmpty());
        QVERIFY(normalizeSuggestUrl({}).isEmpty());

        QCOMPARE(knownSuggestUrl(u"https://cn.bing.com/search?q=%s"_s), u"https://api.bing.com/osjson.aspx?query=%s"_s);
        QVERIFY(knownSuggestUrl(u"https://fanyi.baidu.com/x?q=%s"_s).isEmpty()); // not 百度's search
        QVERIFY(knownSuggestUrl(u"https://www.zhihu.com/search?q=%s"_s).isEmpty()); // 403 without signing in

        WebShortcut page {u"blog"_s, u"Blog"_s, u"https://example.com/blog"_s, {}, {}, u"https://example.com/s?q=%s"_s};
        QVERIFY(page.suggestUrl(u"x"_s).isEmpty()); // a page takes no words
        const WebShortcut site {u"ex"_s, u"Ex"_s, u"https://example.com/?q=%s"_s, {}, {}, u"https://example.com/s?q=%s&n=5"_s};
        QCOMPARE(site.suggestUrl(u"a b&c"_s), u"https://example.com/s?q=a%20b%26c&n=5"_s);
    }

    void webSuggestReplies()
    {
        using ws::websuggest::decodeReply;
        using ws::websuggest::parse;
        // 百度's JSONP, in GBK: unquoted keys, as its su?wd= sends it (显卡, 显卡天梯图).
        const QByteArray gbk = "window.baidu.sug({q:\"\xCF\xD4\xBF\xA8\",p:false,s:[\"\xCF\xD4\xBF\xA8\xCC\xEC\xCC\xDD\xCD\xBC\","
                               "\"\xCF\xD4\xBF\xA8\"]});";
        const QString text = decodeReply(gbk, u"text/javascript; charset=gbk");
        QVERIFY(text.contains(u"显卡天梯图"_s));
        // The word typed is left out: the row above searches it.
        QCOMPARE(parse(text, u"显卡"_s), (QStringList {u"显卡天梯图"_s}));
        // No charset named, not UTF-8: GB18030 all the same.
        QCOMPARE(decodeReply(QByteArray("\xCF\xD4\xBF\xA8"), {}), u"显卡"_s);
        QCOMPARE(decodeReply(QByteArray("\xE6\x98\xBE\xE5\x8D\xA1"), {}), u"显卡"_s); // UTF-8 stays UTF-8
        QCOMPARE(decodeReply(QByteArray("\xEF\xBB\xBF[1]"), u"text/plain; charset=gbk"), u"[1]"_s); // a BOM wins

        // OpenSearch (必应, Google: extra members after the list), and the cap.
        QCOMPARE(parse(u"[\"gpu\",[\"gpu z\",\"GPU Z\",\"gpu\",\"gpu test\",\" gpu  tweak \",\"e\",\"f\",\"g\"],[],{\"x\":[[512]]}]", u"gpu"_s),
            (QStringList {u"gpu z"_s, u"gpu test"_s, u"gpu tweak"_s, u"e"_s, u"f"_s}));
        QCOMPARE(parse(u"[\"x\",[\"a\",\"b\"]]", u"x"_s, 1), (QStringList {u"a"_s}));
        // JSONP around OpenSearch, a callback with dots, and a trailing semicolon.
        QCOMPARE(parse(u"jQuery.cb_1( [\"q\", [\"a\"]] );", u"q"_s), (QStringList {u"a"_s}));
        // 淘宝: arrays of [word, count].
        QCOMPARE(parse(u"{\"result\":[[\"显卡支架\",\"100\"],[\"显卡二手\",\"100\"]]}", u"显卡"_s),
            (QStringList {u"显卡支架"_s, u"显卡二手"_s}));
        // 哔哩哔哩: objects, "value" plain (its "name" carries markup).
        QCOMPARE(parse(u"{\"code\":0,\"result\":{\"tag\":[{\"value\":\"显卡推荐\",\"name\":\"<em class=\\\"h\\\">显卡</em>推荐\"}]}}", u"显卡"_s),
            (QStringList {u"显卡推荐"_s}));
        // 百度's sugrec: objects under g, the word under q.
        QCOMPARE(parse(u"{\"q\":\"显卡\",\"p\":false,\"g\":[{\"type\":\"sug\",\"q\":\"显卡价格\"}]}", u"显卡"_s),
            (QStringList {u"显卡价格"_s}));
        // JavaScript's 'strings', with a quote of the other kind inside, and an escaped one.
        QCOMPARE(parse(u"cb({s:['say \"hi\"', 'it\\'s']})", u""_s), (QStringList {u"say \"hi\""_s, u"it's"_s}));
        // A key-like word inside a string is left alone.
        QCOMPARE(parse(u"cb({s:[\"a, b: c\"]})", u""_s), (QStringList {u"a, b: c"_s}));

        // Anything else: nothing, quietly.
        QVERIFY(parse(u"<!DOCTYPE html><html>Sina Visitor System</html>", u"x"_s).isEmpty());
        QVERIFY(parse(u"", u"x"_s).isEmpty());
        QVERIFY(parse(u"{\"code\":403,\"message\":\"forbidden\"}", u"x"_s).isEmpty());
        QVERIFY(parse(u"[1, 2, 3]", u"x"_s).isEmpty());
        QVERIFY(parse(u"alert(1)", u"x"_s).isEmpty());
        QVERIFY(parse(u"x = (function(){})()", u"x"_s).isEmpty());
    }

    void webSuggestCache()
    {
        using ws::websuggest::Cache;
        Cache cache;
        const QString bing = u"https://api.bing.com/osjson.aspx?query=%s"_s;
        const QString mine = u"https://example.com/sug?q=%s"_s;
        cache.insert(bing, u"显卡"_s, {u"显卡驱动"_s}, 1000);
        QCOMPARE(cache.find(bing, u"显卡"_s, 1000 + Cache::kKeepMs), (QStringList {u"显卡驱动"_s}));
        QVERIFY(!cache.find(bing, u"显卡"_s, 1001 + Cache::kKeepMs)); // gone stale
        QVERIFY(!cache.find(bing, u"显"_s, 1000)); // other words
        // The address changed in the settings (the Lertaro bug): the same words
        // from the new address are not the old answer.
        QVERIFY(!cache.find(mine, u"显卡"_s, 1000));
        cache.insert(mine, u"显卡"_s, {}, 1000); // "no suggestions" is an answer too
        QCOMPARE(cache.find(mine, u"显卡"_s, 1000), QStringList());
        QCOMPARE(cache.find(bing, u"显卡"_s, 1000), (QStringList {u"显卡驱动"_s}));
        cache.clear();
        QVERIFY(!cache.find(bing, u"显卡"_s, 1000));
        QCOMPARE(cache.size(), 0);
        // Full: what went stale goes, then the oldest.
        for (int i = 0; i < Cache::kMaxEntries + 10; ++i)
            cache.insert(bing, QString::number(i), {}, i);
        QVERIFY(cache.size() <= Cache::kMaxEntries);
        QVERIFY(!cache.find(bing, u"0"_s, 300));
        QVERIFY(cache.find(bing, QString::number(Cache::kMaxEntries + 9), Cache::kMaxEntries + 9));
    }

    void releaseVersions()
    {
        QVERIFY(release::isNewer(u"0.2.0"_s, u"0.1.0"_s));
        QVERIFY(release::isNewer(u"v0.10.0"_s, u"0.9.9"_s)); // numbers, not text
        QVERIFY(release::isNewer(u"1.0"_s, u"0.9.9"_s));
        QVERIFY(!release::isNewer(u"0.2"_s, u"0.2.0"_s));
        QVERIFY(!release::isNewer(u"0.2.0-beta"_s, u"0.2.0"_s)); // the suffix is not looked at
        QVERIFY(!release::isNewer(u"0.1.9"_s, u"0.2.0"_s));
        QCOMPARE(release::normalized(u" V1.2.3 "_s), u"1.2.3"_s);
    }

    void releaseParse()
    {
        // Not raw string literals: moc cannot parse them.
        const auto info = release::parse("{\"tag_name\": \"v0.2.1\", "
                                         "\"html_url\": \"https://github.com/x/y/releases/tag/v0.2.1\", "
                                         "\"body\": \"notes\", \"assets\": []}");
        QVERIFY(info);
        QCOMPARE(info->version, u"0.2.1"_s);
        QCOMPARE(info->pageUrl, u"https://github.com/x/y/releases/tag/v0.2.1"_s);
        QCOMPARE(info->notes, u"notes"_s);
        QVERIFY(!release::parse("{\"message\": \"Not Found\"}"));
        QVERIFY(!release::parse("not json"));
    }

    void releaseNotes()
    {
        const QString notes = u"界面有了英文，还能切换主题。**Now** in English, with themes.\n"
                              u"\n"
                              u"## 下载 · Download\n"
                              u"\n"
                              u"Something.\n"
                              u"\n"
                              u"## 新功能 · What's new\n"
                              u"\n"
                              u"- 🌐 **中英双语**：随时切换。\n"
                              u"  **English and Chinese**: switch any time.\n"
                              u"- 没有图标的一条\n"
                              u"- 🎨 主题\n"
                              u"  Themes\n"
                              u"  in two lines\n"
                              u"\n"
                              u"## 其他 · Other\n"
                              u"- 不算\n"_s;
        QCOMPARE(release::summary(notes, true), u"界面有了英文，还能切换主题。"_s);
        QCOMPARE(release::summary(notes, false), u"**Now** in English, with themes."_s);
        const QList<release::Highlight> zh = release::highlights(notes, true);
        QCOMPARE(zh.size(), 3);
        QCOMPARE(zh[0], (release::Highlight {u"🌐"_s, u"**中英双语**：随时切换。"_s}));
        QCOMPARE(zh[1], (release::Highlight {QString(), u"没有图标的一条"_s}));
        const QList<release::Highlight> en = release::highlights(notes, false);
        QCOMPARE(en[0].text, u"**English and Chinese**: switch any time."_s);
        QCOMPARE(en[1].text, u"没有图标的一条"_s); // no English line: the Chinese one
        QCOMPARE(en[2], (release::Highlight {u"🎨"_s, u"Themes in two lines"_s}));
        QCOMPARE(release::summary(u"## Only headings"_s, true), QString());
        QCOMPARE(release::summary(u"No Chinese at all."_s, true), u"No Chinese at all."_s);
    }

    void wtf8RoundTrip()
    {
        const std::u16string samples[]
            = {u"plain.txt", u"中文文件名.txt", u"emoji 😀.png", std::u16string(1, char16_t(0xD800)) + u"x"};
        for (const auto& s : samples) {
            const std::string bytes = wtf8::fromUtf16(s);
            std::u16string back;
            wtf8::decodeAppend(back, bytes);
            QCOMPARE(back, s);
        }
        QCOMPARE(wtf8::toQString(wtf8::fromUtf16(u"报告.docx")), u"报告.docx"_s);
    }

    void foldAndFind()
    {
        QCOMPARE(text::foldAscii("ReadMe.TXT"), std::string("readme.txt"));
        QCOMPARE(text::findFolded("MyReport.docx", "report"), std::size_t {2});
        QCOMPARE(text::findFolded("abc", "abcd"), text::npos);
        QVERIFY(text::equalsFolded("ABC", "abc"));
        const std::string chinese = wtf8::fromUtf16(u"年度报告");
        QCOMPARE(text::findFolded(chinese, wtf8::fromUtf16(u"报告")), std::size_t {6});
    }

    void simdFindMatchesScalar()
    {
        std::mt19937 rng(42);
        const std::string alphabet = "abcABC._ 1" + wtf8::fromUtf16(u"报告");
        const auto randomString = [&](std::size_t maxLength) {
            std::string s(rng() % (maxLength + 1), '\0');
            for (char& c : s)
                c = alphabet[rng() % alphabet.size()];
            return s;
        };
        for (int iteration = 0; iteration < 50000; ++iteration) {
            const std::string hay = randomString(40);
            const std::string needle = text::foldAscii(randomString(5));
            const std::string padded = hay + std::string(text::kReadPastEnd, 'a'); // padding that could match
            const std::string_view view(padded.data(), hay.size());
            QCOMPARE(text::findFoldedPadded(view, needle), text::findFolded(view, needle));
        }
    }

    void glob()
    {
        QVERIFY(text::globMatch("Report.PDF", "*.pdf"));
        QVERIFY(text::globMatch("a1c", "a?c"));
        QVERIFY(!text::globMatch("abc.txt", "*.pdf"));
        QVERIFY(text::globMatch(wtf8::fromUtf16(u"报告.txt"), "??.txt")); // '?' is one character, not one byte
        QVERIFY(text::globMatch("anything", "*"));
    }

    void wordStart()
    {
        QVERIFY(text::isWordStart("my_report", 3));
        QVERIFY(text::isWordStart("myReport", 2));
        QVERIFY(text::isWordStart("v2final", 2));
        QVERIFY(!text::isWordStart("deportation", 2));
    }

    void parseQuery()
    {
        const ParsedQuery q = ws::parseQuery(uR"(report "annual plan" !draft ext:pdf,.docx proj\read)"_s);
        QCOMPARE(q.terms.size(), std::size_t {4});
        QCOMPARE(q.terms[0].text, std::string("report"));
        QCOMPARE(q.terms[1].text, std::string("annual plan"));
        QVERIFY(q.terms[2].negated);
        QCOMPARE(q.terms[3].text, std::string("read"));
        QCOMPARE(q.terms[3].ancestors, std::vector<std::string> {"proj"});
        QCOMPARE(q.extensions, (std::vector<std::string> {"pdf", "docx"}));
        QCOMPARE(q.highlights, (QStringList {u"report"_s, u"annual plan"_s, u"read"_s}));

        QVERIFY(ws::parseQuery(u"   "_s).isEmpty());
        QVERIFY(ws::parseQuery(u"*.TXT"_s).terms[0].wildcard);
        QCOMPARE(ws::parseQuery(u"rep*2024?.PDF"_s).terms[0].literal, std::string("2024"));

        // A whole path is one term: the space is in a folder's name.
        const ParsedQuery path = ws::parseQuery(uR"(C:\Users\me\Tencent Files\nt_qq\emoji)"_s);
        QCOMPARE(path.terms.size(), std::size_t {1});
        QCOMPARE(path.terms[0].text, std::string("emoji"));
        QCOMPARE(path.terms[0].ancestors, (std::vector<std::string> {"c:", "users", "me", "tencent files", "nt_qq"}));
        QCOMPARE(path.highlights, (QStringList {u"emoji"_s}));
        QVERIFY(NameMatcher(path).matchPath(uR"(C:\Users\me\Tencent Files\nt_qq\emoji)"_s, true) >= 0);
        QCOMPARE(ws::parseQuery(uR"(D:\Projects\ ext:pdf)"_s).extensions, std::vector<std::string> {"pdf"}); // no path: as before
    }

    // size: and dm: narrow by size and time (EntryFilter); what cannot be
    // read stays a word.
    void parseQueryFilters()
    {
        const std::uint64_t mb = 1 << 20;
        ParsedQuery q = ws::parseQuery(u"report size:>10mb"_s);
        QCOMPARE(q.terms.size(), std::size_t {1});
        QVERIFY(q.filter.bySize() && !q.filter.byTime());
        QCOMPARE(q.filter.minSize, packSize(10 * mb + 1));
        QCOMPARE(q.filter.maxSize, kUnknownSize);
        QVERIFY(!q.isEmpty());
        QVERIFY(!ws::parseQuery(u"size:>1gb"_s).isEmpty()); // a filter alone lists what fits it
        q = ws::parseQuery(u"SIZE:1mb..5MB"_s);
        QCOMPARE(q.filter.minSize, packSize(mb));
        QCOMPARE(q.filter.maxSize, packSize(5 * mb));
        q = ws::parseQuery(u"size:<=1.5k"_s);
        QCOMPARE(q.filter.minSize, FileSize {0});
        QCOMPARE(q.filter.maxSize, packSize(1536));
        q = ws::parseQuery(u"size:large"_s);
        QCOMPARE(q.filter.minSize, packSize(mb));
        QCOMPARE(q.filter.maxSize, packSize(16 * mb - 1));
        q = ws::parseQuery(u"size:>100mb size:<1gb"_s); // both: the narrower bounds
        QCOMPARE(q.filter.minSize, packSize(100 * mb + 1));
        QCOMPARE(q.filter.maxSize, packSize(1024 * mb - 1));
        q = ws::parseQuery(u"size:abc"_s); // not a size: a word
        QCOMPARE(q.terms.size(), std::size_t {1});
        QVERIFY(!q.filter.any());

        const QDateTime now(QDate(2026, 10, 15), QTime(14, 30));
        EntryFilter f;
        QVERIFY(ws::parseDateToken(u"dm:today", now, f));
        QCOMPARE(f.after, static_cast<FileTime>(QDate(2026, 10, 15).startOfDay().toSecsSinceEpoch()));
        QCOMPARE(f.before, FileTime {0});
        f = {};
        QVERIFY(ws::parseDateToken(u"dm:7d", now, f));
        QCOMPARE(f.after, static_cast<FileTime>(now.addDays(-7).toSecsSinceEpoch()));
        f = {};
        QVERIFY(ws::parseDateToken(u"dm:2026-01", now, f));
        QCOMPARE(f.after, static_cast<FileTime>(QDate(2026, 1, 1).startOfDay().toSecsSinceEpoch()));
        QCOMPARE(f.before, static_cast<FileTime>(QDate(2026, 2, 1).startOfDay().toSecsSinceEpoch()));
        f = {};
        QVERIFY(ws::parseDateToken(u"dm:2025-12-30..2026-01-02", now, f));
        QCOMPARE(f.after, static_cast<FileTime>(QDate(2025, 12, 30).startOfDay().toSecsSinceEpoch()));
        QCOMPARE(f.before, static_cast<FileTime>(QDate(2026, 1, 3).startOfDay().toSecsSinceEpoch()));
        f = {};
        QVERIFY(ws::parseDateToken(u"dm:<2026", now, f));
        QCOMPARE(f.after, FileTime {0});
        QCOMPARE(f.before, static_cast<FileTime>(QDate(2026, 1, 1).startOfDay().toSecsSinceEpoch()));
        f = {};
        QVERIFY(!ws::parseDateToken(u"dm:soon", now, f));
        QVERIFY(!ws::parseDateToken(u"dm:2026-13", now, f));
        QVERIFY(ws::parseQuery(u"dm:yesterday"_s).filter.byTime());
        QCOMPARE(ws::parseQuery(u"dm:never"_s).terms.size(), std::size_t {1});

        // What the filter lets through: bounds on the time leave out a time
        // not known (1970, 1980: knownTime); on the size, folders and sizes
        // not known.
        EntryFilter bounds;
        bounds.after = 1'700'000'000;
        QVERIFY(bounds.fits(1'700'000'000, kUnknownSize, true));
        QVERIFY(!bounds.fits(1'699'999'999, kUnknownSize, true));
        QVERIFY(!bounds.fits(0, kUnknownSize, false));
        QVERIFY(!bounds.fits(10, kUnknownSize, false)); // 1970: not known
        bounds.before = 1'700'000'100;
        QVERIFY(bounds.fits(1'700'000'099, kUnknownSize, false));
        QVERIFY(!bounds.fits(1'700'000'100, kUnknownSize, false));
        EntryFilter sizes;
        sizes.minSize = packSize(1000);
        QVERIFY(sizes.fits(0, packSize(1000), false));
        QVERIFY(!sizes.fits(0, packSize(999), false));
        QVERIFY(!sizes.fits(0, kUnknownSize, false));
        QVERIFY(!sizes.fits(0, kUnknownSize, true));

        QCOMPARE(ws::sizeText(0), u"0 B"_s);
        QCOMPARE(ws::sizeText(999), u"999 B"_s);
        QCOMPARE(ws::sizeText(1536), u"1.50 KB"_s);
        QCOMPARE(ws::sizeText(12 * mb + mb / 3), u"12.3 MB"_s);
        QCOMPARE(ws::sizeText(1'300'000'000), u"1.21 GB"_s);
        // Packed sizes: exact to 16 MB, close above, in order always.
        QCOMPARE(unpackSize(packSize(0)), std::uint64_t {0});
        QCOMPARE(unpackSize(packSize(16'777'215)), std::uint64_t {16'777'215});
        QVERIFY(packSize(16'777'217) > packSize(16'777'215));
        const std::uint64_t big = 7'000'000'000'000ull;
        QVERIFY(unpackSize(packSize(big)) > big - big / 100'000 && unpackSize(packSize(big)) < big + big / 100'000);
        QVERIFY(packSize(big) < kUnknownSize && packSize(big) > packSize(1 << 30));
    }

    // The panel's filters and column orders (NameSearchOptions::filter,
    // order): every search is a pass over the index, so a synthetic one of
    // files with known times and sizes says what must come out.
    void nameSearchFiltersAndOrders()
    {
        FileIndex index;
        const EntryId root = index.addRoot("D:");
        const EntryId work = index.add(root, "work", EntryFlag::Directory);
        const EntryId old = index.add(root, "old", EntryFlag::Directory);
        constexpr FileTime day = 86'400;
        constexpr FileTime t0 = 1'760'000'000; // 2025-10
        struct Spec {
            const char* name;
            EntryId parent;
            FileTime modified;
            std::uint64_t size; // kUnknownSize bits: not known
        };
        const Spec specs[] = {
            {"a.txt", work, t0 + 10 * day, 100},
            {"b.txt", work, t0 + 9 * day, 2'000'000},
            {"c.txt", old, t0 + 1 * day, 50'000},
            {"d.txt", old, t0 + 20 * day, 0xFFFF'FFFFull},
            {"e.txt", old, 0, 300}, // never known when
            {"f.log", work, t0 + 5 * day, 400},
        };
        std::vector<EntryId> ids;
        for (const Spec& s : specs) {
            const EntryId id = index.add(s.parent, s.name, 0);
            index.setModified(id, s.modified);
            if (s.size != 0xFFFF'FFFFull)
                index.setSize(id, packSize(s.size));
            ids.push_back(id);
        }
        const EntryId sub = index.add(work, "sub", EntryFlag::Directory);
        index.setModified(sub, t0 + 8 * day);
        WorkerPool pool(4);
        const auto names = [&](const NameSearchOutput& out) {
            QStringList list;
            for (const NameHit& h : out.hits)
                list.append(QString::fromUtf8(index.name(h.id)));
            return list;
        };
        const NameMatcher everything(ws::parseQuery(u""_s)); // the panel with nothing typed (browse)
        const NameMatcher txt(ws::parseQuery(u"ext:txt"_s));

        NameSearchOptions o;
        o.limit = 100;
        o.order = {SortKey::Name, false};
        auto out = searchNames(index, everything, o, pool, {});
        QCOMPARE(out.totalMatches, std::size_t {9}); // two folders, the sub folder, six files
        QCOMPARE(names(out), (QStringList {u"a.txt"_s, u"b.txt"_s, u"c.txt"_s, u"d.txt"_s, u"e.txt"_s, u"f.log"_s, u"old"_s, u"sub"_s, u"work"_s}));
        o.order = {SortKey::Name, true};
        QCOMPARE(names(searchNames(index, everything, o, pool, {})).first(), u"work"_s);

        // By size: the largest first; a size not known (and every folder) last.
        o.order = {SortKey::Size, true};
        out = searchNames(index, txt, o, pool, {});
        QCOMPARE(names(out).first(3), (QStringList {u"b.txt"_s, u"c.txt"_s, u"e.txt"_s}));
        QCOMPARE(names(out).last(), u"d.txt"_s);
        o.order = {SortKey::Size, false};
        out = searchNames(index, txt, o, pool, {});
        QCOMPARE(names(out).first(2), (QStringList {u"a.txt"_s, u"e.txt"_s}));
        QCOMPARE(names(out).last(), u"d.txt"_s);
        QCOMPARE(out.hits.front().size, packSize(100));

        // By time: a time not known last either way.
        o.order = {SortKey::Modified, true};
        out = searchNames(index, txt, o, pool, {});
        QCOMPARE(names(out), (QStringList {u"d.txt"_s, u"a.txt"_s, u"b.txt"_s, u"c.txt"_s, u"e.txt"_s}));
        o.order = {SortKey::Modified, false};
        out = searchNames(index, txt, o, pool, {});
        QCOMPARE(names(out), (QStringList {u"c.txt"_s, u"b.txt"_s, u"a.txt"_s, u"d.txt"_s, u"e.txt"_s}));

        // By path: a folder's files together (its path, then the name), folders by their own.
        o.order = {SortKey::Path, false};
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(names(out), (QStringList {u"old"_s, u"work"_s, u"c.txt"_s, u"d.txt"_s, u"e.txt"_s, u"a.txt"_s, u"b.txt"_s, u"f.log"_s, u"sub"_s}));

        // Filters: when written (a time not known left out), how large
        // (folders and sizes not known left out), under which folders.
        o.order = {};
        o.filter.after = t0 + 5 * day;
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(out.totalMatches, std::size_t {5}); // a, b, d, f, sub
        o.filter.before = t0 + 10 * day;
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(out.totalMatches, std::size_t {3}); // b, f, sub
        o.filter = {};
        o.filter.minSize = packSize(300);
        o.filter.maxSize = packSize(50'000);
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(out.totalMatches, std::size_t {3}); // c, e, f
        o.filter = {};
        o.filter.folders = {old};
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(out.totalMatches, std::size_t {3});
        o.filter.folders = {old, work};
        std::ranges::sort(o.filter.folders);
        o.filter.minSize = packSize(1000);
        out = searchNames(index, txt, o, pool, {});
        QCOMPARE(names(out), (QStringList {u"b.txt"_s, u"c.txt"_s}));
        // A page more comes in the same order (ids break every tie).
        o.filter = {};
        o.order = {SortKey::Name, false};
        o.limit = 3;
        const auto page1 = names(searchNames(index, everything, o, pool, {}));
        o.limit = 6;
        QCOMPARE(names(searchNames(index, everything, o, pool, {})).first(3), page1);
    }

    // The query read aloud (homophoneQueries), for names that sound the same.
    void homophoneQueries()
    {
        // The term as pinyin (the most common readings first: 度 is also
        // duo), the rest of the query as it was.
        const std::vector<ParsedQuery> spoken = ws::homophoneQueries(ws::parseQuery(u"年度报高 2025 !草稿 ext:docx"_s));
        QCOMPARE(spoken.size(), std::size_t {2});
        QCOMPARE(spoken[1].terms[0].text, std::string("nianduobaogao"));
        QCOMPARE(spoken[0].terms.size(), std::size_t {3});
        QCOMPARE(spoken[0].terms[0].text, std::string("niandubaogao"));
        QCOMPARE(spoken[0].terms[1].text, std::string("2025"));
        QVERIFY(spoken[0].terms[2].negated);
        QCOMPARE(spoken[0].terms[2].text, std::string("草稿")); // exclusions stay as typed
        QCOMPARE(spoken[0].extensions, std::vector<std::string> {"docx"});
        QCOMPARE(spoken[0].highlights, (QStringList {u"niandubaogao"_s, u"2025"_s}));
        // Other characters in the term stay: "报高v2" reads "baogaov2".
        QCOMPARE(ws::homophoneQueries(ws::parseQuery(u"报高v2"_s))[0].terms[0].text, std::string("baogaov2"));
        // A character with several readings: a query per reading, the most
        // common first (行: xing, then hang).
        const std::vector<ParsedQuery> bank = ws::homophoneQueries(ws::parseQuery(u"银行"_s));
        QVERIFY(bank.size() >= 2);
        QCOMPARE(bank[0].terms[0].text, std::string("yinxing"));
        QVERIFY(std::ranges::any_of(bank, [](const ParsedQuery& q) { return q.terms[0].text == "yinhang"; }));
        // Two such characters: every combination, up to the few allowed.
        QVERIFY(ws::homophoneQueries(ws::parseQuery(u"银行长"_s)).size() >= 4);
        QVERIFY(ws::homophoneQueries(ws::parseQuery(u"行行行行行行"_s)).size() <= 8);
        // Nothing to read: no queries. A wildcard term is not read either.
        QVERIFY(ws::homophoneQueries(ws::parseQuery(u"report 2025"_s)).empty());
        QVERIFY(ws::homophoneQueries(ws::parseQuery(u"报告*"_s)).empty());
        QVERIFY(ws::homophoneQueries(ws::parseQuery(u""_s)).empty());

        // In an index: 年度报高 is nowhere, read aloud it finds 年度报告 (and
        // not 其他); the pinyin highlights read back into the name.
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId annual = index.add(root, "年度报告.docx", 0);
        index.add(root, "其他.txt", 0);
        index.add(root, "年度计划.txt", 0);
        const ParsedQuery typed = ws::parseQuery(u"年度报高"_s);
        WorkerPool pool(2);
        QCOMPARE(searchNames(index, NameMatcher(typed), {.limit = 10}, pool, {}).hits.size(), std::size_t {0});
        const auto found = searchNames(index, NameMatcher(ws::homophoneQueries(typed)[0]), {.limit = 10}, pool, {});
        QCOMPARE(found.hits.size(), std::size_t {1});
        QCOMPARE(found.hits[0].id, annual);
        QVERIFY(pinyin::Matcher("niandubaogao").findUtf16(u"年度报告.docx").has_value());
    }

    void matcherRanking()
    {
        const NameMatcher m(ws::parseQuery(u"report"_s));
        const int exact = m.matchPath(u"C:\\report"_s, true);
        const int stem = m.matchPath(u"C:\\Report.docx"_s, false);
        const int prefix = m.matchPath(u"C:\\report_2024_final.docx"_s, false);
        const int word = m.matchPath(u"C:\\my_report.txt"_s, false);
        const int inner = m.matchPath(u"C:\\unreported.pdf"_s, false);
        QVERIFY(exact > stem);
        QVERIFY(stem > prefix);
        QVERIFY(prefix > word);
        QVERIFY(word > inner);
        QCOMPARE(m.matchPath(u"C:\\summary.txt"_s, false), -1);

        const NameMatcher negated(ws::parseQuery(u"report !draft"_s));
        QCOMPARE(negated.matchPath(u"C:\\report_draft.txt"_s, false), -1);

        const NameMatcher ext(ws::parseQuery(u"ext:pdf"_s));
        QVERIFY(ext.matchPath(u"C:\\a.PDF"_s, false) >= 0);
        QCOMPARE(ext.matchPath(u"C:\\a.pdf"_s, true), -1); // folders have no extension

        const NameMatcher path(ws::parseQuery(u"users\\notes"_s));
        QVERIFY(path.matchPath(u"C:\\Users\\me\\notes.txt"_s, false) >= 0);
        QCOMPARE(path.matchPath(u"D:\\Other\\notes.txt"_s, false), -1);
    }

    // Folders named among the words of a content search (ContentFolderSpec).
    void contentFolderSpecs()
    {
        using Spec = ws::ContentFolderSpec;
        const auto split = [](const QString& text) { return ws::splitContentQuery(text); };
        const auto names = [](const QString& typed, QStringList n) { return Spec {typed, {}, std::move(n)}; };

        ws::ContentQuery q = split(u"年度报告 abc/"_s);
        QCOMPARE(q.words, u"年度报告"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {names(u"abc/"_s, {u"abc"_s})}));
        q = split(u"abc\\ 年度报告"_s); // first, with a backslash
        QCOMPARE(q.words, u"年度报告"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {names(u"abc\\"_s, {u"abc"_s})}));
        const QString wide = u"年度报告 工作"_s + QChar(0xFF0F) + u"abc"_s + QChar(0xFF0F); // as a Chinese layout types /
        q = split(wide);
        QCOMPARE(q.words, u"年度报告"_s);
        QCOMPARE(q.folders.size(), std::size_t {1});
        QCOMPARE(q.folders[0].names, (QStringList {u"工作"_s, u"abc"_s}));
        q = split(u"年度报告 2025 工作/abc/"_s);
        QCOMPARE(q.words, u"年度报告 2025"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {names(u"工作/abc/"_s, {u"工作"_s, u"abc"_s})}));

        // "/abc": only last, after something to find.
        q = split(u"年度报告 /abc"_s);
        QCOMPARE(q.words, u"年度报告"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {names(u"/abc"_s, {u"abc"_s})}));
        q = split(u"/abc 年度报告"_s);
        QCOMPARE(q.words, u"/abc 年度报告"_s);
        QVERIFY(q.folders.empty());
        QCOMPARE(split(u"/usr"_s).words, u"/usr"_s);
        QVERIFY(split(u"/usr"_s).folders.empty());
        QVERIFY(split(u"GET /api/users"_s).folders.empty()); // more than one name: a route
        // A folder alone: where to look, nothing to find yet.
        q = split(u"abc/"_s);
        QVERIFY(q.words.isEmpty());
        QCOMPARE(q.folders, (std::vector<Spec> {names(u"abc/"_s, {u"abc"_s})}));

        // Paths, spaces and all, up to "\ ".
        q = split(u"D:\\My Files\\ 报告 2025"_s);
        QCOMPARE(q.words, u"报告 2025"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {{u"D:\\My Files\\"_s, u"D:\\My Files"_s, {}}}));
        q = split(u"报告 D:/Projects/"_s);
        QCOMPARE(q.words, u"报告"_s);
        QCOMPARE(q.folders.size(), std::size_t {1});
        QCOMPARE(q.folders[0].path, u"D:\\Projects"_s);
        q = split(u"\"D:\\My Files\\\" \"年度 报告\""_s);
        QCOMPARE(q.words, u"\"年度 报告\""_s);
        QCOMPARE(q.folders.size(), std::size_t {1});
        QCOMPARE(q.folders[0].path, u"D:\\My Files"_s);
        QCOMPARE(split(u"D:\\ 报告"_s).folders[0].path, u"D:\\"_s);
        // A folder whose name looks like code is still a folder; code after it stays one phrase.
        q = split(u"C:\\Program Files (x86)\\Steam\\ 存档 2025"_s);
        QCOMPARE(q.words, u"存档 2025"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {{u"C:\\Program Files (x86)\\Steam\\"_s, u"C:\\Program Files (x86)\\Steam"_s, {}}}));
        q = split(u" D:\\src [old]\\ if (a == b)"_s);
        QCOMPARE(q.words, u"if (a == b)"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {{u"D:\\src [old]\\"_s, u"D:\\src [old]"_s, {}}}));
        QCOMPARE(split(u"x = 1; D:\\a\\"_s).words, u"x = 1; D:\\a\\"_s); // code first: all of it
        QCOMPARE(split(u"\\\\nas\\docs\\ 报告"_s).folders[0].path, u"\\\\nas\\docs"_s);
        qputenv("WS_TEST_HOME", "C:\\Users\\me");
        QCOMPARE(split(u"%WS_TEST_HOME%\\Documents\\ 报告"_s).folders[0].path, u"C:\\Users\\me\\Documents"_s);
        qunsetenv("WS_TEST_HOME");
        q = split(u"D:\\Projects\\rep"_s); // a path being typed: no folder said
        QCOMPARE(q.words, u"D:\\Projects\\rep"_s);
        QVERIFY(q.folders.empty());

        // Not folders: what the search finds as it is.
        for (const QString& text : {u"printf(\"a/\")"_s, u"x = a/ b;"_s, u"see http://x.com/"_s, u"\"abc/\" 报告"_s,
                 u"!tmp/ 报告"_s, u"*.log/ x"_s, u"a/../ x"_s, u"报告"_s}) {
            q = split(text);
            QVERIFY2(q.folders.empty(), qPrintable(text));
            QCOMPARE(q.words, text);
        }
    }

    void matcherTermsInFolders()
    {
        QCOMPARE(ws::queryWords(u"  报告 \"annual plan\"\tx "_s), (QStringList {u"报告"_s, u"annual plan"_s, u"x"_s}));
        // Content: the words, or a line of code just as it is.
        QCOMPARE(ws::contentTerms(u" 报告  2025 "_s), (QStringList {u"报告"_s, u"2025"_s}));
        QCOMPARE(ws::contentTerms(u"\"annual plan\" 2025"_s), (QStringList {u"annual plan"_s, u"2025"_s}));
        QCOMPARE(ws::contentTerms(u"printf(\"%d\", *p)"_s), QStringList {u"printf(\"%d\", *p)"_s});
        QCOMPARE(ws::contentTerms(u"const auto lock = index->readLock();"_s),
            QStringList {u"const auto lock = index->readLock();"_s});
        QCOMPARE(ws::contentTerms(u"\"if (a == b)\""_s), QStringList {u"if (a == b)"_s});
        QCOMPARE(ws::contentTerms(u"\"年度 报"_s), QStringList {u"年度 报"_s}); // the closing quote not typed yet
        QVERIFY(ws::contentTerms(u"   "_s).isEmpty());

        // Each term in the name or in a folder above it, in either order; at
        // least one in the name.
        for (const QString& q : {u"src app"_s, u"app src"_s}) {
            const NameMatcher m(ws::parseQuery(q));
            QVERIFY(m.matchPath(u"F:\\proj\\src\\app"_s, true) >= 0);
            QVERIFY(m.matchPath(u"F:\\proj\\src\\app\\App.cpp"_s, false) >= 0);
            QCOMPARE(m.matchPath(u"F:\\proj\\src\\other.txt"_s, false), -1); // none in the name
            QCOMPARE(m.matchPath(u"F:\\proj\\lib\\app.txt"_s, false), -1); // "src" nowhere
            QVERIFY(m.matchPath(u"F:\\src_app.txt"_s, false) > m.matchPath(u"F:\\src\\app.txt"_s, false));
            QCOMPARE(m.matchName("app"), -1); // an app's name has no folders
        }
        // One term: in the name, as before.
        QCOMPARE(NameMatcher(ws::parseQuery(u"src"_s)).matchPath(u"F:\\src\\app.txt"_s, false), -1);
        // With an extension: the word may be the folder's.
        const NameMatcher pdfs(ws::parseQuery(u"合同 ext:pdf"_s));
        QVERIFY(pdfs.matchPath(u"D:\\合同\\扫描1.pdf"_s, false) >= 0);
        QCOMPARE(pdfs.matchPath(u"D:\\合同\\扫描1.jpg"_s, false), -1);
        // Wildcards and exclusions stay with the name.
        QCOMPARE(NameMatcher(ws::parseQuery(u"*.txt docs"_s)).matchPath(u"C:\\a.txt\\docs.md"_s, false), -1);
        QVERIFY(NameMatcher(ws::parseQuery(u"notes !draft"_s)).matchPath(u"C:\\draft\\notes.txt"_s, false) >= 0);

        // In the index, folders by pinyin too, remembered across entries.
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId project = index.add(root, "项目资料", EntryFlag::Directory);
        const EntryId year = index.add(project, "2025", EntryFlag::Directory);
        const EntryId plan = index.add(year, "方案.doc", 0);
        const EntryId notes = index.add(year, "notes.txt", 0);
        const EntryId elsewhere = index.add(root, "方案.doc", 0);
        const NameMatcher m(ws::parseQuery(u"xmzl 方案"_s));
        NameMatcher::Folders folders;
        QVERIFY(m.match(index, index.entry(plan), folders) >= 0);
        QCOMPARE(m.match(index, index.entry(notes), folders), -1);
        QCOMPARE(m.match(index, index.entry(elsewhere), folders), -1);
        QVERIFY(m.match(index, index.entry(plan), folders) >= 0); // from what it noted
        QVERIFY(NameMatcher(ws::parseQuery(u"2025 方案"_s)).match(index, index.entry(plan)) >= 0);
    }

    void bigFolderLookups()
    {
        // Writers' lookups in a folder with thousands of children go through a
        // hash table; it must stay in step with adds, removals, renames and moves.
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId big = index.add(root, "big", EntryFlag::Directory);
        const EntryId other = index.add(root, "other", EntryFlag::Directory);
        std::vector<EntryId> ids;
        for (int i = 0; i < 3000; ++i)
            ids.push_back(index.add(big, "file" + std::to_string(i) + ".txt", 0));
        const auto linear = [&](EntryId parent, std::string_view name) {
            for (EntryId c = index.entry(parent).firstChild; c != kNoEntry; c = index.entry(c).nextSibling) {
                if (index.name(c) == name)
                    return c;
            }
            return kNoEntry;
        };

        QCOMPARE(index.childForUpdate(big, "file1234.txt", true), ids[1234]); // builds the table
        QCOMPARE(index.childForUpdate(big, "FILE1234.TXT", false), ids[1234]);
        QCOMPARE(index.childForUpdate(big, "FILE1234.TXT", true), kNoEntry);
        QCOMPARE(index.findChild(big, "File7.TXT"), ids[7]); // readers use it too

        index.remove(ids[10]);
        QCOMPARE(index.childForUpdate(big, "file10.txt", false), kNoEntry);
        const EntryId added = index.add(big, "new.txt", 0);
        QCOMPARE(index.childForUpdate(big, "NEW.txt", false), added);
        QVERIFY(index.move(ids[20], big, "renamed.txt"));
        QCOMPARE(index.childForUpdate(big, "file20.txt", false), kNoEntry);
        QCOMPARE(index.childForUpdate(big, "renamed.txt", true), ids[20]);
        QVERIFY(index.move(ids[30], other, "moved.txt"));
        QCOMPARE(index.childForUpdate(big, "file30.txt", false), kNoEntry);
        QCOMPARE(index.childForUpdate(other, "moved.txt", true), ids[30]);
        const EntryId upper = index.add(big, "FILE40.TXT", 0); // case-sensitive folder: both spellings
        QCOMPARE(index.childForUpdate(big, "FILE40.TXT", true), upper);
        QCOMPARE(index.childForUpdate(big, "file40.txt", true), ids[40]);

        for (int i = 0; i < 3000; i += 7) { // every name agrees with a plain walk
            const std::string name = "file" + std::to_string(i) + ".txt";
            QCOMPARE(index.childForUpdate(big, name, true), linear(big, name));
        }

        index.compact(); // renumbers: the tables go and come back on demand
        const EntryId bigNow = index.findPath(L"C:\\big");
        QCOMPARE(index.name(index.childForUpdate(bigNow, "file2999.txt", true)), std::string_view("file2999.txt"));
        QCOMPARE(index.pathForUpdate(L"C:\\big\\renamed.txt"), linear(bigNow, "renamed.txt"));
    }

    void siblingLists()
    {
        // Children taken out in any order (removed, or moved elsewhere) leave
        // the list intact: in small folders, which are walked, and in big
        // ones, whose table knows the child before each.
        for (const int count : {5, 300, 3000}) {
            FileIndex index;
            const EntryId root = index.addRoot("C:");
            const EntryId dir = index.add(root, "dir", EntryFlag::Directory);
            const EntryId other = index.add(root, "other", EntryFlag::Directory);
            std::vector<EntryId> ids;
            for (int i = 0; i < count; ++i)
                ids.push_back(index.add(dir, "f" + std::to_string(i), 0));
            std::set<EntryId> expected(ids.begin(), ids.end());
            const auto childrenOf = [&](EntryId parent) {
                std::set<EntryId> seen;
                for (EntryId c = index.entry(parent).firstChild; c != kNoEntry; c = index.entry(c).nextSibling) {
                    if (!seen.insert(c).second)
                        return std::set<EntryId> {kNoEntry}; // a loop
                }
                return seen;
            };
            std::vector<EntryId> order = ids;
            std::shuffle(order.begin(), order.end(), std::mt19937(count));
            std::size_t moved = 0;
            for (std::size_t k = 0; k < order.size(); ++k) {
                if (k % 3 == 0) {
                    QVERIFY(index.move(order[k], other, "m" + std::to_string(k)));
                    ++moved;
                } else {
                    index.remove(order[k]);
                }
                expected.erase(order[k]);
                if (k == order.size() / 2) {
                    for (int n = 0; n < 50; ++n)
                        expected.insert(index.add(dir, "new" + std::to_string(n), 0));
                }
                if (k % 97 == 0)
                    QVERIFY(childrenOf(dir) == expected);
            }
            QVERIFY(childrenOf(dir) == expected);
            QCOMPARE(childrenOf(other).size(), moved);
            for (const EntryId c : expected)
                QCOMPARE(index.childForUpdate(dir, index.name(c), true), c);
        }
    }

    void changeWatcherRoots()
    {
        QTemporaryDir dir;
        const std::wstring root = QDir::toNativeSeparators(dir.path()).toStdWString();
        const auto openExclusively = [&] { // fails while anyone else holds the folder open
            const win32::UniqueHandle h(::CreateFileW(root.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            return h.valid();
        };
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<FsChange> seen;
        ChangeWatcher watcher([&](std::vector<FsChange>&& changes) {
            std::lock_guard lock(mutex);
            seen.insert(seen.end(), changes.begin(), changes.end());
            changed.notify_all();
        });
        QVERIFY(watcher.roots().empty());

        watcher.setRoots({root});
        QCOMPARE(watcher.roots(), std::vector<std::wstring> {root});
        QVERIFY(!openExclusively());
        QFile file(dir.filePath(u"new.txt"_s));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        {
            std::unique_lock lock(mutex);
            QVERIFY(changed.wait_for(lock, std::chrono::seconds(5), [&] {
                return std::any_of(seen.begin(), seen.end(), [&](const FsChange& c) {
                    return c.kind == FsChange::Kind::Added && c.path == root + L"\\new.txt";
                });
            }));
        }

        // Dropped from the set: its handle is closed by the time setRoots() returns.
        watcher.setRoots({});
        QVERIFY(watcher.roots().empty());
        QVERIFY(openExclusively());
    }

    void removeExcludedRules()
    {
        // New exclusions are applied to an index in place.
        FileIndex index;
        const EntryId c = index.addRoot("C:");
        const EntryId a = index.add(c, "a", EntryFlag::Directory);
        const EntryId modules = index.add(a, "node_modules", EntryFlag::Directory);
        index.add(modules, "x.js", 0);
        const EntryId svn = index.add(a, ".svn", EntryFlag::Directory);
        const EntryId pristine = index.add(svn, "pristine", EntryFlag::Directory);
        index.add(pristine, "p", 0);
        const EntryId otherPristine = index.add(a, "pristine", EntryFlag::Directory); // not inside .svn
        const EntryId skip = index.add(c, "Skip", EntryFlag::Directory);
        index.add(skip, "s.txt", 0);
        const EntryId keep = index.add(c, "keep.txt", 0);
        const EntryId upper = index.add(c, "NODE_MODULES", EntryFlag::Directory); // names ignore case
        const EntryId file = index.add(a, "node_modules", 0); // a file of that name is not a folder

        const CrawlRules rules {{L"C:\\skip"}, {L"node_modules", L".svn\\pristine"}, {}, {}};
        QCOMPARE(removeExcluded(index, rules), std::size_t {7});
        for (const EntryId gone : {modules, pristine, skip, upper})
            QVERIFY(index.entry(gone).isDeleted());
        for (const EntryId kept : {a, svn, otherPristine, keep, file})
            QVERIFY(!index.entry(kept).isDeleted());
        QCOMPARE(removeExcluded(index, rules), std::size_t {0}); // again: nothing left to do
    }

    void excludedFolders()
    {
        // "!folder\" leaves out what is inside such a folder, not every name.
        const NameMatcher folder(ws::parseQuery(u"index !node_modules\\"_s));
        QCOMPARE(folder.matchPath(u"C:\\app\\node_modules\\lib\\index.js"_s, false), -1);
        QVERIFY(folder.matchPath(u"C:\\app\\src\\index.js"_s, false) >= 0);
        QVERIFY(folder.matchName("index") >= 0); // apps have no folders

        // With a name too: only those names inside such a folder.
        const NameMatcher logs(ws::parseQuery(u"!tmp\\*.log"_s));
        QCOMPARE(logs.matchPath(u"C:\\tmp\\a.log"_s, false), -1);
        QVERIFY(logs.matchPath(u"C:\\tmp\\a.txt"_s, false) >= 0);
        QVERIFY(logs.matchPath(u"C:\\docs\\a.log"_s, false) >= 0);

        // The same on the index.
        SampleTree t;
        const NameMatcher underUsers(ws::parseQuery(u"!users\\"_s));
        QCOMPARE(underUsers.match(t.index, t.index.entry(t.report)), -1);
        QVERIFY(underUsers.match(t.index, t.index.entry(t.windows)) >= 0);
        QVERIFY(underUsers.match(t.index, t.index.entry(t.users)) >= 0); // the folder itself stays
    }

    void pinyinBasics()
    {
        const auto readings = [](char32_t c) {
            std::vector<std::string> out;
            for (const auto r : pinyin::readings(c))
                out.emplace_back(r);
            return out;
        };
        QCOMPARE(readings(U'报'), (std::vector<std::string> {"bao"}));
        QCOMPARE(readings(U'行').front(), std::string("xing")); // most common first
        QVERIFY(std::ranges::count(readings(U'行'), std::string("hang")) == 1);
        QCOMPARE(readings(U'绿').front(), std::string("lv"));
        QVERIFY(readings(U'a').empty());
        QVERIFY(readings(U'レ').empty());

        QVERIFY(pinyin::hasHan("年度报告.docx"));
        QVERIFY(!pinyin::hasHan("report.docx"));
        QVERIFY(!pinyin::hasHan("レポート.txt")); // Japanese kana are not Chinese characters
        QVERIFY(pinyin::isPinyinTerm("bg"));
        QVERIFY(pinyin::isPinyinTerm("v2bg"));
        QVERIFY(!pinyin::isPinyinTerm("b"));
        QVERIFY(!pinyin::isPinyinTerm("2024"));
        QVERIFY(!pinyin::isPinyinTerm("a.b"));
        QVERIFY(!pinyin::isPinyinTerm(std::string(64, 'a')));
    }

    void pinyinFind()
    {
        const auto find = [](std::string_view term, std::u16string_view text) -> std::pair<int, int> {
            const auto span = pinyin::Matcher(term).findUtf16(text);
            return span ? std::pair {static_cast<int>(span->start), static_cast<int>(span->length)} : std::pair {-1, 0};
        };
        using P = std::pair<int, int>;
        // Initials, whole syllables, prefixes, and mixes of them.
        for (const char* term : {"bg", "baogao", "baog", "bgao", "bga"})
            QCOMPARE(find(term, u"报告"), P(0, 2));
        QCOMPARE(find("bg", u"年度报告.docx"), P(2, 2));
        QCOMPARE(find("ndbg", u"年度报告.docx"), P(0, 4));
        QCOMPARE(find("bg", u"Report报告"), P(6, 2));
        QCOMPARE(find("v2bg", u"V2报告"), P(0, 4)); // ASCII matches itself, case folded
        QCOMPARE(find("yhk", u"银行卡"), P(0, 3)); // 行 read as hang
        QCOMPARE(find("yinhang", u"银行卡"), P(0, 2));
        QCOMPARE(find("cq", u"重庆"), P(0, 2)); // 重 read as chong
        QCOMPARE(find("lvse", u"绿色"), P(0, 2));
        QCOMPARE(find("luse", u"绿色"), P(0, 2)); // ü typed as u
        // Each character needs its own letters, in order, with nothing between.
        QCOMPARE(find("bx", u"报告"), P(-1, 0));
        QCOMPARE(find("baoo", u"报告"), P(-1, 0));
        QCOMPARE(find("bgd", u"报告.docx"), P(-1, 0));
        QCOMPARE(find("ab", u"报告"), P(-1, 0));
        QCOMPARE(find("bg", u"报 告"), P(-1, 0));
        // Spans in UTF-8 bytes for index names.
        const auto span = pinyin::Matcher("bg").findUtf8("年度报告.docx");
        QVERIFY(span);
        QCOMPARE(span->start, std::size_t {6});
        QCOMPARE(span->length, std::size_t {6});
    }

    void pinyinSearch()
    {
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId project = index.add(root, "项目资料", EntryFlag::Directory);
        const EntryId plan = index.add(project, "方案.doc", 0);
        const EntryId annual = index.add(root, "年度报告.docx", 0);
        const EntryId report = index.add(root, "报告.txt", 0);
        const EntryId latin = index.add(root, "bg.txt", 0);
        const EntryId other = index.add(root, "其他.txt", 0);
        QVERIFY(index.entry(annual).flags & EntryFlag::Han);
        QVERIFY(!(index.entry(latin).flags & EntryFlag::Han));
        index.setFlags(annual, EntryFlag::Hidden); // the index keeps its own flag
        QVERIFY(index.entry(annual).flags & EntryFlag::Han);
        const EntryId renamed = index.add(root, "notes.txt", 0);
        QVERIFY(index.move(renamed, root, "笔记.txt")); // renamed to Chinese
        QVERIFY(index.entry(renamed).flags & EntryFlag::Han);

        const NameMatcher bg(ws::parseQuery(u"bg"_s));
        const int literal = bg.match(index, index.entry(latin));
        const int whole = bg.match(index, index.entry(report));
        const int part = bg.match(index, index.entry(annual));
        QVERIFY(whole > 0);
        QVERIFY(part > 0);
        QVERIFY(literal > whole); // spelled out ranks first
        QVERIFY(whole > part); // the whole name before part of one
        QCOMPARE(bg.match(index, index.entry(other)), -1);
        // A less common reading still matches, below the most common one:
        // 行 is mostly xing, also hang.
        const EntryId bank = index.add(root, "银行.txt", 0);
        const int common = NameMatcher(ws::parseQuery(u"yx"_s)).match(index, index.entry(bank));
        const int lessCommon = NameMatcher(ws::parseQuery(u"yh"_s)).match(index, index.entry(bank));
        QVERIFY(lessCommon > 0);
        QVERIFY(common > lessCommon);
        QVERIFY(NameMatcher(ws::parseQuery(u"bj"_s)).match(index, index.entry(renamed)) > 0);

        // Folders in path terms, and history paths.
        QVERIFY(NameMatcher(ws::parseQuery(u"xmzl\\fa"_s)).match(index, index.entry(plan)) > 0);
        QCOMPARE(NameMatcher(ws::parseQuery(u"xx\\fa"_s)).match(index, index.entry(plan)), -1);
        QVERIFY(NameMatcher(ws::parseQuery(u"fa"_s)).matchPath(u"C:\\项目资料\\方案.doc"_s, false) > 0);
        // Exclusions stay literal.
        QVERIFY(NameMatcher(ws::parseQuery(u"txt !bg"_s)).match(index, index.entry(report)) > 0);
    }

    void indexAddFindRemoveMove()
    {
        SampleTree t;
        QCOMPARE(t.index.liveCount(), std::size_t {7});
        QCOMPARE(t.index.findPath(L"c:\\users\\ME\\documents\\report.DOCX"), t.report);
        QCOMPARE(t.index.findPath(L"C:\\"), t.root);
        QCOMPARE(t.index.findPath(L"C:\\nope"), kNoEntry);
        QCOMPARE(t.index.extLength(t.index.entry(t.report)), std::size_t {4});

        QVERIFY(t.index.move(t.report, t.me, "Report-final.docx"));
        QCOMPARE(t.index.path(t.report), u"C:\\Users\\me\\Report-final.docx"_s);
        QCOMPARE(t.index.findChild(t.docs, "report.docx"), kNoEntry);
        QVERIFY(!t.index.move(t.users, t.docs, "Users")); // into its own subtree

        QCOMPARE(t.index.remove(t.users), std::size_t {5});
        QCOMPARE(t.index.liveCount(), std::size_t {2});
        QCOMPARE(t.index.findPath(L"C:\\Users"), kNoEntry);
        QVERIFY(t.index.entry(t.notes).isDeleted());
    }

    void indexPaths()
    {
        SampleTree t;
        QCOMPARE(t.index.path(t.root), u"C:\\"_s);
        QCOMPARE(t.index.path(t.notes), u"C:\\Users\\me\\Documents\\notes.txt"_s);
        QCOMPARE(t.index.depth(t.notes), 4);

        // Deeper than the fast path's fixed buffer.
        EntryId cur = t.root;
        for (int i = 0; i < 80; ++i)
            cur = t.index.add(cur, "d", EntryFlag::Directory);
        QCOMPARE(t.index.path(cur).count(u'\\'), 80);
    }

    // While the index is saved (a long read), a writer waits at its gate, not
    // on the lock, where it would hold up the searches' read locks too.
    void longReadLetsReadersPastWriters()
    {
        using namespace std::chrono_literals;
        FileIndex index;
        std::atomic<bool> wrote = false;
        std::atomic<bool> read = false;
        std::jthread writer;
        std::jthread search;
        std::optional<FileIndex::LongReadLock> save = index.longReadLock(); // let go before the threads are joined
        writer = std::jthread([&] {
            const auto lock = index.writeLock();
            wrote = true;
        });
        std::this_thread::sleep_for(100ms); // waiting by now
        QVERIFY(!wrote);
        search = std::jthread([&] {
            const auto lock = index.readLock();
            read = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(read, 2000);
        QVERIFY(!wrote);
        save.reset();
        writer.join();
        QVERIFY(wrote);
    }

    // With nothing typed the recent items are listed, each looked up on disk:
    // the local ones answer at once, and a share whose server is gone (seconds
    // to say so) neither holds up the search nor shows up.
    void recentItemsDoNotWaitForSlowDrives()
    {
        QTemporaryDir dir;
        const QString here = QDir::toNativeSeparators(dir.filePath(u"here.txt"_s));
        QFile file(here);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QString gone = QDir::toNativeSeparators(dir.filePath(u"gone.txt"_s));
        const QString remote = u"\\\\10.255.255.1\\share\\far.txt"_s; // nobody there

        SearchEngine engine(nullptr);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        const auto listed = [&](const QStringList& history, qint64& elapsedUs) {
            spy.clear();
            SearchEngine::Request request;
            request.history = history;
            engine.submit(std::move(request));
            if (!spy.wait(5000))
                return QStringList {u"(no results)"_s};
            elapsedUs = spy.at(0).at(3).toLongLong();
            QStringList paths;
            for (const SearchResult& r : spy.at(0).at(1).value<SearchResults>())
                paths.append(r.path);
            return paths;
        };

        qint64 us = 0;
        QCOMPARE(listed({here, gone}, us), QStringList {here});
        qInfo("local only: %lld us", us);
        QVERIFY2(us < 50'000, qPrintable(QString::number(us)));

        QCOMPARE(listed({remote, here, gone}, us), QStringList {here});
        qInfo("with the share, first time: %lld us", us);
        QVERIFY2(us < 400'000, qPrintable(QString::number(us))); // at most the one wait (100 ms)

        QCOMPARE(listed({remote, here, gone}, us), QStringList {here});
        qInfo("with the share, again: %lld us", us);
        QVERIFY2(us < 50'000, qPrintable(QString::number(us))); // slow today: not waited for
    }

    void snapshotRoundTrip()
    {
        SampleTree t;
        t.index.remove(t.notes);
        t.index.setFolderRecord(t.root, ntfs::kRootRecord, t.root);
        t.index.setFolderRecord(t.root, 100, t.docs);
        t.index.setFolderRecord(t.root, 101, t.windows);
        const EntryId gone = t.index.add(t.users, "gone", EntryFlag::Directory);
        t.index.setFolderRecord(t.root, 102, gone);
        t.index.remove(gone);
        // Up and down from one entry to the next (zigzag), and the largest.
        t.index.setModified(t.report, 1'700'000'123);
        t.index.setModified(t.windows, 0xFFFF'FFFF);
        t.index.setModified(t.docs, 5);
        t.index.setSize(t.report, packSize(123'456));
        const std::vector<VolumeInfo> volumes {{L"C:", 0x1234, true}};
        const std::vector<JournalPosition> journals {{0xABCDEF, 123456789}};
        QTemporaryDir dir;
        const QString file = dir.filePath(u"index.bin"_s);
        const CrawlRules rules {{L"C:\\skip"}, {L"node_modules", L".svn\\pristine"}, {L"C:\\Windows"}, {L"AppData"}};
        QVERIFY(snapshot::save(t.index, volumes, journals, rules, file));

        const auto contents = snapshot::load(file);
        QVERIFY(contents);
        const auto& loaded = contents->index;
        QVERIFY(loaded);
        QCOMPARE(loaded->liveCount(), std::size_t {6});
        const EntryId report = loaded->findPath(L"C:\\Users\\me\\Documents\\Report.docx");
        QVERIFY(report != kNoEntry);
        QCOMPARE(loaded->path(report), u"C:\\Users\\me\\Documents\\Report.docx"_s);
        QVERIFY(contents->times);
        QCOMPARE(loaded->modified(report), FileTime {1'700'000'123});
        QCOMPARE(loaded->modified(loaded->findPath(L"C:\\Windows")), FileTime {0xFFFF'FFFF});
        QCOMPARE(loaded->modified(loaded->findPath(L"C:\\Users\\me\\Documents")), FileTime {5});
        QCOMPARE(loaded->modified(loaded->findPath(L"C:\\Users")), FileTime {0});
        QCOMPARE(loaded->size(report), packSize(123'456)); // version 8: sizes too
        QCOMPARE(loaded->size(loaded->findPath(L"C:\\Users\\me\\Documents")), kUnknownSize);
        QCOMPARE(loaded->size(loaded->findPath(L"C:\\Windows")), kUnknownSize);
        QVERIFY(loaded->entry(loaded->findPath(L"C:\\Windows")).flags & EntryFlag::LowPriority);
        QCOMPARE(loaded->findPath(L"C:\\Users\\me\\Documents\\notes.txt"), kNoEntry);

        QVERIFY(contents->journals == journals);
        QCOMPARE(contents->volumes.size(), std::size_t {1});
        QVERIFY(contents->volumes[0].root == L"C:" && contents->volumes[0].serial == 0x1234);
        QVERIFY(contents->rules && *contents->rules == rules);
        QVERIFY(contents->attachment.empty());
        const EntryId root = loaded->roots().front();
        QCOMPARE(loaded->folderByRecord(root, ntfs::kRootRecord), root);
        QCOMPARE(loaded->folderByRecord(root, 100), loaded->findPath(L"C:\\Users\\me\\Documents"));
        QCOMPARE(loaded->folderByRecord(root, 101), loaded->findPath(L"C:\\Windows"));
        QCOMPARE(loaded->folderRecords(root)->size(), std::size_t {3}); // the removed folder is not kept

        // Data attached to entries: written with the ids the entries get in the file.
        const QString attachedFile = dir.filePath(u"attached.bin"_s);
        QVERIFY(snapshot::save(t.index, volumes, journals, rules, attachedFile, [&](const std::vector<EntryId>& newIds) {
            const EntryId id = newIds[t.report];
            std::vector<char> data(sizeof id);
            std::memcpy(data.data(), &id, sizeof id);
            return data;
        }));
        const auto attached = snapshot::load(attachedFile);
        QVERIFY(attached && attached->attachment.size() == sizeof(EntryId));
        EntryId reportId = kNoEntry;
        std::memcpy(&reportId, attached->attachment.data(), sizeof reportId);
        QCOMPARE(attached->index->path(reportId), u"C:\\Users\\me\\Documents\\Report.docx"_s);

        QVERIFY(!snapshot::load(dir.filePath(u"missing.bin"_s)));
        QFile truncated(file);
        QVERIFY(truncated.resize(truncated.size() - 3));
        QVERIFY(!snapshot::load(file)); // the end is gone

        // Packed in many blocks: entries and an attachment that straddle them.
        FileIndex big;
        const EntryId folder = big.add(big.addRoot("E:"), "folder", EntryFlag::Directory);
        for (int i = 0; i < 100000; ++i)
            big.add(folder, "file number " + std::to_string(i) + " with a fairly long name.txt", 0);
        std::vector<char> blob(3'000'000);
        for (std::size_t i = 0; i < blob.size(); ++i)
            blob[i] = static_cast<char>(i * 7 + i / 1000);
        const QString bigFile = dir.filePath(u"big.bin"_s);
        QVERIFY(snapshot::save(big, {}, {}, {}, bigFile, [&](const std::vector<EntryId>&) { return blob; }));
        QFile packed(bigFile);
        QVERIFY(packed.open(QIODevice::ReadOnly) && packed.read(8) == QByteArray("QFINDEXZ", 8));
        QVERIFY(packed.size() < 5'000'000); // about 8 MB unpacked
        packed.close();
        const auto bigLoaded = snapshot::load(bigFile);
        QVERIFY(bigLoaded && bigLoaded->attachment == blob);
        QCOMPARE(bigLoaded->index->liveCount(), std::size_t {100002});
        QVERIFY(bigLoaded->index->findPath(L"E:\\folder\\file number 99999 with a fairly long name.txt") != kNoEntry);
        QVERIFY(packed.open(QIODevice::ReadWrite));
        packed.seek(packed.size() / 2);
        packed.write("garbage!", 8); // in the middle of a block
        packed.close();
        QVERIFY(!snapshot::load(bigFile));

        // A file from before snapshots were packed.
        QByteArray plain("QFINDEX", 8);
        const auto put = [&](auto v) { plain.append(reinterpret_cast<const char*>(&v), sizeof v); };
        put(std::uint32_t {4}); // version
        put(std::uint32_t {0}); // volumes
        for (int i = 0; i < 4; ++i)
            put(std::uint32_t {0}); // rules
        put(std::uint32_t {2}); // entries
        put(kNoEntry);
        put(static_cast<std::uint8_t>(EntryFlag::Root | EntryFlag::Directory));
        put(std::uint16_t {2});
        plain.append("D:");
        put(EntryId {0});
        put(std::uint8_t {0});
        put(std::uint16_t {5});
        plain.append("a.txt");
        put(std::uint32_t {0}); // folder record tables
        put(std::uint64_t {0}); // attachment
        put(std::uint32_t {0x21444E45}); // "END!"
        QFile old(dir.filePath(u"old.bin"_s));
        QVERIFY(old.open(QIODevice::WriteOnly) && old.write(plain) == plain.size());
        old.close();
        const auto oldLoaded = snapshot::load(old.fileName());
        QVERIFY(oldLoaded && oldLoaded->index->findPath(L"D:\\a.txt") != kNoEntry);

        // Version 5: names once each, entries record by record (Win顺 0.5).
        QByteArray five("QFINDEX", 8);
        const auto put5 = [&](auto v) { five.append(reinterpret_cast<const char*>(&v), sizeof v); };
        put5(std::uint32_t {5}); // version
        put5(std::uint32_t {0}); // volumes
        for (int i = 0; i < 4; ++i)
            put5(std::uint32_t {0}); // rules
        put5(std::uint32_t {2}); // names
        put5(std::uint16_t {2});
        five.append("D:");
        put5(std::uint16_t {5});
        five.append("a.txt");
        put5(std::uint32_t {3}); // entries: back, flags, name
        put5(std::uint8_t {0});
        put5(static_cast<std::uint8_t>(EntryFlag::Root | EntryFlag::Directory));
        put5(std::uint8_t {0});
        put5(std::uint8_t {1});
        put5(std::uint8_t {0});
        put5(std::uint8_t {1});
        put5(std::uint8_t {2}); // another a.txt, in the root too
        put5(std::uint8_t {0});
        put5(std::uint8_t {1});
        put5(std::uint32_t {1}); // folder record tables
        put5(EntryId {0});
        put5(std::uint32_t {1});
        put5(std::uint32_t {ntfs::kRootRecord});
        put5(EntryId {0});
        put5(std::uint64_t {0}); // attachment
        put5(std::uint32_t {0x21444E45}); // "END!"
        QFile v5(dir.filePath(u"v5.bin"_s));
        QVERIFY(v5.open(QIODevice::WriteOnly) && v5.write(five) == five.size());
        v5.close();
        const auto v5Loaded = snapshot::load(v5.fileName());
        QVERIFY(v5Loaded && v5Loaded->index->liveCount() == 3);
        QVERIFY(v5Loaded->index->findPath(L"D:\\a.txt") != kNoEntry);
        QCOMPARE(v5Loaded->index->folderByRecord(0, ntfs::kRootRecord), EntryId {0});
    }

    void folderRecords()
    {
        // The table against a reference map, with many erases (backward shifts).
        RecordTable table;
        std::unordered_map<std::uint32_t, EntryId> reference;
        std::mt19937 rng(7);
        for (int i = 0; i < 200000; ++i) {
            const auto record = static_cast<std::uint32_t>(rng() % 5000 + 1);
            if (rng() % 3 == 0) {
                table.erase(record);
                reference.erase(record);
            } else {
                table.set(record, static_cast<EntryId>(i));
                reference[record] = static_cast<EntryId>(i);
            }
        }
        QCOMPARE(table.size(), reference.size());
        for (std::uint32_t r = 1; r <= 5000; ++r) {
            const auto it = reference.find(r);
            QCOMPARE(table.find(r), it == reference.end() ? kNoEntry : it->second);
        }

        // Lookups skip removed folders; compaction renumbers; a removed root drops its table.
        SampleTree t;
        FileIndex& index = t.index;
        const EntryId d = index.addRoot("D:");
        const EntryId data = index.add(d, "data", EntryFlag::Directory);
        index.setFolderRecord(t.root, 30, t.docs);
        index.setFolderRecord(t.root, 31, t.me);
        index.setFolderRecord(t.root, 32, t.report); // not a folder: never returned
        index.setFolderRecord(d, 30, data);
        QCOMPARE(index.folderByRecord(t.root, 30), t.docs);
        QCOMPARE(index.folderByRecord(d, 30), data);
        QCOMPARE(index.folderByRecord(t.root, 32), kNoEntry);
        index.remove(t.notes);
        index.remove(t.docs);
        QCOMPARE(index.folderByRecord(t.root, 30), kNoEntry);
        QVERIFY(index.compact() > 0);
        const EntryId me = index.findPath(L"C:\\Users\\me");
        QCOMPARE(index.folderByRecord(t.root, 31), me);
        const EntryId newD = index.roots().back();
        QCOMPARE(index.folderByRecord(newD, 30), index.findPath(L"D:\\data"));
        QCOMPARE(index.folderRecords(t.root)->size(), std::size_t {1}); // 30 and 32 went with docs
        index.forgetFolderRecord(t.root, 31);
        QCOMPARE(index.folderByRecord(t.root, 31), kNoEntry);
        index.remove(newD);
        QVERIFY(!index.folderRecords(newD));
    }

    void mftRecord()
    {
        RecordBuilder b(0x0003); // in use, folder
        b.standardInformation(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE);
        b.fileName(0x0002'0000'0000'04D2ull, u"LONGNA~1", 2); // the DOS alias: skipped
        b.fileName(0x0002'0000'0000'04D2ull, u"Long name 中文", 1);
        const std::u16string longName(150, u'x'); // runs across the first 512-byte stride
        b.fileName(0x0001'0000'0000'0309ull, longName, 0);
        auto bytes = b.finish();

        ntfs::FileRecord record;
        auto copy = bytes;
        QVERIFY(ntfs::parseFileRecord(copy.data(), copy.size(), record));
        QVERIFY(record.inUse);
        QVERIFY(record.directory);
        QCOMPARE(record.baseRecord, std::uint32_t {0});
        QCOMPARE(record.attributes, std::uint32_t {FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE});
        QCOMPARE(record.names.size(), std::size_t {2});
        QCOMPARE(ntfs::recordOf(record.names[0].parent), std::uint32_t {1234});
        QCOMPARE(std::u16string(record.names[0].name), std::u16string(u"Long name 中文"));
        QCOMPARE(ntfs::recordOf(record.names[1].parent), std::uint32_t {777});
        QCOMPARE(std::u16string(record.names[1].name), longName);

        copy = bytes;
        copy[510] ^= std::byte {1}; // torn write: the check value no longer matches
        QVERIFY(!ntfs::parseFileRecord(copy.data(), copy.size(), record));
        copy = bytes;
        copy[0] = std::byte {'B'}; // "BAAD"
        QVERIFY(!ntfs::parseFileRecord(copy.data(), copy.size(), record));

        RecordBuilder unused(0x0000);
        unused.fileName(5, u"deleted.txt", 1);
        copy = unused.finish();
        QVERIFY(ntfs::parseFileRecord(copy.data(), copy.size(), record));
        QVERIFY(!record.inUse);
        QVERIFY(record.names.empty());

        RecordBuilder extension(0x0001, 0x0003'0000'0000'002Aull);
        extension.fileName(5, u"link.txt", 0);
        copy = extension.finish();
        QVERIFY(ntfs::parseFileRecord(copy.data(), copy.size(), record));
        QCOMPARE(record.baseRecord, std::uint32_t {42});
        QCOMPARE(record.names.size(), std::size_t {1});
        QVERIFY(!record.sizeKnown); // no $DATA here

        // The file's size: a resident $DATA's value, a non-resident one's
        // real size (from the piece at VCN 0 only); a named stream is not it.
        RecordBuilder small(0x0001);
        small.fileName(5, u"small.txt", 1);
        small.resident(0x80, std::vector<std::byte>(37));
        copy = small.finish();
        QVERIFY(ntfs::parseFileRecord(copy.data(), copy.size(), record));
        QVERIFY(record.sizeKnown);
        QCOMPARE(record.size, std::uint64_t {37});
        RecordBuilder large(0x0001);
        large.fileName(5, u"large.bin", 1);
        large.nonResidentData({std::byte {0x11}, std::byte {0x01}, std::byte {0x02}, std::byte {0}}, 0, 4096, 3000, 3000, 0);
        copy = large.finish();
        QVERIFY(ntfs::parseFileRecord(copy.data(), copy.size(), record));
        QVERIFY(record.sizeKnown);
        QCOMPARE(record.size, std::uint64_t {3000});
        RecordBuilder later(0x0001, 0x0003'0000'0000'002Aull); // a later piece of a fragmented file: says nothing
        later.nonResident(0x80, {std::byte {0x11}, std::byte {0x01}, std::byte {0x02}, std::byte {0}}, 9, 40960, 5);
        copy = later.finish();
        QVERIFY(ntfs::parseFileRecord(copy.data(), copy.size(), record));
        QVERIFY(!record.sizeKnown);
    }

    void mftRunList()
    {
        const auto bytes = [](std::initializer_list<int> v) {
            std::vector<std::byte> out;
            for (const int b : v)
                out.push_back(static_cast<std::byte>(b));
            return out;
        };
        // 0x10 clusters at 0x4000, then 8 clusters 0x1000 clusters back (negative delta).
        const auto runs = ntfs::decodeRunList(bytes({0x31, 0x10, 0x00, 0x40, 0x00, 0x21, 0x08, 0x00, 0xF0, 0x00}), 4096);
        QVERIFY(runs);
        QCOMPARE(runs->size(), std::size_t {2});
        QCOMPARE((*runs)[0].offset, std::uint64_t {0x4000} * 4096);
        QCOMPARE((*runs)[0].length, std::uint64_t {0x10} * 4096);
        QCOMPARE((*runs)[1].offset, std::uint64_t {0x3000} * 4096);
        QCOMPARE((*runs)[1].length, std::uint64_t {8} * 4096);
        QVERIFY(!ntfs::decodeRunList(bytes({0x31, 0x10, 0x00, 0x40, 0x00}), 4096)); // no end marker
        QVERIFY(!ntfs::decodeRunList(bytes({0x01, 0x10, 0x00}), 4096)); // sparse
        QVERIFY(!ntfs::decodeRunList(bytes({0x31, 0x10, 0x00}), 4096)); // cut short
    }

    void mftLayouts()
    {
        QVERIFY(ntfs::supported({512, 512, 1024})); // converted from FAT32
        QVERIFY(ntfs::supported({512, 2048, 4096})); // "format /L" with small clusters
        QVERIFY(ntfs::supported({4096, 4096, 4096})); // a 4K-sector disk
        QVERIFY(ntfs::supported({512, 2u << 20, 1024})); // the largest NTFS clusters
        QVERIFY(!ntfs::supported({4096, 2048, 4096})); // a cluster smaller than a sector
        QVERIFY(!ntfs::supported({512, 4096, 1000}));
        QVERIFY(!ntfs::supported({512, 4096, 256}));
        QVERIFY(!ntfs::supported({512, 0, 1024}));

        // Reads stay within extents, take whole clusters, and stop past the bytes asked for.
        const std::vector<ntfs::Extent> extents {{8192, 3 * 512}, {40960, 20 * 512}, {1024, 2 * 512}};
        const auto plan = ntfs::planReads(extents, 3 * 512 + 9 * 512 + 100, 512, 4 * 512);
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> want {
            {8192, 1536}, {40960, 2048}, {43008, 2048}, {45056, 1024}};
        QCOMPARE(plan.size(), want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            QCOMPARE(plan[i].offset, want[i].first);
            QCOMPARE(plan[i].length, want[i].second);
        }
        QCOMPARE(ntfs::planReads(extents, 100 * 512, 512, 4 * 512).size(), std::size_t {7}); // all, and short
        for (std::size_t i = 0, at = 0; i < plan.size(); at += plan[i].length, ++i)
            QCOMPARE(plan[i].at, std::uint64_t {at});

        // A stretch left out across two extents: the reads go on after it, where it ends.
        const std::vector<ntfs::Extent> two {{0x10000, 16384}, {0x40000, 16384}};
        const std::vector<ntfs::Stretch> skip {{8192, 16384}};
        const auto around = ntfs::planReads(two, 32768, 4096, 65536, skip);
        QCOMPARE(around.size(), std::size_t {2});
        QCOMPARE(around[0].offset, std::uint64_t {0x10000});
        QCOMPARE(around[0].length, std::uint64_t {8192});
        QCOMPARE(around[0].at, std::uint64_t {0});
        QCOMPARE(around[1].offset, std::uint64_t {0x40000 + 8192});
        QCOMPARE(around[1].length, std::uint64_t {8192});
        QCOMPARE(around[1].at, std::uint64_t {24576});

        // Free stretches: whole granules (here 8 records, one byte of the bitmap) of free records.
        const auto bits = [](std::initializer_list<int> v) {
            std::vector<std::byte> out;
            for (const int b : v)
                out.push_back(static_cast<std::byte>(b));
            return out;
        };
        const auto bitmap = bits({0xFF, 0, 0, 0x01, 0, 0, 0, 0x80});
        const auto stretches = [&](std::uint64_t minBytes, std::uint64_t bytes) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
            for (const ntfs::Stretch& s : ntfs::freeStretches(bitmap, 1024, 8192, minBytes, bytes))
                out.emplace_back(s.at, s.length);
            return out;
        };
        using Stretches = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
        QCOMPARE(stretches(8192, 65536), (Stretches {{8192, 16384}, {32768, 24576}}));
        QCOMPARE(stretches(24576, 65536), (Stretches {{32768, 24576}})); // the shorter one is read
        QCOMPARE(stretches(8192, 131072), (Stretches {{8192, 16384}, {32768, 24576}})); // past the bitmap: in use
        QCOMPARE(stretches(8192, 40960), (Stretches {{8192, 16384}, {32768, 8192}})); // only the bytes asked for
        QVERIFY(ntfs::freeStretches(bitmap, 1024, 4096, 4096, 65536).empty()); // a granule must be whole bytes
    }

    // An MFT split into extents of odd lengths, out of order on the volume:
    // with clusters smaller than records, records run across extents and
    // across the reader's blocks. Each comes back whole, in order. With its
    // bitmap, the free records from 1.5 to 3.5 MB into it (garbage here) are
    // left out where they fill a whole megabyte, from 2 to 3 MB.
    void mftReader_data()
    {
        QTest::addColumn<int>("clusterSize");
        QTest::addColumn<int>("recordSize");
        QTest::addColumn<bool>("fromRecordZero"); // the extents read from the MFT's own record
        QTest::addColumn<bool>("withBitmap");
        QTest::addColumn<int>("bitmapHolder"); // the record with the bitmap, named in record 0's attribute list; 0: record 0
        QTest::addColumn<int>("readsInFlight");
        QTest::newRow("512-byte clusters") << 512 << 1024 << false << false << 0 << 2;
        QTest::newRow("512-byte clusters, record 0") << 512 << 1024 << true << false << 0 << 2;
        QTest::newRow("512-byte clusters, 4 KB records") << 512 << 4096 << true << false << 0 << 3;
        QTest::newRow("2 KB clusters, 4 KB records") << 2048 << 4096 << false << false << 0 << 2;
        QTest::newRow("4 KB clusters") << 4096 << 1024 << false << false << 0 << 4;
        QTest::newRow("64 KB clusters") << 65536 << 4096 << true << false << 0 << 2;
        QTest::newRow("512-byte clusters, bitmap") << 512 << 1024 << false << true << 0 << 2;
        QTest::newRow("512-byte clusters, 4 KB records, bitmap") << 512 << 4096 << true << true << 0 << 4;
        QTest::newRow("2 KB clusters, 4 KB records, bitmap") << 2048 << 4096 << true << true << 0 << 3;
        QTest::newRow("64 KB clusters, bitmap") << 65536 << 4096 << false << true << 0 << 2;
        // Record 2 runs across the first two extents with 512-byte clusters.
        QTest::newRow("bitmap in another record") << 512 << 1024 << false << true << 2 << 2;
        QTest::newRow("bitmap in another record, 4 KB") << 4096 << 4096 << true << true << 9 << 2;
        // Its run list in two parts: the first cluster in record 0, the rest in record 2.
        QTest::newRow("bitmap in two parts") << 512 << 1024 << false << true << -2 << 2;
    }
    void mftReader()
    {
        QFETCH(int, clusterSize);
        QFETCH(int, recordSize);
        QFETCH(bool, fromRecordZero);
        QFETCH(bool, withBitmap);
        QFETCH(int, bitmapHolder);
        QFETCH(int, readsInFlight);
        const bool twoParts = bitmapHolder < 0; // the first in record 0, the second in record -bitmapHolder
        const auto holder = static_cast<std::uint32_t>(std::abs(bitmapHolder));
        const auto cluster = static_cast<std::uint64_t>(clusterSize);
        const auto size = static_cast<std::size_t>(recordSize);
        const auto isFree = [&](std::uint64_t record) {
            return withBitmap && record * size >= (3u << 19) && record * size < (7u << 19);
        };
        const auto leftOut = [&](std::uint64_t record) {
            return withBitmap && record * size >= (2u << 20) && record * size < (3u << 20);
        };

        // Extents in clusters. The first holds record 0 whole, as on a real
        // volume; the fourth is longer than a read (4 MB).
        std::vector<std::uint64_t> clusters {std::max<std::uint64_t>(3, 2 * size / cluster + 1), 1, 5,
            (9u << 19) / cluster + 1, 7, 2, 9, 4};
        std::uint64_t total = 0;
        for (const std::uint64_t c : clusters)
            total += c * cluster;
        const std::uint32_t count = static_cast<std::uint32_t>((total - 2 * cluster) / size); // the rest is allocated, unused
        // On the volume: in another order, with a cluster of garbage before each.
        const std::size_t order[] {0, 4, 2, 6, 1, 5, 3, 7};
        std::vector<ntfs::Extent> extents(clusters.size());
        std::uint64_t at = 16 * cluster;
        for (const std::size_t k : order) {
            at += cluster;
            extents[k] = {at, clusters[k] * cluster};
            at += extents[k].length;
        }
        // The bitmap after them: a bit per record, set when it is in use.
        std::vector<std::byte> bitmap((count + 63) / 64 * 8);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (i % 7 != 3 && !isFree(i))
                bitmap[i / 8] |= std::byte {1} << (i % 8);
        }
        const ntfs::Extent bitmapExtent {at + cluster, (bitmap.size() + cluster - 1) / cluster * cluster};
        std::vector<std::byte> image(static_cast<std::size_t>(bitmapExtent.offset + bitmapExtent.length), std::byte {0xCC});
        std::memcpy(image.data() + bitmapExtent.offset, bitmap.data(), bitmap.size());

        // The records, laid along the extents.
        const auto file = [](std::uint32_t i) { return u"f" + QString::number(i).toStdU16String(); };
        std::size_t extent = 0;
        std::uint64_t inExtent = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const bool holds = withBitmap && i == holder; // the $BITMAP attribute
            RecordBuilder b(i % 7 == 3 && !holds ? 0x0000 : 0x0001, holds && i != 0 ? 0x0001'0000'0000'0000ull : 0, size);
            const std::uint64_t lastVcn = bitmapExtent.length / cluster - 1;
            if (i == 0) {
                b.fileName(0x0001'0000'0000'0005ull, u"$MFT", 3);
                if (withBitmap && holder != 0) {
                    // The attribute list: an entry for each part of the $BITMAP attribute.
                    std::vector<std::byte> list;
                    const auto entry = [&](std::uint64_t vcn, std::uint32_t number) {
                        std::vector<std::byte> e(0x20);
                        put<std::uint32_t>(e, 0, 0xB0);
                        put<std::uint16_t>(e, 4, 0x20);
                        e[7] = std::byte {0x1A};
                        put<std::uint64_t>(e, 8, vcn);
                        put<std::uint64_t>(e, 0x10, 0x0001'0000'0000'0000ull | number);
                        list.insert(list.end(), e.begin(), e.end());
                    };
                    if (twoParts)
                        entry(0, 0);
                    entry(twoParts ? 1 : 0, holder);
                    b.resident(0x20, list);
                }
                b.nonResident(0x80, runList(extents, cluster), total / cluster - 1);
                if (withBitmap && twoParts) // its first cluster
                    b.nonResident(0xB0, runList({{bitmapExtent.offset, cluster}}, cluster), 0, bitmap.size());
            }
            if (holds && twoParts) {
                b.nonResident(0xB0, runList({{bitmapExtent.offset + cluster, bitmapExtent.length - cluster}}, cluster),
                    lastVcn, 0, 1);
            } else if (holds) {
                b.nonResident(0xB0, runList({bitmapExtent}, cluster), lastVcn, bitmap.size());
            } else if (i != 0) {
                b.fileName(0x0001'0000'0000'0005ull, file(i), 1);
            }
            const std::vector<std::byte> record = isFree(i) ? std::vector<std::byte>(size, std::byte {0xCC}) : b.finish();
            for (std::size_t done = 0; done < size;) {
                const std::size_t n = std::min<std::size_t>(size - done, extents[extent].length - inExtent);
                std::memcpy(image.data() + extents[extent].offset + inExtent, record.data() + done, n);
                done += n;
                inExtent += n;
                if (inExtent == extents[extent].length) {
                    ++extent;
                    inExtent = 0;
                }
            }
        }

        const ntfs::Geometry geometry {512, static_cast<std::uint32_t>(clusterSize), static_cast<std::uint32_t>(recordSize)};
        const std::uint64_t validBytes = std::uint64_t {count} * size;
        ntfs::MftReader reader(image, geometry, fromRecordZero ? std::vector<ntfs::Extent>() : extents,
            extents[0].offset, validBytes, {readsInFlight, true});
        QVERIFY2(reader.valid(), qPrintable(QString::fromStdWString(reader.error())));
        QCOMPARE(reader.extentsFromRecordZero(), fromRecordZero);
        QCOMPARE(reader.recordCount(), std::uint64_t {count});
        std::uint32_t next = 0;
        ntfs::FileRecord record;
        while (reader.next()) {
            if (reader.firstRecord() != next) { // only over the megabyte left out
                QCOMPARE(std::uint64_t {next} * size, std::uint64_t {2u << 20});
                QCOMPARE(std::uint64_t {reader.firstRecord()} * size, std::uint64_t {3u << 20});
                next = reader.firstRecord();
            }
            for (std::size_t i = 0; i < reader.blockRecords(); ++i, ++next) {
                QVERIFY(!leftOut(next));
                if (isFree(next)) {
                    QVERIFY(!reader.parse(i, record)); // garbage, read all the same
                    continue;
                }
                QVERIFY2(reader.parse(i, record), qPrintable(u"record %1"_s.arg(next)));
                if (withBitmap && holder != 0 && next == holder) {
                    QVERIFY(record.inUse && record.names.empty()); // holds the bitmap, nothing else
                    continue;
                }
                QCOMPARE(record.inUse, next % 7 != 3);
                if (record.inUse) {
                    QCOMPARE(record.names.size(), std::size_t {1});
                    QCOMPARE(std::u16string(record.names[0].name), next == 0 ? std::u16string(u"$MFT") : file(next));
                }
            }
        }
        QVERIFY2(reader.valid(), qPrintable(QString::fromStdWString(reader.error())));
        QCOMPARE(next, count);
        QCOMPARE(reader.bytesDone(), validBytes);
        QCOMPARE(reader.bytesSkipped(), withBitmap ? std::uint64_t {1u << 20} : std::uint64_t {0});
        QCOMPARE(reader.bitmapRecord(), withBitmap && !twoParts ? holder : 0u);

        // A read that comes back short (past the end of the volume) is an error, not the end.
        image.resize(static_cast<std::size_t>(extents[3].offset + cluster));
        ntfs::MftReader cut(image, geometry, extents, extents[0].offset, validBytes);
        while (cut.next()) {
        }
        QVERIFY(!cut.valid());
    }

    // What the bulk path reads out of a file's record (ntfs::parseFileInfo,
    // ntfs::decodeDataRuns): the $DATA's shape and sizes, the flags that keep
    // a file from being read that way, the ids, the names.
    void fileInfo()
    {
        // $STANDARD_INFORMATION as Windows writes it (0x48 bytes): the times,
        // the attributes at 0x20, the security id at 0x34.
        const auto standardInformation = [](std::uint32_t attributes, std::uint32_t securityId) {
            std::vector<std::byte> value(0x48);
            put<std::int64_t>(value, 0x08, 0x01DC'0000'1111'2222ll); // modified
            put<std::int64_t>(value, 0x10, 0x01DC'0000'3333'4444ll); // MFT changed
            put(value, 0x20, attributes);
            put(value, 0x34, securityId);
            return value;
        };
        const auto fixedUp = [](RecordBuilder& b) {
            std::vector<std::byte> record = b.finish();
            ntfs::FileRecord parsed;
            if (!ntfs::parseFileRecord(record.data(), record.size(), parsed)) // fixups applied in place
                record.clear();
            return record;
        };
        ntfs::FileInfo info;

        // A small file: resident data, a Win32 name and its DOS alias (two
        // $FILE_NAMEs, one name), a security id.
        {
            RecordBuilder b(0x0001);
            b.resident(0x10, standardInformation(FILE_ATTRIBUTE_ARCHIVE, 0x123));
            b.fileName(0x0001'0000'0000'0005ull, u"readme.txt", 1);
            b.fileName(0x0001'0000'0000'0005ull, u"README~1.TXT", 2);
            const std::string text = "hello, world";
            std::vector<std::byte> data(text.size());
            std::memcpy(data.data(), text.data(), text.size());
            b.resident(0x80, data);
            const std::vector<std::byte> record = fixedUp(b);
            QVERIFY(!record.empty() && ntfs::parseFileInfo(record, info));
            QVERIFY(info.inUse && !info.directory);
            QCOMPARE(info.sequence, std::uint16_t {1});
            QCOMPARE(info.names, std::uint16_t {1});
            QCOMPARE(info.standardInfoBytes, 0x48u);
            QCOMPARE(info.securityId, 0x123u);
            QCOMPARE(info.attributes, static_cast<std::uint32_t>(FILE_ATTRIBUTE_ARCHIVE));
            QCOMPARE(info.modified, 0x01DC'0000'1111'2222ll);
            QCOMPARE(info.mftChanged, 0x01DC'0000'3333'4444ll);
            QCOMPARE(info.data, ntfs::FileInfo::Data::Resident);
            QCOMPARE(info.size, std::uint64_t {text.size()});
            QCOMPARE(std::string(reinterpret_cast<const char*>(info.resident.data()), info.resident.size()), text);
            QVERIFY(!info.hasAttributeList && !info.hasReparse && !info.hasSecurityDescriptor);
        }
        // Non-resident: two runs, a valid data length short of the size.
        {
            RecordBuilder b(0x0001);
            b.resident(0x10, standardInformation(FILE_ATTRIBUTE_ARCHIVE, 0x124));
            b.fileName(0x0001'0000'0000'0005ull, u"big.log", 3);
            b.nonResidentData(runList({{40 * 4096, 3 * 4096}, {100 * 4096, 2 * 4096}}, 4096), 4, 5 * 4096, 18000, 9000, 0);
            const std::vector<std::byte> record = fixedUp(b);
            QVERIFY(!record.empty() && ntfs::parseFileInfo(record, info));
            QCOMPARE(info.data, ntfs::FileInfo::Data::NonResident);
            QCOMPARE(info.size, std::uint64_t {18000});
            QCOMPARE(info.initialized, std::uint64_t {9000});
            QCOMPARE(info.allocated, std::uint64_t {5 * 4096});
            QCOMPARE(info.lowestVcn, std::uint64_t {0});
            QCOMPARE(info.highestVcn, std::uint64_t {4});
            QCOMPARE(info.dataFlags, std::uint16_t {0});
            const auto runs = ntfs::decodeDataRuns(info.runList);
            QVERIFY(runs);
            QCOMPARE(runs->size(), std::size_t {2});
            QCOMPARE((*runs)[0].lcn, std::int64_t {40});
            QCOMPARE((*runs)[0].clusters, std::uint64_t {3});
            QCOMPARE((*runs)[1].lcn, std::int64_t {100});
            QCOMPARE((*runs)[1].clusters, std::uint64_t {2});
        }
        // Compressed, encrypted, sparse: the flags; a reparse point and an
        // attribute list; two unnamed $DATAs.
        {
            RecordBuilder b(0x0001);
            b.resident(0x10, standardInformation(FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_REPARSE_POINT, 0x125));
            b.resident(0x20, std::vector<std::byte>(0x20)); // $ATTRIBUTE_LIST
            b.fileName(0x0001'0000'0000'0005ull, u"packed.bin", 1);
            b.nonResidentData(runList({{40 * 4096, 16 * 4096}}, 4096), 15, 16 * 4096, 60000, 60000,
                ntfs::kDataCompressed | ntfs::kDataSparse | ntfs::kDataEncrypted);
            std::vector<std::byte> reparse(0x10);
            put<std::uint32_t>(reparse, 0, 0x8000'0017u); // IO_REPARSE_TAG_WOF
            b.resident(0xC0, reparse);
            const std::vector<std::byte> record = fixedUp(b);
            QVERIFY(!record.empty() && ntfs::parseFileInfo(record, info));
            QVERIFY(info.hasAttributeList);
            QVERIFY(info.hasReparse);
            QCOMPARE(info.reparseTag, 0x8000'0017u);
            QVERIFY(info.dataFlags & ntfs::kDataCompressed);
            QVERIFY(info.dataFlags & ntfs::kDataSparse);
            QVERIFY(info.dataFlags & ntfs::kDataEncrypted);
            QVERIFY(info.attributes & FILE_ATTRIBUTE_REPARSE_POINT);
        }
        {
            RecordBuilder b(0x0001);
            b.resident(0x10, standardInformation(FILE_ATTRIBUTE_ARCHIVE, 7));
            b.resident(0x80, std::vector<std::byte>(3));
            b.resident(0x80, std::vector<std::byte>(5));
            const std::vector<std::byte> record = fixedUp(b);
            QVERIFY(!record.empty() && ntfs::parseFileInfo(record, info));
            QCOMPARE(info.data, ntfs::FileInfo::Data::Several);
        }
        // Old volumes: a $SECURITY_DESCRIPTOR of the file's own, a short $STANDARD_INFORMATION.
        {
            RecordBuilder b(0x0001);
            b.resident(0x10, std::vector<std::byte>(0x30));
            b.resident(0x50, std::vector<std::byte>(0x40));
            b.resident(0x80, std::vector<std::byte>(1));
            const std::vector<std::byte> record = fixedUp(b);
            QVERIFY(!record.empty() && ntfs::parseFileInfo(record, info));
            QVERIFY(info.hasSecurityDescriptor);
            QCOMPARE(info.standardInfoBytes, 0x30u);
            QCOMPARE(info.securityId, 0u);
        }
        // A free record parses as not in use; garbage does not parse.
        {
            RecordBuilder b(0x0000);
            const std::vector<std::byte> record = fixedUp(b);
            QVERIFY(!record.empty() && ntfs::parseFileInfo(record, info));
            QVERIFY(!info.inUse);
            std::vector<std::byte> garbage(1024, std::byte {0xCC});
            QVERIFY(!ntfs::parseFileInfo(garbage, info));
        }

        // Run lists with a sparse run (no offset): kept as a run without
        // clusters; the next run's offset counts from the one before it.
        const std::vector<std::byte> withHole {std::byte {0x11}, std::byte {0x04}, std::byte {0x10}, // 4 clusters at 0x10
            std::byte {0x01}, std::byte {0x03}, // 3 clusters, sparse
            std::byte {0x11}, std::byte {0x02}, std::byte {0x05}, // 2 clusters at 0x15
            std::byte {0x00}};
        const auto runs = ntfs::decodeDataRuns(withHole);
        QVERIFY(runs);
        QCOMPARE(runs->size(), std::size_t {3});
        QCOMPARE((*runs)[0].lcn, std::int64_t {0x10});
        QCOMPARE((*runs)[1].lcn, std::int64_t {-1});
        QCOMPARE((*runs)[1].clusters, std::uint64_t {3});
        QCOMPARE((*runs)[2].lcn, std::int64_t {0x15});
        QVERIFY(!ntfs::decodeRunList(withHole, 4096)); // the MFT's own decoder refuses sparse runs
        const std::vector<std::byte> cut {std::byte {0x21}, std::byte {0x04}}; // ends inside a run
        QVERIFY(!ntfs::decodeDataRuns(cut));
        const std::vector<std::byte> noEnd {std::byte {0x11}, std::byte {0x04}, std::byte {0x10}};
        QVERIFY(!ntfs::decodeDataRuns(noEnd));
    }

    // The grams the bulk path takes from a file's bytes are those readFile()
    // takes from the file: the encoding from the first chunk, the chunks the same.
    void collectTextMatchesReadFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        std::mt19937_64 rng(20261014);
        const auto check = [&](const char* name, const QByteArray& bytes) {
            const QString path = QDir::toNativeSeparators(dir.filePath(QString::fromLatin1(name))); // as the index has paths
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write(bytes) == bytes.size());
            file.close();
            const ContentIndexer::FileText fromFile = ContentIndexer::readFile(path.toStdWString(), 64ll << 20, {});
            const std::optional<std::uint32_t> stamp = ContentIndexer::stampOf(path.toStdWString());
            QVERIFY(stamp);
            const ContentIndexer::FileText fromBytes
                = ContentIndexer::collectText({bytes.constData(), static_cast<std::size_t>(bytes.size())}, *stamp);
            QCOMPARE(fromBytes.outcome, fromFile.outcome);
            QCOMPARE(fromBytes.stamp, fromFile.stamp);
            QVERIFY2(fromBytes.keys == fromFile.keys, name);
            QVERIFY(fromBytes.outcome == ContentIndexer::Outcome::Empty || !fromBytes.keys.empty());
        };
        check("ascii.txt", QByteArray("The quick brown fox jumps over the lazy dog.\nfor (int i = 0; i < n; ++i)\n"));
        check("utf8.md", QString(u"季度报告：第三季度收入增长 12%，详见附录。\nUnicode café naïve 日本語テキスト\n"_s).toUtf8());
        QByteArray utf16;
        utf16.append("\xFF\xFE", 2);
        const QString wide = u"合同编号 2026-10 甲方乙方 WinShun"_s;
        utf16.append(reinterpret_cast<const char*>(wide.utf16()), wide.size() * 2);
        check("utf16.txt", utf16);
        // Longer than a chunk (ContentScanner::kChunkBytes): several feeds,
        // words running across the chunk boundary.
        QByteArray big;
        const char* words[] = {"alpha", "beta", "gamma", "delta", "报告", "合同", "epsilon"};
        while (big.size() < static_cast<qsizetype>(ContentScanner::kChunkBytes) * 2 + 777) {
            big.append(words[rng() % std::size(words)]);
            big.append(rng() % 7 == 0 ? '\n' : ' ');
        }
        check("big.txt", big);
        check("empty.txt", QByteArray());
        QByteArray binary(3000, '\0');
        for (char& c : binary)
            c = static_cast<char>(rng());
        check("binary.dat", binary);
    }

    void usnRecords()
    {
        std::vector<std::byte> buffer(8);
        const std::int64_t next = 0x12345678;
        std::memcpy(buffer.data(), &next, 8);
        appendUsn(buffer, 0x0005'0000'0000'1000ull, 0x0001'0000'0000'0005ull, 4096, USN_REASON_FILE_CREATE,
            FILE_ATTRIBUTE_ARCHIVE, u"新建文本.txt");
        appendUsn(buffer, 0x77, 0x5, 4200, USN_REASON_FILE_DELETE, 0, u"v3 record", 3); // skipped
        appendUsn(buffer, 0x0001'0000'0000'2000ull, 0x5, 4300, USN_REASON_RENAME_NEW_NAME | USN_REASON_CLOSE,
            FILE_ATTRIBUTE_DIRECTORY, u"dir");
        std::vector<ntfs::UsnRecord> records;
        QCOMPARE(ntfs::parseUsnRecords(buffer, records), next);
        QCOMPARE(records.size(), std::size_t {2});
        QCOMPARE(ntfs::recordOf(records[0].file), std::uint32_t {0x1000});
        QCOMPARE(ntfs::recordOf(records[0].parent), ntfs::kRootRecord);
        QCOMPARE(records[0].usn, std::int64_t {4096});
        QCOMPARE(records[0].reason, std::uint32_t {USN_REASON_FILE_CREATE});
        QCOMPARE(std::u16string(records[0].name), std::u16string(u"新建文本.txt"));
        QCOMPARE(records[1].attributes, std::uint32_t {FILE_ATTRIBUTE_DIRECTORY});
        QCOMPARE(std::u16string(records[1].name), std::u16string(u"dir"));

        buffer.resize(buffer.size() - 4); // the last record cut short
        records.clear();
        QCOMPARE(ntfs::parseUsnRecords(buffer, records), std::int64_t {-1});
    }

    // The index service's wire format (VolumeProtocol.h): each payload comes
    // back as it went; one cut short, or with a name running past its end,
    // or a root that is no drive, is refused; a frame too long is refused
    // by its header alone; bytes a later version adds at the end are left alone.
    void volumeProtocol()
    {
        using namespace volproto;
        const auto payloadOf = [](std::span<const std::byte> frame) { return frame.subspan(Writer::kHeaderBytes); };
        const auto header = [](std::span<const std::byte> frame, Type& type, std::uint32_t& length) {
            return parseHeader(frame.first<8>(), type, length);
        };
        Type type {};
        std::uint32_t length = 0;

        Hello hello;
        hello.minor = 7;
        hello.version[0] = 1;
        hello.version[1] = 2;
        hello.version[2] = 3;
        hello.pid = 4242;
        Writer w;
        encode(w, hello);
        std::span<const std::byte> frame = w.finish(Type::Hello);
        QVERIFY(header(frame, type, length));
        QCOMPARE(type, Type::Hello);
        QCOMPARE(std::size_t {length}, frame.size() - Writer::kHeaderBytes);
        Hello back;
        QVERIFY(decode(payloadOf(frame), back));
        QCOMPARE(back.major, kMajor);
        QCOMPARE(back.minor, std::uint16_t {7});
        QCOMPARE(back.version[2], std::uint16_t {3});
        QCOMPARE(back.pid, std::uint32_t {4242});
        QVERIFY(!decode(payloadOf(frame).first(payloadOf(frame).size() - 1), back)); // cut short
        std::vector<std::byte> longer(payloadOf(frame).begin(), payloadOf(frame).end());
        longer.push_back(std::byte {0x99});
        QVERIFY(decode(longer, back)); // a later minor's field: ignored

        w.reset();
        encode(w, OpenMft {L"d:", 0xCAFEBABE, true});
        frame = w.finish(Type::OpenMft);
        OpenMft open;
        QVERIFY(header(frame, type, length) && type == Type::OpenMft);
        QVERIFY(decode(payloadOf(frame), open));
        QCOMPARE(open.root, std::wstring(L"d:"));
        QCOMPARE(open.serial, std::uint32_t {0xCAFEBABE});
        QVERIFY(open.lowPriority);
        for (const wchar_t* bad : {L"D:\\", L"DD", L"", L"\\\\?\\D:", L"1:"}) {
            w.reset();
            encode(w, OpenMft {bad, 0, false});
            QVERIFY2(!decode(payloadOf(w.finish(Type::OpenMft)), open), qPrintable(QString::fromWCharArray(bad)));
        }
        // A name whose length runs past the payload.
        w.reset();
        w.u16(2);
        w.u16(u'D');
        QVERIFY(!decode(payloadOf(w.finish(Type::OpenMft)), open));
        // A frame longer than any read: refused by the header.
        w.reset();
        w.u32(0);
        frame = w.finish(Type::OpenMft);
        std::vector<std::byte> tooLong(frame.begin(), frame.end());
        const std::uint32_t huge = kMaxPayloadBytes + 1;
        std::memcpy(tooLong.data(), &huge, 4);
        QVERIFY(!header(tooLong, type, length));

        w.reset();
        encode(w, MftInfo {{4096, 65536, 4096}, 123456789, 123456789ull * 4096});
        MftInfo info;
        QVERIFY(decode(payloadOf(w.finish(Type::MftInfo)), info));
        QCOMPARE(info.geometry.sectorSize, 4096u);
        QCOMPARE(info.geometry.clusterSize, 65536u);
        QCOMPARE(info.geometry.recordSize, 4096u);
        QCOMPARE(info.recordCount, std::uint64_t {123456789});
        QCOMPARE(info.validBytes, 123456789ull * 4096);

        w.reset();
        encode(w, MftDone {1, L"reading the MFT failed, error 1117"});
        MftDone done;
        QVERIFY(decode(payloadOf(w.finish(Type::MftDone)), done));
        QCOMPARE(done.status, 1u);
        QCOMPARE(done.error, std::wstring(L"reading the MFT failed, error 1117"));

        w.reset();
        encode(w, QueryJournal {L"E:", 77});
        QueryJournal query;
        QVERIFY(decode(payloadOf(w.finish(Type::QueryJournal)), query));
        QCOMPARE(query.root, std::wstring(L"E:"));
        QCOMPARE(query.serial, 77u);

        w.reset();
        encode(w, ntfs::JournalInfo {0x0123456789ABCDEFull, -5, 1ll << 40});
        ntfs::JournalInfo journal;
        QVERIFY(decode(payloadOf(w.finish(Type::JournalInfo)), journal));
        QCOMPARE(journal.id, 0x0123456789ABCDEFull);
        QCOMPARE(journal.firstUsn, std::int64_t {-5});
        QCOMPARE(journal.nextUsn, std::int64_t {1} << 40);

        w.reset();
        encode(w, FollowJournal {L"C:", 1, 2});
        FollowJournal follow;
        QVERIFY(decode(payloadOf(w.finish(Type::FollowJournal)), follow));
        QCOMPARE(follow.root, std::wstring(L"C:"));
        QCOMPARE(follow.serial, 1u);
        QCOMPARE(follow.journalId, std::uint64_t {2});

        w.reset();
        encode(w, Read {-1, true});
        Read read;
        QVERIFY(decode(payloadOf(w.finish(Type::Read)), read));
        QCOMPARE(read.usn, std::int64_t {-1});
        QVERIFY(read.wait);

        w.reset();
        encode(w, JournalLost {1181});
        JournalLost lost;
        QVERIFY(decode(payloadOf(w.finish(Type::JournalLost)), lost));
        QCOMPARE(lost.error, 1181u);

        w.reset();
        encode(w, CloseVolume {L"F:"});
        CloseVolume close;
        QVERIFY(decode(payloadOf(w.finish(Type::CloseVolume)), close));
        QCOMPARE(close.root, std::wstring(L"F:"));

        w.reset();
        encode(w, Closed {3});
        Closed closed;
        QVERIFY(decode(payloadOf(w.finish(Type::Closed)), closed));
        QCOMPARE(closed.closed, 3u);

        w.reset();
        encode(w, Error {5, L"拒绝访问"});
        Error error;
        QVERIFY(decode(payloadOf(w.finish(Type::Error)), error));
        QCOMPARE(error.code, 5u);
        QCOMPARE(error.message, std::wstring(L"拒绝访问"));

        w.reset();
        encode(w, HelloRefused {static_cast<std::uint32_t>(Refusal::TooManyConnections)});
        HelloRefused refused;
        QVERIFY(decode(payloadOf(w.finish(Type::HelloRefused)), refused));
        QCOMPARE(refused.reason, 2u);

        w.reset();
        HelloOk ok;
        ok.capabilities = 0x80000001;
        encode(w, ok);
        HelloOk okBack;
        QVERIFY(decode(payloadOf(w.finish(Type::HelloOk)), okBack));
        QCOMPARE(okBack.capabilities, 0x80000001u);
        QVERIFY(decode({}, okBack) == false);
    }

    // An MFT block as the service streams it: records parsed from the
    // builder's bytes, written, read back the same, record by record; a
    // free record, one that is no record, an extension record. Then the
    // block mangled every way a bad service could: refused whole.
    void volumeProtocolBlock()
    {
        using namespace volproto;
        RecordBuilder b(0x0003); // in use, folder
        b.standardInformation(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE);
        b.fileName(0x0002'0000'0000'04D2ull, u"Long name 中文", 1);
        const std::u16string longName(255, u'x');
        b.fileName(0x0001'0000'0000'0309ull, longName, 0);
        // Each record's bytes stay: the parsed names point into them.
        std::vector<std::byte> folderBytes = b.finish();
        ntfs::FileRecord folder;
        QVERIFY(ntfs::parseFileRecord(folderBytes.data(), folderBytes.size(), folder));
        RecordBuilder unused(0x0000);
        unused.fileName(5, u"deleted.txt", 1);
        std::vector<std::byte> freeBytes = unused.finish();
        ntfs::FileRecord free;
        QVERIFY(ntfs::parseFileRecord(freeBytes.data(), freeBytes.size(), free));
        RecordBuilder extension(0x0001, 0x0003'0000'0000'002Aull);
        extension.fileName(5, u"link.txt", 0);
        std::vector<std::byte> extBytes = extension.finish();
        ntfs::FileRecord ext;
        QVERIFY(ntfs::parseFileRecord(extBytes.data(), extBytes.size(), ext));

        RecordBuilder fileBuilder(0x0001);
        fileBuilder.fileName(5, u"data.bin", 1);
        fileBuilder.resident(0x80, std::vector<std::byte>(300));
        std::vector<std::byte> fileBytes = fileBuilder.finish();
        ntfs::FileRecord file;
        QVERIFY(ntfs::parseFileRecord(fileBytes.data(), fileBytes.size(), file));
        QVERIFY(file.sizeKnown && file.size == 300);

        BlockWriter bw;
        bw.begin(1000);
        bw.add(folder, true);
        bw.add(free, true);
        bw.add(folder, false); // garbage on the volume
        bw.add(ext, true);
        bw.add(file, true);
        QCOMPARE(bw.count(), 5u);
        const std::span<const std::byte> frame = bw.finish(12345);
        Type type {};
        std::uint32_t length = 0;
        QVERIFY(parseHeader(frame.first<8>(), type, length));
        QCOMPARE(type, Type::MftBlock);
        const std::vector<std::byte> payload(frame.begin() + Writer::kHeaderBytes, frame.end());
        QCOMPARE(std::size_t {length}, payload.size());

        BlockReader br;
        QVERIFY(br.load(std::vector<std::byte>(payload)));
        QCOMPARE(br.firstRecord(), 1000u);
        QCOMPARE(br.count(), 5u);
        QCOMPARE(br.bytesDone(), std::uint64_t {12345});
        ntfs::FileRecord out;
        QVERIFY(br.record(0, out));
        QVERIFY(out.inUse && out.directory && !out.hasAttributeList);
        QCOMPARE(out.baseRecord, 0u);
        QCOMPARE(out.attributes, std::uint32_t {FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE});
        QCOMPARE(out.modified, folder.modified);
        QCOMPARE(out.names.size(), std::size_t {2});
        QCOMPARE(out.names[0].parent, 0x0002'0000'0000'04D2ull);
        QCOMPARE(std::u16string(out.names[0].name), std::u16string(u"Long name 中文"));
        QCOMPARE(std::u16string(out.names[1].name), longName);
        QVERIFY(br.record(1, out));
        QVERIFY(!out.inUse && out.names.empty());
        QVERIFY(!br.record(2, out));
        QVERIFY(br.record(3, out));
        QVERIFY(out.inUse);
        QCOMPARE(out.baseRecord, 42u);
        QCOMPARE(out.names.size(), std::size_t {1});
        QCOMPARE(std::u16string(out.names[0].name), std::u16string(u"link.txt"));
        QVERIFY(!out.sizeKnown);
        QVERIFY(br.record(4, out));
        QVERIFY(out.sizeKnown);
        QCOMPARE(out.size, std::uint64_t {300});
        QVERIFY(!br.record(5, out)); // past the end

        // The other end at minor 1: no sizes in the records, either way.
        BlockWriter old;
        old.begin(1000, false);
        old.add(file, true);
        const std::span<const std::byte> oldFrame = old.finish(1);
        const std::vector<std::byte> oldPayload(oldFrame.begin() + Writer::kHeaderBytes, oldFrame.end());
        BlockReader oldReader;
        QVERIFY(!oldReader.load(std::vector<std::byte>(oldPayload), true)); // told wrongly: cut short
        QVERIFY(oldReader.load(std::vector<std::byte>(oldPayload), false));
        QVERIFY(oldReader.record(0, out));
        QVERIFY(out.inUse && !out.sizeKnown && out.size == 0);
        QCOMPARE(std::u16string(out.names[0].name), std::u16string(u"data.bin"));

        // Mangled: each refused whole, and the reader keeps nothing.
        const auto refused = [&](std::vector<std::byte> bad, const char* what) {
            BlockReader r;
            QVERIFY2(!r.load(std::move(bad)), what);
            QCOMPARE(r.count(), 0u);
        };
        std::vector<std::byte> bad = payload;
        put<std::uint32_t>(bad, 4, 6); // one record more than there is
        refused(bad, "count past the end");
        bad = payload;
        bad.pop_back(); // the last name cut short
        refused(bad, "cut short");
        bad = payload;
        bad.push_back(std::byte {0}); // a byte after the last record
        refused(bad, "trailing bytes");
        bad = payload;
        bad[16] = std::byte {3}; // a record kind that does not exist
        refused(bad, "unknown kind");
        bad = payload;
        bad[16 + 1 + 4 + 4 + 8 + 1] = std::byte {5}; // the folder says five names, has two
        refused(bad, "names past the end");
        bad = payload;
        put<std::uint16_t>(bad, 16 + 1 + 4 + 4 + 8 + 1 + 1 + 8, 0); // an empty name
        refused(bad, "empty name");
        refused({}, "nothing at all");
        bad.assign(16, std::byte {0}); // an empty block is fine
        BlockReader empty;
        QVERIFY(empty.load(std::move(bad)));
        QCOMPARE(empty.count(), 0u);
    }

    void usnApply()
    {
        // A synthetic volume: the root's name is a real folder, for the steps
        // that look at the disk (hard links, folders moved in from outside).
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::wstring rootPath = QDir::toNativeSeparators(tmp.path()).toStdWString();
        const std::string rootName = wtf8::fromUtf16(wtf8::view(rootPath));
        CrawlRules rules;
        rules.excludedNames.push_back(L"skip");
        const Crawler crawler(rules);
        FileIndex index;
        const EntryId root = index.addRoot(rootName);
        const EntryId docs = index.add(root, "docs", EntryFlag::Directory);
        const EntryId dst = index.add(root, "dst", EntryFlag::Directory);
        const EntryId a = index.add(docs, "a.txt", 0);
        index.add(docs, "x", 0);
        const EntryId upperX = index.add(docs, "X", 0);
        index.setFolderRecord(root, ntfs::kRootRecord, root);
        index.setFolderRecord(root, 100, docs);
        index.setFolderRecord(root, 200, dst);

        constexpr DWORD kDir = FILE_ATTRIBUTE_DIRECTORY;
        const auto ref = [](std::uint32_t record) { return std::uint64_t {record} | (std::uint64_t {1} << 48); };
        std::int64_t usn = 0;
        const auto rec = [&](std::uint32_t file, std::uint32_t parent, std::uint32_t reason, DWORD attributes,
                             std::u16string_view name) {
            return ntfs::UsnRecord {ref(file), ref(parent), usn += 100, reason, attributes, name};
        };
        // (findPath starts from a drive letter; this root is a whole folder path.)
        const auto childOf = [&](EntryId parent, std::string_view name) { return index.findChild(parent, name); };

        const std::vector<ntfs::UsnRecord> batch {
            rec(1000, 100, USN_REASON_FILE_CREATE, 0, u"new.txt"),
            rec(1000, 100, USN_REASON_FILE_CREATE | USN_REASON_DATA_EXTEND, 0, u"new.txt"), // a repeat
            rec(1000, 100, USN_REASON_FILE_CREATE | USN_REASON_DATA_EXTEND | USN_REASON_CLOSE, 0, u"new.txt"),
            rec(300, 100, USN_REASON_FILE_CREATE, kDir, u"sub"),
            rec(1001, 300, USN_REASON_FILE_CREATE | USN_REASON_CLOSE, 0, u"inner.txt"),
            rec(301, 100, USN_REASON_FILE_CREATE, kDir, u"skip"), // excluded by name
            rec(1002, 301, USN_REASON_FILE_CREATE, 0, u"ignored.txt"), // inside it
            rec(1003, 100, USN_REASON_RENAME_OLD_NAME, 0, u"a.txt"), // a.txt -> dst\b.txt
            rec(1003, 200, USN_REASON_RENAME_NEW_NAME, 0, u"b.txt"),
            rec(1003, 200, USN_REASON_RENAME_NEW_NAME | USN_REASON_CLOSE, 0, u"b.txt"),
            rec(300, 100, USN_REASON_RENAME_OLD_NAME, kDir, u"sub"), // sub -> dst\Sub2, with its file
            rec(300, 200, USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME, kDir, u"Sub2"),
            rec(1004, 100, USN_REASON_FILE_DELETE | USN_REASON_CLOSE, 0, u"X"), // only "X", not "x"
            rec(1000, 100, USN_REASON_BASIC_INFO_CHANGE, FILE_ATTRIBUTE_HIDDEN, u"new.txt"),
            // 0x40000 on disk: extended attributes (code integrity), not a cloud file.
            rec(1007, 100, USN_REASON_FILE_CREATE | USN_REASON_CLOSE, 0x40000 | FILE_ATTRIBUTE_ARCHIVE, u"signed.dll"),
        };
        std::vector<EntryId> written;
        UsnApplier applier(index, crawler, rootName, [&](std::span<const EntryId> ids) {
            written.insert(written.end(), ids.begin(), ids.end());
        });
        applier.apply(batch, {});
        QVERIFY(!written.empty());
        QVERIFY(std::all_of(written.begin(), written.end(), [&](EntryId id) { return index.name(id) == "new.txt"; }));
        written.clear();
        applier.apply(std::vector {rec(1007, 100, USN_REASON_DATA_OVERWRITE | USN_REASON_CLOSE, 0, u"signed.dll"),
                          rec(1008, 100, USN_REASON_DATA_EXTEND, 0, u"unknown.txt")}, {});
        QCOMPARE(written.size(), std::size_t {1}); // not the file the index does not have
        QCOMPARE(index.name(written[0]), std::string_view("signed.dll"));
        const EntryId sub = index.folderByRecord(root, 300);
        QVERIFY(sub != kNoEntry);
        QCOMPARE(index.path(sub), QString::fromStdWString(rootPath + L"\\dst\\Sub2"));
        QVERIFY(childOf(sub, "inner.txt") != kNoEntry);
        QCOMPARE(childOf(docs, "skip"), kNoEntry);
        QCOMPARE(index.folderByRecord(root, 301), kNoEntry);
        QCOMPARE(childOf(docs, "a.txt"), kNoEntry);
        QCOMPARE(childOf(dst, "b.txt"), a); // moved, same entry
        QVERIFY(index.entry(upperX).isDeleted());
        QVERIFY(childOf(docs, "x") != kNoEntry);
        const EntryId created = childOf(docs, "new.txt");
        QVERIFY(created != kNoEntry);
        QVERIFY(index.entry(created).flags & EntryFlag::Hidden);
        const EntryId dll = childOf(docs, "signed.dll");
        QVERIFY(dll != kNoEntry);
        QVERIFY(!(index.entry(dll).flags & EntryFlag::Offline));
        QCOMPARE(applier.safePosition(usn + 1), usn + 1);

        const std::vector<ntfs::UsnRecord> second {
            rec(1001, 300, USN_REASON_FILE_DELETE | USN_REASON_CLOSE, 0, u"inner.txt"),
            rec(300, 200, USN_REASON_FILE_DELETE | USN_REASON_CLOSE, kDir, u"Sub2"),
            rec(1000, 100, USN_REASON_RENAME_OLD_NAME, 0, u"new.txt"), // into a folder outside the index
            rec(1000, 9999, USN_REASON_RENAME_NEW_NAME, 0, u"new.txt"),
            rec(1005, 200, USN_REASON_RENAME_OLD_NAME, 0, u"b.txt"), // its new name comes later
        };
        applier.apply(second, {});
        QCOMPARE(index.folderByRecord(root, 300), kNoEntry);
        QVERIFY(index.entry(sub).isDeleted());
        QCOMPARE(childOf(docs, "new.txt"), kNoEntry);
        QCOMPARE(applier.safePosition(usn + 1), usn); // replay from the pending rename

        // Replaying everything (as after a restart) changes nothing.
        const std::size_t live = index.liveCount();
        UsnApplier replay(index, crawler, rootName);
        replay.apply(batch, {});
        replay.apply(second, {});
        QCOMPARE(index.liveCount(), live);
        QCOMPARE(childOf(docs, "new.txt"), kNoEntry);
        QCOMPARE(childOf(dst, "b.txt"), a);
        QCOMPARE(index.folderByRecord(root, 300), kNoEntry);

        // Hard links: the disk tells whether the named link was added or removed.
        QVERIFY(QDir(tmp.path()).mkpath(u"dst"_s));
        QFile link(tmp.filePath(u"dst/link.txt"_s));
        QVERIFY(link.open(QIODevice::WriteOnly));
        link.close();
        applier.apply(std::vector {rec(1006, 200, USN_REASON_HARD_LINK_CHANGE | USN_REASON_CLOSE, 0, u"link.txt")}, {});
        QVERIFY(childOf(dst, "link.txt") != kNoEntry);
        QVERIFY(link.remove());
        applier.apply(std::vector {rec(1006, 200, USN_REASON_HARD_LINK_CHANGE | USN_REASON_CLOSE, 0, u"link.txt")}, {});
        QCOMPARE(childOf(dst, "link.txt"), kNoEntry);

        // A folder moved in from outside the index is walked, and its subfolders registered.
        QVERIFY(QDir(tmp.path()).mkpath(u"docs/moved/deeper"_s));
        QFile deep(tmp.filePath(u"docs/moved/deeper/deep.txt"_s));
        QVERIFY(deep.open(QIODevice::WriteOnly));
        deep.close();
        applier.apply(std::vector {rec(500, 100, USN_REASON_RENAME_NEW_NAME | USN_REASON_CLOSE, kDir, u"moved")}, {});
        const EntryId moved = index.folderByRecord(root, 500);
        QVERIFY(moved != kNoEntry);
        const EntryId deeper = childOf(moved, "deeper");
        QVERIFY(deeper != kNoEntry);
        QVERIFY(childOf(deeper, "deep.txt") != kNoEntry);
        QCOMPARE(index.folderByRecord(root, recordOfPath(tmp.filePath(u"docs/moved/deeper"_s))), deeper);
    }

    void interning()
    {
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        index.setInterning(true);
        const EntryId a = index.add(root, "index.js", 0);
        const EntryId dir = index.add(root, "lib", EntryFlag::Directory);
        const EntryId b = index.add(dir, "index.js", 0);
        const EntryId c = index.add(dir, "INDEX.js", 0); // different bytes: not shared
        QCOMPARE(index.entry(a).nameOffset, index.entry(b).nameOffset);
        QVERIFY(index.entry(a).nameOffset != index.entry(c).nameOffset);
        for (int i = 0; i < 200000; ++i) // forces the table to grow
            index.add(dir, "n" + std::to_string(i % 70000), 0);
        index.setInterning(false);
        const EntryId d = index.add(dir, "index.js", 0); // interning off: stored again
        QVERIFY(index.entry(d).nameOffset != index.entry(a).nameOffset);
        QCOMPARE(index.name(d), std::string_view("index.js"));
        QCOMPARE(index.path(b), u"C:\\lib\\index.js"_s);
    }

    void nameStorage()
    {
        // Names of every length (one- and two-byte lengths), with and without
        // extensions and Chinese characters, over many chunks: each read back
        // as it was, and each once in a pass over the stored names.
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId dir = index.add(root, "目录.d", EntryFlag::Directory);
        QVERIFY(index.entry(dir).flags & EntryFlag::Han);
        QCOMPARE(index.extLength(index.entry(dir)), std::size_t {0}); // a folder's
        std::vector<std::pair<EntryId, std::string>> added;
        for (int i = 0; i < 40000; ++i) {
            std::string name = "n" + std::to_string(i) + std::string(static_cast<std::size_t>(i % 300), 'x');
            if (i % 3 == 0)
                name += ".txt";
            if (i % 7 == 0)
                name += "报告";
            added.push_back({index.add(dir, name, 0), name});
        }
        const EntryId cut = index.add(dir, std::string(FileIndex::kMaxName + 10, 'y'), 0);
        QCOMPARE(index.name(cut), std::string_view(std::string(FileIndex::kMaxName, 'y')));
        const EntryId longExtension = index.add(dir, "a." + std::string(200, 'e'), 0);
        QCOMPARE(index.extLength(index.entry(longExtension)), std::size_t {0}); // 128 bytes or more: none
        for (const auto& [id, name] : added) {
            QCOMPARE(index.name(id), std::string_view(name));
            QCOMPARE(index.extLength(index.entry(id)), std::size_t {extensionLength(name, false)});
            QCOMPARE((index.entry(id).flags & EntryFlag::Han) != 0, pinyin::hasHan(name));
        }

        QVERIFY(index.nameChunkCount() > 10);
        std::set<std::uint32_t> offsets;
        bool same = true;
        for (std::size_t c = 0; c < index.nameChunkCount(); ++c) {
            index.forEachName(c, [&](std::uint32_t offset, std::string_view name) {
                same = same && index.nameAt(offset) == name;
                offsets.insert(offset);
            });
        }
        QVERIFY(same);
        QCOMPARE(offsets.size(), added.size() + 4); // and the root, the folder, the two above
        for (std::size_t i = 0; i < index.slotCount(); ++i)
            QVERIFY(offsets.contains(index.entry(static_cast<EntryId>(i)).nameOffset));

        // After a compaction, the same names.
        for (std::size_t i = 0; i < added.size(); i += 2)
            index.remove(added[i].first);
        index.compact();
        std::size_t live = 0;
        for (std::size_t i = 0; i < index.slotCount(); ++i) {
            const std::string_view name = index.name(static_cast<EntryId>(i));
            live += name.starts_with('n');
        }
        QCOMPARE(live, added.size() / 2);
        QCOMPARE(index.name(index.findPath(L"C:\\目录.d\\n1x")), std::string_view("n1x"));
    }

    void compact()
    {
        SampleTree t;
        FileIndex& index = t.index;
        index.setInterning(true);
        const EntryId lib = index.add(t.me, "lib", EntryFlag::Directory);
        const EntryId js1 = index.add(lib, "index.js", 0);
        const EntryId js2 = index.add(t.docs, "index.js", 0);
        index.setInterning(false);
        QCOMPARE(index.entry(js1).nameOffset, index.entry(js2).nameOffset);
        QCOMPARE(index.compact(), std::size_t {0}); // nothing removed yet

        index.remove(t.notes);
        index.remove(lib); // with its child
        QVERIFY(index.move(t.report, t.users, "Report-final.docx")); // leaves the old name behind
        index.setModified(t.report, 1'700'000'000);
        index.setModified(js2, 1'600'000'000);
        QCOMPARE(index.slotCount(), std::size_t {10});
        QCOMPARE(index.liveCount(), std::size_t {7});

        // Ids kept elsewhere are renumbered alongside, in the same order.
        const std::uint64_t generation = index.generation();
        std::vector<std::pair<EntryId, QString>> kept;
        for (EntryId id = 0; id < index.slotCount(); ++id)
            kept.emplace_back(id, index.entry(id).isDeleted() ? QString() : index.path(id));
        {
            const FileIndex::IdPin pin = index.pinIds();
            QVERIFY(index.idsPinned());
        }
        QVERIFY(!index.idsPinned());
        QCOMPARE(index.compact(false, [&](const FileIndex::Renumber& renumber) {
            for (auto& [id, path] : kept)
                id = renumber(id);
        }), std::size_t {3});
        QCOMPARE(index.generation(), generation + 1);
        for (const auto& [id, path] : kept)
            QCOMPARE(id == kNoEntry ? QString() : index.path(id), path);
        QCOMPARE(index.slotCount(), std::size_t {7});
        QCOMPARE(index.liveCount(), std::size_t {7});
        for (std::size_t i = 0; i < index.slotCount(); ++i)
            QVERIFY(!index.entry(static_cast<EntryId>(i)).isDeleted());
        QCOMPARE(index.roots().size(), std::size_t {1});
        QCOMPARE(index.path(index.roots().front()), u"C:\\"_s);

        const EntryId report = index.findPath(L"C:\\Users\\Report-final.docx");
        QVERIFY(report != kNoEntry);
        QCOMPARE(index.extLength(index.entry(report)), std::size_t {4});
        QCOMPARE(index.modified(report), FileTime {1'700'000'000}); // the times move with their entries
        const EntryId js = index.findPath(L"C:\\Users\\me\\Documents\\index.js");
        QVERIFY(js != kNoEntry);
        QCOMPARE(index.modified(js), FileTime {1'600'000'000});
        QCOMPARE(index.modified(index.add(t.users, "new.txt", 0)), FileTime {0}); // a slot used again starts with none
        QCOMPARE(index.path(js), u"C:\\Users\\me\\Documents\\index.js"_s);
        QVERIFY(index.entry(index.findPath(L"C:\\Windows")).flags & EntryFlag::LowPriority);
        QCOMPARE(index.findPath(L"C:\\Users\\me\\lib"), kNoEntry);
        QCOMPARE(index.findPath(L"C:\\Users\\me\\Documents\\notes.txt"), kNoEntry);

        // The sibling lists still work both ways: remove the middle child, add new ones.
        const EntryId users = index.findPath(L"C:\\Users");
        const EntryId me = index.findPath(L"C:\\Users\\me");
        QCOMPARE(index.remove(me), std::size_t {3});
        QCOMPARE(index.findChild(users, "Report-final.docx"), report);
        const EntryId added = index.add(users, "new.txt", 0);
        QCOMPARE(index.path(added), u"C:\\Users\\new.txt"_s);
        QCOMPARE(index.compact(), std::size_t {3});
        QCOMPARE(index.path(index.findPath(L"C:\\Users\\new.txt")), u"C:\\Users\\new.txt"_s);
        QCOMPARE(index.path(index.findPath(L"C:\\Users\\Report-final.docx")), u"C:\\Users\\Report-final.docx"_s);

        // Large enough to span several chunks and grow the name map.
        FileIndex big;
        const EntryId root = big.addRoot("D:");
        std::vector<EntryId> files;
        big.setInterning(true);
        for (int i = 0; i < 200000; ++i)
            files.push_back(big.add(root, "f" + std::to_string(i % 5000) + ".txt", 0));
        big.setInterning(false);
        for (std::size_t i = 0; i < files.size(); i += 3)
            big.remove(files[i]);
        const std::size_t live = big.liveCount();
        QCOMPARE(big.compact(), files.size() + 1 - live);
        QCOMPARE(big.slotCount(), live);
        std::set<std::uint32_t> offsets; // shared names stay shared: 5000 file names + "D:"
        for (std::size_t i = 0; i < live; ++i)
            offsets.insert(big.entry(static_cast<EntryId>(i)).nameOffset);
        QCOMPARE(offsets.size(), std::size_t {5001});
        std::size_t children = 0;
        for (EntryId c = big.entry(big.roots().front()).firstChild; c != kNoEntry; c = big.entry(c).nextSibling) {
            QCOMPARE(big.entry(c).parent, big.roots().front());
            ++children;
        }
        QCOMPARE(children, live - 1);
        QCOMPARE(big.name(static_cast<EntryId>(live - 1)), std::string_view("f4999.txt"));
    }

    void syncWithDisk()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir base(tmp.path());
        const auto touch = [&](const QString& rel) {
            QFile f(base.filePath(rel));
            QVERIFY(f.open(QIODevice::WriteOnly));
        };
        QVERIFY(base.mkpath(u"docs/sub"_s));
        QVERIFY(base.mkpath(u"node_modules/pkg"_s));
        QVERIFY(base.mkpath(u"skip/inner"_s));
        QVERIFY(base.mkpath(u".tool"_s));
        touch(u"docs/a.txt"_s);
        touch(u"docs/sub/b.txt"_s);
        touch(u"node_modules/pkg/index.js"_s);
        touch(u"skip/inner/c.txt"_s);

        const std::wstring rootPath = QDir::toNativeSeparators(tmp.path()).toStdWString();
        CrawlRules rules;
        rules.excludedPaths.push_back(rootPath + L"\\skip");
        rules.lowPriorityNames.push_back(L"node_modules");
        const Crawler crawler(rules);

        FileIndex index;
        const EntryId root = index.addRoot("X:");
        QVERIFY(crawler.sync(index, {{root, rootPath, 0}}, 4, false, {}));
        // docs, sub, a, b, node_modules, pkg, index.js, .tool  (+ root)
        QCOMPARE(index.liveCount(), std::size_t {9});
        QCOMPARE(index.findPath(L"X:\\skip"), kNoEntry);
        const EntryId js = index.findPath(L"X:\\node_modules\\pkg\\index.js");
        QVERIFY(js != kNoEntry);
        QVERIFY(index.entry(js).flags & EntryFlag::LowPriority); // inherited
        QVERIFY(index.entry(index.findPath(L"X:\\.tool")).flags & EntryFlag::LowPriority);

        // Change the disk, then sync again in place.
        QVERIFY(QFile::remove(base.filePath(u"docs/a.txt"_s)));
        QVERIFY(QDir(base.filePath(u"docs/sub"_s)).removeRecursively());
        touch(u"docs/new.txt"_s);
        QVERIFY(base.rename(u"node_modules"_s, u"Node_Modules2"_s));
        const EntryId docs = index.findPath(L"X:\\docs");
        QVERIFY(crawler.sync(index, {{root, rootPath, 0}}, 2, true, {}));
        QCOMPARE(index.findPath(L"X:\\docs"), docs); // unchanged folders keep their entry
        QCOMPARE(index.findPath(L"X:\\docs\\a.txt"), kNoEntry);
        QCOMPARE(index.findPath(L"X:\\docs\\sub"), kNoEntry);
        QVERIFY(index.findPath(L"X:\\docs\\new.txt") != kNoEntry);
        QVERIFY(index.findPath(L"X:\\Node_Modules2\\pkg\\index.js") != kNoEntry);
        QVERIFY(!(index.entry(index.findPath(L"X:\\Node_Modules2")).flags & EntryFlag::LowPriority));
        QCOMPARE(index.liveCount(), std::size_t {7});

        // A case-only rename keeps the entry; a stale entry that differs only
        // in case from one on disk is dropped instead of piling up.
        const EntryId fresh = index.findPath(L"X:\\docs\\new.txt");
        QVERIFY(base.rename(u"docs/new.txt"_s, u"docs/NEW.txt"_s));
        touch(u"docs/x.txt"_s);
        index.add(docs, "x.txt", 0);
        index.add(docs, "X.TXT", 0);
        QVERIFY(crawler.sync(index, {{root, rootPath, 0}}, 2, true, {}));
        QCOMPARE(index.findPath(L"X:\\docs\\NEW.txt"), fresh);
        QCOMPARE(index.name(fresh), std::string_view("NEW.txt"));
        std::vector<std::string> names;
        for (EntryId c = index.entry(docs).firstChild; c != kNoEntry; c = index.entry(c).nextSibling)
            names.emplace_back(index.name(c));
        std::sort(names.begin(), names.end());
        QCOMPARE(names, (std::vector<std::string> {"NEW.txt", "x.txt"}));
    }

    void uncPaths()
    {
        QCOMPARE(unc::normalize(L"\\\\Host\\Share\\"), std::wstring(L"\\\\Host\\Share"));
        QCOMPARE(unc::normalize(L"//nas/docs/a//b/"), std::wstring(L"\\\\nas\\docs\\a\\b"));
        QCOMPARE(unc::normalize(L"\\\\192.168.1.10\\资料"), std::wstring(L"\\\\192.168.1.10\\资料"));
        QVERIFY(unc::normalize(L"\\\\host").empty()); // a server, no share
        QVERIFY(unc::normalize(L"\\\\host\\").empty());
        QVERIFY(unc::normalize(L"\\\\?\\UNC\\host\\share").empty());
        QVERIFY(unc::normalize(L"C:\\x").empty());
        QVERIFY(unc::normalize(L"\\\\host\\sh*re").empty());
        QVERIFY(unc::isUnc(L"\\\\nas\\docs") && !unc::isUnc(L"\\\\.\\C:") && !unc::isUnc(L"C:\\"));
        QCOMPARE(unc::host(L"\\\\nas\\docs\\a"), std::wstring_view(L"nas"));
        QCOMPARE(unc::host(L"\\\\192.168.1.10"), std::wstring_view(L"192.168.1.10")); // signing in to a server
        QVERIFY(unc::shareRoot(L"\\\\192.168.1.10").empty());
        QCOMPARE(unc::shareRoot(L"\\\\nas\\docs\\a\\b"), std::wstring_view(L"\\\\nas\\docs"));
        QVERIFY(unc::contains(L"\\\\nas\\docs", L"\\\\NAS\\Docs\\a"));
        QVERIFY(unc::contains(L"\\\\nas\\docs", L"\\\\nas\\docs"));
        QVERIFY(!unc::contains(L"\\\\nas\\docs", L"\\\\nas\\docs2\\a"));
    }

    // Shares are roots named by their path, next to the drives; a path finds
    // the share it is on by the longest name that starts it.
    void indexUncRoots()
    {
        FileIndex index;
        const EntryId c = index.addRoot("C:");
        const EntryId share = ShareIndexer::ensureRoot(index, L"\\\\nas\\docs");
        const EntryId share2 = ShareIndexer::ensureRoot(index, L"\\\\nas\\docs2");
        const EntryId deeper = ShareIndexer::ensureRoot(index, L"\\\\nas\\docs\\team");
        QCOMPARE(ShareIndexer::ensureRoot(index, L"\\\\NAS\\Docs"), share); // made once
        QVERIFY(index.entry(share).flags & EntryFlag::Remote);
        const EntryId a = index.add(share, "a", EntryFlag::Directory | EntryFlag::Remote);
        const EntryId file = index.add(a, "plan.xlsx", EntryFlag::Remote);
        const EntryId other = index.add(share2, "a", EntryFlag::Directory | EntryFlag::Remote);
        const EntryId team = index.add(deeper, "x.txt", EntryFlag::Remote);
        index.add(c, "a", EntryFlag::Directory);

        QCOMPARE(index.path(share), u"\\\\nas\\docs\\"_s); // a root alone ends in '\', as "C:\" does
        QCOMPARE(index.path(file), u"\\\\nas\\docs\\a\\plan.xlsx"_s);
        QCOMPARE(index.findPath(L"\\\\NAS\\docs\\A\\plan.xlsx"), file);
        QCOMPARE(index.findPath(L"\\\\nas\\docs"), share);
        QCOMPARE(index.findPath(L"\\\\nas\\docs\\"), share);
        QCOMPARE(index.findPath(L"\\\\nas\\docs2\\a"), other); // not docs + "2\a"
        QCOMPARE(index.findPath(L"\\\\nas\\docs\\team\\x.txt"), team); // the longest
        QCOMPARE(index.findPath(L"\\\\nas\\other\\a"), kNoEntry);
        QCOMPARE(index.findPath(L"\\\\nas"), kNoEntry);
        QVERIFY(index.findPath(L"C:\\a") != kNoEntry);

        // A share no longer set is dropped from an index loaded from disk.
        QCOMPARE(ShareIndexer::dropUnknownRoots(index, {{L"\\\\nas\\docs", {}}}), std::size_t {4}); // 2 roots, 2 items
        QCOMPARE(index.findPath(L"\\\\nas\\docs2\\a"), kNoEntry);
        QCOMPARE(index.findPath(L"\\\\nas\\docs\\team\\x.txt"), kNoEntry);
        QCOMPARE(index.findPath(L"\\\\nas\\docs\\a\\plan.xlsx"), file);
        QVERIFY(index.findPath(L"C:\\a") != kNoEntry);
    }

    // Excluded names count from the share on: a server called like an
    // excluded folder is no reason to leave its shares out.
    void uncExcludedPath()
    {
        const Crawler crawler(CrawlRules {{L"\\\\nas\\docs\\private"}, {L"nas", L"#recycle", L"docs"}, {}, {}});
        QVERIFY(!crawler.isExcludedPath(L"\\\\nas\\docs\\a\\b.txt"));
        QVERIFY(crawler.isExcludedPath(L"\\\\nas\\docs\\#recycle\\b.txt"));
        QVERIFY(crawler.isExcludedPath(L"\\\\nas\\docs\\a\\docs\\b.txt"));
        QVERIFY(crawler.isExcludedPath(L"\\\\nas\\docs\\private\\b.txt"));
    }

    // A whole drive excluded (GitHub #17): "X:\" in the settings is "X:" here.
    void driveExcludedPath()
    {
        const Crawler crawler(CrawlRules {{L"X:", L"C:\\skip"}, {}, {}, {}});
        QVERIFY(crawler.isExcludedDir(L"X:", L"X:"));
        QVERIFY(crawler.isExcludedPath(L"X:"));
        QVERIFY(crawler.isExcludedPath(L"x:\\a\\b.txt"));
        QVERIFY(crawler.isExcludedPath(L"C:\\skip\\b.txt"));
        QVERIFY(!crawler.isExcludedPath(L"C:\\a\\b.txt"));
        QVERIFY(!crawler.isExcludedPath(L"Y:\\a"));
    }

    // A sync that defers removals keeps what is gone until told; a folder
    // that cannot be read keeps what it had.
    void syncDefersRemovals()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir base(tmp.path());
        const auto touch = [&](const QString& rel) {
            QFile f(base.filePath(rel));
            QVERIFY(f.open(QIODevice::WriteOnly));
        };
        QVERIFY(base.mkpath(u"docs/sub"_s));
        QVERIFY(base.mkpath(u"locked"_s));
        touch(u"docs/a.txt"_s);
        touch(u"docs/sub/b.txt"_s);
        touch(u"locked/c.txt"_s);
        const std::wstring rootPath = QDir::toNativeSeparators(tmp.path()).toStdWString();
        const Crawler crawler(CrawlRules {});
        FileIndex index;
        const EntryId root = index.addRoot("X:");
        QVERIFY(crawler.sync(index, {{root, rootPath, 0}}, 2, false, {}));
        QCOMPARE(index.liveCount(), std::size_t {7});

        QVERIFY(QFile::remove(base.filePath(u"docs/a.txt"_s)));
        QVERIFY(QDir(base.filePath(u"docs/sub"_s)).removeRecursively());
        QVERIFY(QFile::remove(base.filePath(u"locked/c.txt"_s)));
        touch(u"docs/new.txt"_s);
        // "locked" cannot be read this time: as if access was denied.
        const Crawler::Lister list = [&](const Crawler::Root& dir, DirListing& out) {
            if (dir.path.ends_with(L"\\locked"))
                return false;
            return crawler.listDirectory(dir, out);
        };
        std::vector<EntryId> gone;
        Crawler::SyncStats stats;
        QVERIFY(crawler.sync(index, {{root, rootPath, 0}}, 2, false, {}, list, &gone, &stats));
        QVERIFY(index.findPath(L"X:\\docs\\new.txt") != kNoEntry); // added at once
        QVERIFY(index.findPath(L"X:\\docs\\a.txt") != kNoEntry); // removed only when told
        QVERIFY(index.findPath(L"X:\\locked\\c.txt") != kNoEntry);
        QCOMPARE(gone.size(), std::size_t {2}); // a.txt and sub (with b.txt in it)
        QCOMPARE(stats.failed, std::size_t {1});
        QCOMPARE(stats.listed, std::size_t {2}); // the root and docs
        for (const EntryId id : gone)
            index.remove(id);
        QCOMPARE(index.findPath(L"X:\\docs\\a.txt"), kNoEntry);
        QCOMPARE(index.findPath(L"X:\\docs\\sub\\b.txt"), kNoEntry);
        QVERIFY(index.findPath(L"X:\\locked\\c.txt") != kNoEntry);
    }

    void shareProblems()
    {
        using P = share::Problem;
        QCOMPARE(share::classify(ERROR_BAD_NETPATH, true), P::Offline);
        QCOMPARE(share::classify(ERROR_NETNAME_DELETED, false), P::Dropped);
        QCOMPARE(share::classify(ERROR_BAD_NET_NAME, true), P::NotFound);
        QCOMPARE(share::classify(ERROR_ACCESS_DENIED, true), P::NeedsSignIn);
        QCOMPARE(share::classify(ERROR_ACCESS_DENIED, false), P::Denied); // one folder kept from us
        QCOMPARE(share::classify(ERROR_LOGON_FAILURE, false), P::NeedsSignIn);
        QCOMPARE(share::classify(1272, true), P::GuestBlocked); // ERROR_SMB_GUEST_LOGON_BLOCKED
        QVERIFY(share::affectsShare(P::Offline) && share::affectsShare(P::NeedsSignIn));
        QVERIFY(!share::affectsShare(P::Denied));
    }

    // The content search never opens a file on a network share.
    void remoteNotInContent()
    {
        FileIndex index;
        const EntryId share = ShareIndexer::ensureRoot(index, L"\\\\nas\\docs");
        const EntryId remote = index.add(share, "notes.txt", EntryFlag::Remote);
        const EntryId local = index.add(index.addRoot("C:"), "notes.txt", 0);
        ContentFilter filter;
        filter.extensions = ExtensionFilter({u"txt"_s});
        QVERIFY(filter.accepts(index, index.entry(local)));
        QVERIFY(!filter.accepts(index, index.entry(remote)));
    }

    // Shares stay in the snapshot, as roots with their items and when they
    // were last read in full.
    void snapshotKeepsShares()
    {
        FileIndex index;
        const EntryId c = index.addRoot("C:");
        index.add(c, "local.txt", 0);
        const EntryId share = ShareIndexer::ensureRoot(index, L"\\\\192.168.1.10\\资料");
        index.setModified(share, 1'790'000'000);
        const EntryId folder = index.add(share, "2026", EntryFlag::Directory | EntryFlag::Remote);
        index.add(folder, "预算.xlsx", EntryFlag::Remote);
        QTemporaryDir dir;
        const QString file = dir.filePath(u"index.bin"_s);
        QVERIFY(snapshot::save(index, {{L"C:", 1, true}}, {{1, 1}}, CrawlRules {}, file));
        const auto contents = snapshot::load(file);
        QVERIFY(contents && contents->index);
        const FileIndex& loaded = *contents->index;
        const EntryId budget = loaded.findPath(L"\\\\192.168.1.10\\资料\\2026\\预算.xlsx");
        QVERIFY(budget != kNoEntry);
        QVERIFY(loaded.entry(budget).flags & EntryFlag::Remote);
        QCOMPARE(loaded.modified(loaded.findPath(L"\\\\192.168.1.10\\资料")), FileTime {1'790'000'000});
        QCOMPARE(loaded.liveCount(), std::size_t {5});
    }

    // The whole of it over a real share: this computer's own Users share,
    // where there is one. Read, then verified, a few files gone (believed), then
    // most of them (not believed, until accepted).
    void shareIndexerLoopback()
    {
        using namespace std::chrono_literals;
        QTemporaryDir tmp; // under %TEMP%: C:\Users\<me>\AppData\Local\Temp
        QVERIFY(tmp.isValid());
        const QString local = QDir::toNativeSeparators(tmp.path());
        const QString users = QDir::toNativeSeparators(QDir(qEnvironmentVariable("SystemDrive") + u"/Users"_s).path());
        if (!local.startsWith(users + u'\\', Qt::CaseInsensitive))
            QSKIP("the temporary folder is not under C:\\Users");
        const std::wstring path = L"\\\\localhost\\Users" + local.mid(users.size()).toStdWString();
        if (!QFileInfo(QString::fromStdWString(path)).isDir())
            QSKIP("no Users share on this computer");
        // Asked as Windows asks (SMB 1 framing offering SMB 2), Windows answers in SMB 2.
        QVERIFY(share::probe(L"localhost", 1500ms, true) == share::Answer::Smb2);
        QDir base(tmp.path());
        QVERIFY(base.mkpath(u"many"_s));
        for (int i = 0; i < 1500; ++i) {
            QFile f(base.filePath(u"many/f%1.txt"_s.arg(i)));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        for (const QString& name : {u"a.txt"_s, u"b.txt"_s, u"._a.txt"_s, u".DS_Store"_s}) {
            QFile f(base.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }

        auto index = std::make_shared<FileIndex>();
        ShareIndexer::Options o;
        o.shares = {{path, {}}};
        o.excludedFiles = {L"._*", L".DS_Store"};
        o.verifyEvery = 0min;
        o.verifyEveryUnwatched = 0min;
        o.watch = false; // what goes is found by the next pass alone, as on a server that does not tell
        ShareIndexer shares({[&] { return index; }, [] { return true; }}, o, {});
        const auto status = [&] {
            const auto all = shares.statuses();
            return all.empty() ? ShareIndexer::Status {} : all.front();
        };
        const auto settled = [&](ShareIndexer::State state) {
            for (int i = 0; i < 300; ++i) { // 30 s
                const auto s = status();
                if (s.state == state)
                    return true;
                std::this_thread::sleep_for(100ms);
            }
            return false;
        };
        shares.start();
        QVERIFY(settled(ShareIndexer::State::Online));
        QCOMPARE(status().items, std::size_t {1503}); // many, its 1500, a.txt, b.txt: no ._a.txt, .DS_Store
        {
            const auto lock = index->readLock();
            const EntryId a = index->findPath(path + L"\\a.txt");
            QVERIFY(a != kNoEntry);
            QVERIFY(index->entry(a).flags & EntryFlag::Remote);
            QCOMPARE(index->findPath(path + L"\\._a.txt"), kNoEntry);
        }

        QVERIFY(QFile::remove(base.filePath(u"b.txt"_s)));
        for (int i = 0; i < 1400; ++i)
            QVERIFY(QFile::remove(base.filePath(u"many/f%1.txt"_s.arg(i))));
        shares.verifyNow(path);
        QVERIFY(settled(ShareIndexer::State::Suspect));
        QCOMPARE(status().missing, std::size_t {1401});
        QCOMPARE(index->liveCount(), std::size_t {1504}); // nothing removed yet
        shares.acceptMissing(path);
        QVERIFY(settled(ShareIndexer::State::Online));
        QCOMPARE(status().items, std::size_t {102});
        QCOMPARE(index->liveCount(), std::size_t {103});
        shares.stop();
    }

    void nameSearchTopK()
    {
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId dir = index.add(root, "data", EntryFlag::Directory);
        for (int i = 0; i < 70000; ++i)
            index.add(dir, "file_" + std::to_string(i) + ".txt", 0);
        index.add(dir, "file_1", 0); // exact name: must win

        const NameMatcher matcher(ws::parseQuery(u"file_1"_s));
        WorkerPool pool(4);
        const auto out = searchNames(index, matcher, {.limit = 10}, pool, {});
        QVERIFY(!out.cancelled);
        QCOMPARE(out.hits.size(), std::size_t {10});
        QCOMPARE(index.name(out.hits.front().id), std::string_view("file_1"));
        // file_1, file_10..19, ..., file_10000..19999 (.txt), plus the exact name
        QCOMPARE(out.totalMatches, std::size_t {1 + 10 + 100 + 1000 + 10000 + 1});
        for (std::size_t i = 1; i < out.hits.size(); ++i)
            QVERIFY(out.hits[i - 1].score >= out.hits[i].score);

        const auto cancelled = searchNames(index, matcher, {.limit = 10}, pool, [] { return true; });
        QVERIFY(cancelled.cancelled);
    }

    void nameSearchTestsNamesFirst()
    {
        // A search rules names out first (NameMatcher::nameMayMatch), once per
        // stored name: it must find what matching every entry finds, for every
        // kind of query.
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const char* folders[] = {"src", "app", "报告", "node_modules", "proj", "tmp", "Data 2025"};
        const char* files[] = {"readme.md", "Report.pdf", "报告2025.docx", "index.js", "a.c", "abc.txt", "我的文档.txt",
            "data.json", "x", "TEST.PDF", "draft report.doc", "app.log", "src", "合同.xlsx", "README"};
        std::vector<EntryId> level {root};
        for (int depth = 0; depth < 3; ++depth) {
            std::vector<EntryId> next;
            for (const EntryId parent : level) {
                for (const char* folder : folders)
                    next.push_back(index.add(parent, folder, EntryFlag::Directory));
                for (const char* file : files)
                    index.add(parent, file, 0);
            }
            level = std::move(next);
        }
        WorkerPool pool(4);
        for (const QString& q : {u"report"_s, u"bg"_s, u"baogao"_s, u"报告"_s, u"src app"_s, u"report !pdf"_s,
                 u"!node_modules\\ readme"_s, u"a?c*"_s, u"*.pdf"_s, u"ext:pdf"_s, u"ext:docx 报告"_s, u"proj\\readme"_s,
                 u"2025"_s, u"README"_s, u"!*.log app"_s, u"wd"_s, u"x"_s, u"src\\"_s, u"\"draft report\""_s,
                 u"tmp\\ data"_s, u"ext:txt,md"_s, u"!src\\ src"_s}) {
            const NameMatcher matcher(ws::parseQuery(q));
            std::size_t matches = 0;
            for (std::size_t i = 0; i < index.slotCount(); ++i) {
                const Entry& e = index.entry(static_cast<EntryId>(i));
                if ((e.flags & EntryFlag::Root) || matcher.match(index, e) < 0)
                    continue;
                ++matches;
                QVERIFY2(matcher.nameMayMatch(index.name(e), index.nameExtLength(e.nameOffset),
                             index.nameHasHan(e.nameOffset)),
                    qPrintable(q));
            }
            const auto out = searchNames(index, matcher, {.limit = 1000}, pool, {});
            QVERIFY2(out.totalMatches == matches, qPrintable(q));
            QVERIFY2(matches > 0 || q == u"!src\\ src"_s, qPrintable(q));
        }
    }

    void nameSearchKindsAndTimes()
    {
        constexpr FileTime T = 1'700'000'000; // 2023: before 1980 is no time (knownTime)
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId dir = index.add(root, "work", EntryFlag::Directory);
        // Many files that match better than the folders: a list of the best
        // 20 of all would hold no folder.
        for (int i = 0; i < 50; ++i)
            index.setModified(index.add(dir, "plan" + std::to_string(i) + ".txt", 0), T + 1000 + i);
        const EntryId folderA = index.add(dir, "old plans", EntryFlag::Directory);
        const EntryId folderB = index.add(dir, "my plans", EntryFlag::Directory);
        index.setModified(folderA, T + 10);
        index.setModified(folderB, T + 2000);
        const NameMatcher matcher(ws::parseQuery(u"plan"_s));
        WorkerPool pool(4);
        const auto isDir = [&](const NameHit& h) { return index.entry(h.id).isDir(); };

        const auto all = searchNames(index, matcher, {.limit = 20}, pool, {});
        QCOMPARE(all.hits.size(), std::size_t {20});
        QVERIFY(std::none_of(all.hits.begin(), all.hits.end(), isDir));

        const auto apart = searchNames(index, matcher, {.limit = 20, .kindsApart = true}, pool, {});
        QCOMPARE(apart.hits.size(), std::size_t {22}); // 20 files, and the 2 folders
        QCOMPARE(std::count_if(apart.hits.begin(), apart.hits.end(), isDir), std::ptrdiff_t {2});
        QCOMPARE(apart.totalMatches, std::size_t {52});

        const auto folders = searchNames(index, matcher, {.limit = 20, .foldersOnly = true}, pool, {});
        QCOMPARE(folders.hits.size(), std::size_t {2});
        QVERIFY(std::all_of(folders.hits.begin(), folders.hits.end(), isDir));
        QCOMPARE(folders.totalMatches, std::size_t {2});

        // Newest first, whatever the match.
        const auto byTime = searchNames(index, matcher, {.limit = 5, .rankBy = RankBy::Newest}, pool, {});
        QCOMPARE(byTime.hits.size(), std::size_t {5});
        QCOMPARE(byTime.hits[0].id, folderB);
        QCOMPARE(index.name(byTime.hits[1].id), std::string_view("plan49.txt"));
        for (std::size_t i = 1; i < byTime.hits.size(); ++i)
            QVERIFY(byTime.hits[i - 1].modified > byTime.hits[i].modified);

        // Oldest first; a time not known goes last, not first.
        // 1970-01-01 (unpacked from an archive that knew no time) and the
        // DOS epoch are not known either: last too, and said to be 0.
        const EntryId undated = index.add(dir, "plan undated.txt", 0); // modified 0
        const EntryId epoch = index.add(dir, "plan epoch.txt", 0);
        const EntryId dos = index.add(dir, "plan dos.txt", 0);
        index.setModified(epoch, 1);
        index.setModified(dos, 315'532'800 - 8 * 3600); // 1980-01-01 00:00 in UTC+8
        const auto oldest = searchNames(index, matcher, {.limit = 60, .rankBy = RankBy::Oldest}, pool, {});
        QCOMPARE(oldest.hits.size(), std::size_t {55});
        QCOMPARE(oldest.hits[0].id, folderA);
        QCOMPARE(index.name(oldest.hits[1].id), std::string_view("plan0.txt"));
        const std::vector<EntryId> lastThree {oldest.hits[52].id, oldest.hits[53].id, oldest.hits[54].id};
        for (const EntryId id : {undated, epoch, dos})
            QVERIFY(std::ranges::find(lastThree, id) != lastThree.end());
        QVERIFY(std::all_of(oldest.hits.begin() + 52, oldest.hits.end(), [](const NameHit& h) { return h.modified == 0; }));
        for (std::size_t i = 2; i < 52; ++i)
            QVERIFY(oldest.hits[i - 1].modified < oldest.hits[i].modified);
        // Kept to a few, the oldest still: the heap must not let a 0 in.
        const auto oldestFew = searchNames(index, matcher, {.limit = 3, .rankBy = RankBy::Oldest}, pool, {});
        QCOMPARE(oldestFew.hits.size(), std::size_t {3});
        QCOMPARE(oldestFew.hits[0].id, folderA);
        QVERIFY(std::none_of(oldestFew.hits.begin(), oldestFew.hits.end(), [](const NameHit& h) { return h.modified == 0; }));
        const auto newestAll = searchNames(index, matcher, {.limit = 60, .rankBy = RankBy::Newest}, pool, {});
        QVERIFY(std::all_of(newestAll.hits.end() - 3, newestAll.hits.end(), [](const NameHit& h) { return h.modified == 0; }));
        QCOMPARE(knownTime(315'619'200), FileTime {315'619'200});
        QCOMPARE(knownTime(315'532'800), FileTime {0});

        // Equal matches: the one written last first.
        const EntryId a = index.add(root, "same1.txt", 0);
        const EntryId b = index.add(root, "same2.txt", 0);
        index.setModified(a, T + 100);
        index.setModified(b, T + 300);
        const auto ties = searchNames(index, NameMatcher(ws::parseQuery(u"same"_s)), {.limit = 5}, pool, {});
        QCOMPARE(ties.hits.size(), std::size_t {2});
        QCOMPARE(ties.hits[0].score, ties.hits[1].score);
        QCOMPARE(ties.hits[0].id, b);

        // Content matches: the better first, or by time with a time not
        // known (1970 too) last both ways, the better match on a tie.
        const auto row = [](std::uint32_t modified, int relevance) {
            SearchResult r;
            r.modified = modified;
            r.relevance = relevance;
            return r;
        };
        const SearchResult older = row(T + 100, 9), newer = row(T + 200, 1), undatedRow = row(1, 50);
        const SearchResult twin = row(T + 200, 5);
        QVERIFY(rankedBefore(older, newer, RankBy::Match));
        QVERIFY(rankedBefore(newer, older, RankBy::Newest));
        QVERIFY(rankedBefore(older, newer, RankBy::Oldest));
        QVERIFY(rankedBefore(older, undatedRow, RankBy::Oldest));
        QVERIFY(rankedBefore(older, undatedRow, RankBy::Newest));
        QVERIFY(!rankedBefore(undatedRow, older, RankBy::Oldest));
        QVERIFY(rankedBefore(twin, newer, RankBy::Newest));
        QVERIFY(!rankedBefore(newer, twin, RankBy::Oldest));

        // Dated far ahead (an installer's 2076): a clock gone wrong, ordered
        // as not known, last both ways; a day ahead is still trusted.
        const FileTime until = trustedUntil();
        const SearchResult ahead = row(0xC000'0000u, 50); // 2072
        QVERIFY(rankedBefore(newer, ahead, RankBy::Newest));
        QVERIFY(rankedBefore(older, ahead, RankBy::Oldest));
        QCOMPARE(sortTime(0xC000'0000u, until), FileTime {0});
        QCOMPARE(sortTime(until - 3600, until), until - 3600);
        QCOMPARE(sortTime(1, until), FileTime {0});

        // The panel's newest first: the one dated ahead last, its own date
        // still known to the index.
        const EntryId future = index.add(root, "same-ahead.txt", 0);
        index.setModified(future, 0xC000'0000u);
        const auto newestFirst = searchNames(index, NameMatcher(ws::parseQuery(u"same"_s)),
            {.limit = 5, .order = {SortKey::Modified, true}, .trustedUntil = until}, pool, {});
        QVERIFY(!newestFirst.hits.empty());
        QVERIFY(newestFirst.hits.front().id != future);
        QCOMPARE(newestFirst.hits.back().id, future);
        QCOMPARE(newestFirst.hits.back().modified, FileTime {0});
        QCOMPARE(index.modified(future), 0xC000'0000u);
    }

    // 优先显示的文件类型 (PreferredTypes): any case, without the dot; a
    // name's extension as the index has it (after its last dot).
    void preferredTypesTable()
    {
        const PreferredTypes none;
        QVERIFY(none.empty());
        QVERIFY(!none.has("exe"));
        QVERIFY(!none.hasName("curl.exe"));

        const PreferredTypes types({"exe", "PS1", "", "averyveryverylongone", "exe"});
        QVERIFY(!types.empty());
        QVERIFY(types.has("exe"));
        QVERIFY(types.has("EXE"));
        QVERIFY(types.has("ps1"));
        QVERIFY(!types.has("ex"));
        QVERIFY(!types.has("exe2"));
        QVERIFY(!types.has(""));
        QVERIFY(!types.has("averyveryverylongone")); // longer than any extension kept
        QVERIFY(types.hasName("curl.EXE"));
        QVERIFY(types.hasName("node-v22.tar.exe"));
        QVERIFY(!types.hasName("curl.exe.bak"));
        QVERIFY(!types.hasName("exe"));
        QVERIFY(!types.hasName("curl."));

        // At most 32 kept.
        std::vector<std::string> many;
        for (int i = 0; i < 40; ++i)
            many.push_back("x" + std::to_string(i));
        const PreferredTypes full(many);
        QVERIFY(full.has("x31"));
        QVERIFY(!full.has("x32"));
    }

    // The scores the types listed first get (forum #104, #116), in the
    // search over the index: comparable matches go their way, much better
    // ones stay ahead, and a program named just what was typed loses nothing
    // for being in a program folder. Only by match.
    void nameSearchPreferredTypes()
    {
        FileIndex index;
        const EntryId f = index.addRoot("F:");
        const EntryId test = index.add(f, "wsexetest", EntryFlag::Directory);
        const EntryId folder = index.add(test, "curl", EntryFlag::Directory);
        const EntryId header = index.add(test, "curl.h", 0);
        const EntryId exe = index.add(test, "curl.exe", 0);
        index.add(test, "curl-config", 0);
        index.add(test, "libcurl.dll", 0);
        const EntryId notes = index.add(test, "curl tools.txt", 0); // at the start, as:
        const EntryId updater = index.add(test, "curl-updater.exe", 0);
        const EntryId c = index.addRoot("C:");
        const auto aside = static_cast<std::uint8_t>(EntryFlag::Directory | EntryFlag::LowPriority);
        const EntryId windows = index.add(c, "Windows", aside);
        const EntryId system32 = index.add(windows, "System32", aside);
        const EntryId systemCurl = index.add(system32, "curl.exe", EntryFlag::LowPriority);
        const EntryId helper = index.add(system32, "curl-helper.exe", EntryFlag::LowPriority);
        WorkerPool pool(4);
        const NameMatcher matcher(ws::parseQuery(u"curl"_s));
        const PreferredTypes programs({"exe", "bat", "cmd", "ps1", "msi"});

        const auto scores = [&](const NameSearchOptions& options) {
            std::unordered_map<EntryId, int> out;
            for (const NameHit& hit : searchNames(index, matcher, options, pool, {}).hits)
                out[hit.id] = hit.score;
            return out;
        };
        auto plain = scores({.limit = 20, .kindsApart = true});
        QCOMPARE(plain.size(), std::size_t {9});
        // As before: the folder, the header, then the program.
        QVERIFY(plain[folder] > plain[header]);
        QVERIFY(plain[header] > plain[exe]);
        QVERIFY(plain[notes] > plain[updater]);
        QVERIFY(plain[header] > plain[systemCurl]);

        auto preferred = scores({.limit = 20, .kindsApart = true, .preferred = programs});
        QCOMPARE(preferred[exe], plain[exe] + PreferredTypes::kBonus);
        QVERIFY(preferred[exe] > preferred[header]);
        QVERIFY(preferred[updater] > preferred[notes]); // comparable: the program first
        // Much better matches stay ahead: the folder named so, the header named so.
        QVERIFY(preferred[folder] > preferred[updater]);
        QVERIFY(preferred[header] > preferred[updater]);
        // C:\Windows\System32\curl.exe is the curl asked for, in a program folder or not...
        QVERIFY(preferred[systemCurl] > preferred[header]);
        QCOMPARE(preferred[systemCurl], plain[systemCurl] + PreferredTypes::kBonus + 35);
        // ... but the rest of a program folder stays below the user's own.
        QVERIFY(preferred[helper] < preferred[notes]);
        QCOMPARE(preferred[helper], plain[helper] + PreferredTypes::kBonus);
        QCOMPARE(preferred[folder], plain[folder]);

        // By time, or by a column, the scores are as they were (ties only).
        QVERIFY(scores({.limit = 20, .rankBy = RankBy::Newest, .kindsApart = true, .preferred = programs})
            == scores({.limit = 20, .rankBy = RankBy::Newest, .kindsApart = true}));
        QVERIFY(scores({.limit = 20, .order = {SortKey::Name, false}, .preferred = programs})
            == scores({.limit = 20, .order = {SortKey::Name, false}}));
        // Empty: nothing listed first.
        QVERIFY(scores({.limit = 20, .kindsApart = true, .preferred = PreferredTypes()}) == plain);
        // Another list: the header's type.
        auto headers = scores({.limit = 20, .kindsApart = true, .preferred = PreferredTypes({"H"})});
        QVERIFY(headers[header] > headers[exe]);
        QCOMPARE(headers[exe], plain[exe]);
    }

    // The same through the engine (arrange): among the names typed just so,
    // the types listed first come first, in 全部 and 文件, folders first or
    // not; a folder named so still goes before a program that only begins
    // so; by time, nothing changes.
    void preferredTypesFirst()
    {
        QTemporaryDir dir;
        IndexService::Options options;
        options.content.enabled = false;
        IndexService service(dir.path() + u"/index"_s, options);
        {
            const auto index = service.index();
            const auto lock = index->writeLock();
            const EntryId f = index->addRoot("F:");
            const EntryId test = index->add(f, "wsexetest", EntryFlag::Directory);
            index->add(test, "curl", EntryFlag::Directory);
            for (const char* name : {"curl.h", "curl.exe", "curl-config", "libcurl.dll", "tools-helper.exe"})
                index->add(test, name, 0);
            index->add(test, "tools", EntryFlag::Directory);
            const EntryId c = index->addRoot("C:");
            const auto aside = static_cast<std::uint8_t>(EntryFlag::Directory | EntryFlag::LowPriority);
            const EntryId system32 = index->add(index->add(c, "Windows", aside), "System32", aside);
            index->add(system32, "curl.exe", EntryFlag::LowPriority);
            FileTime t = 1'700'000'000;
            for (std::size_t i = 0; i < index->slotCount(); ++i)
                index->setModified(static_cast<EntryId>(i), t += 100); // each a little later
        }
        SearchEngine engine(&service);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        const auto search = [&](const QString& text, Scope scope, const QStringList& preferred,
                                KindOrder kindOrder = KindOrder::FilesFirst, RankBy rankBy = RankBy::Match) {
            spy.clear();
            SearchEngine::Request request;
            request.text = text;
            request.scope = scope;
            request.preferredTypes = preferred;
            request.kindOrder = kindOrder;
            request.rankBy = rankBy;
            engine.submit(std::move(request));
            QStringList paths;
            if (!spy.wait(5000))
                return QStringList {u"(no results)"_s};
            for (const SearchResult& r : spy.at(0).at(1).value<SearchResults>())
                paths.append(r.path);
            return paths;
        };
        const QStringList programs {u"exe"_s, u"bat"_s, u"cmd"_s, u"ps1"_s, u"msi"_s};
        const QString exe = u"F:\\wsexetest\\curl.exe"_s;
        const QString systemExe = u"C:\\Windows\\System32\\curl.exe"_s;
        const QString folder = u"F:\\wsexetest\\curl"_s;
        const QString header = u"F:\\wsexetest\\curl.h"_s;

        // Forum #116: "curl" lists curl.exe first, not the folder curl or curl.h.
        for (const Scope scope : {Scope::All, Scope::Files}) {
            const QStringList found = search(u"curl"_s, scope, programs);
            QCOMPARE(found.mid(0, 4), (QStringList {exe, systemExe, folder, header}));
            QCOMPARE(found.size(), 6);
        }
        QCOMPARE(search(u"curl"_s, Scope::All, programs, KindOrder::FoldersFirst).mid(0, 4),
            (QStringList {exe, systemExe, folder, header}));
        // Empty: as before.
        QCOMPARE(search(u"curl"_s, Scope::All, {}).mid(0, 4), (QStringList {folder, header, exe, systemExe}));
        // Another list.
        QCOMPARE(search(u"curl"_s, Scope::All, {u"h"_s}).first(), header);
        // The folder named "tools" before the program that only begins so.
        QCOMPARE(search(u"tools"_s, Scope::All, programs),
            (QStringList {u"F:\\wsexetest\\tools"_s, u"F:\\wsexetest\\tools-helper.exe"_s}));
        // Newest first: the list does not care.
        QCOMPARE(search(u"curl"_s, Scope::Files, programs, KindOrder::FilesFirst, RankBy::Newest),
            search(u"curl"_s, Scope::Files, {}, KindOrder::FilesFirst, RankBy::Newest));
    }

    void newestFiles()
    {
        constexpr FileTime T = 1'700'000'000;
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId docs = index.add(root, "Docs", EntryFlag::Directory);
        const EntryId build = index.add(root, "build", EntryFlag::Directory);
        index.setModified(docs, T + 950);
        index.setModified(build, T + 100);
        const EntryId appData = index.add(root, "AppData", EntryFlag::Directory | EntryFlag::LowPriority);
        const auto file = [&](EntryId folder, const char* name, FileTime when, std::uint8_t flags = 0) {
            const EntryId id = index.add(folder, name, flags);
            index.setModified(id, when);
            return id;
        };
        const EntryId report = file(docs, "report.docx", T + 900);
        const EntryId plan = file(docs, "plan.xlsx", T + 800);
        file(docs, "notes.txt", T + 700); // a third from Docs: over its share
        const EntryId exe = file(build, "app.exe", T + 850);
        file(build, "main.obj", T + 2000); // compiler output
        file(build, "app.pdb", T + 2000);
        file(build, ".ninja_log", T + 2000); // a dot-name
        file(docs, "~$report.docx", T + 2000); // Office's lock file
        file(docs, "movie.mp4.crdownload", T + 2000); // a download under way
        file(docs, "secret.txt", T + 2000, EntryFlag::Hidden);
        file(appData, "cache.dat", T + 2000, EntryFlag::LowPriority);
        file(docs, "camera.jpg", T + 5000); // dated in the future
        file(docs, "undated.txt", 0);
        file(docs, "from-a-zip.txt", 1); // 1970-01-01: no time
        // A build folder (CMakeCache.txt in it): all it holds, however new.
        const EntryId outDir = index.add(root, "out", EntryFlag::Directory);
        file(outDir, "CMakeCache.txt", T + 1500);
        file(index.add(outDir, "bin", EntryFlag::Directory), "app.exe", T + 3000);
        file(outDir, "compile_commands.json", T + 3000);
        file(index.add(root, "CMakeFiles", EntryFlag::Directory), "x.json", T + 3000);
        const EntryId folder = index.add(docs, "Project", EntryFlag::Directory);
        index.setModified(folder, T + 3000);
        WorkerPool pool(4);

        const auto ids = [](const NewestOutput& out) {
            std::vector<EntryId> ids;
            for (const NameHit& hit : out.hits)
                ids.push_back(hit.id);
            return ids;
        };
        const NewestOutput out = ws::newestFiles(index, {.limit = 5, .notAfter = T + 4000}, pool, {});
        QVERIFY(!out.cancelled);
        QCOMPARE(ids(out), (std::vector<EntryId> {report, exe, plan}));

        // The folders of those files, each once, by their newest file (not
        // by their own time: Project's is newer, but nothing in it is).
        const NewestOutput folders = ws::newestFiles(index, {.limit = 5, .folders = true, .notAfter = T + 4000}, pool, {});
        QCOMPARE(ids(folders), (std::vector<EntryId> {docs, build}));
        QCOMPARE(folders.hits[0].modified, FileTime {T + 900});

        // An archive unpacked with new times, many folders deep: three from
        // it, the rest from elsewhere.
        const EntryId pack = index.add(root, "pack", EntryFlag::Directory);
        for (int a = 0; a < 6; ++a) {
            const EntryId chapter = index.add(pack, "part" + std::to_string(a), EntryFlag::Directory);
            for (int f = 0; f < 4; ++f)
                file(chapter, ("pic" + std::to_string(f) + ".jpg").c_str(), T + 3500);
        }
        const NewestOutput mixed = ws::newestFiles(index, {.limit = 5, .notAfter = T + 4000}, pool, {});
        QCOMPARE(mixed.hits.size(), std::size_t {5});
        QCOMPARE(std::count_if(mixed.hits.begin(), mixed.hits.end(), [&](const NameHit& h) { return h.modified == T + 3500; }),
            std::ptrdiff_t {3});
        QCOMPARE(mixed.hits[3].id, report);

        QVERIFY(ws::isWorkFile("Report.TMP", false));
        QVERIFY(ws::isWorkFile("~WRL0001.tmp", false));
        QVERIFY(!ws::isWorkFile("report.docx", false));
        QVERIFY(!ws::isWorkFile("Makefile", false));
        QVERIFY(!ws::isWorkFile("logs.v2", true)); // a folder: its name only
        QVERIFY(ws::isWorkFile("28090b2f866c9be9746cf4e6457c181b", false));
        QVERIFY(ws::isWorkFile("f76b2dcdd318e482d5b110bfbcb42f18_t.jpg", false));
        QVERIFY(!ws::isWorkFile("20261010123456789.jpg", false)); // digits alone
        QVERIFY(!ws::isWorkFile("deadbeef.txt", false)); // short
    }

    void relativePath()
    {
        FileIndex index;
        const EntryId root = index.addRoot("F:");
        const EntryId tools = index.add(root, "tools", EntryFlag::Directory);
        const EntryId out = index.add(index.add(tools, "work", EntryFlag::Directory), "out", EntryFlag::Directory);
        const EntryId covers = index.add(out, "covers", EntryFlag::Directory);
        index.add(covers, "og.png", 0);
        const EntryId coversFile = index.add(out, "Covers", 0); // a file of the name too
        index.add(index.add(root, "old-tools", EntryFlag::Directory), "work", EntryFlag::Directory);
        const EntryId other = index.add(index.add(index.add(root, "tools", EntryFlag::Directory), "out",
            EntryFlag::Directory), "covers", EntryFlag::Directory); // tools\out\covers, no work between
        WorkerPool pool(4);
        const auto find = [&](std::vector<std::string> names, bool foldersOnly) {
            auto ids = findRelativePath(index, names, foldersOnly, 10, pool, {});
            std::ranges::sort(ids);
            return ids;
        };

        QCOMPARE(find({"tools", "work", "out", "covers"}, true), std::vector<EntryId> {covers});
        QCOMPARE(find({"TOOLS", "Work", "out", "covers"}, false), (std::vector<EntryId> {covers, coversFile}));
        QCOMPARE(find({"work", "out", "covers"}, true), std::vector<EntryId> {covers}); // from anywhere along the way
        QCOMPARE(find({"out", "covers"}, true), (std::vector<EntryId> {covers, other}));
        QCOMPARE(find({"tools", "out", "covers"}, true), std::vector<EntryId> {other}); // one inside the other, no gaps
        QCOMPARE(find({"tools", "work"}, true), std::vector<EntryId> {index.entry(out).parent}); // not old-tools\work
        QVERIFY(find({"tools", "cov"}, false).empty()); // still being typed
        QVERIFY(findRelativePath(index, {"out", "covers"}, true, 10, pool, [] { return true; }).empty());
        QCOMPARE(findRelativePath(index, {"out", "covers"}, true, 1, pool, {}).size(), std::size_t {1});
    }

    void exactName()
    {
        const NameMatcher readme(ws::parseQuery(u"README"_s));
        QVERIFY(readme.isExactName("readme", true));
        QVERIFY(readme.isExactName("README.md", false));
        QVERIFY(!readme.isExactName("README.md", true)); // a folder's name has no extension
        QVERIFY(!readme.isExactName("readme-old.md", false));
        QVERIFY(!NameMatcher(ws::parseQuery(u"read me"_s)).isExactName("read me", true)); // two terms
        QVERIFY(!NameMatcher(ws::parseQuery(u"read*"_s)).isExactName("readme", true));
    }

    void appSearch()
    {
        const auto app = [](const QString& name, const QString& id, const QString& target,
                             AppKind kind = AppKind::Desktop) {
            AppInfo a;
            a.name = name;
            a.id = id;
            a.target = target;
            a.kind = kind;
            a.prepare();
            return a;
        };
        const AppList apps {
            app(u"Visual Studio Code"_s, u"Microsoft.VisualStudioCode"_s, uR"(C:\VS Code\Code.exe)"_s),
            app(u"Word"_s, u"Microsoft.Office.WINWORD.EXE.15"_s, uR"(C:\Office\WINWORD.EXE)"_s),
            app(u"计算器"_s, u"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"_s, uR"(C:\Program Files\WindowsApps\Calc)"_s,
                AppKind::Store),
            app(u"Telegram"_s, u"Telegram.TelegramDesktop"_s, uR"(C:\Telegram\Telegram.exe)"_s),
            app(u"Uninstall Telegram"_s, uR"(C:\Telegram\unins000.exe)"_s, uR"(C:\Telegram\unins000.exe)"_s),
            app(u"Telegram FAQ"_s, uR"(C:\Telegram\faq.html)"_s, uR"(C:\Telegram\faq.html)"_s),
            // English first in the user's languages: Windows names it in English.
            app(u"Notepad"_s, u"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App"_s, uR"(C:\Program Files\WindowsApps\Notepad)"_s,
                AppKind::Store),
        };
        const auto names = [&](const QString& text, const QStringList& history = {}) {
            const ParsedQuery query = ws::parseQuery(text);
            const NameMatcher matcher(query);
            QStringList out;
            for (const AppHit& hit : searchApps(apps, query, matcher, history))
                out.append(apps[hit.index].name);
            return out;
        };
        // The name a row shows: the one it was found by.
        const auto shown = [&](const QString& text) {
            const ParsedQuery query = ws::parseQuery(text);
            const NameMatcher matcher(query);
            QStringList out;
            for (const AppHit& hit : searchApps(apps, query, matcher, {}))
                out.append(hit.otherName >= 0 ? apps[hit.index].otherNames[hit.otherName] : apps[hit.index].name);
            return out;
        };

        QCOMPARE(apps[0].initials, std::string("vsc"));
        QCOMPARE(apps[1].program, std::string("WINWORD"));
        QVERIFY(apps[2].program.empty()); // packaged: no program file of its own
        QVERIFY(!apps[3].auxiliary);
        QVERIFY(apps[4].auxiliary); // uninstaller
        QVERIFY(apps[5].auxiliary); // a document

        QCOMPARE(names(u"jsq"_s), QStringList {u"计算器"_s}); // pinyin initials
        QCOMPARE(names(u"jisuan"_s), QStringList {u"计算器"_s});
        QCOMPARE(names(u"vsc"_s), QStringList {u"Visual Studio Code"_s}); // initials of the words
        QCOMPARE(names(u"winword"_s), QStringList {u"Word"_s}); // the program's file name
        QCOMPARE(names(u"telegram"_s), (QStringList {u"Telegram"_s, u"Telegram FAQ"_s, u"Uninstall Telegram"_s}));
        QCOMPARE(names(u"telegram !faq"_s), (QStringList {u"Telegram"_s, u"Uninstall Telegram"_s}));
        // Windows' own apps by their names in the other language too.
        QCOMPARE(apps[2].otherNames, QStringList {u"Calculator"_s});
        QCOMPARE(apps[6].otherNames, QStringList {u"记事本"_s});
        QVERIFY(apps[0].otherNames.isEmpty());
        QCOMPARE(shown(u"记事本"_s), QStringList {u"记事本"_s});
        QCOMPARE(shown(u"jsb"_s), QStringList {u"记事本"_s}); // and its pinyin
        QCOMPARE(shown(u"note"_s), QStringList {u"Notepad"_s});
        QCOMPARE(shown(u"calc"_s), QStringList {u"Calculator"_s});
        QCOMPARE(shown(u"计算器"_s), QStringList {u"计算器"_s});
        QVERIFY(names(u"ext:exe"_s).isEmpty()); // file syntax: never an app
        QVERIFY(names(uR"(vs\code)"_s).isEmpty());
        QVERIFY(names(QString()).isEmpty());

        // Recently opened apps rank higher.
        const QStringList history {uR"(D:\notes.txt)"_s, apps[4].launchPath()};
        QCOMPARE(names(u"telegram"_s, history),
            (QStringList {u"Telegram"_s, u"Uninstall Telegram"_s, u"Telegram FAQ"_s}));

        QCOMPARE(apps[2].launchPath(), u"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"_s);
        QCOMPARE(appIdOf(apps[2].launchPath()), apps[2].id);
        QVERIFY(isAppLaunchPath(u"SHELL:appsfolder\\x"_s));
        QVERIFY(appIdOf(uR"(C:\Telegram\Telegram.exe)"_s).isEmpty());
    }

    void historyCounts()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(u"history.txt"_s);
        {
            // Kept before counts were: each opened once, at an unknown time.
            QFile old(file);
            QVERIFY(old.open(QIODevice::WriteOnly));
            old.write("C:\\a.txt\nD:\\b.txt\n");
        }
        History history(file, 3);
        history.load();
        QCOMPARE(history.items(), (QStringList {uR"(C:\a.txt)"_s, uR"(D:\b.txt)"_s}));
        QCOMPARE(history.use(uR"(c:\A.TXT)"_s).count, 1); // any case
        QCOMPARE(history.use(uR"(c:\A.TXT)"_s).last, 0);

        history.record(uR"(D:\b.txt)"_s, 1000);
        history.record(uR"(D:\b.txt)"_s, 2000);
        history.record(uR"(E:\c.txt)"_s, 3000);
        history.record(uR"(F:\d.txt)"_s, 4000); // over capacity: the oldest goes
        QCOMPARE(history.items(), (QStringList {uR"(F:\d.txt)"_s, uR"(E:\c.txt)"_s, uR"(D:\b.txt)"_s}));
        QCOMPARE(history.use(uR"(D:\b.txt)"_s).count, 3);
        QCOMPARE(history.use(uR"(C:\a.txt)"_s).count, 0);

        History again(file, 3);
        again.load();
        QCOMPARE(again.items(), history.items());
        QCOMPARE(again.use(uR"(D:\b.txt)"_s).count, 3);
        QCOMPARE(again.use(uR"(D:\b.txt)"_s).last, 2000);
        again.remove(uR"(d:\B.txt)"_s);
        QCOMPARE(again.use(uR"(D:\b.txt)"_s).count, 0);
    }

    void frequentAppRanking()
    {
        const auto app = [](const QString& name, const QString& id, const QString& target,
                             AppKind kind = AppKind::Desktop) {
            AppInfo a;
            a.name = name;
            a.id = id;
            a.target = target;
            a.kind = kind;
            a.prepare();
            return a;
        };
        const AppList apps {
            app(u"Code"_s, u"Microsoft.VisualStudioCode"_s, uR"(C:\VS Code\Code.exe)"_s),
            app(u"Word"_s, u"Microsoft.Office.WINWORD.EXE.15"_s, uR"(C:\Office\WINWORD.EXE)"_s),
            app(u"计算器"_s, u"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"_s, uR"(C:\WindowsApps\Calc)"_s,
                AppKind::Store),
            app(u"Uninstall Code"_s, uR"(C:\VS Code\unins000.exe)"_s, uR"(C:\VS Code\unins000.exe)"_s),
            app(u"Win顺"_s, uR"(C:\WinShun\WinShun.exe)"_s, uR"(C:\WinShun\WinShun.exe)"_s),
            app(u"Notes"_s, uR"(C:\Notes\Notes.exe)"_s, uR"(C:\Notes\Notes.exe)"_s),
        };
        constexpr qint64 day = 86'400'000;
        const qint64 now = 1000 * day;
        QTemporaryDir dir;
        History history(dir.filePath(u"history.txt"_s));
        const auto names = [&](const std::vector<AppUse>& windows, std::size_t limit = 6) {
            QStringList out;
            for (const std::size_t i : frequentApps(apps, history, windows, now, limit))
                out.append(apps[i].name);
            return out;
        };
        QVERIFY(names({}).isEmpty());

        // Opened from Win顺: often beats lately, within the same week.
        history.record(apps[0].launchPath(), now - 2 * day);
        history.record(apps[0].launchPath(), now - day);
        history.record(apps[2].launchPath(), now - 1000);
        history.record(uR"(D:\report.docx)"_s, now); // not an app
        history.record(apps[3].launchPath(), now); // an uninstaller
        history.record(apps[4].launchPath(), now); // Win顺 itself
        QCOMPARE(names({}), (QStringList {u"Code"_s, u"计算器"_s}));

        // Then what Windows saw started lately: by id, or by the program an
        // AppUserModelID app starts; not what it saw long ago, nor twice.
        const std::vector<AppUse> windows {
            {uR"(C:\Office\WINWORD.EXE)"_s, 1, now - 3 * day}, // Word's id is an AppUserModelID
            {uR"(C:\Notes\Notes.exe)"_s, 5, now - 200 * day}, // long ago
            {u"Microsoft.VisualStudioCode"_s, 9, now - day}, // already in
            {uR"(C:\Unknown\x.exe)"_s, 9, now - day}, // not an app
        };
        QCOMPARE(names(windows), (QStringList {u"Code"_s, u"计算器"_s, u"Word"_s}));
        QCOMPARE(names(windows, 1), QStringList {u"Code"_s});
    }

    void settingsIndexParsing()
    {
        // As Windows ships them: a newer file (pages as <Node>), a Control
        // Panel task, and an older file's entry (<PageID>, <PolicyIds>).
        const QByteArray xml = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?><PCSettings>"
                               "<SearchableContent IncludeWithFeature=\"X\">"
                               "<Filename>AAA_SettingsPageNetworkEthernet</Filename>"
                               "<ApplicationInformation><FontFamily>Segoe Fluent Icons</FontFamily><Glyph>x</Glyph>"
                               "</ApplicationInformation>"
                               "<SettingIdentity><SettingPaths>"
                               "<Path><Node Type=\"Page\">SettingsPageNetworkEthernet</Node><Node Type=\"Group\">G</Node></Path>"
                               "<Path><Node Type=\"Page\">SettingsPageOther</Node></Path>"
                               "</SettingPaths></SettingIdentity>"
                               "<SettingInformation><Description>@{windows?ms-resource://a/b}</Description>"
                               "<HighKeywords>@{windows?ms-resource://a/c}</HighKeywords></SettingInformation>"
                               "</SearchableContent>"
                               "<SearchableContent><Filename>Classic_{9EF86966}</Filename>"
                               "<ApplicationInformation><DeepLink>Microsoft.WindowsFirewall\\PageConfigureApps</DeepLink>"
                               "<Icon>%SystemRoot%\\System32\\netcenter.dll,-1</Icon></ApplicationInformation>"
                               "<SettingIdentity><PageID>{8E908FC9}</PageID><HostID>{12B1697E}</HostID>"
                               "<Condition>shcond://v1#IsServer;1</Condition></SettingIdentity>"
                               "<SettingInformation><Description>@shell32.dll,-24389</Description>"
                               "<Keywords>@@shell32.dll,-25174@shell32.dll,-25173</Keywords></SettingInformation>"
                               "</SearchableContent>"
                               "<SearchableContent><Filename>AAA_SettingsPageAudio</Filename>"
                               "<SettingIdentity><SettingPaths><Path><PageID>SettingsPageAudio</PageID>"
                               "<PolicyIds>apps-volume;sound;sound-devices</PolicyIds></Path></SettingPaths></SettingIdentity>"
                               "<SettingInformation><Description>Sound</Description></SettingInformation>"
                               "</SearchableContent></PCSettings>";
        const std::vector<SettingsIndexEntry> entries = parseSettingsIndex(xml);
        QCOMPARE(entries.size(), std::size_t {3});
        QCOMPARE(entries[0].key, u"AAA_SettingsPageNetworkEthernet"_s);
        QCOMPARE(entries[0].page, u"SettingsPageNetworkEthernet"_s); // the first path's page
        QVERIFY(entries[0].deepLink.isEmpty());
        QCOMPARE(entries[0].description, u"@{windows?ms-resource://a/b}"_s);
        QCOMPARE(entries[0].keywords, QStringList {u"@{windows?ms-resource://a/c}"_s});
        QCOMPARE(entries[1].deepLink, uR"(Microsoft.WindowsFirewall\PageConfigureApps)"_s);
        QCOMPARE(entries[1].icon, uR"(%SystemRoot%\System32\netcenter.dll,-1)"_s);
        QCOMPARE(entries[1].host, u"{12B1697E}"_s);
        QCOMPARE(entries[1].condition, u"shcond://v1#IsServer;1"_s);
        QCOMPARE(entries[1].keywords, (QStringList {u"@shell32.dll,-25174"_s, u"@shell32.dll,-25173"_s}));
        QCOMPARE(entries[2].page, u"SettingsPageAudio"_s);
        QCOMPARE(entries[2].policyIds, u"apps-volume;sound;sound-devices"_s);
        QCOMPARE(entries[2].description, u"Sound"_s);

        // Which of a page's ms-settings: names opens the page itself.
        QCOMPARE(choosePageUri(u"SettingsPageAudio"_s, u"apps-volume;audio-outputdevices;sound;sound-devices"_s), u"sound"_s);
        QCOMPARE(choosePageUri(u"SettingsPageInstalledApps"_s, u"appsfeatures;appsfeatures-app;installed-apps"_s),
            u"installed-apps"_s); // named by the page
        QCOMPARE(choosePageUri(u"SettingsPageNetworkAirplaneMode"_s, u"network-airplanemode;proximity"_s),
            u"network-airplanemode"_s);
        QCOMPARE(choosePageUri(u"SettingsPageNetworkManageAdapterOptions"_s,
                     u"network-advancedsettings;network-advancedsharing"_s),
            u"network-advancedsettings"_s); // nothing to go by: the first
        QCOMPARE(choosePageUri(u"SettingsPageX"_s, u"display"_s), u"display"_s);
        QVERIFY(choosePageUri(u"SettingsPageX"_s, QString()).isEmpty());
    }

    void placeCommands()
    {
        const QString windir = QDir::toNativeSeparators(qEnvironmentVariable("WINDIR"));
        QCOMPARE(commandForDeepLink(u"Microsoft.DeviceManager"_s),
            uR"(%windir%\system32\control.exe /name Microsoft.DeviceManager)"_s);
        QCOMPARE(commandForDeepLink(uR"(Microsoft.WindowsFirewall\PageConfigureApps)"_s),
            uR"(%windir%\system32\control.exe /name Microsoft.WindowsFirewall /page PageConfigureApps)"_s);
        QCOMPARE(commandForDeepLink(uR"(%windir%\system32\dccw.exe)"_s), uR"(%windir%\system32\dccw.exe)"_s);
        QCOMPARE(commandForDeepLink(u"shell:::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}"_s),
            u"shell:::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}"_s);
        QVERIFY(commandForDeepLink(u"AccountProtection"_s).isEmpty()); // Windows Security's own
        QVERIFY(commandForDeepLink(QString()).isEmpty());

        using Split = std::pair<QString, QString>;
        QCOMPARE(splitCommand(uR"(%windir%\system32\control.exe /name Microsoft.DeviceManager)"_s),
            (Split {windir + uR"(\system32\control.exe)"_s, u"/name Microsoft.DeviceManager"_s}));
        QCOMPARE(splitCommand(u"ms-settings:display"_s), (Split {u"ms-settings:display"_s, QString()}));
        QCOMPARE(splitCommand(uR"(shell:::{26EE0668}\3\::{7007ACC7})"_s), (Split {uR"(shell:::{26EE0668}\3\::{7007ACC7})"_s, QString()}));
        QCOMPARE(splitCommand(u"windowsdefender://threat"_s), (Split {u"windowsdefender://threat"_s, QString()}));
        QCOMPARE(splitCommand(u"\"C:\\Program Files\\x.exe\" -a b"_s), (Split {uR"(C:\Program Files\x.exe)"_s, u"-a b"_s}));
        QCOMPARE(splitCommand(u"mmsys.cpl"_s), (Split {u"mmsys.cpl"_s, QString()}));
        QCOMPARE(splitCommand(u"%windir%"_s), (Split {windir, QString()}));

        QCOMPARE(describeCommand(uR"(%windir%\system32\mmc.exe %windir%\system32\diskmgmt.msc)"_s), u"diskmgmt.msc"_s);
        QCOMPARE(describeCommand(uR"(%windir%\system32\control.exe mmsys.cpl)"_s), u"mmsys.cpl"_s);
        QCOMPARE(describeCommand(u"mmsys.cpl"_s), u"mmsys.cpl"_s);
        QCOMPARE(describeCommand(u"shell:startup"_s), u"shell:startup"_s);
        QCOMPARE(describeCommand(u"%windir%"_s), windir); // a folder: where it is
        QCOMPARE(describeCommand(uR"(%windir%\system32\rundll32.exe sysdm.cpl,EditEnvironmentVariables)"_s),
            u"rundll32.exe sysdm.cpl,EditEnvironmentVariables"_s);

        QCOMPARE(placePath(u"AAA_SettingsPageAudio"_s), u"winshun-place:AAA_SettingsPageAudio"_s);
        QVERIFY(isPlacePath(placePath(u"x"_s)));
        QCOMPARE(placeKeyOf(placePath(u"winshun/disk-management"_s)), u"winshun/disk-management"_s);
        QVERIFY(placeKeyOf(uR"(C:\x.txt)"_s).isEmpty());
        QVERIFY(!isPlacePath(u"shell:AppsFolder\\x"_s));
    }

    void placeExtras()
    {
        const auto place = [](const QString& name, const QString& command, bool page = false) {
            PlaceInfo p;
            p.name = name;
            p.key = name;
            p.command = command;
            p.kind = AppKind::Setting;
            p.page = page;
            return p;
        };
        PlaceList places {
            place(u"视差背景"_s, u"ms-settings:personalization-background"_s),
            place(u"背景图像设置"_s, u"ms-settings:personalization-background"_s, true),
            place(u"检查防火墙状态"_s, u"FW"_s),
            place(u"Windows Defender 防火墙"_s, u"fw"_s),
        };
        const QString text = u"# comment\n"
                             u"[ms-settings:personalization-background]\n"
                             u"keywords = 壁纸; 桌面背景；换壁纸 ;\n"
                             u"\n"
                             u"[fw]\n"
                             u"keywords = 防火墙\n"
                             u"[ms-settings:nowhere]\n"
                             u"keywords = x\n"
                             u"[disk-management]\n"
                             u"name = 磁盘管理 | Disk Management\n"
                             u"open = diskmgmt.msc\n"
                             u"keywords = 分区; diskmgmt\n"
                             u"[virus]\n"
                             u"name = 病毒和威胁防护 | Virus & threat protection\n"
                             u"open = windowsdefender://threat\n"
                             u"kind = security\n"
                             u"icon = app:Microsoft.SecHealthUI_8wekyb3d8bbwe!SecHealthUI\n"_s;
        const QStringList unmatched = applyPlaceExtras(places, text, true);
        QCOMPARE(unmatched, QStringList {u"ms-settings:nowhere"_s});
        QVERIFY(places[0].ownKeywords.isEmpty()); // a task on the page: the page itself gets them
        QCOMPARE(places[1].ownKeywords, (QStringList {u"壁纸"_s, u"桌面背景"_s, u"换壁纸"_s}));
        QCOMPARE(places[2].ownKeywords, QStringList {u"防火墙"_s}); // no page among them: each one, any case
        QCOMPARE(places[3].ownKeywords, QStringList {u"防火墙"_s});
        QCOMPARE(places.size(), std::size_t {6});
        QCOMPARE(places[4].name, u"磁盘管理"_s);
        QCOMPARE(places[4].key, u"winshun/disk-management"_s);
        QCOMPARE(places[4].command, u"diskmgmt.msc"_s);
        QVERIFY(places[4].kind == AppKind::Tool);
        QCOMPARE(places[4].ownKeywords, (QStringList {u"分区"_s, u"diskmgmt"_s}));
        QVERIFY(places[5].kind == AppKind::Security);
        QCOMPARE(places[5].icon, u"app:Microsoft.SecHealthUI_8wekyb3d8bbwe!SecHealthUI"_s);

        PlaceList english;
        applyPlaceExtras(english, text, false);
        QCOMPARE(english[0].name, u"Disk Management"_s);
    }

    void placeSearch()
    {
        const auto place = [](const QString& name, const QString& command, const QStringList& keywords,
                               const QStringList& own = {}) {
            PlaceInfo p;
            p.name = name;
            p.key = u"k/"_s + name;
            p.command = command;
            p.keywords = keywords;
            p.ownKeywords = own;
            p.prepare();
            return p;
        };
        const PlaceList places {
            place(u"查看网络连接"_s, u"shell:::{7007ACC7}"_s, {u"适配器"_s, u"卡"_s, u"网络"_s, u"connections"_s},
                {u"网卡"_s, u"ncpa.cpl"_s, u"ip地址"_s}),
            place(u"设备管理器"_s, u"control.exe /name Microsoft.DeviceManager"_s, {u"适配器"_s, u"驱动程序"_s}),
            place(u"管理网络适配器设置"_s, u"ms-settings:network-advancedsettings"_s, {u"网络"_s}),
            place(u"网络重置"_s, u"MS-SETTINGS:network-advancedsettings"_s, {u"重置"_s}),
            place(u"节电模式设置"_s, u"ms-settings:batterysaver"_s, {u"battery saver settings"_s}),
            place(u"更改显示器的分辨率"_s, u"ms-settings:display"_s, {u"显示器"_s}),
            place(u"保护历史记录"_s, u"windowsdefender://history"_s, {}, {u"隔离区"_s}),
            place(u"凭据管理器"_s, u"control.exe /name Microsoft.CredentialManager"_s, {}),
            place(u"当我看向别处时，自动调暗屏幕"_s, u"ms-settings:batterysaver-dim"_s, {}),
        };
        const auto names = [&](const QString& text, const QStringList& history = {}) {
            const ParsedQuery query = ws::parseQuery(text);
            const NameMatcher matcher(query);
            QStringList out;
            for (const PlaceHit& hit : searchPlaces(places, query, matcher, history))
                out.append(places[hit.index].name);
            return out;
        };

        QCOMPARE(names(u"网卡"_s).value(0), u"查看网络连接"_s); // an own keyword
        QCOMPARE(names(u"NCPA.cpl"_s), QStringList {u"查看网络连接"_s});
        // Pinyin of a keyword, above a name the initials merely touch (我看)...
        QCOMPARE(names(u"wk"_s), (QStringList {u"查看网络连接"_s, u"当我看向别处时，自动调暗屏幕"_s}));
        // ... but below one they fill half of: 管理器, not 隔离区.
        QCOMPARE(names(u"glq"_s), (QStringList {u"设备管理器"_s, u"凭据管理器"_s, u"保护历史记录"_s}));
        QCOMPARE(names(u"geliqu"_s).value(0), u"保护历史记录"_s); // spelled out, it is meant
        QCOMPARE(names(u"glq"_s, {places[6].path()}).value(0), u"保护历史记录"_s); // opened recently
        QCOMPARE(names(u"ip 地址"_s), QStringList {u"查看网络连接"_s}); // each word in a keyword
        // A whole keyword ranks above a name that only contains the word.
        QCOMPARE(names(u"适配器"_s), (QStringList {u"设备管理器"_s, u"查看网络连接"_s, u"管理网络适配器设置"_s}));
        QCOMPARE(names(u"saver"_s), QStringList {u"节电模式设置"_s}); // a word inside a keyword
        QVERIFY(names(u"aver"_s).isEmpty()); // ... not the middle of one
        QVERIFY(names(u"卡"_s).isEmpty()); // one character: the name must have it
        QCOMPARE(names(u"驱动"_s), QStringList {u"设备管理器"_s}); // two Chinese characters: anywhere in a keyword
        QCOMPARE(names(u"网络 适配器"_s).value(0), u"查看网络连接"_s); // one word in the name, one in a keyword
        // Both tasks open the same page: one row, the better one.
        QCOMPARE(names(u"重置"_s), QStringList {u"网络重置"_s});
        QCOMPARE(names(u"网络"_s).count(u"管理网络适配器设置"_s) + names(u"网络"_s).count(u"网络重置"_s), 1);
        QVERIFY(!names(u"网卡 !查看"_s).contains(u"查看网络连接"_s));
        QVERIFY(names(u"ext:txt"_s).isEmpty()); // file syntax
        QVERIFY(names(uR"(网络\适配器)"_s).isEmpty());
        QVERIFY(names(QString()).isEmpty());
        QCOMPARE(names(u"分辨率"_s), QStringList {u"更改显示器的分辨率"_s});

        // Recently opened places rank higher.
        QCOMPARE(names(u"适配器"_s, {uR"(D:\a.txt)"_s, places[0].path()}).value(0), u"查看网络连接"_s);
    }

    void appLogoChoice()
    {
        // As ChatGPT's package ships them.
        QStringList files {u"Square44x44Logo.png"_s, u"Square44x44Logo.scale-200.png"_s};
        for (const int size : {16, 20, 24, 30, 32, 36, 40, 44, 48, 60, 64, 72, 80, 96, 256}) {
            for (const QString& form : {u"unplated"_s, u"lightunplated"_s})
                files.append(u"Square44x44Logo.targetsize-%1_altform-%2.png"_s.arg(size).arg(form));
        }
        const QString logo = u"Square44x44Logo.png"_s;
        const auto pick = [&](const QStringList& names, int pixels, bool dark) {
            const auto choice = chooseLogoFile(logo, names, pixels, dark);
            return choice ? choice->name : QString();
        };
        // The size drawn for exactly these pixels, in the theme's variant.
        QCOMPARE(pick(files, 48, true), u"Square44x44Logo.targetsize-48_altform-unplated.png"_s);
        QCOMPARE(pick(files, 48, false), u"Square44x44Logo.targetsize-48_altform-lightunplated.png"_s);
        // Else the next bigger one, scaled down, never a smaller one scaled up.
        QCOMPARE(pick(files, 42, true), u"Square44x44Logo.targetsize-44_altform-unplated.png"_s);
        QCOMPARE(pick(files, 300, true), u"Square44x44Logo.scale-200.png"_s); // bigger than any target size
        QVERIFY(!chooseLogoFile(logo, files, 48, true)->plated);

        // Older apps: logos made for a plate, by display scale; the biggest.
        const QStringList plated {u"Square44x44Logo.scale-100.png"_s, u"Square44x44Logo.scale-200.png"_s,
            u"Square44x44Logo.scale-150.png"_s, u"Square44x44Logo.scale-100_contrast-black.png"_s,
            u"StoreLogo.png"_s};
        QCOMPARE(pick(plated, 48, true), u"Square44x44Logo.scale-200.png"_s);
        QVERIFY(chooseLogoFile(logo, plated, 48, true)->plated);

        // Only an unplated variant: used in either theme.
        QCOMPARE(pick({u"Square44x44Logo.targetsize-32_altform-unplated.png"_s}, 32, false),
            u"Square44x44Logo.targetsize-32_altform-unplated.png"_s);
        // Files for another language than the one resolved are left out.
        const QStringList languages {u"Square44x44Logo.targetsize-48_altform-unplated_lang-en-us.png"_s,
            u"Square44x44Logo.targetsize-48_altform-unplated_lang-zh-cn.png"_s};
        QCOMPARE(chooseLogoFile(logo, languages, 48, true, u"Square44x44Logo.scale-100_lang-zh-cn.png"_s)->name,
            u"Square44x44Logo.targetsize-48_altform-unplated_lang-zh-cn.png"_s);
        QVERIFY(!chooseLogoFile(logo, {u"Other.png"_s, u"Square44x44Logo.targetsize-48.jpg"_s}, 48, true));
        // One choice across folders: a resource package holds only its scale's files.
        QCOMPARE(pick({u"C:/app_split.scale-150/Assets/Square44x44Logo.scale-150.png"_s,
                          u"C:/app/Assets/Square44x44Logo.targetsize-48_altform-unplated.png"_s},
                     48, true),
            u"C:/app/Assets/Square44x44Logo.targetsize-48_altform-unplated.png"_s);
    }

    void installedApps() // this machine's real list
    {
        const AppList apps = loadInstalledApps({});
        QVERIFY(!apps.empty()); // every Windows has some (File Explorer, Settings, ...)
        QVERIFY(std::all_of(apps.begin(), apps.end(), [](const AppInfo& a) { return !a.name.isEmpty() && !a.id.isEmpty(); }));
        QVERIFY(std::none_of(apps.begin(), apps.end(), [](const AppInfo& a) { return a.target.contains(u"://"_s); }));
        QCOMPARE(installedAppsFingerprint(), installedAppsFingerprint());
    }

    void contentUtf8()
    {
        const QByteArray text = u"first line\n\tSecond line has the NEEDLE here\nthird"_s.toUtf8();
        const ContentScanner scanner(u"needle"_s);
        const auto m = scanner.scan(reader(text), 4096, {});
        QVERIFY(m);
        QCOMPARE(m->line, 2);
        QCOMPARE(m->snippet, u"Second line has the NEEDLE here"_s);
        QCOMPARE(m->snippet.mid(m->matches.at(0).first, m->matches.at(0).second), u"NEEDLE"_s);

        const QByteArray bom = QByteArray("\xEF\xBB\xBF") + u"中文内容：季度报告\n"_s.toUtf8();
        const auto m2 = ContentScanner(u"报告"_s).scan(reader(bom), 4096, {});
        QVERIFY(m2);
        QCOMPARE(m2->line, 1);
        QCOMPARE(m2->snippet, u"中文内容：季度报告"_s);
    }

    void contentTerms()
    {
        // Every term somewhere in the file; the line shown has the most of them.
        const QByteArray text = u"intro\nthe Report is due\nbudget 2025 numbers\nreport on the 2025 budget\n"_s.toUtf8();
        const ContentScanner scanner(QStringList {u"report"_s, u"2025"_s});
        const auto m = scanner.scan(reader(text), 4096, {});
        QVERIFY(m);
        QCOMPARE(m->line, 4);
        QCOMPARE(m->snippet, u"report on the 2025 budget"_s);
        QCOMPARE(m->matches, (QList<std::pair<qsizetype, qsizetype>> {{0, 6}, {14, 4}}));
        for (std::size_t chunk : {16, 18, 32, 100}) {
            for (std::size_t step : {1, 7}) {
                const auto small = scanner.scan(reader(text, step), chunk, {});
                QVERIFY2(small && small->line >= 2, qPrintable(u"chunk %1 step %2"_s.arg(chunk).arg(step)));
            }
        }

        // On different lines: the first one's line, and where the other is after it.
        const auto apart = ContentScanner(QStringList {u"beta"_s, u"alpha"_s}).scan(reader("alpha\nbeta\n"), 4096, {});
        QVERIFY(apart);
        QCOMPARE(apart->line, 1);
        QCOMPARE(apart->snippet, u"alpha … beta"_s);
        QCOMPARE(apart->matches, (QList<std::pair<qsizetype, qsizetype>> {{0, 5}, {8, 4}}));
        QVERIFY(!ContentScanner(QStringList {u"alpha"_s, u"gamma"_s}).scan(reader("alpha\nbeta\n"), 4096, {}));
        QVERIFY(!ContentScanner(u"alpha beta"_s).scan(reader("alpha\nbeta\n"), 4096, {})); // a phrase: just so
        // GBK: every term must be one the code page can write.
        const QByteArray gbk = encode(u"测试文本\n"_s, 936);
        QVERIFY(ContentScanner(QStringList {u"测试"_s, u"文本"_s}, 936).scan(reader(gbk), 4096, {}));
        QVERIFY(!ContentScanner(QStringList {u"测试"_s, u"\U0001F600"_s}, 936).scan(reader(gbk), 4096, {}));
    }

    void contentRanking()
    {
        // Of lines as good, the one with the terms at the start of words.
        const ContentScanner todo(QStringList {u"todo"_s, u"fix"_s});
        const auto m = todo.scan(reader("// todo: prefix it\n// TODO: fix it\n"), 4096, {});
        QVERIFY(m);
        QCOMPARE(m->line, 2);
        QCOMPARE(m->termsOnLine, 2);
        QCOMPARE(m->wordStarts, 0b11u);
        QVERIFY(!m->longLine);
        const auto prefix = todo.scan(reader("// todo: prefix it\n"), 4096, {});
        QVERIFY(prefix);
        QCOMPARE(prefix->wordStarts, 0b01u);
        // "Lock" starts a word in "readLock"; a CJK term anywhere.
        const auto camel = ContentScanner(QStringList {u"lock"_s, u"报告"_s}).scan(reader("readLock 年度报告\n"), 4096, {});
        QVERIFY(camel);
        QCOMPARE(camel->wordStarts, 0b11u);
        // Packed onto one line (no break for 8 KB); a long paragraph is not.
        const QByteArray packed = QByteArray(10000, 'x') + " fix " + QByteArray(10000, 'y');
        const auto minified = ContentScanner(u"fix"_s).scan(reader(packed), 65536, {});
        QVERIFY(minified);
        QVERIFY(minified->longLine);
        const QByteArray paragraph = u"很长的一段话。"_s.repeated(150).toUtf8() + " fix " + u"又一段。"_s.repeated(150).toUtf8();
        const auto prose = ContentScanner(u"fix"_s).scan(reader(paragraph), 65536, {});
        QVERIFY(prose);
        QVERIFY(!prose->longLine);
        const QByteArray halves = QByteArray(5000, 'x') + " fix " + QByteArray(5000, 'y'); // 8 KB together
        QVERIFY(ContentScanner(u"fix"_s).scan(reader(halves), 65536, {})->longLine);

        // The snippet starts where the most terms fit, not at the first one.
        const QString spread = u"alpha "_s + QString(200, u'.') + u" alpha and beta here"_s;
        const auto both = ContentScanner(QStringList {u"alpha"_s, u"beta"_s}).scan(reader(spread.toUtf8()), 4096, {});
        QVERIFY(both);
        QCOMPARE(both->matches.size(), 2);
        QVERIFY(both->snippet.contains(u"alpha and beta"_s));

        // Every term shows within the width of a result (28 ideographs): the
        // ones the line lacks after it, with words around them.
        const auto shows = [](const ContentMatch& m, const QStringList& terms) {
            int width = 0;
            for (qsizetype i = 0; i < m.snippet.size(); ++i) {
                const bool wide = m.snippet[i].unicode() >= 0x2E80;
                for (const auto& [at, length] : m.matches) {
                    if (at == i && width + (wide ? 2 : 1) * length > 60)
                        return false;
                }
                width += wide ? 2 : 1;
            }
            for (const QString& t : terms) {
                if (std::ranges::none_of(m.matches, [&](const auto& r) {
                        return m.snippet.mid(r.first, r.second).compare(t, Qt::CaseInsensitive) == 0;
                    }))
                    return false;
            }
            return true;
        };
        const QStringList three {u"弯管机"_s, u"角度"_s, u"优化"_s};
        const QString manual = u"目录\n弯管机用户手册由于软件会不断进行优化，如说明书与实际界面有差异，请以实际界面为准。\n"_s
            + u"第二章\n设定折弯角度时，先回零点。\n"_s;
        const auto pieced = ContentScanner(three).scan(reader(manual.toUtf8()), 4096, {});
        QVERIFY(pieced);
        QCOMPARE(pieced->line, 2);
        QVERIFY2(pieced->snippet.startsWith(u"弯管机用户手册"_s), qPrintable(pieced->snippet));
        QVERIFY2(pieced->snippet.contains(u" … "_s), qPrintable(pieced->snippet));
        QVERIFY2(shows(*pieced, three), qPrintable(pieced->snippet));
        // Far apart on one line: two pieces of it.
        const QString wide = u"弯管机"_s + u"很长的一段话。"_s.repeated(20) + u"角度和优化"_s + u"后面的话。"_s.repeated(5);
        const auto split = ContentScanner(three).scan(reader(wide.toUtf8()), 4096, {});
        QVERIFY(split);
        QVERIFY2(shows(*split, three), qPrintable(split->snippet));
        // All together: one piece, as before.
        const auto oneLine = ContentScanner(three).scan(reader(u"弯管机角度优化说明\n"_s.toUtf8()), 4096, {});
        QVERIFY(oneLine);
        QCOMPARE(oneLine->snippet, u"弯管机角度优化说明"_s);

        const QStringList terms {u"todo"_s, u"fix"_s};
        const auto rank = [&](const ContentMatch& match, QStringView name = u"a.txt", int priority = 1,
                              std::int64_t age = -1) { return contentRelevance(match, terms, name, priority, age); };
        ContentMatch apart {.termsOnLine = 1, .wordStarts = 0b01};
        ContentMatch together {.termsOnLine = 2, .wordStarts = 0b11};
        QVERIFY(rank(together) > rank(apart));
        QVERIFY(rank(*m) > rank(*prefix));
        QVERIFY(rank(apart, u"todo-fix.txt") > rank(apart));
        QVERIFY(rank(apart, u"a.txt", 0) > rank(apart, u"a.txt", 3)); // your own folders before system ones
        QVERIFY(rank(apart, u"a.txt", 1, 3600) > rank(apart, u"a.txt", 1, 400LL * 24 * 3600));
        ContentMatch packedLine = together;
        packedLine.longLine = true;
        QVERIFY(rank(packedLine) < rank(apart));
    }

    void contentChunkBoundaries()
    {
        // Matches straddling chunk borders, with tiny chunks and odd read sizes.
        QString body;
        for (int i = 0; i < 50; ++i)
            body += u"line %1 filler text\n"_s.arg(i);
        body += u"the target 关键词 is here\n"_s;
        const QByteArray utf8 = body.toUtf8();
        for (std::size_t chunk : {16, 18, 32, 100, 4096}) {
            for (std::size_t step : {1, 3, 7, 1000}) {
                const auto m = ContentScanner(u"关键词"_s).scan(reader(utf8, step), chunk, {});
                QVERIFY2(m, qPrintable(u"chunk %1 step %2"_s.arg(chunk).arg(step)));
                QCOMPARE(m->line, 51);
            }
        }
    }

    void contentUtf16()
    {
        const QByteArray data = utf16le(u"alpha\r\nbeta Gamma 测试\r\n"_s, true);
        for (std::size_t chunk : {16, 20, 4096}) {
            const auto m = ContentScanner(u"gamma 测试"_s).scan(reader(data), chunk, {});
            QVERIFY(m);
            QCOMPARE(m->line, 2);
            QCOMPARE(m->snippet.mid(m->matches.at(0).first, m->matches.at(0).second), u"Gamma 测试"_s);
            if (chunk == 4096)
                QCOMPARE(m->snippet, u"beta Gamma 测试"_s);
        }
    }

    void contentGbk()
    {
        const QByteArray gbk = encode(u"第一行\n这是测试文本 Hello\n"_s, 936);
        QCOMPARE(ContentScanner::detect({gbk.constData(), static_cast<std::size_t>(gbk.size())}, nullptr),
            TextEncoding::Ansi);
        const auto m = ContentScanner(u"测试文本"_s, 936).scan(reader(gbk), 4096, {});
        QVERIFY(m);
        QCOMPARE(m->line, 2);
        QCOMPARE(m->snippet, u"这是测试文本 Hello"_s);

        // A GBK character whose trail byte is 'a' must not match the needle "a".
        QChar tricky;
        for (char16_t c = 0x4E00; c < 0x9FA5 && tricky.isNull(); ++c) {
            const QByteArray b = encode(QString(QChar(c)), 936);
            if (b.size() == 2 && b[1] == 'a')
                tricky = QChar(c);
        }
        QVERIFY(!tricky.isNull());
        const QString line = QString(4, tricky) + u'\n';
        QVERIFY(!ContentScanner(u"a"_s, 936).scan(reader(encode(line + line, 936)), 4096, {}));
        const auto real = ContentScanner(u"a"_s, 936).scan(reader(encode(line + tricky + u" A\n"_s, 936)), 4096, {});
        QVERIFY(real);
        QCOMPARE(real->line, 2);
    }

    void contentNoMatch()
    {
        QVERIFY(!ContentScanner(u"absent"_s).scan(reader("nothing to see\n"), 4096, {}));
        QVERIFY(!ContentScanner(u"x"_s).scan(reader(QByteArray()), 4096, {}));
        const auto cancelled = ContentScanner(u"x"_s).scan(reader("xxxx"), 4096, [] { return true; });
        QVERIFY(!cancelled);
    }

    void contentFile()
    {
        QTemporaryDir dir;
        QFile file(dir.filePath(u"notes.txt"_s));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("first line\nsecond line has the Needle\n");
        file.close();
        const std::wstring path = QDir::toNativeSeparators(file.fileName()).toStdWString();
        const ContentScanner scanner(u"needle"_s);
        const auto match = scanner.scanFile(path, 0, {});
        QVERIFY(match);
        QCOMPARE(match->line, 2);
        bool large = false;
        QVERIFY(!scanner.scanFile(path, 10, {}, &large)); // larger than the limit
        QVERIFY(large);
        QVERIFY(!scanner.scanFile(path + L".missing", 0, {}, &large));
        QVERIFY(!large);
        QVERIFY(scanner.scanFile(path, 0, {}, &large) && !large);
    }

    void gramsOfText()
    {
        const QString text = u"第一行 English 中文内容\n季度报告：收入增长。日本語のテキスト 한국어 𠀀𠀁 end"_s;
        const std::vector<grams::Key> keys = gramsOf(text);
        for (const QString& phrase : {u"中文"_s, u"中文内容"_s, u"文内"_s, u"第一行"_s, u"季度报告"_s, u"中"_s, u"日本語のテキスト"_s,
                 u"한국어"_s, u"𠀀𠀁"_s, u"行 English 中"_s, u"收入增长。"_s})
            QVERIFY2(hasAll(keys, phrase), qPrintable(phrase));
        for (const QString& phrase : {u"行中"_s, u"容季"_s /* across the line break */, u"告收"_s /* across "：" */, u"中国"_s})
            QVERIFY2(!hasAll(keys, phrase), qPrintable(phrase));

        // Whatever the encoding and the chunks it arrives in.
        const QByteArray utf8 = text.toUtf8();
        QByteArray utf16be;
        for (const QChar c : text) {
            utf16be.append(static_cast<char>(c.unicode() >> 8));
            utf16be.append(static_cast<char>(c.unicode() & 0xFF));
        }
        const QString chinese = u"第一行 English 中文内容\n季度报告：收入增长。"_s; // what GBK can hold
        for (const std::size_t chunk : {1, 2, 3, 5, 7, 4096}) {
            QCOMPARE(gramsOf(utf8, TextEncoding::Utf8, chunk), keys);
            QCOMPARE(gramsOf(utf16le(text, false), TextEncoding::Utf16LE, chunk), keys);
            QCOMPARE(gramsOf(utf16be, TextEncoding::Utf16BE, chunk), keys);
            QCOMPARE(gramsOf(encode(chinese, 936), TextEncoding::Ansi, chunk), gramsOf(chinese));
        }
        // Malformed UTF-8 breaks a pair, as the U+FFFD the scanner decodes it to would.
        const QByteArray broken = u"中"_s.toUtf8() + "\xFF" + u"文"_s.toUtf8() + "\xE6\x96" + u"字"_s.toUtf8();
        const std::vector<grams::Key> brokenKeys = gramsOf(broken, TextEncoding::Utf8);
        QVERIFY(hasAll(brokenKeys, u"中"_s) && hasAll(brokenKeys, u"文"_s) && hasAll(brokenKeys, u"字"_s));
        QVERIFY(!hasAll(brokenKeys, u"中文"_s) && !hasAll(brokenKeys, u"文字"_s));

        // Runs of three ASCII characters, whatever their case, spaces and
        // punctuation too; any run of whitespace is one space.
        for (const QString& phrase : {u"English"_s, u"ENGLISH"_s, u"glis"_s, u"end"_s, u"English 中文"_s, u" end"_s})
            QVERIFY2(hasAll(keys, phrase), qPrintable(phrase));
        for (const QString& phrase : {u"Englishes"_s, u"end2"_s, u"lishend"_s, u"English-"_s})
            QVERIFY2(!hasAll(keys, phrase), qPrintable(phrase));
        const std::vector<grams::Key> code = gramsOf(u"auto my_var = getElementById(x2y);\nif (p->ok)  {\r\n\treturn;"_s);
        for (const QString& phrase : {u"my_var"_s, u"y_v"_s, u"getElementById(x2y)"_s, u"x2y"_s, u"auto my_var = get"_s,
                 u"(x2y);"_s, u"if (p->ok) { return;"_s, u"P->OK"_s, u"ok)     {"_s})
            QVERIFY2(hasAll(code, phrase), qPrintable(phrase));
        for (const QString& phrase : {u"automy"_s, u"x2yz"_s, u"auto  my_var=get"_s, u"if(p"_s, u"p.ok"_s, u"var=="_s})
            QVERIFY2(!hasAll(code, phrase), qPrintable(phrase));
        { // a single-byte code page: bytes above 0x7F are not ASCII
            grams::Collector collector(TextEncoding::Ansi, 1252);
            const QByteArray latin = encode(u"café_bar Zürich"_s, 1252);
            collector.feed(latin.constData(), static_cast<std::size_t>(latin.size()));
            const std::vector<grams::Key> latinKeys = collector.finish();
            QVERIFY(hasAll(latinKeys, u"caf"_s) && hasAll(latinKeys, u"_bar"_s) && hasAll(latinKeys, u"rich"_s));
            QVERIFY(!hasAll(latinKeys, u"cafe"_s) && !hasAll(latinKeys, u"zur"_s));
        }

        QCOMPARE(grams::ofPhrase(u"report 2024"_s).size(), std::size_t {9}); // rep epo por ort "rt " "t 2" " 20" 202 024
        QCOMPARE(grams::ofPhrase(u"a  \t b"_s).size(), std::size_t {1}); // "a b"
        QVERIFY(grams::ofPhrase(u"aé bè1"_s).empty()); // no three ASCII characters in a row
        QCOMPARE(grams::ofPhrase(u"季度报告"_s).size(), std::size_t {3}); // pairs only
        QCOMPARE(grams::ofPhrase(u"Qt 窗口 a 类"_s).size(), std::size_t {4}); // "qt ", 窗口, " a ", 类
        QVERIFY(grams::decides(u"中"_s) && grams::decides(u"中文"_s));
        QVERIFY(!grams::decides(u"中文字"_s) && !grams::decides(u"a中"_s) && !grams::decides(u""_s));

        ContentSizeLimits limits;
        limits.bytes = {100, 20, 30, 400};
        QCOMPARE(ContentSizeLimits::kindOf("CPP"), ContentSizeLimits::Code);
        QCOMPARE(ContentSizeLimits::kindOf("json"), ContentSizeLimits::Data);
        QCOMPARE(ContentSizeLimits::kindOf("log"), ContentSizeLimits::Text);
        QCOMPARE(ContentSizeLimits::kindOf("c++"), ContentSizeLimits::Code);
        QCOMPARE(ContentSizeLimits::kindOf("verylongext"), ContentSizeLimits::Text);
        QCOMPARE(ContentSizeLimits::kindOf("PDF"), ContentSizeLimits::Document);
        QCOMPARE(limits.of(L"D:\\Reports\\Q3.xlsx"), std::int64_t {400});
        QVERIFY(isDocumentPath(L"C:\\a\\Report.DOCX") && isDocumentPath(L"x.et") && !isDocumentPath(L"x.docxx"));
        QVERIFY(!isDocumentPath(L"C:\\a.pdf\\notes") && !isDocumentPath(L"C:\\a\\pdf") && !isDocumentPath(L"a.pdf\u00e9"));
        QCOMPARE(documentExtensions().size(), qsizetype {28});
        QCOMPARE(limits.of(L"C:\\src\\main.CPP"), std::int64_t {20});
        QCOMPARE(limits.of(L"C:\\a.b\\data.json"), std::int64_t {30});
        QCOMPARE(limits.of(L"C:\\a.json\\README"), std::int64_t {100}); // no extension: text
        QCOMPARE(limits.of(L"C:\\notes.txt"), std::int64_t {100});
        QCOMPARE(limits.of(L"C:\\x.verylongext"), std::int64_t {100});
        QCOMPARE(limits.of(L"C:\\x.héllo"), std::int64_t {100});
        QCOMPARE(limits.of(L"C:\\bundle.min.js"), std::int64_t {20});

        const ExtensionFilter filter({u".TXT"_s, u"*.md"_s, u"verylongextension"_s});
        QVERIFY(filter.matches("Notes.txt", 3) && filter.matches("README.MD", 2));
        QVERIFY(filter.matches("a.VeryLongExtension", 17));
        QVERIFY(!filter.matches("a.tx", 2) && !filter.matches("a.txt", 0) && !filter.matches("a.markdown", 8));
    }

    // What a content scan passes over, and counts (ContentFilter::classify,
    // OtherTextTypes): the kinds of text the list leaves out, and the files
    // of listed types in system and program folders.
    void contentLeftOut()
    {
        const OtherTextTypes other({u"txt"_s, u".md"_s, u"*.js"_s, u"JSON"_s});
        QVERIFY(other.count() > 50);
        QVERIFY(other.indexOf("App.vue", 3) >= 0 && other.indexOf("Main.KT", 2) >= 0 && other.indexOf("x.rst", 3) >= 0);
        QCOMPARE(other.extension(static_cast<std::size_t>(other.indexOf("App.vue", 3))), u"vue"_s);
        QCOMPARE(other.indexOf("notes.md", 2), -1); // listed
        QCOMPARE(other.indexOf("app.js", 2), -1);
        QCOMPARE(other.indexOf("data.json", 4), -1);
        QCOMPARE(other.indexOf("setup.exe", 3), -1); // not text
        QCOMPARE(other.indexOf("report.docx", 4), -1); // a document: read under its own setting
        QCOMPARE(other.indexOf("x.dat", 3), -1); // may hold anything
        QCOMPARE(other.indexOf("README", 0), -1);
        QVERIFY(other.extensions().contains(u"swift"_s) && !other.extensions().contains(u"txt"_s));
        QCOMPARE(other.extensions().size(), static_cast<qsizetype>(other.count()));

        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId program = index.add(root, "Program Files", EntryFlag::Directory | EntryFlag::LowPriority);
        const EntryId own = index.add(root, "notes.txt", 0);
        const EntryId code = index.add(root, "App.vue", 0);
        const EntryId system = index.add(program, "readme.txt", EntryFlag::LowPriority);
        const EntryId systemCode = index.add(program, "lib.kt", EntryFlag::LowPriority);
        const EntryId lock = index.add(root, "~$notes.txt", 0);
        const EntryId bare = index.add(root, "README", 0);
        ContentFilter filter;
        filter.extensions = ExtensionFilter({u"txt"_s});
        std::string_view name;
        std::size_t extLength = 0;
        const auto verdict = [&](EntryId id) { return filter.classify(index, index.entry(id), name, extLength); };
        QCOMPARE(verdict(own), ContentVerdict::Read);
        QCOMPARE(name, std::string_view("notes.txt"));
        QCOMPARE(extLength, std::size_t {3});
        QCOMPARE(verdict(code), ContentVerdict::OtherType);
        QVERIFY(other.indexOf(name, extLength) >= 0);
        QCOMPARE(verdict(system), ContentVerdict::LowPriority);
        QCOMPARE(verdict(systemCode), ContentVerdict::OtherType); // its type first: the one-off over types would not read it either
        QCOMPARE(verdict(lock), ContentVerdict::Never);
        QCOMPARE(verdict(bare), ContentVerdict::Never);
        QCOMPARE(verdict(program), ContentVerdict::Never);
        QVERIFY(filter.accepts(index, index.entry(own)) && !filter.accepts(index, index.entry(system)));
        filter.includeLowPriority = true; // asked for: read as any other
        QCOMPARE(verdict(system), ContentVerdict::Read);
        QVERIFY(filter.accepts(index, index.entry(system)));
    }


    void contentIndex()
    {
        QTemporaryDir dir;
        ContentIndex content(dir.path());
        QVERIFY(content.add(10, gramsOf(u"季度报告"_s), false, 1, 0));
        QVERIFY(content.add(20, gramsOf(u"年度报告和计划"_s), false, 2, 0));
        QVERIFY(content.add(30, {}, true, 3, 0, {}, true)); // too large: nothing to find in it
        QVERIFY(content.add(5, gramsOf(u"English only"_s), false, 4, 0));

        const auto lookup = content.lookup(u"报告"_s);
        QVERIFY(lookup.usable && lookup.decisive);
        QCOMPARE(knownOf(content, u"报告"_s), (std::vector<EntryId> {5, 10, 20, 30}));
        QCOMPARE(lookup.large, (std::vector<EntryId> {30}));
        QCOMPARE(matchesOf(content, u"报告"_s), (std::vector<EntryId> {10, 20}));
        QCOMPARE(matchesOf(content, u"季度报告"_s), (std::vector<EntryId> {10}));
        QVERIFY(!content.lookup(u"季度报告"_s).decisive);
        QCOMPARE(matchesOf(content, u"度报"_s), (std::vector<EntryId> {10, 20}));
        QVERIFY(matchesOf(content, u"计划 report"_s).empty()); // "report" is not in it
        QVERIFY(matchesOf(content, u"中文"_s).empty());
        QCOMPARE(matchesOf(content, u"ENGLISH"_s), (std::vector<EntryId> {5}));
        QCOMPARE(matchesOf(content, u"lish onl"_s), (std::vector<EntryId> {5}));
        QVERIFY(matchesOf(content, u"englishman"_s).empty());
        QVERIFY(content.lookup(u"only"_s).usable && !content.lookup(u"only"_s).decisive);
        QVERIFY(!content.lookup(u"on"_s).usable); // too short to look up

        // Several terms: every one, anywhere.
        const auto both = [&](const QStringList& terms) {
            std::vector<EntryId> out;
            for (const std::uint32_t k : content.lookup(terms).known) {
                if (k & ContentIndex::Lookup::kMatch)
                    out.push_back(k & ~ContentIndex::Lookup::kMatch);
            }
            return out;
        };
        QCOMPARE(both({u"计划"_s, u"年度"_s}), (std::vector<EntryId> {20}));
        QCOMPARE(both({u"报告"_s, u"english"_s}), (std::vector<EntryId> {}));
        QCOMPARE(both({u"english"_s, u"only"_s}), (std::vector<EntryId> {5}));
        QVERIFY(content.lookup(QStringList {u"报告"_s, u"计划"_s}).decisive);
        QVERIFY(!content.lookup(QStringList {u"报告"_s, u"only"_s}).decisive);

        // A file written to is not known until it is read again; one written
        // to while it is being read stays unknown.
        content.markChanged(std::vector<EntryId> {10});
        QCOMPARE(knownOf(content, u"报告"_s), (std::vector<EntryId> {5, 20, 30}));
        content.beginReads();
        const std::uint64_t since = content.changeSequence();
        content.markChanged(std::vector<EntryId> {20});
        QVERIFY(content.add(20, gramsOf(u"新的计划"_s), false, 5, since));
        content.endReads();
        QCOMPARE(knownOf(content, u"报告"_s), (std::vector<EntryId> {5, 30}));
        QVERIFY(content.add(10, gramsOf(u"报告已更新"_s), false, 6, content.changeSequence()));
        QCOMPARE(matchesOf(content, u"更新"_s), (std::vector<EntryId> {10}));
        const auto docs = content.documents();
        QCOMPARE(docs.size(), std::size_t {4});
        QVERIFY(docs[2].entry == 20 && docs[2].need == ContentIndex::Need::Read);
        QVERIFY(docs[1].entry == 10 && docs[1].need == ContentIndex::Need::None && docs[1].stamp == 6);

        // Unsure (the journal was lost): checked, then current again.
        content.markUnsure([](EntryId e) { return e == 5; });
        QCOMPARE(content.documents()[0].need, ContentIndex::Need::Check);
        content.confirm(5);
        QCOMPARE(content.documents()[0].need, ContentIndex::Need::None);

        // Into a segment file; compaction renumbers; saved and restored.
        QVERIFY(content.merge());
        QCOMPARE(content.stats().segments, std::size_t {1});
        QCOMPARE(content.stats().memoryPairs, std::size_t {0});
        QCOMPARE(matchesOf(content, u"报告"_s), (std::vector<EntryId> {10}));
        content.remap([](EntryId e) { return e == 5 ? kNoEntry : e - 1; });
        QCOMPARE(knownOf(content, u"报告"_s), (std::vector<EntryId> {9, 29}));
        QCOMPARE(content.lookup(u"报告"_s).large, (std::vector<EntryId> {29}));
        QCOMPARE(matchesOf(content, u"已更新"_s), (std::vector<EntryId> {9}));
        content.retire(std::vector<EntryId> {29});
        QCOMPARE(knownOf(content, u"报告"_s), (std::vector<EntryId> {9}));
        QVERIFY(content.lookup(u"报告"_s).large.empty());

        std::vector<EntryId> newIds(40, kNoEntry);
        newIds[9] = 1;
        newIds[19] = 2;
        std::vector<std::uint64_t> segments;
        const std::vector<char> state = content.serialize(newIds, segments);
        QCOMPARE(segments.size(), std::size_t {1});
        ContentIndex restored(dir.path());
        QVERIFY(restored.restore(state, 3));
        QCOMPARE(matchesOf(restored, u"报告"_s), (std::vector<EntryId> {1}));
        QCOMPARE(restored.documents().size(), std::size_t {2});
        QCOMPARE(restored.documents()[1].need, ContentIndex::Need::Read); // still dirty
        QVERIFY(restored.add(2, gramsOf(u"计划书"_s), false, 7, restored.changeSequence()));
        QCOMPARE(matchesOf(restored, u"计划"_s), (std::vector<EntryId> {2}));

        // A document too large for the size limit stays so when saved and
        // read back, and of the empty documents it alone is read again when
        // the limit goes up. A state of the version before had not recorded
        // which were too large: read back, every empty document is read
        // again, and the state is to be saved anew.
        QVERIFY(restored.add(0, {}, true, 8, 0, {}, true));
        QCOMPARE(restored.lookup(u"报告"_s).large, (std::vector<EntryId> {0}));
        QCOMPARE(knownOf(restored, u"报告"_s), (std::vector<EntryId> {0, 1, 2}));
        const std::vector<EntryId> threeIds {0, 1, 2};
        std::vector<std::uint64_t> moreSegments;
        std::vector<char> saved = restored.serialize(threeIds, moreSegments);
        ContentIndex reread(dir.path());
        QVERIFY(reread.restore(saved, 3));
        QVERIFY(!reread.takeChanged());
        QCOMPARE(reread.lookup(u"报告"_s).large, (std::vector<EntryId> {0}));
        QVERIFY(reread.add(1, {}, true, 9, 0)); // nothing in it, and within the limit
        reread.markEmptyChanged();
        QCOMPARE(reread.documents()[0].need, ContentIndex::Need::Read);
        QCOMPARE(reread.documents()[1].need, ContentIndex::Need::None);
        QCOMPARE(reread.documents()[2].need, ContentIndex::Need::None);
        QVERIFY(reread.lookup(u"报告"_s).large.empty()); // dirty: not known until read again
        saved[0] = 7; // the version, in its first byte
        ContentIndex older(dir.path());
        QVERIFY(older.restore(saved, 3));
        QVERIFY(older.takeChanged());
        QCOMPARE(older.documents()[0].need, ContentIndex::Need::Read);
        QCOMPARE(older.documents()[1].need, ContentIndex::Need::None);
        QCOMPARE(older.documents()[2].need, ContentIndex::Need::None);
        QVERIFY(older.lookup(u"报告"_s).large.empty());
        saved[0] = 6;
        QVERIFY(!ContentIndex(dir.filePath(u"older"_s)).restore(saved, 3)); // older still: not read

        // Enough grams for segments of their own, and a merge of them all.
        ContentIndex big(dir.filePath(u"big"_s));
        std::vector<grams::Key> keys(1001);
        for (EntryId doc = 0; doc < 2500; ++doc) {
            for (std::size_t i = 0; i < 1000; ++i)
                keys[i] = ((grams::Key {0x4E00} + (doc % 7)) << grams::kCharBits) | (0x4E00 + i);
            keys[1000] = (grams::Key {0x6000} + doc) << grams::kCharBits; // a character of its own: no copies
            QVERIFY(big.add(doc * 2, keys, false, 0, 0));
        }
        QCOMPARE(big.stats().segments, std::size_t {2});
        const QString pair = QString(QChar(0x4E00 + 3)) + QChar(0x4E00 + 5); // docs where doc % 7 == 3
        QCOMPARE(matchesOf(big, pair).size(), std::size_t {357});
        big.retire(std::vector<EntryId> {6, 20});
        QVERIFY(big.merge());
        QCOMPARE(big.stats().segments, std::size_t {1});
        const std::vector<EntryId> found = matchesOf(big, pair);
        QCOMPARE(found.size(), std::size_t {355});
        QCOMPARE(found.front(), EntryId {34});
        QVERIFY(std::all_of(found.begin(), found.end(), [](EntryId e) { return (e / 2) % 7 == 3; }));

        // Segments of a level merge into one of the next level as they come.
        ContentIndex tiers(dir.filePath(u"tiers"_s));
        const auto abc = [](std::uint32_t i) { // "000", "004", ... "zz_"
            constexpr char kWord[] = "0123456789_abcdefghijklmnopqrstuvwxyz";
            const std::uint32_t t = i * 4;
            return (grams::Key {static_cast<unsigned char>(kWord[t / (37 * 37)])} << grams::kCharBits)
                | (static_cast<unsigned char>(kWord[t / 37 % 37]) << 8) | static_cast<unsigned char>(kWord[t % 37]);
        };
        const auto tierKeys = [&](EntryId doc) {
            std::vector<grams::Key> many;
            for (std::size_t i = 0; i < 5000; ++i)
                many.push_back(((grams::Key {0x4E00} + (doc % 5)) << grams::kCharBits) | (0x4E00 + i));
            if (doc % 10 == 4) { // most trigrams
                for (std::uint32_t i = 0; i < 10500; ++i)
                    many.push_back(abc(i));
            } else if (doc % 10 == 5) {
                many.push_back(abc(2)); // "008"
            }
            many.push_back((grams::Key {0x6000} + doc) << grams::kCharBits); // no copies
            std::sort(many.begin(), many.end());
            return many;
        };
        for (EntryId doc = 0; doc < 1700; ++doc) {
            QVERIFY(tiers.add(doc, tierKeys(doc), false, 0, 0));
            if (doc == 3)
                tiers.retire(std::vector<EntryId> {2});
            if (tiers.needsMerge())
                QVERIFY(tiers.mergeDue());
        }
        QCOMPARE(tiers.stats().segments, std::size_t {2}); // nine written from memory, eight of them merged
        QVERIFY(tiers.stats().memoryPairs > 0);
        const QString twos = QString(QChar(0x4E00 + 2)) + QChar(0x4E00 + 7); // docs where doc % 5 == 2
        std::vector<EntryId> expected;
        for (EntryId doc = 7; doc < 1700; doc += 5)
            expected.push_back(doc);
        QCOMPARE(matchesOf(tiers, twos), expected);
        std::vector<EntryId> fours; // docs where doc % 10 is 4 or 5
        for (EntryId doc = 4; doc < 1700; doc += 10) {
            fours.push_back(doc);
            fours.push_back(doc + 1);
        }
        QCOMPARE(matchesOf(tiers, u"008"_s), fours);
        QVERIFY(matchesOf(tiers, u"009"_s).empty());
        QVERIFY(tiers.merge());
        QCOMPARE(matchesOf(tiers, twos), expected);
        QCOMPARE(matchesOf(tiers, u"008"_s), fours);

        // Small segments merge while documents come (the indexer's readers
        // go on meanwhile); then everything, on several threads.
        ContentIndex busy(dir.filePath(u"busy"_s));
        {
            std::atomic<bool> adding {true};
            std::jthread merger([&] {
                while (adding.load()) {
                    busy.mergeTails();
                    std::this_thread::yield();
                }
            });
            for (EntryId doc = 0; doc < 1700; ++doc) {
                QVERIFY(busy.add(doc, tierKeys(doc), false, 0, 0));
                if (doc == 3)
                    busy.retire(std::vector<EntryId> {2});
            }
            adding = false;
        }
        QVERIFY(busy.mergeTails());
        QCOMPARE(busy.stats().segments, std::size_t {2});
        QCOMPARE(matchesOf(busy, twos), expected);
        QCOMPARE(matchesOf(busy, u"008"_s), fours);
        QVERIFY(busy.merge(4));
        QCOMPARE(busy.stats().segments, std::size_t {1});
        QCOMPARE(matchesOf(busy, twos), expected);
        QCOMPARE(matchesOf(busy, u"008"_s), fours);
        QVERIFY(matchesOf(busy, u"009"_s).empty());

        // Documents added on several threads at once, each found as soon as
        // it is in, while segments are written (outside the lock) and merged.
        ContentIndex crowd(dir.filePath(u"crowd"_s));
        {
            std::atomic<bool> adding {true};
            std::atomic<int> missed {0};
            std::jthread merger([&] {
                while (adding.load()) {
                    crowd.mergeTails();
                    std::this_thread::yield();
                }
            });
            {
                std::vector<std::jthread> adders;
                for (EntryId first = 0; first < 4; ++first) {
                    adders.emplace_back([&, first] {
                        for (EntryId doc = first; doc < 1700; doc += 4) {
                            const QString own(QChar(char16_t(0x6000 + doc))); // its character of its own
                            if (!crowd.add(doc, tierKeys(doc), false, 0, 0)
                                || matchesOf(crowd, own) != std::vector<EntryId> {doc})
                                ++missed;
                        }
                    });
                }
            } // joins
            adding = false;
            QCOMPARE(missed.load(), 0);
        }
        crowd.retire(std::vector<EntryId> {2}); // as above
        QVERIFY(crowd.mergeTails());
        QCOMPARE(crowd.stats().documents, std::size_t {1699});
        QCOMPARE(matchesOf(crowd, twos), expected);
        QCOMPARE(matchesOf(crowd, u"008"_s), fours);
        QVERIFY(crowd.merge(4));
        QCOMPARE(matchesOf(crowd, twos), expected);
        QCOMPARE(matchesOf(crowd, u"008"_s), fours);

        // Postings of every density, and documents with most trigrams, give
        // what the grams say: in memory, written out, merged, restored.
        ContentIndex coded(dir.filePath(u"coded"_s));
        constexpr char kWord[] = "0123456789_abcdefghijklmnopqrstuvwxyz";
        const auto trigram = [&](std::uint32_t t) {
            t %= 37 * 37 * 37;
            return (grams::Key {static_cast<unsigned char>(kWord[t / (37 * 37)])} << grams::kCharBits)
                | (static_cast<unsigned char>(kWord[t / 37 % 37]) << 8) | static_cast<unsigned char>(kWord[t % 37]);
        };
        const auto spread = [](std::uint32_t x) { return (x * 0x9E37'79B9u) >> 7; };
        constexpr grams::Key kOne = grams::Key {0x4E00} << grams::kCharBits; // "一": about half the documents
        constexpr grams::Key kTwo = grams::Key {0x4E01} << grams::kCharBits; // "丁": about a tenth
        constexpr grams::Key kRun = grams::Key {0x4E03} << grams::kCharBits; // "七": 1000 to 1999
        std::vector<std::vector<grams::Key>> docKeys(3000);
        for (EntryId doc = 0; doc < docKeys.size(); ++doc) {
            std::vector<grams::Key>& k = docKeys[doc];
            if (doc % 25 == 7) { // dense: 10500 trigrams
                for (std::uint32_t i = 0; i < 10500; ++i)
                    k.push_back(trigram(doc * 7919 + i * 3));
            } else {
                for (std::uint32_t i = 0; i < 10; ++i)
                    k.push_back(trigram(doc * 31 + i * 101));
            }
            if (spread(doc) % 2 == 0)
                k.push_back(kOne);
            if (spread(doc) % 10 == 3)
                k.push_back(kTwo);
            if (doc >= 1000 && doc < 2000)
                k.push_back(kRun);
            k.push_back(((grams::Key {0x4E10} + doc % 50) << grams::kCharBits) | 0x4E20);
            std::sort(k.begin(), k.end());
            k.erase(std::unique(k.begin(), k.end()), k.end());
            QVERIFY(coded.add(doc, k, false, 0, 0));
        }
        QCOMPARE(coded.stats().segments, std::size_t {1}); // the dense documents filled one
        QVERIFY(coded.stats().memoryPairs > 0);
        std::vector<QString> phrases = {u"一"_s, u"丁"_s, u"七"_s, u"一丁"_s};
        for (std::uint32_t t = 0; t < 37 * 37 * 37; t += 401) {
            const grams::Key key = trigram(t);
            phrases.push_back(QString(QChar(char16_t(key >> grams::kCharBits))) + QChar(char16_t((key >> 8) & 0xFF))
                + QChar(char16_t(key & 0xFF)));
        }
        phrases.push_back(QString(QChar(0x4E10 + 7)) + QChar(0x4E20));
        const auto check = [&](const ContentIndex& content, const std::vector<bool>& gone) {
            for (const QString& phrase : phrases) {
                const std::vector<grams::Key> wanted = grams::ofPhrase(phrase);
                std::vector<EntryId> expected;
                for (EntryId doc = 0; doc < docKeys.size(); ++doc) {
                    if (!gone[doc]
                        && std::includes(docKeys[doc].begin(), docKeys[doc].end(), wanted.begin(), wanted.end()))
                        expected.push_back(doc);
                }
                if (matchesOf(content, phrase) != expected)
                    return false;
            }
            return true;
        };
        std::vector<bool> gone(docKeys.size(), false);
        QVERIFY(check(coded, gone));
        QVERIFY(coded.merge());
        QVERIFY(check(coded, gone));
        // On several threads, stretches of keys each: the same file.
        ContentIndex twin(dir.filePath(u"twin"_s));
        for (EntryId doc = 0; doc < docKeys.size(); ++doc)
            QVERIFY(twin.add(doc, docKeys[doc], false, 0, 0));
        QVERIFY(twin.merge(4));
        QVERIFY(check(twin, gone));
        const auto segmentBytes = [](const ContentIndex& content) {
            QFile file(content.segmentPaths().front());
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        QVERIFY(!segmentBytes(coded).isEmpty());
        QCOMPARE(segmentBytes(twin), segmentBytes(coded));
        std::vector<EntryId> retired;
        for (EntryId doc = 0; doc < docKeys.size(); doc += 3) {
            retired.push_back(doc);
            gone[doc] = true;
        }
        coded.retire(retired);
        QVERIFY(check(coded, gone));
        QVERIFY(coded.merge());
        QVERIFY(check(coded, gone));
        std::vector<EntryId> same(docKeys.size());
        std::iota(same.begin(), same.end(), EntryId {0});
        std::vector<std::uint64_t> codedSegments;
        const std::vector<char> codedState = coded.serialize(same, codedSegments);
        ContentIndex reopened(dir.filePath(u"coded"_s));
        QVERIFY(reopened.restore(codedState, docKeys.size()));
        QVERIFY(check(reopened, gone));

        // Copies of a file share one content: in memory, written out, saved
        // and restored; it goes once the last copy is gone.
        ContentIndex copies(dir.filePath(u"copies"_s));
        const std::vector<grams::Key> report = gramsOf(u"季度报告 quarterly report"_s);
        QVERIFY(copies.add(1, report, false, 0, 0));
        QVERIFY(copies.add(2, report, false, 0, 0));
        QVERIFY(copies.add(3, gramsOf(u"年度计划"_s), false, 0, 0));
        QCOMPARE(copies.stats().contents, std::size_t {2});
        QCOMPARE(matchesOf(copies, u"报告"_s), (std::vector<EntryId> {1, 2}));
        QVERIFY(copies.merge());
        QVERIFY(copies.add(4, report, false, 0, 0)); // found in the segment
        QCOMPARE(copies.stats().contents, std::size_t {2});
        QCOMPARE(copies.stats().memoryPairs, std::size_t {0});
        copies.retire(std::vector<EntryId> {1, 2});
        QVERIFY(copies.merge());
        QCOMPARE(matchesOf(copies, u"quarterly"_s), (std::vector<EntryId> {4}));
        std::vector<EntryId> fiveIds(5);
        std::iota(fiveIds.begin(), fiveIds.end(), EntryId {0});
        std::vector<std::uint64_t> copySegments;
        ContentIndex reloaded(dir.filePath(u"copies"_s));
        QVERIFY(reloaded.restore(copies.serialize(fiveIds, copySegments), 5));
        QVERIFY(reloaded.add(1, report, false, 0, 0));
        QCOMPARE(reloaded.stats().contents, std::size_t {2});
        QCOMPARE(matchesOf(reloaded, u"季度"_s), (std::vector<EntryId> {1, 4}));
        reloaded.retire(std::vector<EntryId> {1, 4});
        QCOMPARE(reloaded.stats().contents, std::size_t {1});
        QVERIFY(reloaded.merge());
        QVERIFY(matchesOf(reloaded, u"季度"_s).empty());
        QVERIFY(reloaded.add(2, report, false, 0, 0)); // a content of its own again, in memory
        QCOMPARE(matchesOf(reloaded, u"季度"_s), (std::vector<EntryId> {2}));
        ContentIndex again(dir.filePath(u"copies"_s));
        QVERIFY(again.restore(reloaded.serialize(fiveIds, copySegments), 5));
        QVERIFY(again.stats().memoryPairs > 0);
        QVERIFY(again.add(4, report, false, 0, 0)); // the one in memory, known from its grams
        QCOMPARE(again.stats().contents, std::size_t {2});
        QCOMPARE(again.stats().memoryPairs, report.size());
        QCOMPARE(matchesOf(again, u"季度报告"_s), (std::vector<EntryId> {2, 4}));
        QVERIFY(again.merge());
        QCOMPARE(matchesOf(again, u"report"_s), (std::vector<EntryId> {2, 4}));
        QCOMPARE(matchesOf(again, u"计划"_s), (std::vector<EntryId> {3}));

        // State that does not fit the snapshot's entries is not restored.
        ContentIndex wrong(dir.filePath(u"other"_s));
        QVERIFY(!wrong.restore(state, 3)); // its segment is in another folder
        QVERIFY(!wrong.restore({}, 0));
        QCOMPARE(wrong.stats().documents, std::size_t {0});
    }

    void contentIndexerReadsFiles()
    {
        QTemporaryDir dir;
        const auto write = [&](const QString& name, const QByteArray& data) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::WriteOnly))
                return std::wstring();
            f.write(data);
            return QDir::toNativeSeparators(f.fileName()).toStdWString();
        };
        const std::wstring utf8 = write(u"a.txt"_s, u"第一行\n合同条款"_s.toUtf8());
        const auto text = ContentIndexer::readFile(utf8, 0, {});
        QCOMPARE(text.outcome, ContentIndexer::Outcome::Indexed);
        QVERIFY(hasAll(text.keys, u"合同条款"_s) && !hasAll(text.keys, u"行合"_s));
        const auto stamp = ContentIndexer::stampOf(utf8);
        QVERIFY(stamp && *stamp == text.stamp);

        const std::wstring bom = write(u"b.txt"_s, utf16le(u"合同"_s, true));
        QVERIFY(hasAll(ContentIndexer::readFile(bom, 0, {}).keys, u"合同"_s));
        const auto tooLarge = ContentIndexer::readFile(utf8, 4, {});
        QCOMPARE(tooLarge.outcome, ContentIndexer::Outcome::Empty); // too large
        QVERIFY(tooLarge.large);
        const auto nothing = ContentIndexer::readFile(write(u"c.txt"_s, {}), 0, {});
        QCOMPARE(nothing.outcome, ContentIndexer::Outcome::Empty);
        QVERIFY(!nothing.large);
        QCOMPARE(ContentIndexer::readFile(utf8 + L".missing", 0, {}).outcome, ContentIndexer::Outcome::Skipped);
        QVERIFY(!ContentIndexer::stampOf(utf8 + L".missing"));

        // A search's read: the match, and the same grams as the indexer's,
        // the rest of the file read after the match (here: chunks later).
        QByteArray big = u"合同 第一条\n"_s.toUtf8();
        big += QByteArray(static_cast<qsizetype>(ContentScanner::kChunkBytes) * 2, 'x');
        big += u"\n季度报告"_s.toUtf8();
        const std::wstring large = write(u"d.txt"_s, big);
        const ContentScanner contract(u"合同"_s);
        const auto scanned = ContentIndexer::scanFile(large, 0, contract, {});
        QVERIFY(scanned.match && scanned.match->line == 1);
        QCOMPARE(scanned.text.outcome, ContentIndexer::Outcome::Indexed);
        const auto whole = ContentIndexer::readFile(large, 0, {});
        QCOMPARE(scanned.text.keys, whole.keys);
        QCOMPARE(scanned.text.stamp, whole.stamp);
        QVERIFY(hasAll(scanned.text.keys, u"季度报告"_s));
        const auto none = ContentIndexer::scanFile(utf8, 0, ContentScanner(u"nowhere"_s), {});
        QVERIFY(!none.match);
        QCOMPARE(none.text.keys, text.keys);
        const auto ansi = write(u"e.txt"_s, QByteArray("plain \xB0\xA1 GBK"));
        QCOMPARE(ContentIndexer::scanFile(ansi, 0, ContentScanner(u"\U0001F600"_s), {}).text.keys,
            ContentIndexer::readFile(ansi, 0, {}).keys); // the phrase cannot be in it: read anyway
        const auto overLimit = ContentIndexer::scanFile(utf8, 4, contract, {});
        QCOMPARE(overLimit.text.outcome, ContentIndexer::Outcome::Empty);
        QVERIFY(overLimit.text.large && !overLimit.match);
        QVERIFY(!scanned.text.large);
        const auto cut = ContentIndexer::scanFile(large, 0, ContentScanner(u"nowhere"_s), [] { return true; });
        QCOMPARE(cut.text.outcome, ContentIndexer::Outcome::Skipped);
    }

    // What a search reads goes into the index, unless the indexer is off,
    // paused, or the file is on a volume it does not follow.
    void contentIntake()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString root = QDir::toNativeSeparators(tmp.path());
        const std::string rootName = wtf8::fromUtf16(wtf8::view(root));
        auto index = std::make_shared<FileIndex>();
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] { return std::vector<ContentIndexer::Volume> {{rootName, false}}; };
        bool ready = false;
        source.ready = [&] { return ready; };
        using namespace std::chrono_literals;
        ContentIndexer indexer(content, source, {}, {1h, 1h, 1h, 0ms}); // no pass of its own
        const std::vector<grams::Key> keys = grams::ofPhrase(u"合同条款"_s);
        const ContentIndexer::FileText text {ContentIndexer::Outcome::Indexed, keys, 7};

        {
            ContentIndexer::Intake intake(&indexer);
            QVERIFY(!intake.wants((root + u"\\a.txt"_s).toStdWString())); // the file index is not complete
        }
        ready = true;
        {
            ContentIndexer::Intake intake(&indexer);
            QVERIFY(intake.wants((root + u"\\a.txt"_s).toStdWString()));
            QVERIFY(!intake.wants(L"Q:\\a.txt"));
            QVERIFY(!intake.wants(root.toStdWString())); // the root itself is no file
            intake.take(1, text, intake.since());
            intake.take(2, {ContentIndexer::Outcome::Empty, {}, 8}, intake.since());
            intake.take(3, {}, intake.since()); // Skipped: not taken
            ContentIndexer::FileText tooLarge;
            tooLarge.outcome = ContentIndexer::Outcome::Empty;
            tooLarge.large = true;
            tooLarge.stamp = 9;
            intake.take(6, tooLarge, intake.since());
        }
        QCOMPARE(content->stats().documents, std::size_t {3});
        QVERIFY(content->knows(1) && content->knows(2) && !content->knows(3) && content->knows(6));
        QCOMPARE(matchesOf(*content, u"合同"_s), std::vector<EntryId> {1});
        QCOMPARE(knownOf(*content, u"x"_s), (std::vector<EntryId> {1, 2, 6})); // no grams, yet all known
        QCOMPARE(content->lookup(u"x"_s).large, (std::vector<EntryId> {6})); // ... and one for its size

        // Written to while the search read it: taken, but dirty.
        {
            ContentIndexer::Intake intake(&indexer);
            const std::uint64_t since = intake.since();
            content->markChanged(std::vector<EntryId> {4});
            intake.take(4, text, since);
        }
        QVERIFY(!content->knows(4));

        indexer.pause();
        {
            ContentIndexer::Intake intake(&indexer);
            QVERIFY(!intake.wants((root + u"\\a.txt"_s).toStdWString()));
        }
        indexer.resume();
        {
            ContentIndexer::Intake intake(&indexer);
            const std::uint64_t since = intake.since();
            indexer.pause(); // while the search runs
            intake.take(5, text, since);
            indexer.resume();
        }
        QVERIFY(!content->knows(5));
    }

    void contentIndexerFollowsFiles()
    {
        // A volume whose root is a real folder, so the indexer reads real files.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto write = [&](const QString& name, const QString& text) {
            QFile f(tmp.filePath(name));
            return f.open(QIODevice::WriteOnly) && f.write(text.toUtf8()) >= 0;
        };
        QVERIFY(write(u"a.txt"_s, u"合同条款"_s) && write(u"b.md"_s, u"English"_s) && write(u"c.log"_s, u"季度报告"_s)
            && write(u"d.bin"_s, u"合同"_s));
        const std::string rootName = wtf8::fromUtf16(wtf8::view(QDir::toNativeSeparators(tmp.path())));
        auto index = std::make_shared<FileIndex>();
        EntryId a = kNoEntry;
        EntryId b = kNoEntry;
        EntryId c = kNoEntry;
        {
            auto lock = index->writeLock();
            const EntryId root = index->addRoot(rootName);
            a = index->add(root, "a.txt", 0);
            b = index->add(root, "b.md", 0);
            c = index->add(root, "c.log", 0);
            index->add(root, "d.bin", 0);
        }
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] { return std::vector<ContentIndexer::Volume> {{rootName, false}}; };
        source.ready = [] { return true; };
        ContentIndexer::Options options;
        options.extensions = {u"txt"_s, u"md"_s, u"log"_s};
        using namespace std::chrono_literals;
        ContentIndexer indexer(content, source, options, {0ms, 0ms, 1h, 0ms});
        const auto waitFor = [](const auto& done) {
            for (int i = 0; i < 1000 && !done(); ++i)
                std::this_thread::sleep_for(10ms);
            return done();
        };
        QVERIFY(waitFor([&] { return content->stats().documents == 3; }));
        QCOMPARE(matchesOf(*content, u"合同"_s), std::vector<EntryId> {a});
        QCOMPARE(knownOf(*content, u"报告"_s), (std::vector<EntryId> {a, b, c}));
        QCOMPARE(matchesOf(*content, u"english"_s), std::vector<EntryId> {b});

        // Written to (the journal says so): read again.
        QVERIFY(write(u"a.txt"_s, u"新的内容"_s));
        content->markChanged(std::vector<EntryId> {a});
        QVERIFY(waitFor([&] { return matchesOf(*content, u"内容"_s) == std::vector<EntryId> {a}; }));
        QVERIFY(matchesOf(*content, u"合同"_s).empty());

        // Files a search no longer looks in lose their documents.
        options.extensions = {u"txt"_s, u"log"_s};
        indexer.setOptions(options);
        QVERIFY(waitFor([&] { return content->stats().documents == 2; }));
        QCOMPARE(knownOf(*content, u"报告"_s), (std::vector<EntryId> {a, c}));
        options.enabled = false;
        indexer.setOptions(options);
        QCOMPARE(content->stats().documents, std::size_t {0});
    }

    // ---- documents -------------------------------------------------------------

    void extractWriter()
    {
        using doctext::PlaceKind;
        doctext::DocText doc;
        extract::Writer w(doc, 1000);
        w.put(u"  a  \t b ");
        w.newline();
        w.newline();
        const std::uint16_t sheet = w.addSheet("S");
        w.place(PlaceKind::Row, 3, sheet);
        w.putUtf8("x");
        w.newline();
        w.place(PlaceKind::Row, 4, sheet); // follows on: no new place
        w.putUtf8("y");
        w.newline();
        w.place(PlaceKind::Row, 7, sheet);
        w.putUtf8("z");
        w.put(char32_t {0xFEFF});
        w.put(char32_t {0x200B});
        w.put(char32_t {0xA0});
        w.putUtf8("w\xFF");
        w.finish();
        QCOMPARE(doc.text, std::string("a\tb\nx\ny\nz w\xEF\xBF\xBD\n"));
        QCOMPARE(doc.places.size(), std::size_t {2});
        QVERIFY((doc.places[0] == doctext::Place {2, 3, sheet, PlaceKind::Row}));
        QVERIFY((doc.places[1] == doctext::Place {4, 7, sheet, PlaceKind::Row}));
        QCOMPARE(doctext::locate(doc, 3).number, std::uint32_t {4});
        QCOMPARE(doctext::locate(doc, 1).kind, PlaceKind::None);

        // Cut at the limit, on a whole character.
        doctext::DocText small;
        extract::Writer s(small, 8);
        s.putUtf8("合同合同合同");
        s.finish();
        QVERIFY(small.truncated);
        QCOMPARE(small.text, std::string("合同\n"));
    }

    void extractXml()
    {
        const std::string xml = "<?xml version=\"1.0\"?><!DOCTYPE x [<!ENTITY e \"boom\">]><!-- c > -->"
                                "<w:doc a='1>2' w:b=\"&lt;&#x4E2D;&#25991;\" r:id=\"rId9\" id=\"7\">"
                                "<t>A&amp;B &e; &#xZZ;</t><![CDATA[<raw>&amp;]]><e/></w:doc>";
        for (const std::size_t chunk : {std::size_t {1}, std::size_t {3}, std::size_t {1} << 16}) {
            std::size_t at = 0;
            extract::XmlReader r([&](char* buffer, std::size_t capacity) {
                const std::size_t n = std::min({capacity, chunk, xml.size() - at});
                std::memcpy(buffer, xml.data() + at, n);
                at += n;
                return n;
            });
            QStringList events;
            std::string text;
            for (auto t = r.next(); t != extract::XmlReader::Token::Done; t = r.next()) {
                const QString name = QString::fromUtf8(r.name());
                if (t == extract::XmlReader::Token::Start) {
                    events.append(u'<' + name);
                    if (name == u"doc") {
                        QCOMPARE(r.attribute("a"), std::string("1>2"));
                        QCOMPARE(r.attribute("b"), std::string("<中文"));
                        QCOMPARE(r.attribute("id"), std::string("7"));
                        QCOMPARE(r.prefixedAttribute("id"), std::string("rId9"));
                    }
                } else if (t == extract::XmlReader::Token::End) {
                    events.append(u'/' + name);
                } else {
                    text += r.text();
                }
            }
            QCOMPARE(events, (QStringList {u"<doc"_s, u"<t"_s, u"/t"_s, u"<e"_s, u"/e"_s, u"/doc"_s}));
            QCOMPARE(text, std::string("A&B &e; &#xZZ;<raw>&amp;"));
            QVERIFY(!r.failed());
        }
    }

    void extractDocx()
    {
        const std::string rels
            = R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
              R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>)";
        const std::string documentRels
            = R"(<Relationships><Relationship Id="rId5" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/header" Target="header1.xml"/>)"
              R"(<Relationship Id="rId6" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" Target="http://x" TargetMode="External"/></Relationships>)";
        const std::string body
            = R"(<w:document xmlns:w="w" xmlns:mc="mc"><w:body>)"
              R"(<w:p><w:r><w:t>合同</w:t></w:r><w:r><w:t xml:space="preserve">编号 </w:t></w:r><w:r><w:tab/><w:t>A-1</w:t></w:r></w:p>)"
              R"(<w:p><w:pPr><w:tabs><w:tab w:val="left" w:pos="720"/></w:tabs></w:pPr><w:r><w:fldChar w:fldCharType="begin"/></w:r>)"
              R"(<w:r><w:instrText> PAGE </w:instrText></w:r><w:r><w:fldChar w:fldCharType="separate"/></w:r><w:r><w:t>7</w:t></w:r>)"
              R"(<w:r><w:fldChar w:fldCharType="end"/></w:r><w:del><w:r><w:delText>删掉的</w:delText></w:r></w:del></w:p>)"
              R"(<w:tbl><w:tr><w:tc><w:p><w:r><w:t>甲</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>乙</w:t></w:r></w:p>)"
              R"(<w:p><w:r><w:t>丙</w:t></w:r></w:p></w:tc></w:tr></w:tbl>)"
              R"(<w:p><w:r><w:lastRenderedPageBreak/><w:t>第二页</w:t></w:r></w:p>)"
              R"(<w:p><w:r><w:t>长段落前半</w:t></w:r><w:r><w:lastRenderedPageBreak/><w:t>后半</w:t></w:r></w:p>)"
              R"(<w:p><mc:AlternateContent><mc:Choice><w:r><w:t>新</w:t></w:r></mc:Choice>)"
              R"(<mc:Fallback><w:r><w:t>旧</w:t></w:r></mc:Fallback></mc:AlternateContent></w:p>)"
              R"(</w:body></w:document>)";
        const QByteArray docx = makeZip({{"[Content_Types].xml", "<Types/>"}, {"_rels/.rels", rels},
            {"word/_rels/document.xml.rels", documentRels}, {"word/document.xml", body},
            {"word/header1.xml", "<w:hdr><w:p><w:r><w:t>页眉文字</w:t></w:r></w:p></w:hdr>"}});
        const Extracted out = extractBytes(docx);
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text),
            (QStringList {u"page 1|合同编号\tA-1"_s, u"page 1|7"_s, u"page 1|甲\t乙 丙"_s, u"page 2|第二页"_s,
                u"page 2|长段落前半后半"_s, u"page 3|新"_s, u"-|页眉文字"_s}));
    }

    void extractXlsx()
    {
        const std::string rels
            = R"(<Relationships><Relationship Id="rId1" Type="http://purl.oclc.org/ooxml/officeDocument/relationships/officeDocument" Target="/xl/workbook.xml"/></Relationships>)";
        const std::string workbookRels
            = R"(<Relationships><Relationship Id="rId1" Type="x/worksheet" Target="worksheets/sheet1.xml"/>)"
              R"(<Relationship Id="rId2" Type="x/worksheet" Target="worksheets/sheet2.xml"/>)"
              R"(<Relationship Id="rId4" Type="x/chartsheet" Target="chartsheets/sheet1.xml"/>)"
              R"(<Relationship Id="rId3" Type="x/sharedStrings" Target="sharedStrings.xml"/></Relationships>)";
        const std::string workbook = R"(<workbook xmlns:r="r"><sheets><sheet name="销售" sheetId="1" r:id="rId1"/>)"
                                     R"(<sheet name="Chart" sheetId="3" r:id="rId4"/>)"
                                     R"(<sheet name="Hidden" sheetId="2" state="hidden" r:id="rId2"/></sheets></workbook>)";
        const std::string strings = "<sst><si><t>合同</t></si><si><r><t>富</t></r><r><t>文本</t></r><rPh><t>ふ</t></rPh></si>"
                                    "<si><t>多\n行</t></si></sst>";
        const std::string sheet1
            = R"(<worksheet><sheetData><row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1"><v>1234.5</v></c><c r="C1" t="b"><v>1</v></c></row>)"
              R"(<row r="2"><c r="A2" t="s"><v>1</v></c></row>)"
              R"(<row r="5"><c r="A5" t="inlineStr"><is><t>内联</t></is></c><c r="B5" t="str"><f>A1&amp;"x"</f><v>合同x</v></c>)"
              R"(<c r="C5" t="s"><v>2</v></c><c r="D5" t="s"><v>99</v></c></row></sheetData></worksheet>)";
        const std::string sheet2
            = R"(<x:worksheet xmlns:x="x"><x:sheetData><x:row r="3"><x:c r="A3" t="s"><x:v>0</x:v></x:c></x:row></x:sheetData></x:worksheet>)";
        const QByteArray xlsx = makeZip({{"_rels/.rels", rels}, {"xl/workbook.xml", workbook},
            {"xl/_rels/workbook.xml.rels", workbookRels}, {"xl/sharedStrings.xml", strings},
            {"xl/worksheets/sheet1.xml", sheet1}, {"xl/worksheets/sheet2.xml", sheet2}});
        const Extracted out = extractBytes(xlsx);
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text), (QStringList {u"销售!1|合同\t1234.5"_s, u"销售!2|富文本"_s,
                                           u"销售!5|内联\t合同x\t多 行"_s, u"Hidden!3|合同"_s}));
        QCOMPARE(out.text.places.size(), std::size_t {3}); // rows 1 and 2 follow on
    }

    void extractPptx()
    {
        const std::string rels
            = R"(<Relationships><Relationship Id="rId1" Type="t/officeDocument" Target="ppt/presentation.xml"/></Relationships>)";
        const std::string presentationRels = R"(<Relationships><Relationship Id="rId2" Type="t/slide" Target="slides/slide1.xml"/>)"
                                             R"(<Relationship Id="rId3" Type="t/slide" Target="slides/slide2.xml"/></Relationships>)";
        const std::string presentation
            = R"(<p:presentation xmlns:p="p" xmlns:r="r"><p:sldIdLst><p:sldId id="256" r:id="rId3"/><p:sldId id="257" r:id="rId2"/></p:sldIdLst></p:presentation>)";
        const auto slide = [](const std::string& paragraphs) {
            return R"(<p:sld xmlns:p="p" xmlns:a="a"><p:cSld><p:spTree><p:sp><p:txBody>)" + paragraphs
                + "</p:txBody></p:sp></p:spTree></p:cSld></p:sld>";
        };
        const QByteArray pptx = makeZip({{"_rels/.rels", rels}, {"ppt/presentation.xml", presentation},
            {"ppt/_rels/presentation.xml.rels", presentationRels},
            {"ppt/slides/slide1.xml", slide("<a:p><a:r><a:t>幻灯片一</a:t></a:r></a:p>")},
            {"ppt/slides/slide2.xml",
                slide("<a:p><a:r><a:t>幻灯片二</a:t></a:r><a:br><a:rPr/></a:br><a:r><a:t>B</a:t></a:r></a:p>")},
            {"ppt/slides/_rels/slide2.xml.rels",
                R"(<Relationships><Relationship Id="rId1" Type="t/notesSlide" Target="../notesSlides/notesSlide1.xml"/></Relationships>)"},
            {"ppt/notesSlides/notesSlide1.xml", slide("<a:p><a:r><a:t>备注二</a:t></a:r></a:p>")}});
        const Extracted out = extractBytes(pptx);
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text), (QStringList {u"slide 1|幻灯片二 B"_s, u"slide 1|备注二"_s, u"slide 2|幻灯片一"_s}));
    }

    void extractDoc()
    {
        // WordDocument: the FIB (Word 97 layout), then the text of two pieces.
        QByteArray word(0x1000, '\0');
        const auto put16 = [&](int at, quint16 v) { std::memcpy(word.data() + at, &v, 2); };
        const auto put32 = [&](int at, quint32 v) { std::memcpy(word.data() + at, &v, 4); };
        put16(0, 0xA5EC);
        put16(2, 0xC1);
        put16(0x0A, 0x0200); // the table stream is 1Table
        put16(0x20, 14);
        put16(0x3E, 22);
        put16(0x98, 0x5D);
        const std::u16string unicode = u"合同编号\r\x13 PAGE \x14" u"5\x15\r";
        const QByteArray compressed("Caf\xE9 \x93q\x94\r");
        std::memcpy(word.data() + 0x800, unicode.data(), unicode.size() * 2);
        std::memcpy(word.data() + 0x900, compressed.constData(), static_cast<std::size_t>(compressed.size()));
        const auto n0 = static_cast<quint32>(unicode.size());
        const auto n1 = static_cast<quint32>(compressed.size());
        // 1Table: the Clx, a property run first, then the piece table.
        QByteArray plc = le32(0) + le32(n0) + le32(n0 + n1);
        plc += le16({0}) + le32(0x800) + le16({0});
        plc += le16({0}) + le32((0x900 * 2) | 0x40000000u) + le16({0});
        const QByteArray clx = QByteArray("\x01\x02\x00\xAA\xBB", 5) + QByteArray(1, '\x02')
            + le32(static_cast<quint32>(plc.size())) + plc;
        put32(0x1A2, 0);
        put32(0x1A6, static_cast<quint32>(clx.size()));
        const QByteArray doc = makeCfb({{u"WordDocument", word}, {u"1Table", clx}});
        const Extracted out = extractBytes(doc);
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(QString::fromUtf8(out.text.text), u"合同编号\n5\nCafé “q”\n"_s);

        put16(0x0A, 0x0300); // encrypted
        QCOMPARE(extractBytes(makeCfb({{u"WordDocument", word}, {u"1Table", clx}})).status, extract::Status::Encrypted);
    }

    void extractXls()
    {
        const QByteArray bof = le16({0x0600, 0x0005}) + QByteArray(12, '\0');
        const QByteArray sheetBof = le16({0x0600, 0x0010}) + QByteArray(12, '\0');
        // Shared strings: "合同", then "Total" cut by a CONTINUE that goes on in UTF-16.
        const QByteArray sst = le32(2) + le32(2) + le16({2}) + QByteArray(1, '\x01') + utf16(u"合同") + le16({5})
            + QByteArray(1, '\0') + QByteArray("Tot");
        const QByteArray continued = QByteArray(1, '\x01') + utf16(u"al");
        const auto boundSheet = [&](quint32 at) {
            return biff(0x0085, le32(at) + QByteArray(2, '\0') + QByteArray(1, '\x02') + QByteArray(1, '\x01') + utf16(u"表一"));
        };
        const QByteArray globalsWithout = biff(0x0809, bof) + boundSheet(0) + biff(0x00FC, sst) + biff(0x003C, continued)
            + biff(0x000A, {});
        const QByteArray globals = biff(0x0809, bof) + boundSheet(static_cast<quint32>(globalsWithout.size()))
            + biff(0x00FC, sst) + biff(0x003C, continued) + biff(0x000A, {});
        const auto cell = [](quint16 row, quint16 col) { return le16({row, col, 0}); };
        double number = 1234.5;
        const QByteArray formulaValue = QByteArray(1, '\0') + QByteArray(5, '\0') + QByteArray("\xFF\xFF", 2);
        const QByteArray sheet = biff(0x0809, sheetBof) + biff(0x00FD, cell(0, 0) + le32(0))
            + biff(0x0203, cell(0, 1) + QByteArray(reinterpret_cast<const char*>(&number), 8))
            + biff(0x0006, cell(0, 2) + formulaValue + QByteArray(8, '\0'))
            + biff(0x0207, le16({2}) + QByteArray(1, '\x01') + utf16(u"公式")) + biff(0x00FD, cell(2, 0) + le32(1))
            + biff(0x027E, cell(4, 0) + le32((42u << 2) | 2u)) + biff(0x000A, {});
        const Extracted out = extractBytes(makeCfb({{u"Workbook", globals + sheet}}));
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text), (QStringList {u"表一!1|合同\t1234.5\t公式"_s, u"表一!3|Total"_s, u"表一!5|42"_s}));

        const QByteArray locked = biff(0x0809, bof) + biff(0x002F, QByteArray(6, '\0')) + biff(0x000A, {});
        QCOMPARE(extractBytes(makeCfb({{u"Workbook", locked}})).status, extract::Status::Encrypted);
    }

    void extractPpt()
    {
        const auto atom = [](quint16 type, const QByteArray& body) { return pptRecord(0, type, body); };
        const QByteArray slideList = atom(0x03F3, QByteArray(20, '\0')) + atom(0x0FA0, utf16(u"标题一\r正文"))
            + atom(0x03F3, QByteArray(20, '\0')) + atom(0x0FA8, "Second slide");
        const QByteArray stream = pptRecord(0x000F, 0x03E8,
                                      pptRecord(0x000F, 0x0FF0, slideList) + pptRecord(0x001F, 0x0FF0, atom(0x0FA0, utf16(u"母版"))))
            + pptRecord(0x000F, 0x03F8, atom(0x0FA0, utf16(u"母版文字")))
            + pptRecord(0x000F, 0x03EE, pptRecord(0x000F, 0xF00D, atom(0x0FA0, utf16(u"文本框"))));
        const Extracted out = extractBytes(makeCfb({{u"PowerPoint Document", stream}}));
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text),
            (QStringList {u"slide 1|标题一"_s, u"slide 1|正文"_s, u"slide 2|Second slide"_s, u"-|文本框"_s}));
    }

    void extractPdf()
    {
        const Extracted out = extractBytes(makePdf({{"Hello", "World"}, {"Second page"}}));
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text), (QStringList {u"page 1|Hello World"_s, u"page 2|Second page"_s}));
        QVERIFY(extractBytes(QByteArray("%PDF-1.4\n1 0 obj << >> garbage")).status != extract::Status::Ok);
    }

    void extractTextUnderDocumentNames()
    {
        // An HTML table a web system saved as ".xls", in GBK.
        const QByteArray html = encode(u"<html><head><meta charset=\"gb2312\"><style>td{}</style>"
                                       u"<script>var a='<td>';</script></head><body><table>"
                                       u"<tr><td>合同</td><td>金额&nbsp;100</td></tr><tr><td>第二行</td></tr></table></body></html>"_s,
            936);
        Extracted out = extractBytes(html);
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text), (QStringList {u"-|合同\t金额 100"_s, u"-|第二行"_s}));
        out = extractBytes(encode(u"a,b\r\n合同,1\r\n"_s, 936));
        QCOMPARE(placedLines(out.text), (QStringList {u"-|a,b"_s, u"-|合同,1"_s}));
    }

    void extractRejects()
    {
        QCOMPARE(extractBytes(QByteArray("\x01\x02\x00\x04 binary", 10)).status, extract::Status::Unsupported);
        QCOMPARE(extractBytes(makeZip({{"a.txt", "x"}})).status, extract::Status::Unsupported);
        const QByteArray docx = makeZip({{"_rels/.rels", "<Relationships/>"}, {"word/document.xml", "<w:document/>"}});
        QCOMPARE(extractBytes(docx.left(docx.size() / 2)).status, extract::Status::Broken);
        QCOMPARE(extractBytes(makeCfb({{u"EncryptedPackage", QByteArray(100, 'x')}})).status, extract::Status::Encrypted);
        QCOMPARE(extractBytes(makeCfb({{u"Other", QByteArray(100, 'x')}})).status, extract::Status::Unsupported);
        QCOMPARE(extractBytes(makeCfb({})).status, extract::Status::Unsupported);
        // A compound file whose FAT points past its end, or into a loop.
        QByteArray cfb = makeCfb({{u"WordDocument", QByteArray(5000, 'x')}});
        QByteArray looped = cfb;
        std::memset(looped.data() + looped.size() - 512, 0, 512); // every sector's next is sector 0
        QVERIFY(extractBytes(looped).status != extract::Status::Ok);
        QVERIFY(extractBytes(cfb.left(1024)).status != extract::Status::Ok);
    }

    void docTextSerialization()
    {
        doctext::DocText doc;
        doc.text = "第一行\n第二行\n";
        doc.sheets = {"表"};
        doc.places = {{1, 3, 0, doctext::PlaceKind::Row}, {2, 9, doctext::Place::kNoSheet, doctext::PlaceKind::Page}};
        doc.truncated = true;
        const std::string bytes = doctext::serialize(doc);
        QVERIFY(doctext::deserialize(bytes, 1000) == doc);
        QVERIFY(!doctext::deserialize(bytes, 4)); // more text than allowed
        QVERIFY(!doctext::deserialize(bytes.substr(0, bytes.size() - 1), 1000));
        QVERIFY(!doctext::deserialize(bytes + "x", 1000));
        std::string badSheet = bytes;
        badSheet[20 + 8] = 5; // the first place's sheet: none such
        QVERIFY(!doctext::deserialize(badSheet, 1000));
        std::string badLine = bytes;
        badLine[20] = 0; // line 0
        QVERIFY(!doctext::deserialize(badLine, 1000));
    }

    void contentIndexKeepsTexts()
    {
        QTemporaryDir dir;
        doctext::DocText a;
        a.text = "合同编号\n";
        a.places = {{1, 3, doctext::Place::kNoSheet, doctext::PlaceKind::Page}};
        doctext::DocText b = a;
        b.text = "新合同\n";
        std::vector<char> state;
        std::vector<std::uint64_t> files;
        {
            ContentIndex content(dir.path());
            QVERIFY(content.add(10, gramsOf(u"合同编号"_s), false, 1, 0, ContentIndex::packText(a)));
            QVERIFY(content.add(20, gramsOf(u"其他"_s), false, 2, 0));
            QVERIFY(content.textOf(10) == a);
            QVERIFY(!content.textOf(20));
            QVERIFY(!content.textOf(30));
            QVERIFY(content.add(10, gramsOf(u"新合同"_s), false, 3, 0, ContentIndex::packText(b))); // replaces it
            QVERIFY(content.textOf(10) == b);
            content.markChanged(std::vector<EntryId> {10});
            QVERIFY(!content.textOf(10)); // written to: the text is not current
            QVERIFY(content.add(10, gramsOf(u"合同编号"_s), false, 4, content.changeSequence(), ContentIndex::packText(a)));
            QCOMPARE(content.stats().texts, std::size_t {1});
            const std::uint64_t before = content.stats().textBytes;
            QVERIFY(content.merge()); // renumbers documents
            QVERIFY(content.textOf(10) == a);
            QVERIFY(content.compactTexts());
            QVERIFY(content.textOf(10) == a);
            QVERIFY(content.stats().textBytes < before);
            std::vector<EntryId> newIds(40, kNoEntry);
            newIds[10] = 1;
            newIds[20] = 2;
            state = content.serialize(newIds, files);
            content.saved(files);
        }
        QCOMPARE(QDir(dir.path()).entryList({u"*.texts"_s}).size(), qsizetype {1}); // the old one is gone
        ContentIndex restored(dir.path());
        QVERIFY(restored.restore(state, 3));
        QVERIFY(restored.textOf(1) == a);
        QVERIFY(!restored.textOf(2));
        restored.retire(std::vector<EntryId> {1});
        QVERIFY(!restored.textOf(1));
        QCOMPARE(restored.stats().texts, std::size_t {0});

        // Nothing to find in it: an empty text, so a search need not read the file.
        QVERIFY(restored.add(2, {}, true, 7, 0));
        QVERIFY(restored.textOf(2) && restored.textOf(2)->text.empty());

        // Moved to another folder (IndexService::moveTo): copied as
        // indexfolder::copy() does (the text file open for writing meanwhile),
        // then used from there.
        QVERIFY(restored.add(1, gramsOf(u"新合同"_s), false, 8, 0, ContentIndex::packText(b)));
        QTemporaryDir moved;
        for (const QFileInfo& f : QDir(dir.path()).entryInfoList({u"*.grams"_s, u"*.texts"_s}, QDir::Files)) {
            const std::wstring from = QDir::toNativeSeparators(f.absoluteFilePath()).toStdWString();
            const std::wstring to = QDir::toNativeSeparators(moved.filePath(f.fileName())).toStdWString();
            QVERIFY(::CopyFileExW(from.c_str(), to.c_str(), nullptr, nullptr, nullptr, 0));
        }
        QVERIFY(restored.relocate(moved.path()));
        QVERIFY(restored.textOf(1) == b);
        QVERIFY(restored.add(3, gramsOf(u"合同编号"_s), false, 9, 0, ContentIndex::packText(a))); // written there
        QVERIFY(restored.textOf(3) == a);
        const QStringList texts = QDir(moved.path()).entryList({u"*.texts"_s}, QDir::Files);
        QCOMPARE(texts.size(), qsizetype {1});
        // By its path: a directory listing has the size from before the last
        // writes until they are flushed (docs/pitfalls.md).
        QCOMPARE(QFileInfo(moved.filePath(texts.first())).size(), static_cast<qint64>(restored.stats().textBytes));
    }

    // Documents with the same text (copies of a file) share it; a copy found
    // before it is read gets the original's document.
    void contentIndexSharesTexts()
    {
        QTemporaryDir dir;
        doctext::DocText a;
        a.text = "合同编号\n";
        doctext::DocText b;
        b.text = "季度报告\n";
        const QByteArray packed = ContentIndex::packText(a);
        const auto once = static_cast<std::uint64_t>(packed.size());
        std::vector<char> state;
        std::vector<std::uint64_t> files;
        {
            ContentIndex content(dir.path());
            QVERIFY(content.add(10, gramsOf(u"合同编号"_s), false, 1, 0, packed));
            QVERIFY(content.add(20, gramsOf(u"合同编号"_s), false, 2, 0, packed));
            QCOMPARE(content.stats().texts, std::size_t {2});
            QCOMPARE(content.stats().distinctTexts, std::size_t {1});
            QCOMPARE(content.stats().textBytes, once);
            QVERIFY(content.add(10, gramsOf(u"合同编号"_s), false, 3, 0, packed)); // read again: the same text
            QCOMPARE(content.stats().distinctTexts, std::size_t {1});
            QCOMPARE(content.stats().textBytes, once);

            QVERIFY(content.addCopy(30, 10, 3, 4, 0));
            QVERIFY(content.textOf(30) == a);
            QCOMPARE(matchesOf(content, u"合同"_s), (std::vector<EntryId> {10, 20, 30}));
            QVERIFY(!content.addCopy(31, 10, 99, 5, 0)); // the original was read from another file
            QVERIFY(!content.addCopy(31, 11, 0, 5, 0)); // no such document
            QVERIFY(content.add(40, {}, true, 6, 0)); // nothing to find in it
            QVERIFY(content.addCopy(41, 40, 6, 7, 0));
            QVERIFY(content.textOf(41) && content.textOf(41)->text.empty());
            content.markChanged(std::vector<EntryId> {20});
            QVERIFY(!content.addCopy(32, 20, 2, 8, 0)); // not current
            QCOMPARE(content.stats().textBytes, once);

            content.retire(std::vector<EntryId> {10, 20});
            QVERIFY(content.textOf(30) == a); // the copy still has it
            QVERIFY(content.add(50, gramsOf(u"季度报告"_s), false, 9, 0, ContentIndex::packText(b)));
            QCOMPARE(content.stats().distinctTexts, std::size_t {2});
            content.retire(std::vector<EntryId> {50});
            QCOMPARE(content.stats().distinctTexts, std::size_t {1});
            QVERIFY(content.merge());
            QVERIFY(content.compactTexts());
            QCOMPARE(content.stats().textBytes, once); // a, once
            QVERIFY(content.textOf(30) == a);
            std::vector<EntryId> newIds(60, kNoEntry);
            newIds[30] = 3;
            newIds[41] = 4;
            state = content.serialize(newIds, files);
            content.saved(files);
        }
        ContentIndex restored(dir.path());
        QVERIFY(restored.restore(state, 10));
        QVERIFY(restored.textOf(3) == a);
        QVERIFY(restored.textOf(4) && restored.textOf(4)->text.empty());
        QVERIFY(restored.add(5, gramsOf(u"合同编号"_s), false, 10, 0, packed)); // shares it still
        QCOMPARE(restored.stats().distinctTexts, std::size_t {1});
        QCOMPARE(restored.stats().textBytes, once);
        QVERIFY(restored.textOf(5) == a);
    }

    void docExtractorSandbox()
    {
        if (!QFile::exists(QString::fromStdWString(DocExtractor::programPath())))
            QSKIP("WinShunExtract.exe is not built with this preset");
        QTemporaryDir dir;
        const auto write = [&](const QString& name, const QByteArray& data) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::WriteOnly))
                return std::wstring();
            f.write(data);
            return QDir::toNativeSeparators(f.fileName()).toStdWString();
        };
        const std::wstring pdf = write(u"a.pdf"_s, makePdf({{"Contract", "number"}}));
        const std::wstring bad = write(u"b.docx"_s, QByteArray("\x01\x02\x00\x04 binary", 10));
        DocExtractor extractor(1, DocExtractor::Priority::Normal);
        DocExtractor::Result read = extractor.extractFile(pdf, 1 << 20, {});
        QCOMPARE(read.status, extractproto::Status::Ok);
        QCOMPARE(read.text.text, std::string("Contract number\n"));
        QCOMPARE(extractor.extractFile(bad, 1 << 20, {}).status, extractproto::Status::Unsupported);
        QCOMPARE(extractor.extractFile(pdf + L".missing", 1 << 20, {}).status, extractproto::Status::Failed);
        QVERIFY(!extractor.extractFile(pdf + L".missing", 1 << 20, {}).large);
        const DocExtractor::Result tooLarge = extractor.extractFile(pdf, 10, {});
        QCOMPARE(tooLarge.status, extractproto::Status::Failed); // too large
        QVERIFY(tooLarge.large && !read.large);

        // What the content indexer keeps of a document: its grams and its text.
        const ContentIndexer::FileText text = ContentIndexer::readFile(pdf, 0, {}, &extractor);
        QCOMPARE(text.outcome, ContentIndexer::Outcome::Indexed);
        QVERIFY(hasAll(text.keys, u"contract number"_s));
        QVERIFY(!text.text.isEmpty());
        QVERIFY(text.document && text.document->text == "Contract number\n"); // for a search to look through
        const ContentIndexer::FileText overLimit = ContentIndexer::readFile(pdf, 10, {}, &extractor);
        QCOMPARE(overLimit.outcome, ContentIndexer::Outcome::Empty);
        QVERIFY(overLimit.large && !text.large);
        QCOMPARE(ContentIndexer::readFile(pdf, 0, {}, nullptr).outcome, ContentIndexer::Outcome::Skipped);
        QCOMPARE(ContentIndexer::readFile(bad, 0, {}, &extractor).outcome, ContentIndexer::Outcome::Empty);
        extractor.closeIdle();
    }

    // Copies of a document (the same bytes) are not read again: they get
    // the original's document.
    void contentIndexerReadsCopiesOnce()
    {
        if (!QFile::exists(QString::fromStdWString(DocExtractor::programPath())))
            QSKIP("WinShunExtract.exe is not built with this preset");
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto write = [&](const QString& name, const QByteArray& data) {
            QFile f(tmp.filePath(name));
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        const QByteArray contract = makePdf({{"Contract", "number"}});
        const QByteArray contrast = makePdf({{"Contrast", "number"}});
        QCOMPARE(contract.size(), contrast.size()); // told apart by their bytes
        QVERIFY(write(u"a.pdf"_s, contract) && write(u"b.pdf"_s, contrast) && write(u"c.pdf"_s, contract)
            && write(u"d.pdf"_s, contract));
        const std::string rootName = wtf8::fromUtf16(wtf8::view(QDir::toNativeSeparators(tmp.path())));
        auto index = std::make_shared<FileIndex>();
        EntryId a = kNoEntry;
        EntryId b = kNoEntry;
        EntryId c = kNoEntry;
        EntryId d = kNoEntry;
        {
            auto lock = index->writeLock();
            const EntryId root = index->addRoot(rootName);
            a = index->add(root, "a.pdf", 0);
            b = index->add(root, "b.pdf", 0);
            c = index->add(root, "c.pdf", 0);
            d = index->add(root, "d.pdf", 0);
        }
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] { return std::vector<ContentIndexer::Volume> {{rootName, false}}; };
        source.ready = [] { return true; };
        using namespace std::chrono_literals;
        ContentIndexer indexer(content, source, {}, {0ms, 0ms, 1h, 0ms});
        for (int i = 0; i < 1000 && content->stats().documents < 4; ++i)
            std::this_thread::sleep_for(10ms);
        QCOMPARE(content->stats().documents, std::size_t {4});
        QCOMPARE(matchesOf(*content, u"contract"_s), (std::vector<EntryId> {a, c, d}));
        QCOMPARE(matchesOf(*content, u"contrast"_s), std::vector<EntryId> {b});
        QCOMPARE(indexer.copies(), std::size_t {2}); // one of a, c and d was read
        QCOMPARE(content->stats().distinctTexts, std::size_t {2});
        QVERIFY(content->textOf(c) && content->textOf(c) == content->textOf(a));
    }

    void doubleTap()
    {
        constexpr std::uint32_t ctrl = 0xA2;
        {
            DoubleTapDetector d;
            QVERIFY(!d.keyDown(ctrl, 0, 0, 0));
            QVERIFY(!d.keyUp(ctrl, 80, 0, 0));
            QVERIFY(!d.keyDown(ctrl, 200, 0, 0));
            QVERIFY(!d.keyDown(ctrl, 230, 0, 0)); // auto-repeat
            QVERIFY(d.keyUp(ctrl, 260, 0, 0));
        }
        { // Ctrl+C in between
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyUp(ctrl, 50, 0, 0);
            d.keyDown(ctrl, 100, 0, 0);
            d.keyDown('C', 120, 0, 0);
            d.keyUp('C', 140, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 160, 0, 0));
        }
        { // too slow
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyUp(ctrl, 50, 0, 0);
            d.keyDown(ctrl, 900, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 950, 0, 0));
        }
        { // held too long
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyUp(ctrl, 600, 0, 0);
            d.keyDown(ctrl, 700, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 750, 0, 0));
        }
        { // mouse moved: Ctrl+click multi-select, not a double tap
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 100, 100);
            d.keyUp(ctrl, 50, 100, 100);
            d.keyDown(ctrl, 150, 300, 100);
            QVERIFY(!d.keyUp(ctrl, 200, 300, 100));
        }
        { // a click (or the wheel) while Ctrl is down, twice on the same spot: not a double tap
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 100, 100);
            d.mouseUsed();
            d.keyUp(ctrl, 50, 100, 100);
            d.keyDown(ctrl, 150, 100, 100);
            d.mouseUsed();
            QVERIFY(!d.keyUp(ctrl, 200, 100, 100));
        }
        { // a click right after a tap: no double tap with the next one
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyUp(ctrl, 50, 0, 0);
            d.mouseUsed();
            d.keyDown(ctrl, 150, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 200, 0, 0));
        }
        { // the release was lost (Ctrl+Alt+Del): the next double tap still counts
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyDown(ctrl, 500, 0, 0); // auto-repeat
            d.keyDown(ctrl, 530, 0, 0);
            QVERIFY(!d.keyDown(ctrl, 10'000, 0, 0));
            QVERIFY(!d.keyUp(ctrl, 10'080, 0, 0));
            QVERIFY(!d.keyDown(ctrl, 10'200, 0, 0));
            QVERIFY(d.keyUp(ctrl, 10'260, 0, 0));
        }
        { // held a long while, repeating: not a tap when let go
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyUp(ctrl, 50, 0, 0);
            d.keyDown(ctrl, 100, 0, 0);
            for (std::uint32_t t = 600; t <= 5'000; t += 33)
                d.keyDown(ctrl, t, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 5'010, 0, 0));
        }
        { // the gap follows the double-click time
            DoubleTapDetector d;
            d.setMaxGap(700);
            d.keyDown(ctrl, 0, 0, 0);
            d.keyUp(ctrl, 50, 0, 0);
            d.keyDown(ctrl, 650, 0, 0);
            QVERIFY(d.keyUp(ctrl, 700, 0, 0));
        }
        { // timestamps wrap around
            DoubleTapDetector d;
            d.keyDown(ctrl, 0xFFFFFF00u, 0, 0);
            d.keyUp(ctrl, 0xFFFFFF40u, 0, 0);
            d.keyDown(ctrl, 0x00000010u, 0, 0);
            QVERIFY(d.keyUp(ctrl, 0x00000050u, 0, 0));
        }
    }

    void doubleTapKeys()
    {
        constexpr std::uint32_t left = 0xA2, right = 0xA3, alt = 0xA4, shift = 0xA0, win = 0x5B;
        const auto tapTwice = [](DoubleTapDetector& d, std::uint32_t first, std::uint32_t second, std::uint32_t t) {
            d.keyDown(first, t, 0, 0);
            d.keyUp(first, t + 50, 0, 0);
            d.keyDown(second, t + 150, 0, 0);
            return d.keyUp(second, t + 200, 0, 0);
        };
        { // Alt held, its hotkey's key eaten by the tool's hook, Ctrl tapped by
          // the tool after each press (AutoHotkey's menu mask): not a double tap
            DoubleTapDetector d;
            d.keyDown(alt, 0, 0, 0);
            QVERIFY(!tapTwice(d, left, left, 300));
            d.keyUp(alt, 600, 0, 0);
            QVERIFY(tapTwice(d, left, left, 700)); // and after Alt is let go, it works
        }
        { // the same with Shift or Win
            for (const std::uint32_t modifier : {shift, win, 0x10u /*VK_SHIFT*/, 0x12u /*VK_MENU*/}) {
                DoubleTapDetector d;
                d.keyDown(modifier, 0, 0, 0);
                QVERIFY(!tapTwice(d, left, left, 300));
            }
        }
        { // Alt's release was lost (Ctrl+Alt+Del): once it is quiet and up, taps count again
            static bool altDown = true;
            DoubleTapDetector::Config config;
            config.isDown = [](std::uint32_t vk) { return vk == 0x12 && altDown; };
            DoubleTapDetector d(config);
            d.keyDown(alt, 0, 0, 0);
            QVERIFY(!tapTwice(d, left, left, 5'000)); // still held, as the system says
            altDown = false;
            QVERIFY(tapTwice(d, left, left, 6'000));
            altDown = true;
        }
        { // the other Ctrl starts over: right after left is not a double tap
            DoubleTapDetector d;
            QVERIFY(!tapTwice(d, left, right, 0));
            d.keyDown(right, 300, 0, 0);
            QVERIFY(d.keyUp(right, 340, 0, 0)); // with the right one just tapped, it is
        }
        { // both Ctrl keys at once
            DoubleTapDetector d;
            d.keyDown(left, 0, 0, 0);
            d.keyDown(right, 20, 0, 0);
            d.keyUp(right, 40, 0, 0);
            d.keyUp(left, 50, 0, 0);
            d.keyDown(left, 150, 0, 0);
            QVERIFY(!d.keyUp(left, 200, 0, 0));
        }
        { // left only: the right Ctrl is another key
            DoubleTapDetector::Config config;
            config.side = DoubleTapDetector::Side::Left;
            DoubleTapDetector d(config);
            QVERIFY(!tapTwice(d, right, right, 0));
            QVERIFY(tapTwice(d, left, left, 1'000));
            QVERIFY(tapTwice(d, 0x11 /*VK_CONTROL*/, 0x11, 2'000));
            d.keyDown(left, 3'000, 0, 0);
            d.keyUp(left, 3'050, 0, 0);
            d.keyDown(right, 3'100, 0, 0); // between the two
            d.keyUp(right, 3'120, 0, 0);
            d.keyDown(left, 3'150, 0, 0);
            QVERIFY(!d.keyUp(left, 3'200, 0, 0));
        }
        { // right only
            DoubleTapDetector d;
            d.setSide(DoubleTapDetector::Side::Right);
            QVERIFY(!tapTwice(d, left, left, 0));
            QVERIFY(tapTwice(d, right, right, 1'000));
        }
    }

    void doubleClick()
    {
        constexpr std::uintptr_t view = 0x1234;
        {
            DoubleClickDetector d;
            QVERIFY(!d.leftDown(0, 100, 100, view, false));
            QVERIFY(d.leftDown(300, 101, 99, view, false));
            QVERIFY(!d.firstClosedMenu());
            QVERIFY(!d.leftDown(400, 101, 99, view, false)); // a third press starts over
            QVERIFY(d.leftDown(600, 101, 99, view, false));
        }
        { // too slow
            DoubleClickDetector d;
            d.leftDown(0, 100, 100, view, false);
            QVERIFY(!d.leftDown(501, 100, 100, view, false));
            QVERIFY(d.leftDown(900, 100, 100, view, false)); // that one was a first press
        }
        { // moved out of the box
            DoubleClickDetector d;
            d.leftDown(0, 100, 100, view, false);
            QVERIFY(!d.leftDown(200, 103, 100, view, false));
            DoubleClickDetector e;
            e.leftDown(0, 100, 100, view, false);
            QVERIFY(!e.leftDown(200, 100, 97, view, false));
        }
        { // another window (a press on the menu, then one on File Explorer)
            DoubleClickDetector d;
            d.leftDown(0, 100, 100, view, false);
            QVERIFY(!d.leftDown(200, 100, 100, view + 1, false));
        }
        { // another button or the wheel in between
            DoubleClickDetector d;
            d.leftDown(0, 100, 100, view, false);
            d.otherInput();
            QVERIFY(!d.leftDown(200, 100, 100, view, false));
        }
        { // the first press closed the menu: a double click that means "close"
            DoubleClickDetector d;
            d.leftDown(0, 100, 100, view, true);
            QVERIFY(d.leftDown(200, 100, 100, view, false));
            QVERIFY(d.firstClosedMenu());
            d.leftDown(1000, 100, 100, view, false);
            QVERIFY(d.leftDown(1200, 100, 100, view, false));
            QVERIFY(!d.firstClosedMenu());
        }
        { // the tick count wraps around
            DoubleClickDetector d;
            d.leftDown(0xFFFFFF00u, 100, 100, view, false);
            QVERIFY(d.leftDown(0x00000010u, 100, 100, view, false));
        }
    }

    // Typing in a File Explorer folder window: which keys, where the focus
    // is, start a search with Win顺 (platform/TypeToSearch decides by it).
    void searchTyping()
    {
        using namespace searchtyping;
        const auto starts = [](unsigned vk, bool press = true, bool combination = false, Focus focus = Focus::ItemsView,
                                bool elevated = false) { return explorerStarts(vk, press, combination, focus, elevated); };
        // Letters, digits, punctuation, the number pad: typing.
        QVERIFY(starts('A'));
        QVERIFY(starts('Z'));
        QVERIFY(starts('0'));
        QVERIFY(starts(kVkNumpad0));
        QVERIFY(starts(kVkDivide));
        QVERIFY(starts(kVkOem1)); // ;
        QVERIFY(starts(kVkOem3)); // `
        QVERIFY(starts(kVkOem4)); // [
        QVERIFY(starts(kVkOem8));
        QVERIFY(starts(kVkOem102)); // the key by the left Shift on some layouts
        // Commands of Explorer's view, and keys that do nothing in an empty box.
        QVERIFY(!starts(kVkSpace)); // Listary leaves it to Explorer too
        QVERIFY(!starts(kVkBack));
        QVERIFY(!starts(kVkReturn));
        QVERIFY(!starts(kVkEscape));
        QVERIFY(!starts(kVkTab));
        QVERIFY(!starts(kVkDelete));
        QVERIFY(!starts(kVkLeft));
        QVERIFY(!starts(kVkF1));
        QVERIFY(!starts(kVkF1 + 1)); // F2: rename
        QVERIFY(!starts(kVkF24));
        QVERIFY(!starts(kVkProcessKey)); // never seen by the hook; harmless all the same
        // Modifiers alone, and anything with Ctrl, Alt or Win held.
        QVERIFY(!starts(kVkShift));
        QVERIFY(!starts(kVkLShift));
        QVERIFY(!starts(kVkControl));
        QVERIFY(!starts(kVkMenu));
        QVERIFY(!starts(kVkRMenu));
        QVERIFY(!starts(kVkLWin));
        QVERIFY(!starts(kVkRWin));
        QVERIFY(!starts(kVkCapital));
        QVERIFY(!starts(kVkNumlock));
        QVERIFY(!starts('A', true, true)); // Ctrl+A
        QVERIFY(!starts('1', true, true)); // Alt+1
        // Releases: never (the press was taken, or not).
        QVERIFY(!starts('A', false));
        // Not with the focus elsewhere in the window: a name being renamed
        // (an Edit), the address bar, the search box, the navigation pane.
        QVERIFY(!starts('A', true, false, Focus::Other));
        // Not an elevated Explorer: Windows would neither show the hook its
        // keys nor let them be handed back.
        QVERIFY(!starts('A', true, false, Focus::ItemsView, true));
        QVERIFY(isModifier(kVkShift) && isModifier(kVkRMenu) && isModifier(kVkLWin));
        QVERIFY(!isModifier('A') && !isModifier(kVkSpace));
    }

    // The folder menu's hotkey (GitHub #25): a press opens, closes or
    // forgets, and where a menu opened by a key goes.
    void menuHotkey()
    {
        using menuhotkey::Toggle;
        using Action = Toggle::Action;
        {
            Toggle t;
            int asks = 0;
            QCOMPARE(t.pressed(false, asks), Action::Open);
            t.asked(++asks);
            QVERIFY(t.pending(asks));
            t.answered(asks); // the menu came up
            QVERIFY(!t.pending(asks));
            QCOMPARE(t.pressed(true, asks), Action::Close);
            QCOMPARE(t.pressed(false, asks), Action::Open); // and again
        }
        { // pressed twice before the menu is up: forgotten, the next press opens
            Toggle t;
            int asks = 0;
            QCOMPARE(t.pressed(false, asks), Action::Open);
            t.asked(++asks);
            QCOMPARE(t.pressed(false, asks), Action::Drop);
            ++asks; // the menu moves on, so that the answer is dropped
            QVERIFY(!t.pending(asks));
            QCOMPARE(t.pressed(false, asks), Action::Open);
        }
        { // nothing came of the ask (the window in front changed): the next press opens
            Toggle t;
            int asks = 0;
            t.pressed(false, asks);
            t.asked(++asks);
            t.answered(asks);
            QCOMPARE(t.pressed(false, asks), Action::Open);
        }
        { // a double click asked after us: ours is no longer the latest, and not pending
            Toggle t;
            int asks = 0;
            t.pressed(false, asks);
            t.asked(++asks);
            ++asks;
            QVERIFY(!t.pending(asks));
            QCOMPARE(t.pressed(false, asks), Action::Open);
            t.answered(1); // the old answer, dropped by the menu: changes nothing for the new ask
            QVERIFY(t.pending(asks) == false); // Open was returned, asked() not yet called
            t.asked(++asks);
            QVERIFY(t.pending(asks));
        }
        { // the menu is up (a double click's) while ours is on its way: Close, and ours is forgotten
            Toggle t;
            int asks = 0;
            t.pressed(false, asks);
            t.asked(++asks);
            QCOMPARE(t.pressed(true, asks), Action::Close);
            QVERIFY(!t.pending(asks));
        }
        using menuhotkey::Point;
        const auto same = [](Point a, Point b) { return a.x == b.x && a.y == b.y; };
        QVERIFY(same(menuhotkey::anchor(true, {500, 600}, {10, 20}, 16), Point {500, 600}));
        QVERIFY(same(menuhotkey::anchor(false, {500, 600}, {10, 20}, 16), Point {26, 36}));
        QVERIFY(same(menuhotkey::anchor(false, {500, 600}, {-1920, 0}, 24), Point {-1896, 24})); // a monitor left of the primary
    }

    void gameGuard()
    {
        const GameGuardOptions defaults; // games on, full screen off, no list
        auto in = [](bool covers, bool hidden, bool confined, bool exclusive = false) {
            ForegroundFacts facts;
            facts.program = u"Game.exe"_s;
            facts.coversMonitor = covers;
            facts.cursorHidden = hidden;
            facts.cursorConfined = confined;
            facts.exclusiveFullScreen = exclusive;
            return facts;
        };
        // An ordinary window, a game in its menu, a full-screen page
        QCOMPARE(doubleCtrlIgnoreReason(in(false, false, false), defaults), IgnoreReason::None);
        QCOMPARE(doubleCtrlIgnoreReason(in(true, false, false), defaults), IgnoreReason::None);
        // Turning the view: windowed (confined) or full screen
        QCOMPARE(doubleCtrlIgnoreReason(in(false, true, true), defaults), IgnoreReason::MouseTaken);
        QCOMPARE(doubleCtrlIgnoreReason(in(true, true, false), defaults), IgnoreReason::MouseTaken);
        // Hidden while typing in a window, or confined but shown (a strategy game)
        QCOMPARE(doubleCtrlIgnoreReason(in(false, true, false), defaults), IgnoreReason::None);
        QCOMPARE(doubleCtrlIgnoreReason(in(false, false, true), defaults), IgnoreReason::None);
        QCOMPARE(doubleCtrlIgnoreReason(in(false, false, false, true), defaults), IgnoreReason::ExclusiveFullScreen);

        GameGuardOptions off = defaults;
        off.games = false;
        QCOMPARE(doubleCtrlIgnoreReason(in(true, true, true, true), off), IgnoreReason::None);
        GameGuardOptions fullScreen = defaults;
        fullScreen.fullScreen = true;
        QCOMPARE(doubleCtrlIgnoreReason(in(true, false, false), fullScreen), IgnoreReason::FullScreen);
        GameGuardOptions listed = off;
        listed.programs = {u"game.EXE"_s};
        QCOMPARE(doubleCtrlIgnoreReason(in(false, false, false), listed), IgnoreReason::Listed);

        // Win顺's own windows and the desktop come without a program
        ForegroundFacts ours;
        ours.coversMonitor = ours.cursorHidden = ours.exclusiveFullScreen = true;
        QCOMPARE(doubleCtrlIgnoreReason(ours, fullScreen), IgnoreReason::None);
    }

    void settingsMatch()
    {
        using namespace settingsmatch;
        const auto score = [](const QString& query, const Fields& fields) { return match(parse(query), fields).score; };

        // The query made comparable: full-width forms, punctuation, "Win + V".
        const Query query = parse(u"  剪贴板，图片  Ｗｉｎ + Ｖ  “KeePass.exe”"_s);
        QCOMPARE(query.terms.size(), std::size_t {4});
        QCOMPARE(query.terms[0].text, u"剪贴板"_s);
        QCOMPARE(query.terms[1].text, u"图片"_s);
        QCOMPARE(query.terms[2].text, u"win+v"_s);
        QCOMPARE(query.terms[3].text, u"keepass.exe"_s);
        QVERIFY(parse(u"，、。 "_s).isEmpty());
        QVERIFY(parse(u"a"_s).terms[0].single);
        QVERIFY(!parse(u"图"_s).terms[0].single);
        QVERIFY(parse(u"kjj"_s).terms[0].pinyin.valid());
        QVERIFY(!parse(u"快捷键"_s).terms[0].pinyin.valid());

        Fields hotkey;
        hotkey.title = u"快捷键"_s;
        hotkey.keywords = {u"热键"_s, u"hotkey"_s};
        hotkey.context = {u"打开 Win顺"_s};
        hotkey.description = u"再设一个组合键来打开搜索框，例如 Alt + Space"_s;

        // The title by pinyin and initials, keywords in any case.
        QVERIFY(score(u"kjj"_s, hotkey) > 0);
        QVERIFY(score(u"kuaijiejian"_s, hotkey) > 0);
        QVERIFY(score(u"热键"_s, hotkey) > 0);
        QVERIFY(score(u"HOTKEY"_s, hotkey) > 0);
        QCOMPARE(match(parse(u"kjj"_s), hotkey).title, (QList<Span> {{0, 3}}));
        // The description word for word only.
        QVERIFY(score(u"组合键"_s, hotkey) > 0);
        QVERIFY(score(u"space"_s, hotkey) > 0);
        QCOMPARE(score(u"zhj"_s, hotkey), -1);
        // Every word somewhere.
        QVERIFY(score(u"打开 快捷键"_s, hotkey) > 0);
        QCOMPARE(score(u"快捷键 剪贴板"_s, hotkey), -1);
        // A lone letter only where a word starts, and never in a sentence.
        QVERIFY(score(u"h"_s, hotkey) > 0);
        QVERIFY(score(u"w"_s, hotkey) > 0);
        QCOMPARE(score(u"o"_s, hotkey), -1);
        QCOMPARE(score(u"s"_s, hotkey), -1);
        // Spelled out beats pinyin, which beats initials; a keyword spelled
        // out beats the title's initials.
        QVERIFY(score(u"快捷键"_s, hotkey) > score(u"kuaijiejian"_s, hotkey));
        QVERIFY(score(u"kuaijiejian"_s, hotkey) > score(u"kjj"_s, hotkey));
        QVERIFY(score(u"热键"_s, hotkey) > score(u"kjj"_s, hotkey));

        // The option's own words, then its page, then a description.
        Fields images;
        images.title = u"记录图片"_s;
        images.context = {u"剪贴板"_s};
        images.description = u"截图和复制的图片也记下来"_s;
        Fields dataFolder;
        dataFolder.title = u"数据文件夹"_s;
        dataFolder.context = {u"高级"_s};
        dataFolder.description = u"搜索记录、剪贴板历史和日志都存放在这里"_s;
        Fields clearClips;
        clearClips.title = u"清除剪贴板历史"_s;
        clearClips.context = {u"剪贴板"_s};
        QVERIFY(score(u"剪贴板 图片"_s, images) > 0);
        QCOMPARE(score(u"剪贴板 图片"_s, dataFolder), -1);
        QVERIFY(score(u"剪贴板"_s, clearClips) > score(u"剪贴板"_s, images));
        QVERIFY(score(u"剪贴板"_s, images) > score(u"剪贴板"_s, dataFolder));
        // The page by pinyin from three letters: two fit too many pages.
        Fields pasted;
        pasted.title = u"最多保留"_s;
        pasted.context = {u"剪贴板"_s, u"粘贴"_s};
        QVERIFY(score(u"jtb"_s, pasted) > 0);
        QVERIFY(score(u"zhantie"_s, pasted) > 0);
        QCOMPARE(score(u"zt"_s, pasted), -1);
        Fields theme;
        theme.title = u"主题"_s;
        QVERIFY(score(u"zt"_s, theme) > 0);

        // Where the words are, merged.
        const Match spans = match(parse(u"图片 截图"_s), images);
        QCOMPARE(spans.title, (QList<Span> {{2, 2}}));
        QCOMPARE(spans.description, (QList<Span> {{0, 2}, {6, 2}}));
        QCOMPARE(match(parse(u"图片 图"_s), images).title, (QList<Span> {{2, 2}}));

        // "Win + V" as typed, or its words apart.
        Fields winV;
        winV.title = u"用 Win+V 打开，代替 Windows 自带的剪贴板"_s;
        QVERIFY(score(u"win + v"_s, winV) > 0);
        QVERIFY(score(u"win v"_s, winV) > 0);

        // What the user filled in.
        Fields names;
        names.title = u"跳过的文件夹名称"_s;
        names.values = {u"build"_s, u"node_modules"_s};
        const Match value = match(parse(u"node_mod"_s), names);
        QVERIFY(value.score > 0);
        QCOMPARE(value.values, QList<int> {1});
        QCOMPARE(score(u"node_mod"_s, Fields {}), -1);

        QCOMPARE(highlight(u"a<b>\n图片"_s, {{5, 2}}, u"#0067c0"_s),
            u"a&lt;b&gt;<br><font color=\"#0067c0\">图片</font>"_s);
        QCOMPARE(highlight(u"x"_s, {{3, 1}}, u"red"_s), u"x"_s); // past the end: ignored
    }
};

QTEST_GUILESS_MAIN(CoreTest)
#include "tst_core.moc"
