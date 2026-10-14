#include "Store.h"

#include "winshun.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace semantic {

namespace {

constexpr char kMagic[8] = {'W', 'S', 'S', 'E', 'M', '1', 0, 0};
constexpr std::size_t kHeader = 16; // magic, dims, records
constexpr std::uint8_t kFileRecord = 1;
constexpr std::uint8_t kRemovedRecord = 2;
constexpr std::size_t kMostSegments = 24;
constexpr std::size_t kTopPassages = 400;

std::string keyOf(const std::string& path)
{
    std::string key = path;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

template <typename T> void put(std::string& out, T value)
{
    out.append(reinterpret_cast<const char*>(&value), sizeof value);
}

void putText(std::string& out, const std::string& s, std::size_t most, bool wide)
{
    std::size_t n = std::min(s.size(), most);
    while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
        --n; // not half a character
    if (wide)
        put(out, static_cast<std::uint16_t>(n));
    else
        put(out, static_cast<std::uint8_t>(n));
    out.append(s.data(), n);
}

// Reads a segment's bytes, checking every step against its end.
struct Reader {
    const std::uint8_t* p;
    const std::uint8_t* end;
    bool ok = true;

    template <typename T> T get()
    {
        T value {};
        if (static_cast<std::size_t>(end - p) < sizeof value) {
            ok = false;
            return value;
        }
        std::memcpy(&value, p, sizeof value);
        p += sizeof value;
        return value;
    }
    std::string_view text(std::size_t n)
    {
        if (static_cast<std::size_t>(end - p) < n) {
            ok = false;
            return {};
        }
        const std::string_view s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    const std::uint8_t* skip(std::size_t n)
    {
        if (static_cast<std::size_t>(end - p) < n) {
            ok = false;
            return nullptr;
        }
        const std::uint8_t* at = p;
        p += n;
        return at;
    }
};

std::wstring segmentName(std::uint32_t number, const wchar_t* extension)
{
    wchar_t name[32];
    swprintf_s(name, L"seg-%08u.%s", number, extension);
    return name;
}

std::int32_t dot(const std::int8_t* a, const std::int8_t* b, std::uint32_t n)
{
    std::int32_t sum = 0;
    for (std::uint32_t i = 0; i < n; ++i)
        sum += static_cast<std::int32_t>(a[i]) * static_cast<std::int32_t>(b[i]);
    return sum;
}

// int8 with one scale: value ≈ q * scale.
float quantize(const std::vector<float>& v, std::int8_t* out)
{
    float most = 0;
    for (const float x : v)
        most = std::max(most, std::fabs(x));
    const float scale = most > 0 ? most / 127.0f : 1.0f;
    for (std::size_t i = 0; i < v.size(); ++i)
        out[i] = static_cast<std::int8_t>(std::lround(std::clamp(v[i] / scale, -127.0f, 127.0f)));
    return scale;
}

} // namespace

struct Store::Segment {
    std::wstring path;
    std::uint32_t number = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const std::uint8_t* base = nullptr;
    std::size_t size = 0;

    ~Segment()
    {
        if (base)
            ::UnmapViewOfFile(base);
        if (mapping)
            ::CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE)
            ::CloseHandle(file);
    }
};

Store::Store(std::wstring folder)
    : m_folder(std::move(folder))
{
}

Store::~Store()
{
    unmapAll();
}

void Store::unmapAll()
{
    for (Segment* s : m_segments)
        delete s;
    m_segments.clear();
    m_files.clear();
    m_passages.clear();
    m_byPath.clear();
    m_deadPassages = 0;
}

bool Store::open(const std::string& signature)
{
    const std::unique_lock lock(m_mutex);
    ::CreateDirectoryW(m_folder.c_str(), nullptr);
    if (::GetFileAttributesW(m_folder.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    m_signature = signature;
    const std::wstring model = m_folder + L"\\model.txt";
    std::string was;
    {
        std::ifstream in(model, std::ios::binary);
        was.assign(std::istreambuf_iterator<char>(in), {});
    }
    unmapAll();
    m_pending.clear();
    m_pendingDims = 0;
    if (was != signature) {
        // Vectors of another model (or of another way of asking it) do not
        // compare with this one's.
        WIN32_FIND_DATAW found;
        const HANDLE find = ::FindFirstFileW((m_folder + L"\\seg-*").c_str(), &found);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                ::DeleteFileW((m_folder + L"\\" + found.cFileName).c_str());
            } while (::FindNextFileW(find, &found));
            ::FindClose(find);
        }
        std::ofstream out(model, std::ios::binary | std::ios::trunc);
        out << signature;
        m_dims = 0;
        return static_cast<bool>(out);
    }
    return load();
}

void Store::wipe()
{
    const std::string signature = m_signature;
    {
        const std::unique_lock lock(m_mutex);
        ::DeleteFileW((m_folder + L"\\model.txt").c_str());
    }
    open(signature);
}

bool Store::load()
{
    unmapAll();
    m_dims = 0;
    std::vector<std::pair<std::uint32_t, std::wstring>> found;
    WIN32_FIND_DATAW data;
    const HANDLE find = ::FindFirstFileW((m_folder + L"\\seg-*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name.ends_with(L".tmp")) { // a write that did not finish
                ::DeleteFileW((m_folder + L"\\" + name).c_str());
                continue;
            }
            if (name.size() == 16 && name.ends_with(L".bin"))
                found.emplace_back(static_cast<std::uint32_t>(std::wcstoul(name.c_str() + 4, nullptr, 10)), m_folder + L"\\" + name);
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
    }
    std::sort(found.begin(), found.end());
    for (const auto& [number, path] : found) {
        if (!mapSegment(path))
            winshun::debug("semantic: a damaged segment was skipped: " + winshun::utf8(path));
    }
    return true;
}

bool Store::mapSegment(const std::wstring& path)
{
    auto segment = std::make_unique<Segment>();
    segment->path = path;
    segment->number = static_cast<std::uint32_t>(std::wcstoul(path.c_str() + path.size() - 12, nullptr, 10));
    segment->file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (segment->file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size {};
    if (!::GetFileSizeEx(segment->file, &size) || size.QuadPart < static_cast<LONGLONG>(kHeader))
        return false;
    segment->size = static_cast<std::size_t>(size.QuadPart);
    segment->mapping = ::CreateFileMappingW(segment->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!segment->mapping)
        return false;
    segment->base = static_cast<const std::uint8_t*>(::MapViewOfFile(segment->mapping, FILE_MAP_READ, 0, 0, 0));
    if (!segment->base)
        return false;

    Reader r {segment->base, segment->base + segment->size};
    if (std::memcmp(r.skip(sizeof kMagic), kMagic, sizeof kMagic) != 0)
        return false;
    const auto dims = r.get<std::uint32_t>();
    const auto records = r.get<std::uint32_t>();
    // 0: no vectors in it (removals, files without text, before the first vector).
    if (!r.ok || dims > 8192 || (dims != 0 && m_dims != 0 && dims != m_dims))
        return false;
    if (dims != 0)
        m_dims = dims;
    const auto index = static_cast<std::uint32_t>(m_segments.size());
    for (std::uint32_t i = 0; i < records && r.ok; ++i) {
        const std::uint8_t* start = r.p;
        const auto type = r.get<std::uint8_t>();
        const std::string path8(r.text(r.get<std::uint16_t>()));
        const auto stamp = r.get<std::uint32_t>();
        if (!r.ok)
            break;
        const std::string key = keyOf(path8);
        kill(key);
        if (type == kRemovedRecord)
            continue;
        if (type != kFileRecord)
            return false;
        File file;
        file.path = path8;
        file.stamp = stamp;
        file.segment = index;
        file.offset = static_cast<std::size_t>(start - segment->base);
        file.firstPassage = static_cast<std::uint32_t>(m_passages.size());
        const auto count = r.get<std::uint16_t>();
        if (count > 0 && dims == 0)
            return false;
        const auto fileIndex = static_cast<std::uint32_t>(m_files.size());
        for (std::uint16_t k = 0; k < count && r.ok; ++k) {
            Passage p {};
            p.where = r.text(r.get<std::uint8_t>());
            p.snippet = r.text(r.get<std::uint16_t>());
            p.scale = r.get<float>();
            p.vector = reinterpret_cast<const std::int8_t*>(r.skip(dims));
            p.file = fileIndex;
            if (r.ok)
                m_passages.push_back(p);
        }
        if (!r.ok) {
            m_passages.resize(file.firstPassage);
            break;
        }
        file.passages = count;
        file.size = static_cast<std::size_t>(r.p - start);
        m_files.push_back(std::move(file));
        m_byPath[key] = fileIndex;
    }
    m_segments.push_back(segment.release());
    return r.ok;
}

void Store::kill(const std::string& key)
{
    const auto it = m_byPath.find(key);
    if (it == m_byPath.end())
        return;
    File& f = m_files[it->second];
    f.live = false;
    m_deadPassages += f.passages;
    m_byPath.erase(it);
}

std::unordered_map<std::string, std::uint32_t> Store::stamps() const
{
    const std::shared_lock lock(m_mutex);
    std::unordered_map<std::string, std::uint32_t> out;
    out.reserve(m_byPath.size());
    for (const auto& [key, index] : m_byPath)
        out.emplace(m_files[index].path, m_files[index].stamp);
    return out;
}

std::size_t Store::files() const
{
    const std::shared_lock lock(m_mutex);
    return m_byPath.size();
}

std::size_t Store::passages() const
{
    const std::shared_lock lock(m_mutex);
    return m_passages.size() - m_deadPassages;
}

std::size_t Store::pending() const
{
    const std::shared_lock lock(m_mutex);
    return m_pending.size();
}

void Store::add(NewFile file)
{
    const std::unique_lock lock(m_mutex);
    std::uint32_t dims = m_dims != 0 ? m_dims : m_pendingDims;
    for (const NewPassage& p : file.passages) {
        if (dims == 0)
            dims = static_cast<std::uint32_t>(p.vector.size());
        if (p.vector.size() != dims)
            return; // the model changed under us; the next pass starts over
    }
    m_pendingDims = dims;
    std::string record;
    put(record, kFileRecord);
    putText(record, file.path, 0xFFFF, true);
    put(record, file.stamp);
    put(record, static_cast<std::uint16_t>(file.passages.size()));
    std::vector<std::int8_t> q(dims);
    for (const NewPassage& p : file.passages) {
        putText(record, p.where, 0xFF, false);
        putText(record, p.snippet, 600, true);
        put(record, quantize(p.vector, q.data()));
        record.append(reinterpret_cast<const char*>(q.data()), q.size());
    }
    m_pending.push_back(std::move(record));
}

void Store::remove(const std::string& path)
{
    const std::unique_lock lock(m_mutex);
    std::string record;
    put(record, kRemovedRecord);
    putText(record, path, 0xFFFF, true);
    put(record, std::uint32_t {0});
    m_pending.push_back(std::move(record));
}

std::uint32_t Store::nextNumber() const
{
    std::uint32_t n = 0;
    for (const Segment* s : m_segments)
        n = std::max(n, s->number);
    return n + 1;
}

bool Store::writeSegment(const std::vector<std::string>& records, std::uint32_t& number)
{
    const std::wstring tmp = m_folder + L"\\" + segmentName(number, L"tmp");
    const std::wstring bin = m_folder + L"\\" + segmentName(number, L"bin");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        std::string header(kMagic, sizeof kMagic);
        put(header, m_dims != 0 ? m_dims : m_pendingDims);
        put(header, static_cast<std::uint32_t>(records.size()));
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        for (const std::string& r : records)
            out.write(r.data(), static_cast<std::streamsize>(r.size()));
        if (!out.flush())
            return false;
    }
    if (!::MoveFileExW(tmp.c_str(), bin.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool Store::flush()
{
    std::vector<std::string> records;
    std::uint32_t number = 0;
    {
        const std::unique_lock lock(m_mutex);
        if (m_pending.empty())
            return true;
        records.swap(m_pending);
        number = nextNumber();
    }
    // Written without the lock: searches go on meanwhile.
    if (!writeSegment(records, number)) {
        const std::unique_lock lock(m_mutex);
        m_pending.insert(m_pending.begin(), records.begin(), records.end());
        return false;
    }
    const std::unique_lock lock(m_mutex);
    return mapSegment(m_folder + L"\\" + segmentName(number, L"bin"));
}

bool Store::compactIfWorth()
{
    std::vector<std::string> records;
    std::uint32_t number = 0;
    std::vector<std::wstring> old;
    {
        const std::shared_lock lock(m_mutex);
        const std::size_t total = m_passages.size();
        const bool many = m_segments.size() > kMostSegments;
        const bool dead = m_deadPassages > 1000 && m_deadPassages * 3 > total;
        const bool deadFiles = m_files.size() > 2 * m_byPath.size() + 1000;
        if (!many && !dead && !deadFiles)
            return true;
        records.reserve(m_byPath.size());
        for (const File& f : m_files) {
            if (f.live)
                records.emplace_back(reinterpret_cast<const char*>(m_segments[f.segment]->base + f.offset), f.size);
        }
        for (const Segment* s : m_segments)
            old.push_back(s->path);
        number = nextNumber();
    }
    if (!writeSegment(records, number))
        return false;
    const std::unique_lock lock(m_mutex);
    unmapAll();
    for (const std::wstring& path : old)
        ::DeleteFileW(path.c_str());
    return load();
}

std::vector<Hit> Store::search(const std::vector<float>& query, std::size_t files) const
{
    const std::shared_lock lock(m_mutex);
    if (m_dims == 0 || query.size() != m_dims)
        return {};
    std::vector<std::int8_t> q(m_dims);
    const float qScale = quantize(query, q.data());
    std::vector<std::pair<float, std::uint32_t>> scored;
    scored.reserve(m_passages.size() - m_deadPassages);
    for (std::uint32_t i = 0; i < m_passages.size(); ++i) {
        const Passage& p = m_passages[i];
        if (!m_files[p.file].live)
            continue;
        scored.emplace_back(static_cast<float>(dot(q.data(), p.vector, m_dims)) * p.scale * qScale, i);
    }
    const std::size_t top = std::min(scored.size(), kTopPassages);
    std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(top), scored.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });
    // A file ranks by its best passage; one with more passages among the
    // best goes a little higher (it is about the subject, not in passing).
    std::vector<Hit> hits;
    std::unordered_map<std::uint32_t, std::size_t> at;
    for (std::size_t i = 0; i < top; ++i) {
        const Passage& p = m_passages[scored[i].second];
        const auto [it, added] = at.emplace(p.file, hits.size());
        if (added) {
            Hit h;
            h.path = m_files[p.file].path;
            h.where = std::string(p.where);
            h.snippet = std::string(p.snippet);
            h.score = scored[i].first;
            hits.push_back(std::move(h));
        }
        ++hits[it->second].passages;
    }
    for (Hit& h : hits)
        h.score += 0.01f * static_cast<float>(std::min(h.passages - 1, 3));
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (hits.size() > files)
        hits.resize(files);
    return hits;
}

} // namespace semantic
