#include "FileText.h"

#include "ContentIndex.h"
#include "ContentScanner.h"
#include "DocExtractor.h"
#include "Documents.h"
#include "FileIndex.h"
#include "Win32Util.h"

#include <QStringDecoder>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string_view>

using namespace Qt::StringLiterals;

namespace ws::filetext {

namespace {

constexpr std::size_t kMaxTextFileBytes = 16u << 20;
constexpr std::size_t kBinaryProbe = 64u << 10; // a NUL in here: not text

// A document's text with a line where each page, slide or sheet begins.
QString withPlaces(const doctext::DocText& doc)
{
    const std::string_view all(doc.text);
    std::string out;
    out.reserve(all.size() + doc.places.size() * 12);
    std::size_t place = 0;
    std::uint16_t sheet = doctext::Place::kNoSheet;
    std::uint32_t line = 1;
    for (std::size_t at = 0; at < all.size(); ++line) {
        for (; place < doc.places.size() && doc.places[place].line <= line; ++place) {
            const doctext::Place& p = doc.places[place];
            if (p.line != line)
                continue;
            switch (p.kind) {
            case doctext::PlaceKind::Page:
                out += "[page " + std::to_string(p.number) + "]\n";
                break;
            case doctext::PlaceKind::Slide:
                out += "[slide " + std::to_string(p.number) + "]\n";
                break;
            case doctext::PlaceKind::Row:
                if (p.sheet != sheet && p.sheet < doc.sheets.size()) {
                    sheet = p.sheet;
                    out += "[sheet " + doc.sheets[sheet] + "]\n";
                }
                break;
            case doctext::PlaceKind::None:
                break;
            }
        }
        const std::size_t end = all.find('\n', at);
        const std::size_t next = end == std::string_view::npos ? all.size() : end + 1;
        out.append(all.substr(at, next - at));
        at = next;
    }
    return QString::fromUtf8(out);
}

QString decode(std::string_view bytes, TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Utf8:
        return QString::fromUtf8(bytes);
    case TextEncoding::Utf16LE:
        return QStringDecoder(QStringConverter::Utf16LE)(QByteArrayView(bytes));
    case TextEncoding::Utf16BE:
        return QStringDecoder(QStringConverter::Utf16BE)(QByteArrayView(bytes));
    case TextEncoding::Ansi: {
        const UINT codePage = ContentScanner::legacyCodePage();
        const int n = ::MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (n <= 0)
            return {};
        QString out(n, Qt::Uninitialized);
        ::MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), reinterpret_cast<wchar_t*>(out.data()), n);
        return out;
    }
    }
    return {};
}

Result readTextFile(const std::wstring& path, std::uint64_t size)
{
    Result result;
    win32::UniqueHandle file(::CreateFileW(win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid())
        return result;
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size, kMaxTextFileBytes));
    std::string bytes(want, '\0');
    std::size_t got = 0;
    while (got < want) {
        DWORD n = 0;
        const DWORD ask = static_cast<DWORD>(std::min<std::size_t>(want - got, 1u << 20));
        if (!::ReadFile(file.get(), bytes.data() + got, ask, &n, nullptr))
            return result;
        if (n == 0)
            break;
        got += n;
    }
    bytes.resize(got);
    std::size_t bom = 0;
    const TextEncoding encoding = ContentScanner::detect(std::string_view(bytes).substr(0, ContentScanner::kChunkBytes), &bom);
    if (encoding != TextEncoding::Utf16LE && encoding != TextEncoding::Utf16BE
        && std::memchr(bytes.data(), 0, std::min(bytes.size(), kBinaryProbe))) {
        result.status = Status::NotText;
        return result;
    }
    result.text = decode(std::string_view(bytes).substr(bom), encoding);
    result.text.replace(u"\r\n"_s, u"\n"_s);
    result.cut = size > got;
    result.status = Status::Ok;
    return result;
}

} // namespace

Result read(const std::wstring& path, const Sources& sources, std::int64_t maxBytes, const std::function<bool()>& cancelled)
{
    Result result;
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!::GetFileAttributesExW(win32::longPath(path).c_str(), GetFileExInfoStandard, &data)) {
        const DWORD error = ::GetLastError();
        result.status = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME
            ? Status::Missing
            : Status::Unreadable;
        return result;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        result.status = Status::Folder;
        return result;
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    result.document = isDocumentPath(path);

    if (result.document && sources.index && sources.content) {
        std::optional<doctext::DocText> kept;
        {
            const auto lock = sources.index->readLock();
            if (const EntryId id = sources.index->findPath(path); id != kNoEntry)
                kept = sources.content->textOf(id);
        }
        if (kept) {
            result.status = Status::Ok;
            result.fromIndex = true;
            result.cut = kept->truncated;
            result.text = withPlaces(*kept);
            return result;
        }
    }

    if (data.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) {
        result.status = Status::CloudOnly;
        return result;
    }
    if (!result.document)
        return readTextFile(path, size);

    if (static_cast<std::int64_t>(size) > maxBytes) {
        result.status = Status::TooLarge;
        return result;
    }
    if (!sources.extractor)
        return result;
    DocExtractor::Result read = sources.extractor->extractFile(path, maxBytes, cancelled);
    if (read.status != extractproto::Status::Ok) {
        result.status = read.large ? Status::TooLarge : Status::Unreadable;
        return result;
    }
    result.status = Status::Ok;
    result.cut = read.text.truncated;
    result.text = withPlaces(read.text);
    return result;
}

} // namespace ws::filetext
