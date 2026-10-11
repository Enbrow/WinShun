// Loads the saved index and reports memory use and search timings.
//
//   wsbench [snapshot] [--stats] [--check] [query ...]
//                      --stats: how the entries and names are made up;
//                      --check: compares each query's results with matching
//                      every entry one by one (exit code: the queries that differ)
//   wsbench --mft [--no-compare] [--depth N] [--no-skip] [D: ...]
//                      (as administrator) reads every NTFS volume's MFT (or
//                      those named), compares the result with a directory
//                      walk, and tests following the change journal on the
//                      build folder's volume
//   wsbench --mft-time D: ...
//                      (as administrator) times reading those MFTs with 2 or 4
//                      reads in flight, leaving out free stretches or not
//   wsbench [snapshot] --resave FILE
//                      saves the snapshot again as the app does (packed), loads
//                      it from there, and reports sizes and times
//   wsbench [snapshot] --content [--sample N] [--needle text] [--no-throughput]
//                      what a content search costs: its candidates, and for a
//                      random sample of them the time to open, read, close and
//                      scan each file (first touch, then again from the cache),
//                      reading past the cache, and throughput by thread count.
//                      The sample is the same on every run (fixed seed), so a
//                      later run shows what the caches kept in the meantime
//   wsbench [snapshot] --content-index [--all] [--threads N] [--sample N] [--seed N] [--background]
//                      [--read-only] [--no-add] [--merge-threads N] [--merge-times] [--dir D] [--keep] [--verify]
//                      builds the content index of those files (without system
//                      and program folders unless --all) and reports its size,
//                      how fast it was read (and the processor time that cost
//                      the whole computer), and lookup times; --verify reads
//                      every file a lookup rules out, to show none is missed;
//                      --background reads in background mode; --read-only
//                      stops after reading (--no-add: hands nothing to the
//                      index either); --seed picks another sample;
//                      --merge-threads: for the merge at the end (as --threads);
//                      --merge-times: then merges again on 16, 8, 4 and 1
//                      threads, each timed
//   wsbench [snapshot] --content-search [--all] [--unknown] phrase ...
//                      with the content index the app keeps (from copies of its
//                      files), what a content search for each phrase reads: how
//                      many files and MB may contain it, the largest of them,
//                      and the time to read them as 内容 does (16 threads);
//                      --unknown reads the files the index does not know too
//   wsbench --service [path ...]
//                      (as administrator) builds the index as the app's first
//                      run does, into a temporary folder; prints which volumes
//                      are being read as that changes, then whether each path
//                      given is in the index
//   wsbench --service --documents [query ...]
//                      (as administrator) the same, then the content indexer
//                      reads every document (through WinShunExtract.exe; copies
//                      of one are not read again), then a content search for
//                      each query, with places and snippets
//   wsbench --service --content [--from D]
//                      (as administrator) as a first run: the index built, then
//                      the content indexer reading every file a content search
//                      looks in (the settings in WinShun.ini); how long it took;
//                      --from: starting from a copy of an index folder D (the
//                      first run after an upgrade)
//   wsbench [snapshot] --extract [--threads N] [--memory] [--sandbox [--background]] [--dump] [path ...]
//                      reads documents (Word, Excel, PowerPoint, PDF) as the
//                      extractor does, in this process: the files or folders
//                      given, else every document a content search looks in;
//                      reports what came out by type, the slowest and the
//                      failed files; --dump prints their text with places;
//                      --memory reads each whole file into memory first;
//                      --sandbox reads them through WinShunExtract.exe instead
//                      (--background: at the content indexer's priority)
//   wsbench --share \host\share [--threads N] [--rounds N] [--buffer KB] [--short-names] [--watch S] [--indexer]
//                      reads a network share as the app does: how long the
//                      server takes to answer, its shares, then rounds of
//                      reading it in full (the first builds, the rest verify);
//                      --watch: then prints its live changes for S seconds;
//                      --indexer: the ShareIndexer itself, its states as they
//                      change, a pass per round
//   wsbench --places [query ...]
//                      reads the places (Settings, Control Panel, places.txt)
//                      as the app does and lists them, or the best places for
//                      each query; reports keyword blocks of places.txt that
//                      match no place here
//   wsbench --mcp-host [--pipe NAME] [--index FOLDER] [--plugins DIR]
//                      serves Win顺's MCP tools (src/mcp) on a named pipe as
//                      the app does, over a copy of the index files (made in
//                      the temporary folder unless --index names a folder
//                      with index.bin), for testing with real AI clients
//                      (WinShunMcp.exe --pipe NAME) while the app runs; the
//                      pipe's name defaults to the app's with ".bench" added.
//                      With --plugins, the plugins of DIR (every one enabled,
//                      actions allowed) are tools too, as the AI 助手 page's
//                      switches would make them (plugin::AiBridge)
//   wsbench --plugin FOLDER --query "TEXT" [--runs N] [--setting KEY=VALUE ...] [--action ID]
//                      a plugin (src/plugin) as the app runs it: checks its
//                      plugin.json, starts it through WinShunPluginHost.exe
//                      (next to wsbench), prints the items of the first
//                      query as JSON and the timings: the cold start (runner
//                      and plugin, to the first answer), the first round trip
//                      after it, and the median and p95 of N more (default 20;
//                      a once plugin starts a process for each). --setting
//                      gives the plugin a declared setting's value (what the
//                      settings page would); --action runs that action on
//                      the first item of the first answer and prints the
//                      effects
//   wsbench --launcher-sim [--plugins DIR] [--index FOLDER] [--key-ms N] ["TEXT" ...]
//                      the launcher's list without the launcher: each TEXT
//                      typed key by key (N ms apart, default 80) over a copy
//                      of the index, with the plugins of DIR (default
//                      tests\plugins next to the source; plugin_fake.exe is
//                      copied in where a manifest wants it) answering as in
//                      the app and merged as the app merges them
//                      (plugin::Merge); prints the rows, where each came
//                      from, and the time from the last key to the plugin's
//                      rows. A text no plugin answers is run again without
//                      plugins and the two lists compared
//
// Default snapshot: %LOCALAPPDATA%\WinShun\index.bin
#include "AiBridge.h"
#include "AppCatalog.h"
#include "ContentIndex.h"
#include "ContentIndexer.h"
#include "ContentScanner.h"
#include "Crawler.h"
#include "DocExtractor.h"
#include "Documents.h"
#include "Extract.h"
#include "Host.h"
#include "IndexService.h"
#include "Items.h"
#include "Manifest.h"
#include "McpServer.h"
#include "McpTools.h"
#include "Merge.h"
#include "Pipe.h"
#include "NameSearch.h"
#include "Ntfs.h"
#include "NtfsIndexer.h"
#include "PathText.h"
#include "Query.h"
#include "SearchEngine.h"
#include "Settings.h"
#include "ShareIndexer.h"
#include "ShareLister.h"
#include "SiteIconImage.h"
#include "SiteIcons.h"
#include "Snapshot.h"
#include "SystemCatalog.h"
#include "TextUtil.h"
#include "Unc.h"
#include "WebShortcut.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>

using namespace Qt::StringLiterals;

namespace {

double privateMB()
{
    PROCESS_MEMORY_COUNTERS_EX pmc {};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
}

double peakCommitMB()
{
    PROCESS_MEMORY_COUNTERS_EX pmc {};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    return static_cast<double>(pmc.PeakPagefileUsage) / (1024.0 * 1024.0);
}

double msSince(const QElapsedTimer& t)
{
    return static_cast<double>(t.nsecsElapsed()) / 1e6;
}

// Processor seconds used so far: by the whole computer (all processes, the
// antivirus scanning the files we open too), and by this process.
struct CpuTimes {
    double system = 0;
    double process = 0;
};

CpuTimes cpuTimes()
{
    const auto seconds = [](const FILETIME& t) {
        return static_cast<double>((std::uint64_t {t.dwHighDateTime} << 32) | t.dwLowDateTime) / 1e7;
    };
    CpuTimes out;
    FILETIME idle {}, kernel {}, user {}, created {}, exited {};
    if (::GetSystemTimes(&idle, &kernel, &user))
        out.system = seconds(kernel) + seconds(user) - seconds(idle); // kernel time includes idle time
    if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
        out.process = seconds(kernel) + seconds(user);
    return out;
}

std::string narrow(std::u16string_view s)
{
    return ws::wtf8::fromUtf16(s);
}

int crawlThreads()
{
    return std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 2, 8);
}

// Builds one volume's index from its MFT, as the app does.
bool buildFromMft(ws::FileIndex& index, const ws::Crawler& crawler, const std::wstring& root, double* readMs, double* syncMs)
{
    const ws::EntryId r = index.addRoot(narrow(ws::wtf8::view(root)));
    index.setFolderRecord(r, ws::ntfs::kRootRecord, r);
    index.setInterning(true);
    QElapsedTimer t;
    t.start();
    ws::MftTree tree;
    std::wstring error;
    if (!tree.read(root, index, {}, &error)) {
        std::printf("  MFT read failed: %s\n", narrow(ws::wtf8::view(error)).c_str());
        return false;
    }
    *readMs = msSince(t);
    t.restart();
    crawler.sync(index, {{r, root, 0, r, ws::ntfs::kRootRecord}}, 1, false, {},
        [&](const ws::Crawler::Root& dir, ws::DirListing& out) { return tree.list(index, crawler, dir, out); });
    index.setInterning(false);
    index.compact(true);
    *syncMs = msSince(t);
    return true;
}

// Every live entry by path, with the flags that matter for search.
std::unordered_map<std::u16string, std::uint8_t> pathsOf(const ws::FileIndex& index)
{
    constexpr std::uint8_t kCompared = ws::EntryFlag::Directory | ws::EntryFlag::Hidden | ws::EntryFlag::LowPriority
        | ws::EntryFlag::Offline;
    std::unordered_map<std::u16string, std::uint8_t> out;
    out.reserve(index.liveCount());
    for (std::size_t i = 0; i < index.slotCount(); ++i) {
        const auto id = static_cast<ws::EntryId>(i);
        if (!index.entry(id).isDeleted())
            out.emplace(index.path16(id), static_cast<std::uint8_t>(index.entry(id).flags & kCompared));
    }
    return out;
}

// The first `depth` components of a path ("C:\Windows\System32").
std::u16string prefixOf(const std::u16string& path, int depth)
{
    std::size_t pos = 0;
    for (int i = 0; i < depth; ++i) {
        pos = path.find(u'\\', pos + 1);
        if (pos == std::u16string::npos)
            return path;
    }
    return path.substr(0, pos);
}

void reportOnly(const char* label, const std::vector<std::u16string>& paths)
{
    std::printf("  only %-5s %zu\n", label, paths.size());
    std::map<std::u16string, std::size_t> groups;
    for (const auto& p : paths)
        ++groups[prefixOf(p, 3)];
    std::vector<std::pair<std::size_t, std::u16string>> top;
    for (const auto& [g, n] : groups)
        top.emplace_back(n, g);
    std::sort(top.rbegin(), top.rend());
    for (std::size_t i = 0; i < top.size() && i < 12; ++i)
        std::printf("      %7zu  %s\\...\n", top[i].first, narrow(top[i].second).c_str());
    for (std::size_t i = 0; i < paths.size() && i < 8; ++i)
        std::printf("      e.g. %s\n", narrow(paths[i]).c_str());
}

// Compares an index built from the MFT with one built by walking the disk.
void compareIndexes(const ws::FileIndex& mft, const ws::FileIndex& walked)
{
    const auto a = pathsOf(mft);
    const auto b = pathsOf(walked);
    std::vector<std::u16string> onlyMft, onlyWalk, flagged;
    std::map<int, std::size_t> flagKinds;
    for (const auto& [path, flags] : a) {
        const auto it = b.find(path);
        if (it == b.end()) {
            onlyMft.push_back(path);
        } else if (it->second != flags) {
            flagged.push_back(path);
            ++flagKinds[(flags ^ it->second)];
        }
    }
    for (const auto& [path, flags] : b) {
        if (!a.contains(path))
            onlyWalk.push_back(path);
    }
    std::sort(onlyMft.begin(), onlyMft.end());
    std::sort(onlyWalk.begin(), onlyWalk.end());
    std::sort(flagged.begin(), flagged.end());
    std::printf("  same %zu of %zu (MFT) / %zu (walk)\n", a.size() - onlyMft.size(), a.size(), b.size());
    reportOnly("MFT", onlyMft);
    reportOnly("walk", onlyWalk);
    std::printf("  flags differ %zu  (xor:count", flagged.size());
    for (const auto& [kind, n] : flagKinds)
        std::printf(" 0x%02X:%zu", kind, n);
    std::printf(")\n");
    for (std::size_t i = 0; i < flagged.size() && i < 8; ++i)
        std::printf("      e.g. %s  mft 0x%02X walk 0x%02X\n", narrow(flagged[i]).c_str(), a.at(flagged[i]),
            b.at(flagged[i]));
}

struct OwnedRecord {
    ws::ntfs::UsnRecord record;
    std::u16string name;
};

// Reads every journal record from `usn` up to now.
std::vector<OwnedRecord> readJournal(const std::wstring& root, const ws::ntfs::JournalInfo& journal, std::int64_t usn)
{
    std::vector<OwnedRecord> out;
    ws::ntfs::JournalReader reader(root, journal.id);
    std::vector<ws::ntfs::UsnRecord> records;
    for (;;) {
        reader.start(usn, false);
        std::int64_t next = 0;
        if (!reader.finish(records, next)) {
            std::printf("  journal read failed (error %lu)\n", ::GetLastError());
            break;
        }
        for (const auto& r : records)
            out.push_back({r, std::u16string(r.name)});
        if (next == usn)
            break;
        usn = next;
    }
    for (auto& o : out)
        o.record.name = o.name;
    return out;
}

void applyAll(ws::UsnApplier& applier, const std::vector<OwnedRecord>& owned)
{
    std::vector<ws::ntfs::UsnRecord> records;
    for (const auto& o : owned)
        records.push_back(o.record);
    applier.apply(records, {});
}

// Paths below `dir` in `index`, relative to it, with flags.
std::map<std::u16string, std::uint8_t> subtree(const ws::FileIndex& index, ws::EntryId dir)
{
    std::map<std::u16string, std::uint8_t> out;
    std::u16string base = index.path16(dir);
    if (!base.ends_with(u'\\'))
        base.push_back(u'\\'); // a root's path already ends in one
    std::vector<ws::EntryId> stack {dir};
    while (!stack.empty()) {
        const ws::EntryId id = stack.back();
        stack.pop_back();
        for (ws::EntryId c = index.entry(id).firstChild; c != ws::kNoEntry; c = index.entry(c).nextSibling) {
            out.emplace(index.path16(c).substr(base.size()),
                static_cast<std::uint8_t>(index.entry(c).flags & (ws::EntryFlag::Directory | ws::EntryFlag::Hidden)));
            stack.push_back(c);
        }
    }
    return out;
}

bool sameTree(const char* label, const std::map<std::u16string, std::uint8_t>& got,
    const std::map<std::u16string, std::uint8_t>& want)
{
    if (got == want) {
        std::printf("  %-28s OK (%zu items)\n", label, want.size());
        return true;
    }
    std::printf("  %-28s DIFFERS\n", label);
    for (const auto& [p, f] : want) {
        const auto it = got.find(p);
        if (it == got.end())
            std::printf("      missing  %s\n", narrow(p).c_str());
        else if (it->second != f)
            std::printf("      flags    %s  got 0x%02X want 0x%02X\n", narrow(p).c_str(), it->second, f);
    }
    for (const auto& [p, f] : got) {
        if (!want.contains(p))
            std::printf("      extra    %s\n", narrow(p).c_str());
    }
    return false;
}

// Note: the folder of the test is still there while this runs.
std::uint32_t recordOfPath(const std::filesystem::path& p)
{
    const HANDLE h = ::CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BY_HANDLE_FILE_INFORMATION info {};
    const bool ok = h != INVALID_HANDLE_VALUE && ::GetFileInformationByHandle(h, &info);
    if (h != INVALID_HANDLE_VALUE)
        ::CloseHandle(h);
    return ok ? ws::ntfs::recordOf((std::uint64_t {info.nFileIndexHigh} << 32) | info.nFileIndexLow) : 0;
}

void touch(const std::filesystem::path& p)
{
    const HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE)
        ::CloseHandle(h);
}

// Makes real changes in a scratch folder, then checks that applying the
// journal (and replaying it) gives the same tree as walking the folder.
void journalSelfTest(const std::wstring& root, const std::filesystem::path& base, ws::CrawlRules rules)
{
    namespace fs = std::filesystem;
    std::printf("\nchange journal self-test in %s\n", narrow(ws::wtf8::view(base.wstring())).c_str());
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / L"keep", ec);
    fs::create_directories(base / L"old", ec);
    fs::create_directories(base / L"ws-excluded" / L"incoming" / L"deep", ec);
    touch(base / L"keep" / L"k.txt");
    touch(base / L"old" / L"o.txt");
    touch(base / L"ws-excluded" / L"incoming" / L"deep" / L"x.txt");
    rules.excludedNames.push_back(L"ws-excluded");
    const ws::Crawler crawler(rules);

    ws::FileIndex index;
    double readMs = 0, syncMs = 0;
    if (!buildFromMft(index, crawler, root, &readMs, &syncMs))
        return;
    const auto journal = ws::ntfs::queryJournal(root);
    if (!journal) {
        std::printf("  no change journal\n");
        return;
    }
    const std::int64_t start = journal->nextUsn;

    // The changes.
    fs::create_directories(base / L"new", ec);
    touch(base / L"new" / L"a.txt");
    touch(base / L"new" / L"b.txt");
    fs::rename(base / L"new" / L"a.txt", base / L"new" / L"A2.txt", ec); // rename
    fs::rename(base / L"new" / L"b.txt", base / L"keep" / L"b.txt", ec); // move
    fs::rename(base / L"old", base / L"Old2", ec); // folder rename, with its file
    fs::rename(base / L"keep" / L"k.txt", base / L"keep" / L"K.TXT", ec); // case only
    ::CreateHardLinkW((base / L"keep" / L"hl.txt").c_str(), (base / L"new" / L"A2.txt").c_str(), nullptr);
    ::CreateHardLinkW((base / L"new" / L"hl2.txt").c_str(), (base / L"new" / L"A2.txt").c_str(), nullptr);
    fs::remove(base / L"new" / L"hl2.txt", ec); // one link of several
    fs::remove(base / L"new" / L"A2.txt", ec); // hl.txt keeps the file
    ::SetFileAttributesW((base / L"keep" / L"b.txt").c_str(), FILE_ATTRIBUTE_HIDDEN);
    fs::rename(base / L"Old2", base / L"ws-excluded" / L"Old2", ec); // moved out of the index
    fs::rename(base / L"ws-excluded" / L"incoming", base / L"incoming", ec); // moved in: walked
    touch(base / L"incoming" / L"deep" / L"y.txt"); // inside a folder only the walk registered
    fs::create_directories(base / L"gone" / L"sub", ec);
    touch(base / L"gone" / L"sub" / L"z.txt");
    fs::remove_all(base / L"gone", ec); // a whole tree

    QElapsedTimer t;
    t.start();
    const auto records = readJournal(root, *journal, start);
    const double readJournalMs = msSince(t);
    t.restart();
    ws::UsnApplier applier(index, crawler, narrow(ws::wtf8::view(root)));
    applyAll(applier, records);
    std::printf("  %zu records read in %.1f ms, applied in %.1f ms\n", records.size(), readJournalMs, msSince(t));

    // What the folder really holds now.
    ws::FileIndex walked;
    const ws::EntryId wroot = walked.addRoot(narrow(ws::wtf8::view(base.wstring())));
    crawler.sync(walked, {{wroot, base.wstring(), 0}}, 1, false, {});
    const auto want = subtree(walked, wroot);
    const ws::EntryId dir = index.findPath(base.wstring());
    if (dir == ws::kNoEntry) {
        std::printf("  test folder missing from the index\n");
    } else {
        sameTree("journal applied", subtree(index, dir), want);
        ws::UsnApplier replay(index, crawler, narrow(ws::wtf8::view(root)));
        applyAll(replay, records); // as after a restart from an older position
        sameTree("replayed again", subtree(index, dir), want);
        // y.txt was only found because the walk registered "deep"; check it directly too.
        const ws::EntryId deep = index.findPath((base / L"incoming" / L"deep").wstring());
        const std::uint32_t deepRecord = recordOfPath(base / L"incoming" / L"deep");
        std::printf("  walked-in folder registered   %s\n",
            deep != ws::kNoEntry && index.folderByRecord(index.roots().front(), deepRecord) == deep ? "yes" : "NO");
    }
    fs::remove_all(base, ec);
}

int runMftCheck(bool compare, const QStringList& only, ws::ntfs::ReadOptions options)
{
    ws::Settings settings;
    settings.load();
    const ws::CrawlRules rules = settings.crawlRules();
    const ws::Crawler crawler(rules);
    const std::vector<ws::VolumeInfo> volumes = ws::listLocalVolumes(false);
    std::vector<std::size_t> all(volumes.size());
    std::iota(all.begin(), all.end(), std::size_t {0});
    std::printf("read together:");
    for (const auto& group : ws::readingGroups(volumes, all)) {
        std::printf("  [");
        for (const std::size_t i : group)
            std::printf(" %s", narrow(ws::wtf8::view(volumes[i].root)).c_str());
        std::printf(" ]");
    }
    std::printf("\n");
    for (const auto& v : volumes) {
        if (!only.isEmpty() && !only.contains(QString::fromStdWString(v.root), Qt::CaseInsensitive))
            continue;
        std::printf("\n%s  %s\n", narrow(ws::wtf8::view(v.root)).c_str(), v.ntfs ? "NTFS" : "not NTFS");
        if (!v.ntfs)
            continue;
        const auto journal = ws::ntfs::queryJournal(v.root);
        if (journal)
            std::printf("  journal id %llx  first %lld  next %lld  (%.1f MB kept)\n",
                static_cast<unsigned long long>(journal->id), static_cast<long long>(journal->firstUsn),
                static_cast<long long>(journal->nextUsn),
                static_cast<double>(journal->nextUsn - journal->firstUsn) / (1024.0 * 1024.0));
        else
            std::printf("  no change journal (error %lu)\n", ::GetLastError());
        {
            ws::ntfs::MftReader reader(v.root, options);
            const ws::ntfs::Geometry& g = reader.geometry();
            std::printf("  %u-byte sectors, %u-byte clusters, %u-byte records\n", g.sectorSize, g.clusterSize,
                g.recordSize);
            if (!reader.valid()) {
                std::printf("  MFT: %s\n", narrow(ws::wtf8::view(reader.error())).c_str());
                continue;
            }
            QElapsedTimer t;
            t.start();
            double parseMs = 0;
            std::size_t inUse = 0;
            std::size_t freeBlocks = 0; // 4 MB blocks without a record in use
            ws::ntfs::FileRecord record;
            QElapsedTimer p;
            std::vector<std::pair<std::uint32_t, std::size_t>> unusedBlocks; // first record, records
            while (reader.next()) {
                p.restart();
                std::size_t used = 0;
                for (std::size_t i = 0; i < reader.blockRecords(); ++i)
                    used += reader.parse(i, record) && record.inUse ? 1 : 0;
                inUse += used;
                if (used == 0) {
                    ++freeBlocks;
                    unusedBlocks.emplace_back(reader.firstRecord(), reader.blockRecords());
                }
                parseMs += msSince(p);
            }
            // What the bitmap says about the blocks without a record in use.
            const auto bitmap = reader.bitmap();
            std::size_t setBits = 0, beyond = 0;
            for (const auto& [first, count] : unusedBlocks) {
                for (std::size_t r = first; r < first + count; ++r) {
                    if (r / 8 >= bitmap.size())
                        ++beyond;
                    else if (std::to_integer<unsigned>(bitmap[r / 8]) & (1u << (r % 8)))
                        ++setBits;
                }
            }
            std::printf("  bitmap %zu bytes (%s); in the unused blocks: %zu records marked in use, %zu past it\n",
                bitmap.size(), narrow(ws::wtf8::view(reader.bitmapNote())).c_str(), setBits, beyond);
            std::printf("  MFT %llu records (%zu in use, %zu of the 4 MB blocks unused), %zu extents from %s,\n"
                        "      read %.0f ms (of which parsing %.0f ms), %.0f of %.0f MB left out as free%s\n",
                static_cast<unsigned long long>(reader.recordCount()), inUse, freeBlocks, reader.extentCount(),
                reader.extentsFromRecordZero() ? "record 0" : "FSCTL_GET_RETRIEVAL_POINTERS", msSince(t), parseMs,
                static_cast<double>(reader.bytesSkipped()) / (1 << 20), static_cast<double>(reader.bytes()) / (1 << 20),
                reader.valid() ? "" : " (failed)");
            if (!reader.valid())
                std::printf("  MFT: %s\n", narrow(ws::wtf8::view(reader.error())).c_str());
        }
        const double before = privateMB();
        ws::FileIndex mft;
        double readMs = 0, syncMs = 0;
        if (!buildFromMft(mft, crawler, v.root, &readMs, &syncMs))
            continue;
        ::HeapCompact(::GetProcessHeap(), 0);
        std::printf("  index from MFT: %zu items, read %.0f ms + build %.0f ms, %.1f MB (peak commit %.0f MB)\n",
            mft.liveCount(), readMs, syncMs, privateMB() - before, peakCommitMB());
        const ws::RecordTable* records = mft.folderRecords(mft.roots().front());
        std::printf("  folder records %zu\n", records ? records->size() : 0);
        if (!compare)
            continue;
        ws::FileIndex walked;
        const ws::EntryId r = walked.addRoot(narrow(ws::wtf8::view(v.root)));
        QElapsedTimer t;
        t.start();
        walked.setInterning(true);
        crawler.sync(walked, {{r, v.root, 0}}, crawlThreads(), false, {});
        walked.setInterning(false);
        std::printf("  index from walk: %zu items in %.0f ms\n", walked.liveCount(), msSince(t));
        compareIndexes(mft, walked);
    }

    // The journal test runs on the volume of the build folder.
    const std::filesystem::path base
        = std::filesystem::path(QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toStdWString())
        / L"usn-selftest";
    journalSelfTest(base.root_name().wstring(), base, rules);
    return 0;
}

// Reads (and parses) the MFT of each volume named with each setting, taking
// turns, `rounds` times: what the reads in flight and leaving out free
// stretches are worth on this machine's disks.
int runMftTiming(const QStringList& roots, int rounds)
{
    struct Setting {
        const char* name;
        ws::ntfs::ReadOptions options;
    };
    const Setting settings[] {{"2 reads, all", {2, false}}, {"2 reads, skip free", {2, true}},
        {"4 reads, all", {4, false}}, {"4 reads, skip free", {4, true}}};
    for (const QString& root : roots) {
        std::printf("\n%s\n", qPrintable(root));
        std::map<std::string, std::vector<double>> times;
        for (int round = 0; round < rounds; ++round) {
            for (const Setting& s : settings) {
                QElapsedTimer t;
                t.start();
                ws::ntfs::MftReader reader(root.toStdWString(), s.options);
                ws::ntfs::FileRecord record;
                std::size_t inUse = 0;
                while (reader.next()) {
                    for (std::size_t i = 0; i < reader.blockRecords(); ++i)
                        inUse += reader.parse(i, record) && record.inUse ? 1 : 0;
                }
                if (!reader.valid()) {
                    std::printf("  %s: %s\n", s.name, narrow(ws::wtf8::view(reader.error())).c_str());
                    return 1;
                }
                times[s.name].push_back(msSince(t));
                if (round == 0)
                    std::printf("  %-20s %zu records in use, %.0f of %.0f MB left out\n", s.name, inUse,
                        static_cast<double>(reader.bytesSkipped()) / (1 << 20),
                        static_cast<double>(reader.bytes()) / (1 << 20));
            }
        }
        for (const Setting& s : settings) {
            std::vector<double>& v = times[s.name];
            std::sort(v.begin(), v.end());
            std::printf("  %-20s median %.0f ms (", s.name, v[v.size() / 2]);
            for (const double ms : v)
                std::printf(" %.0f", ms);
            std::printf(" )\n");
        }
    }
    return 0;
}

// ---- the index service, end to end (--service) ------------------------------

// Builds the index as the app's first run does (into a temporary folder: the
// app's own index is not touched), prints which volumes are being read as
// that changes, and then whether each path given is in the index.
int runService(const QStringList& paths)
{
    QTemporaryDir dir;
    ws::Settings settings;
    settings.load();
    ws::IndexService::Options options;
    options.rules = settings.crawlRules();
    options.content.enabled = false;
    ws::IndexService service(dir.path(), options); // the index files in the temporary folder
    QElapsedTimer t;
    t.start();
    std::vector<std::wstring> shown;
    std::map<std::wstring, std::set<int>> percents; // seen for each volume
    QTimer poll;
    poll.setInterval(20);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        const std::vector<ws::IndexService::Reading> reading = service.readingVolumes();
        std::vector<std::wstring> roots;
        for (const auto& r : reading) {
            roots.push_back(r.root);
            percents[r.root].insert(r.percent);
        }
        if (roots != shown) {
            std::printf("  %7.0f ms  reading:", msSince(t));
            for (const auto& r : reading)
                std::printf(r.percent >= 0 ? " %s %d%%" : " %s", narrow(ws::wtf8::view(r.root)).c_str(), r.percent);
            std::printf("\n");
            shown = roots;
        }
        if (service.state() == ws::IndexService::State::Ready && !service.isRefreshing())
            QCoreApplication::quit();
    });
    service.start();
    poll.start();
    QCoreApplication::exec();
    std::printf("index built in %.0f ms: %zu items\n", msSince(t), service.itemCount());
    for (const auto& [root, seen] : percents)
        std::printf("  %s: %zu different percentages seen, %d to %d\n", narrow(ws::wtf8::view(root)).c_str(),
            seen.size(), *seen.begin(), *seen.rbegin());
    const auto index = service.index();
    for (const QString& path : paths) {
        const auto lock = index->readLock();
        const bool found = index->findPath(path.toStdWString()) != ws::kNoEntry;
        std::printf("  %s  %s\n", found ? "found  " : "MISSING", qPrintable(path));
    }
    service.shutdown();
    return 0;
}

// As the app: the index built into a temporary folder, then the content
// indexer reading every document (and no text files), then content searches
// for each query through SearchEngine, with each result's place and snippet.
int runServiceDocuments(const QStringList& queries)
{
    QTemporaryDir dir;
    ws::Settings settings;
    settings.load();
    ws::IndexService::Options options;
    options.rules = settings.crawlRules();
    options.content.enabled = true;
    options.content.extensions = {};
    options.content.documents = true;
    options.content.sizeLimits = settings.contentSizeLimits();
    ws::IndexService service(dir.path(), options); // the index files in the temporary folder
    QElapsedTimer t;
    t.start();
    double readyMs = 0;
    double lastMs = 0; // when the last document came in
    std::size_t lastDocuments = 0;
    QTimer poll;
    poll.setInterval(500);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (readyMs == 0 && service.state() == ws::IndexService::State::Ready && !service.isRefreshing()) {
            readyMs = msSince(t);
            std::printf("  %7.0f ms  index ready: %zu items; content indexing starts now\n", readyMs,
                service.itemCount());
        }
        const ws::ContentIndex::Stats stats = service.contentIndex()->stats();
        if (stats.documents != lastDocuments) {
            std::printf("  %7.0f ms  %zu documents, %zu with text, %.1f MB of text\n", msSince(t), stats.documents,
                stats.texts, static_cast<double>(stats.textBytes) / 1048576.0);
            lastDocuments = stats.documents;
            lastMs = msSince(t);
        }
        std::fflush(stdout);
        if (readyMs > 0 && stats.documents > 0 && !service.readingContent() && msSince(t) > 40000)
            QCoreApplication::quit();
    });
    service.start();
    poll.start();
    QCoreApplication::exec();
    poll.stop();
    const ws::ContentIndex::Stats stats = service.contentIndex()->stats();
    std::printf("documents read in %.1f s (to the last, at 0.5 s steps): %zu documents (%zu copies not read again), "
                "%zu with text (%zu distinct); text file %.1f MB, grams %.1f MB\n",
        (lastMs - readyMs) / 1000, stats.documents, service.contentIndexer()->copies(), stats.texts,
        stats.distinctTexts, static_cast<double>(stats.textBytes) / 1048576.0,
        static_cast<double>(stats.segmentBytes) / 1048576.0);

    ws::SearchEngine engine(&service);
    for (const QString& query : queries) {
        ws::SearchEngine::Request request;
        request.text = query;
        request.scope = ws::Scope::Content;
        request.contentExtensions = {};
        request.contentDocuments = true;
        request.contentThreads = 16;
        ws::SearchResults results;
        int scanned = 0;
        int total = 0;
        QEventLoop loop;
        QObject::connect(&engine, &ws::SearchEngine::contentResults, &loop,
            [&](quint64, const ws::SearchResults& r) { results += r; });
        QObject::connect(&engine, &ws::SearchEngine::contentProgress, &loop, [&](quint64, int s, int n, bool finished) {
            scanned = s;
            total = n;
            if (finished)
                loop.quit();
        });
        QElapsedTimer searchTime;
        searchTime.start();
        engine.submit(request);
        loop.exec();
        std::printf("\n%s: %lld files with it of %d (%d looked at) in %.0f ms\n", qUtf8Printable(query),
            static_cast<long long>(results.size()), total, scanned, msSince(searchTime));
        for (qsizetype i = 0; i < std::min<qsizetype>(results.size(), 6); ++i) {
            const ws::SearchResult& r = results[i];
            const char* where = r.where == ws::SearchResult::Where::Page ? "page"
                : r.where == ws::SearchResult::Where::Slide          ? "slide"
                : r.where == ws::SearchResult::Where::Row            ? "row"
                : r.where == ws::SearchResult::Where::Document       ? "-"
                                                                     : "line";
            std::printf("  %-5s %s%s%-4d %s\n          %s\n", where, qUtf8Printable(r.sheet), r.sheet.isEmpty() ? "" : "!",
                r.where == ws::SearchResult::Where::Line ? r.line : r.placeNumber, qUtf8Printable(r.name),
                qUtf8Printable(r.snippet.left(80)));
        }
    }
    service.shutdown();
    return 0;
}

// As the app's first run, with the content settings of WinShun.ini: the file
// index built into a temporary folder, then the content indexer reading
// every file a content search looks in (text files and documents). Prints
// how far it got every 10 s, then how long the whole took.
int runServiceContent(const QStringList& args)
{
    QTemporaryDir dir;
    // --from D: an app's index folder (index.bin, content/) to start from, copied.
    if (const qsizetype at = args.indexOf(u"--from"_s); at >= 0 && at + 1 < args.size()) {
        const QDir from(args[at + 1]);
        QDir(dir.path()).mkpath(u"content"_s);
        QFile::copy(from.filePath(u"index.bin"_s), QDir(dir.path()).filePath(u"index.bin"_s));
        for (const QFileInfo& f : QDir(from.filePath(u"content"_s)).entryInfoList(QDir::Files))
            QFile::copy(f.absoluteFilePath(), QDir(dir.path()).filePath(u"content/"_s + f.fileName()));
    }
    ws::Settings settings;
    settings.load();
    ws::IndexService::Options options;
    options.rules = settings.crawlRules();
    options.content.enabled = true;
    options.content.extensions = settings.contentExtensions;
    options.content.documents = settings.contentDocuments;
    options.content.includeLowPriority = settings.contentInLowPriority;
    options.content.sizeLimits = settings.contentSizeLimits();
    ws::IndexService service(dir.path(), options);
    QElapsedTimer t;
    t.start();
    double readyMs = 0;
    double startedMs = 0; // the content indexer began reading
    int idlePolls = 0;
    CpuTimes cpuAtStart;
    std::size_t lastDocuments = 0;
    double lastReport = 0;
    QTimer poll;
    poll.setInterval(500);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (readyMs == 0 && service.state() == ws::IndexService::State::Ready && !service.isRefreshing()) {
            readyMs = msSince(t);
            std::printf("  %6.1f s  file index ready: %zu items\n", readyMs / 1000, service.itemCount());
        }
        const bool reading = service.readingContent();
        if (reading && startedMs == 0) {
            startedMs = msSince(t);
            cpuAtStart = cpuTimes();
            std::printf("  %6.1f s  content indexing started\n", startedMs / 1000);
        }
        const ws::ContentIndex::Stats stats = service.contentIndex()->stats();
        if (msSince(t) - lastReport >= 10000 && startedMs > 0) {
            LASTINPUTINFO input {sizeof input, 0};
            ::GetLastInputInfo(&input);
            std::printf("  %6.1f s  %zu files (%.0f a second), %zu documents' text; last input %lu s ago\n",
                msSince(t) / 1000, stats.documents,
                static_cast<double>(stats.documents - lastDocuments) * 1000.0 / (msSince(t) - lastReport), stats.texts,
                (::GetTickCount() - input.dwTime) / 1000);
            lastDocuments = stats.documents;
            lastReport = msSince(t);
        }
        std::fflush(stdout); // read while it runs (through a pipe, say)
        idlePolls = startedMs > 0 && !reading ? idlePolls + 1 : 0;
        if (idlePolls >= 2)
            QCoreApplication::quit();
    });
    service.start();
    poll.start();
    QCoreApplication::exec();
    poll.stop();
    const double doneMs = msSince(t) - 1000; // two polls ago
    const CpuTimes cpuAtEnd = cpuTimes();
    const ws::ContentIndex::Stats stats = service.contentIndex()->stats();
    const double contentS = (doneMs - startedMs) / 1000;
    std::printf("content indexed %.1f s after start (file index %.1f s, content %.1f s): %zu files (%.0f a second), "
                "%zu documents' text (%zu distinct), %zu copies not read again\n",
        doneMs / 1000, readyMs / 1000, contentS, stats.documents, static_cast<double>(stats.documents) / contentS,
        stats.texts, stats.distinctTexts, service.contentIndexer()->copies());
    std::printf("index %.1f MB in %zu segment(s), text %.1f MB; whole computer %.1f of %u processors busy meanwhile\n",
        static_cast<double>(stats.segmentBytes) / 1048576.0, stats.segments,
        static_cast<double>(stats.textBytes) / 1048576.0, (cpuAtEnd.system - cpuAtStart.system) / contentS,
        std::thread::hardware_concurrency());
    service.shutdown(); // saves the snapshot
    qint64 snapshot = 0;
    for (const QFileInfo& f : QDir(dir.path()).entryInfoList({u"*.bin"_s}, QDir::Files))
        snapshot += f.size();
    std::printf("snapshot %.1f MB\n", static_cast<double>(snapshot) / 1048576.0);
    return 0;
}

// ---- content search costs (--content) ---------------------------------------

using Clock = std::chrono::steady_clock;

double usSince(Clock::time_point t)
{
    return std::chrono::duration<double, std::micro>(Clock::now() - t).count();
}

constexpr std::size_t kReadChunk = 512 * 1024; // as ContentScanner::scanFile reads
constexpr DWORD kSkipAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN
    | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;

// SearchEngine ranks candidates like this: your files first, system / program
// / tool folders (EntryFlag::LowPriority) last; 全部 leaves those out.
const char* const kClassNames[] = {"user profile", "other", "hidden", "low priority"};

struct ContentCandidate {
    ws::EntryId id;
    int cls; // index into kClassNames
};

struct FileCost {
    enum Outcome { OpenFailed, Skipped, Read } outcome = OpenFailed; // Skipped: cloud placeholder, empty, too big
    double openUs = 0;
    double readUs = 0; // with the attribute and size queries before it
    double closeUs = 0;
    double scanUs = 0; // CPU only: the bytes are in memory by then
    std::size_t size = 0;
    ws::TextEncoding encoding = ws::TextEncoding::Utf8;
    bool nonAscii = false;
    bool han = false; // has Chinese characters (for ANSI files: any double-byte character)
};

// Whether the text has any non-ASCII character, and any CJK ideograph (U+4E00..U+9FFF).
void classifyText(const char* d, std::size_t n, ws::TextEncoding encoding, FileCost& cost)
{
    const auto* b = reinterpret_cast<const unsigned char*>(d);
    switch (encoding) {
    case ws::TextEncoding::Utf16LE:
    case ws::TextEncoding::Utf16BE:
        for (std::size_t i = 0; i + 1 < n; i += 2) {
            const unsigned u = encoding == ws::TextEncoding::Utf16LE ? b[i] | (b[i + 1] << 8) : (b[i] << 8) | b[i + 1];
            cost.nonAscii |= u >= 0x80 && u != 0xFEFF;
            if (u >= 0x4E00 && u <= 0x9FFF) {
                cost.han = true;
                return;
            }
        }
        return;
    case ws::TextEncoding::Ansi:
        for (std::size_t i = 0; i + 1 < n; ++i) {
            if (b[i] >= 0x81 && b[i + 1] >= 0x40) {
                cost.nonAscii = cost.han = true;
                return;
            }
        }
        return;
    case ws::TextEncoding::Utf8:
        for (std::size_t i = 0; i + 2 < n; ++i) {
            if (b[i] < 0x80)
                continue;
            if (!(i == 0 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF))
                cost.nonAscii = true;
            if (b[i] >= 0xE4 && b[i] <= 0xE9 && (b[i + 1] & 0xC0) == 0x80 && (b[i + 2] & 0xC0) == 0x80) {
                cost.han = true;
                return;
            }
        }
        return;
    }
}

// Opens and reads one file as ContentScanner::scanFile does, timing each step,
// then scans the bytes from memory. `buffer` is page aligned and holds
// `capacity` bytes (the size limit plus one chunk). Unbuffered reads skip the
// file cache, so they show what the disk itself takes.
FileCost measureFile(const std::wstring& path, const ws::ContentScanner& scanner, char* buffer, std::size_t capacity,
    bool unbuffered)
{
    FileCost cost;
    auto t = Clock::now();
    const HANDLE h = ::CreateFileW(ws::win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        unbuffered ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    cost.openUs = usSince(t);
    if (h == INVALID_HANDLE_VALUE)
        return cost;

    t = Clock::now();
    FILE_BASIC_INFO basic {};
    LARGE_INTEGER size {};
    const bool skip = (::GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic)
                          && (basic.FileAttributes & kSkipAttributes))
        || !::GetFileSizeEx(h, &size) || size.QuadPart <= 0
        || static_cast<std::uint64_t>(size.QuadPart) + kReadChunk > capacity;
    std::size_t total = 0;
    if (!skip) {
        FILE_IO_PRIORITY_HINT_INFO hint {};
        hint.PriorityHint = IoPriorityHintLow;
        ::SetFileInformationByHandle(h, FileIoPriorityHintInfo, &hint, sizeof hint);
        while (total < static_cast<std::size_t>(size.QuadPart)) {
            DWORD got = 0;
            if (!::ReadFile(h, buffer + total, static_cast<DWORD>(kReadChunk), &got, nullptr) || got == 0)
                break;
            total += got;
        }
    }
    cost.readUs = usSince(t);
    t = Clock::now();
    ::CloseHandle(h);
    cost.closeUs = usSince(t);
    if (skip) {
        cost.outcome = FileCost::Skipped;
        return cost;
    }
    cost.outcome = FileCost::Read;
    cost.size = total;
    std::size_t bom = 0;
    cost.encoding = ws::ContentScanner::detect({buffer, std::min(total, kReadChunk)}, &bom);
    classifyText(buffer, total, cost.encoding, cost);
    std::size_t pos = 0;
    const ws::ContentScanner::ReadFn read = [&](char* out, std::size_t room) {
        const std::size_t n = std::min(room, total - pos);
        std::memcpy(out, buffer + pos, n);
        pos += n;
        return n;
    };
    t = Clock::now();
    (void)scanner.scan(read, kReadChunk, {});
    cost.scanUs = usSince(t);
    return cost;
}

// ContentScanner::scanFile, with the low I/O priority hint optional. Returns the bytes read.
std::size_t scanStreaming(const std::wstring& path, const ws::ContentScanner& scanner, std::int64_t maxBytes,
    bool lowPriority)
{
    ws::win32::UniqueHandle file(::CreateFileW(ws::win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file.valid())
        return 0;
    FILE_BASIC_INFO basic {};
    if (::GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof basic)
        && (basic.FileAttributes & kSkipAttributes))
        return 0;
    LARGE_INTEGER size {};
    if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 || size.QuadPart > maxBytes)
        return 0;
    if (lowPriority) {
        FILE_IO_PRIORITY_HINT_INFO hint {};
        hint.PriorityHint = IoPriorityHintLow;
        ::SetFileInformationByHandle(file.get(), FileIoPriorityHintInfo, &hint, sizeof hint);
    }
    std::size_t total = 0;
    const HANDLE h = file.get();
    const ws::ContentScanner::ReadFn read = [&](char* buffer, std::size_t capacity) -> std::size_t {
        DWORD got = 0;
        if (!::ReadFile(h, buffer, static_cast<DWORD>(std::min<std::size_t>(capacity, 1u << 30)), &got, nullptr))
            return 0;
        total += got;
        return got;
    };
    (void)scanner.scan(read, kReadChunk, {});
    return total;
}

void printSpread(const char* label, std::vector<double> values, double scale, const char* unit)
{
    if (values.empty()) {
        std::printf("    %-8s -\n", label);
        return;
    }
    std::sort(values.begin(), values.end());
    const auto at = [&](double q) {
        return values[std::min(values.size() - 1, static_cast<std::size_t>(q * static_cast<double>(values.size())))]
            / scale;
    };
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size()) / scale;
    std::printf("    %-8s mean %9.1f   p50 %9.1f   p90 %9.1f   p99 %9.1f   max %10.1f  %s\n", label, mean, at(0.5),
        at(0.9), at(0.99), values.back() / scale, unit);
}

// Times every file of `paths` one after another; returns the mean cost per file in microseconds.
double reportPass(const char* title, const std::vector<std::pair<std::wstring, int>>& paths,
    const ws::ContentScanner& scanner, char* buffer, std::size_t capacity, bool unbuffered,
    std::array<double, 4>* meanSizeByClass)
{
    std::vector<double> open, read, close, scan, total, sizes;
    std::size_t outcomes[3] = {};
    std::size_t encodings[4] = {};
    std::size_t tiny = 0; // could sit in the MFT record itself
    std::array<double, 4> bytesByClass {};
    std::array<std::size_t, 4> filesByClass {};
    std::array<std::size_t, 4> readByClass {};
    std::array<std::size_t, 4> nonAsciiByClass {};
    std::array<std::size_t, 4> hanByClass {};
    double hanBytes = 0;
    std::vector<double> sizesAbove1MB;
    const auto start = Clock::now();
    for (const auto& [path, cls] : paths) {
        const FileCost c = measureFile(path, scanner, buffer, capacity, unbuffered);
        ++outcomes[c.outcome];
        open.push_back(c.openUs);
        total.push_back(c.openUs + c.readUs + c.closeUs + c.scanUs);
        ++filesByClass[static_cast<std::size_t>(cls)];
        if (c.outcome != FileCost::Read)
            continue;
        read.push_back(c.readUs);
        close.push_back(c.closeUs);
        scan.push_back(c.scanUs);
        sizes.push_back(static_cast<double>(c.size));
        const auto k = static_cast<std::size_t>(cls);
        bytesByClass[k] += static_cast<double>(c.size);
        ++readByClass[k];
        nonAsciiByClass[k] += c.nonAscii ? 1 : 0;
        hanByClass[k] += c.han ? 1 : 0;
        hanBytes += c.han ? static_cast<double>(c.size) : 0.0;
        if (c.size > (1u << 20))
            sizesAbove1MB.push_back(static_cast<double>(c.size));
        ++encodings[static_cast<int>(c.encoding)];
        tiny += c.size <= 600 ? 1 : 0;
    }
    const double seconds = usSince(start) / 1e6;
    std::printf("\n  %s: %zu files in %.1f s, one thread (%zu read, %zu skipped, %zu could not be opened)\n", title,
        paths.size(), seconds, outcomes[FileCost::Read], outcomes[FileCost::Skipped], outcomes[FileCost::OpenFailed]);
    printSpread("open", open, 1.0, "us");
    printSpread("read", read, 1.0, "us");
    printSpread("close", close, 1.0, "us");
    printSpread("scan", scan, 1.0, "us (CPU)");
    printSpread("total", total, 1.0, "us per file");
    printSpread("size", sizes, 1024.0, "KB");
    const double readFiles = std::max<double>(1.0, static_cast<double>(sizes.size()));
    std::printf("    encodings: UTF-8 %zu, UTF-16LE %zu, UTF-16BE %zu, ANSI %zu;  <= 600 bytes: %.1f%%\n", encodings[0],
        encodings[1], encodings[2], encodings[3], 100.0 * static_cast<double>(tiny) / readFiles);
    const double allBytes = std::accumulate(sizes.begin(), sizes.end(), 0.0);
    std::printf("    over 1 MB: %zu files (%.1f%%) holding %.1f%% of the bytes;  files with Chinese hold %.1f%% of the bytes\n",
        sizesAbove1MB.size(), 100.0 * static_cast<double>(sizesAbove1MB.size()) / readFiles,
        100.0 * std::accumulate(sizesAbove1MB.begin(), sizesAbove1MB.end(), 0.0) / std::max(1.0, allBytes),
        100.0 * hanBytes / std::max(1.0, allBytes));
    for (std::size_t k = 0; k < 4; ++k) {
        if (readByClass[k] == 0)
            continue;
        const auto r = static_cast<double>(readByClass[k]);
        std::printf("    %-13s %5zu read: non-ASCII %5.1f%%, Chinese %5.1f%%\n", kClassNames[k], readByClass[k],
            100.0 * static_cast<double>(nonAsciiByClass[k]) / r, 100.0 * static_cast<double>(hanByClass[k]) / r);
    }
    if (meanSizeByClass) {
        for (std::size_t k = 0; k < 4; ++k)
            (*meanSizeByClass)[k] = filesByClass[k] ? bytesByClass[k] / static_cast<double>(filesByClass[k]) : 0.0;
    }
    return std::accumulate(total.begin(), total.end(), 0.0) / std::max<double>(1.0, static_cast<double>(total.size()));
}

int runContentBench(const ws::FileIndex& index, QStringList args)
{
    ws::Settings settings;
    settings.load();
    std::size_t sample = 4000;
    QString needle = u"量子纠缠qzxv"_s; // matches nothing, so every file is read to the end
    if (const qsizetype at = args.indexOf(u"--sample"_s); at >= 0 && at + 1 < args.size())
        sample = std::max(100, args[at + 1].toInt());
    if (const qsizetype at = args.indexOf(u"--needle"_s); at >= 0 && at + 1 < args.size())
        needle = args[at + 1];
    const std::int64_t maxBytes = std::ranges::max(settings.contentSizeLimits().bytes); // the largest kind's

    std::vector<std::string> extensions;
    for (QString ext : settings.contentExtensions) {
        while (ext.startsWith(u'.') || ext.startsWith(u'*'))
            ext.remove(0, 1);
        if (!ext.isEmpty())
            extensions.push_back(ws::text::foldAscii(ws::wtf8::fromUtf16(ws::wtf8::view(ext))));
    }

    // 1. The candidates, collected as SearchEngine::runContentSearch does,
    //    including the buffer it keeps every candidate's full path in.
    const std::wstring profile = ws::win32::expandEnvironment(L"%USERPROFILE%");
    std::vector<ContentCandidate> candidates;
    std::u16string paths;
    std::map<std::string, std::array<std::size_t, 2>> byExtension; // all, in low priority folders
    std::array<std::size_t, 4> byClass {};
    auto t = Clock::now();
    for (std::size_t c = 0; c < index.chunkCount(); ++c) {
        const auto entries = index.chunk(c);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const ws::Entry& e = entries[i];
            if ((e.flags & (ws::EntryFlag::Deleted | ws::EntryFlag::Directory | ws::EntryFlag::Offline))
                || index.extLength(e) == 0)
                continue;
            const std::string_view name = index.name(e);
            const std::string_view ext = name.substr(name.size() - index.extLength(e));
            const auto x = std::find_if(extensions.begin(), extensions.end(),
                [&](const std::string& s) { return ws::text::equalsFolded(ext, s); });
            if (x == extensions.end())
                continue;
            const auto id = static_cast<ws::EntryId>((c << ws::FileIndex::kChunkBits) + i);
            const std::size_t offset = paths.size();
            index.appendPath16(id, paths);
            const std::wstring_view path = ws::wtf8::wview(std::u16string_view(paths).substr(offset));
            int cls = 1;
            if (e.flags & ws::EntryFlag::LowPriority)
                cls = 3;
            else if (e.flags & ws::EntryFlag::Hidden)
                cls = 2;
            else if (path.size() > profile.size() && path[profile.size()] == L'\\'
                && ws::win32::equalsIgnoreCase(path.substr(0, profile.size()), profile))
                cls = 0;
            candidates.push_back({id, cls});
            ++byClass[static_cast<std::size_t>(cls)];
            auto& counts = byExtension[*x];
            ++counts[0];
            counts[1] += cls == 3 ? 1 : 0;
        }
    }
    const double collectMs = usSince(t) / 1000.0;
    const double pathMB = static_cast<double>(paths.capacity() * sizeof(char16_t)) / (1024.0 * 1024.0);
    // The app's Candidate is 12 bytes: offset, length, priority.
    const double listMB = static_cast<double>(candidates.size() * 12) / (1024.0 * 1024.0);
    std::printf("\ncontent candidates  %zu, collected in %.0f ms (%zu extensions, files up to %d MB)\n",
        candidates.size(), collectMs, extensions.size(), static_cast<int>(maxBytes >> 20));
    std::printf("  path buffer %.1f MB allocated (%.1f characters per path) + candidate list %.1f MB\n", pathMB,
        static_cast<double>(paths.size()) / static_cast<double>(std::max<std::size_t>(1, candidates.size())), listMB);
    for (std::size_t k = 0; k < 4; ++k)
        std::printf("  %-13s %9zu  (%4.1f%%)\n", kClassNames[k], byClass[k],
            100.0 * static_cast<double>(byClass[k]) / static_cast<double>(std::max<std::size_t>(1, candidates.size())));
    std::vector<std::pair<std::size_t, std::string>> topExt;
    for (const auto& [ext, counts] : byExtension)
        topExt.emplace_back(counts[0], ext);
    std::sort(topExt.rbegin(), topExt.rend());
    std::printf("  by extension (files, of which in low priority folders):\n");
    for (std::size_t k = 0; k < topExt.size() && k < 16; ++k) {
        const auto& counts = byExtension[topExt[k].second];
        std::printf("    %-6s %9zu  %5.1f%%\n", topExt[k].second.c_str(), counts[0],
            100.0 * static_cast<double>(counts[1]) / static_cast<double>(counts[0]));
    }
    paths = std::u16string();
    if (candidates.empty())
        return 0;

    // 2. Random samples, disjoint, so each one starts as cold as the cache allows.
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), std::size_t {0});
    std::shuffle(order.begin(), order.end(), std::mt19937_64(20261007));
    std::size_t cursor = 0;
    const auto take = [&](std::size_t n) {
        std::vector<std::pair<std::wstring, int>> out;
        for (; n > 0 && cursor < order.size(); --n, ++cursor) {
            const ContentCandidate& c = candidates[order[cursor]];
            out.emplace_back(index.wpath(c.id), c.cls);
        }
        return out;
    };

    const ws::ContentScanner scanner(needle);
    const std::size_t capacity = static_cast<std::size_t>(maxBytes) + kReadChunk;
    auto* buffer = static_cast<char*>(::VirtualAlloc(nullptr, capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buffer)
        return 1;
    std::printf("\nsample of %zu files (needle \"%s\", matches nothing: every file is read to the end)", sample,
        needle.toUtf8().constData());
    const auto first = take(sample);
    std::array<double, 4> meanSize {};
    const double coldUs = reportPass("first touch", first, scanner, buffer, capacity, false, &meanSize);
    const double warmUs = reportPass("same files again (cache warm)", first, scanner, buffer, capacity, false, nullptr);
    reportPass("other files, unbuffered (past the file cache)", take(sample / 2), scanner, buffer, capacity, true,
        nullptr);
    ::VirtualFree(buffer, 0, MEM_RELEASE);

    double totalBytes = 0;
    std::printf("\n  estimated bytes of all candidates (mean size in the sample x count):\n");
    for (std::size_t k = 0; k < 4; ++k) {
        const double bytes = meanSize[k] * static_cast<double>(byClass[k]);
        totalBytes += bytes;
        std::printf("    %-13s %8.2f GB\n", kClassNames[k], bytes / (1024.0 * 1024.0 * 1024.0));
    }
    std::printf("    %-13s %8.2f GB\n", "all", totalBytes / (1024.0 * 1024.0 * 1024.0));
    const double n = static_cast<double>(candidates.size());
    std::printf("  one thread over all candidates: %.0f s first touch, %.0f s warm\n", coldUs * n / 1e6,
        warmUs * n / 1e6);
    if (args.contains(u"--gram-test"_s)) {
        // How big a file-level index of the text would be: per file, its
        // distinct Chinese characters, pairs of adjacent ones, and trigrams
        // (ASCII folded), counted on the first sample (warm by now).
        const auto isHan = [](char16_t c) { return (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF); };
        std::array<std::array<double, 3>, 4> pairsByClass {}; // [class][uni, bi, tri]
        std::array<std::size_t, 4> filesByClass {};
        std::array<double, 3> pairsBig {}; // files over 1 MB
        std::vector<std::uint64_t> uni, bi, tri;
        auto* text = static_cast<char*>(::VirtualAlloc(nullptr, capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        cursor = 0;
        for (const auto& [path, cls] : take(sample)) {
            ws::win32::UniqueHandle file(::CreateFileW(ws::win32::longPath(path).c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
            LARGE_INTEGER size {};
            if (!file.valid() || !::GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0
                || static_cast<std::uint64_t>(size.QuadPart) + kReadChunk > capacity)
                continue;
            std::size_t total = 0;
            DWORD got = 0;
            while (total < static_cast<std::size_t>(size.QuadPart)
                && ::ReadFile(file.get(), text + total, static_cast<DWORD>(kReadChunk), &got, nullptr) && got > 0)
                total += got;
            std::size_t bom = 0;
            const ws::TextEncoding encoding = ws::ContentScanner::detect({text, std::min(total, kReadChunk)}, &bom);
            QString s;
            switch (encoding) {
            case ws::TextEncoding::Utf8:
                s = QString::fromUtf8(text + bom, static_cast<qsizetype>(total - bom));
                break;
            case ws::TextEncoding::Utf16LE:
            case ws::TextEncoding::Utf16BE:
                s.resize(static_cast<qsizetype>((total - bom) / 2));
                for (qsizetype i = 0; i < s.size(); ++i) {
                    const auto* b = reinterpret_cast<const unsigned char*>(text + bom + 2 * i);
                    s[i] = QChar(encoding == ws::TextEncoding::Utf16LE ? b[0] | (b[1] << 8) : (b[0] << 8) | b[1]);
                }
                break;
            case ws::TextEncoding::Ansi: {
                const int wide = ::MultiByteToWideChar(scanner.ansiCodePage(), 0, text, static_cast<int>(total), nullptr, 0);
                s.resize(std::max(wide, 0));
                ::MultiByteToWideChar(scanner.ansiCodePage(), 0, text, static_cast<int>(total),
                    reinterpret_cast<wchar_t*>(s.data()), wide);
                break;
            }
            }
            uni.clear();
            bi.clear();
            tri.clear();
            const char16_t* c = reinterpret_cast<const char16_t*>(s.utf16());
            const auto fold = [](char16_t u) -> std::uint64_t { return u >= 'A' && u <= 'Z' ? u + 32 : u; };
            for (qsizetype i = 0; i < s.size(); ++i) {
                if (isHan(c[i])) {
                    uni.push_back(c[i]);
                    if (i + 1 < s.size() && isHan(c[i + 1]))
                        bi.push_back((std::uint64_t {c[i]} << 16) | c[i + 1]);
                }
                if (i + 2 < s.size())
                    tri.push_back((fold(c[i]) << 32) | (fold(c[i + 1]) << 16) | fold(c[i + 2]));
            }
            const auto distinct = [](std::vector<std::uint64_t>& v) {
                std::sort(v.begin(), v.end());
                return static_cast<double>(std::unique(v.begin(), v.end()) - v.begin());
            };
            const std::array<double, 3> counts {distinct(uni), distinct(bi), distinct(tri)};
            const auto k = static_cast<std::size_t>(cls);
            ++filesByClass[k];
            for (std::size_t g = 0; g < 3; ++g) {
                pairsByClass[k][g] += counts[g];
                if (total > (1u << 20))
                    pairsBig[g] += counts[g];
            }
        }
        ::VirtualFree(text, 0, MEM_RELEASE);
        // Posting lists of file numbers, delta + varint coded: ~1.5 bytes per (gram, file) pair.
        constexpr double kBytesPerPair = 1.5;
        std::printf("\nfile-level index of the text (distinct grams per file; ~%.1f bytes per gram and file):\n",
            kBytesPerPair);
        std::array<double, 3> all {}, mostly {};
        for (std::size_t k = 0; k < 4; ++k) {
            if (filesByClass[k] == 0)
                continue;
            const double f = static_cast<double>(filesByClass[k]);
            std::printf("  %-13s %5zu files: per file %7.0f chars, %7.0f char pairs, %8.0f trigrams\n", kClassNames[k],
                filesByClass[k], pairsByClass[k][0] / f, pairsByClass[k][1] / f, pairsByClass[k][2] / f);
            for (std::size_t g = 0; g < 3; ++g) {
                const double estimate = pairsByClass[k][g] / f * static_cast<double>(byClass[k]) * kBytesPerPair;
                all[g] += estimate;
                if (k != 3)
                    mostly[g] += estimate;
            }
        }
        const double gb = 1024.0 * 1024.0 * 1024.0;
        std::printf("  estimated index, without low priority folders: Chinese chars + pairs %.2f GB, trigrams %.2f GB\n",
            (mostly[0] + mostly[1]) / gb, mostly[2] / gb);
        std::printf("  estimated index, all candidates:               Chinese chars + pairs %.2f GB, trigrams %.2f GB\n",
            (all[0] + all[1]) / gb, all[2] / gb);
        const double sampleTri = pairsByClass[0][2] + pairsByClass[1][2] + pairsByClass[2][2] + pairsByClass[3][2];
        std::printf("  files over 1 MB account for %.1f%% of the trigram pairs, %.1f%% of the Chinese pairs\n",
            100.0 * pairsBig[2] / std::max(1.0, sampleTri),
            100.0 * pairsBig[1]
                / std::max(1.0, pairsByClass[0][1] + pairsByClass[1][1] + pairsByClass[2][1] + pairsByClass[3][1]));
    }
    if (args.contains(u"--open-test"_s)) {
        // Who makes opening slow: the file system, or the scan that real-time
        // protection runs when a file is opened to read its data? Opening for
        // attributes only reads nothing, so it is not scanned.
        cursor = std::max<std::size_t>(cursor, 30000); // past every file earlier runs touched
        std::vector<double> attributesOnly, data, dataAgain;
        for (const auto& [path, cls] : take(1000)) {
            const std::wstring p = ws::win32::longPath(path);
            const auto timeOpen = [&](DWORD access, DWORD flags) {
                const auto start = Clock::now();
                const HANDLE h = ::CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, flags, nullptr);
                const double us = usSince(start);
                if (h != INVALID_HANDLE_VALUE)
                    ::CloseHandle(h);
                return us;
            };
            attributesOnly.push_back(timeOpen(FILE_READ_ATTRIBUTES, 0));
            data.push_back(timeOpen(GENERIC_READ, FILE_FLAG_SEQUENTIAL_SCAN));
            dataAgain.push_back(timeOpen(GENERIC_READ, FILE_FLAG_SEQUENTIAL_SCAN));
        }
        std::printf("\nopening 1000 untouched files, each three times in a row:\n");
        printSpread("attrs", attributesOnly, 1.0, "us  (FILE_READ_ATTRIBUTES, first open)");
        printSpread("data", data, 1.0, "us  (GENERIC_READ, second open)");
        printSpread("again", dataAgain, 1.0, "us  (GENERIC_READ, third open)");
    }
    if (args.contains(u"--no-throughput"_s))
        return 0;

    // 3. Throughput by thread count, each run on files not touched before.
    const std::size_t runFiles = std::max<std::size_t>(500, sample * 3 / 4);
    const int appThreads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 2, 6);
    struct Run {
        int threads;
        bool lowPriority;
    };
    const Run runs[] = {{appThreads, true}, {appThreads, false}, {16, true}, {32, true}, {64, true}};
    std::printf("\nthroughput (%zu new files per run; the app uses %d threads, low I/O priority):\n", runFiles,
        appThreads);
    for (const Run& run : runs) {
        const auto files = take(runFiles);
        if (files.size() < runFiles)
            break;
        std::atomic<std::size_t> next {0};
        std::atomic<std::size_t> bytes {0};
        t = Clock::now();
        {
            std::vector<std::jthread> workers;
            for (int w = 0; w < run.threads; ++w) {
                workers.emplace_back([&] {
                    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    for (std::size_t k; (k = next.fetch_add(1)) < files.size();)
                        bytes += scanStreaming(files[k].first, scanner, maxBytes, run.lowPriority);
                });
            }
        }
        const double seconds = usSince(t) / 1e6;
        const double perSecond = static_cast<double>(files.size()) / seconds;
        std::printf("  %2d threads, %-6s I/O priority: %7.0f files/s, %6.1f MB/s  -> all candidates in ~%.0f s, "
                    "without low priority folders ~%.0f s\n",
            run.threads, run.lowPriority ? "low" : "normal", perSecond,
            static_cast<double>(bytes.load()) / seconds / (1024.0 * 1024.0), n / perSecond,
            (n - static_cast<double>(byClass[3])) / perSecond);
    }
    return 0;
}

// Builds a content index of the files a content search looks in (as the app's
// indexer does, with more threads), then times lookups and the candidate walk
// a content search does with it.
int runContentIndexBench(const ws::FileIndex& index, QStringList args)
{
    ws::Settings settings;
    settings.load();
    const bool all = args.contains(u"--all"_s); // system and program folders too
    const ws::ContentFilter filter {ws::ExtensionFilter(settings.contentExtensions), all};
    int threads = 32;
    if (const qsizetype at = args.indexOf(u"--threads"_s); at >= 0 && at + 1 < args.size())
        threads = std::max(1, args[at + 1].toInt());
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    if (const qsizetype at = args.indexOf(u"--sample"_s); at >= 0 && at + 1 < args.size())
        limit = args[at + 1].toULongLong();
    // Another seed picks other files: the antivirus remembers the ones it scanned.
    std::uint64_t seed = 20261007;
    if (const qsizetype at = args.indexOf(u"--seed"_s); at >= 0 && at + 1 < args.size())
        seed = args[at + 1].toULongLong();
    const bool background = args.contains(u"--background"_s); // as the app's indexer threads were
    const bool lowest = args.contains(u"--lowest"_s); // processor and memory priority low, disk priority not
    const bool readOnly = args.contains(u"--read-only"_s); // the reading speed only: no merge, no lookups
    const bool noAdd = args.contains(u"--no-add"_s); // ... and nothing handed to the index
    int mergeThreads = threads; // the merge at the end, as the indexer has it merge
    if (const qsizetype at = args.indexOf(u"--merge-threads"_s); at >= 0 && at + 1 < args.size())
        mergeThreads = std::max(1, args[at + 1].toInt());
    QString directory = QDir::tempPath() + u"/wsbench-content"_s;
    if (const qsizetype at = args.indexOf(u"--dir"_s); at >= 0 && at + 1 < args.size())
        directory = args[at + 1];
    QDir(directory).removeRecursively();

    std::vector<ws::EntryId> files;
    for (std::size_t c = 0; c < index.chunkCount(); ++c) {
        const auto entries = index.chunk(c);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (filter.accepts(index, entries[i]))
                files.push_back(static_cast<ws::EntryId>((c << ws::FileIndex::kChunkBits) + i));
        }
    }
    if (files.size() > limit) {
        std::mt19937_64 rng(seed);
        std::shuffle(files.begin(), files.end(), rng);
        files.resize(limit);
        std::sort(files.begin(), files.end());
    }
    std::printf("content index: %zu files (%s), %d threads%s, into %s\n", files.size(),
        all ? "all folders" : "without system and program folders", threads,
        background ? " in background mode" : lowest ? " at lowest priority" : "", qPrintable(directory));
    const ws::ContentSizeLimits limits = settings.contentSizeLimits();

    // --grams-only: what taking the grams out of the files costs this process
    // (one thread), from memory: no opening, no reading, no index.
    if (args.contains(u"--grams-only"_s)) {
        std::vector<std::string> texts;
        std::uint64_t bytes = 0;
        for (const ws::EntryId id : files) {
            const std::wstring path = index.wpath(id);
            if (ws::isDocumentPath(path))
                continue;
            QFile file(QString::fromStdWString(path));
            if (!file.open(QIODevice::ReadOnly) || file.size() > limits.of(path))
                continue;
            texts.push_back(file.readAll().toStdString());
            bytes += texts.back().size();
        }
        QElapsedTimer grams;
        grams.start();
        std::uint64_t keys = 0;
        // By kind of file: ASCII only, other UTF-8, UTF-16, ANSI.
        struct Kind {
            const char* name;
            std::size_t files = 0;
            std::uint64_t bytes = 0;
            double ms = 0;
            double detectMs = 0; // of ms: the encoding
            double finishMs = 0; // ... the grams sorted out at the end
        };
        std::array<Kind, 4> kinds {{{"ascii"}, {"utf-8"}, {"utf-16"}, {"ansi"}}};
        for (const std::string& t : texts) {
            QElapsedTimer one;
            one.start();
            std::size_t bom = 0;
            const ws::TextEncoding encoding = ws::ContentScanner::detect(
                std::string_view(t).substr(0, ws::ContentScanner::kChunkBytes), &bom);
            const double detectMs = static_cast<double>(one.nsecsElapsed()) / 1e6;
            ws::grams::Collector collector(encoding, ws::ContentScanner::legacyCodePage());
            for (std::size_t at = bom; at < t.size(); at += ws::ContentScanner::kChunkBytes)
                collector.feed(t.data() + at, std::min(ws::ContentScanner::kChunkBytes, t.size() - at));
            const double fedMs = static_cast<double>(one.nsecsElapsed()) / 1e6;
            keys += collector.finish().size();
            const double ms = static_cast<double>(one.nsecsElapsed()) / 1e6;
            const bool ascii
                = std::all_of(t.begin(), t.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
            Kind& k = kinds[encoding == ws::TextEncoding::Utf8 ? (ascii ? 0 : 1)
                    : encoding == ws::TextEncoding::Ansi           ? 3
                                                                   : 2];
            ++k.files;
            k.bytes += t.size();
            k.ms += ms;
            k.detectMs += detectMs;
            k.finishMs += ms - fedMs;
        }
        const double gramsMs = msSince(grams);
        std::printf("grams        %zu files, %.1f MB: %.0f ms, %.1f us a file, %.0f MB/s, %llu keys\n", texts.size(),
            static_cast<double>(bytes) / 1048576.0, gramsMs,
            gramsMs * 1000.0 / static_cast<double>(std::max<std::size_t>(texts.size(), 1)),
            static_cast<double>(bytes) / 1048576.0 / (gramsMs / 1000.0), static_cast<unsigned long long>(keys));
        for (const Kind& k : kinds) {
            std::printf("  %-7s %6zu files, %7.1f MB, %6.0f ms, %4.0f MB/s (encoding %.0f ms, sorting out %.0f ms)\n",
                k.name, k.files, static_cast<double>(k.bytes) / 1048576.0, k.ms,
                static_cast<double>(k.bytes) / 1048576.0 / std::max(k.ms / 1000.0, 1e-9), k.detectMs, k.finishMs);
        }
        return 0;
    }

    const double privateBefore = privateMB();
    ws::ContentIndex content(directory);
    std::atomic<std::size_t> next {0};
    std::atomic<std::size_t> indexed {0};
    std::atomic<std::size_t> empty {0};
    std::atomic<std::size_t> skipped {0};
    std::atomic<std::uint64_t> pairs {0};
    std::atomic<std::size_t> withGrams {0};
    std::atomic<std::size_t> refused {0}; // the index had no room: must not happen
    // Where the threads' time goes: finding the path, reading the file (open,
    // read, grams), handing it to the index (waiting for its lock included).
    std::atomic<std::int64_t> pathUs {0}, readUs {0}, addUs {0};
    const auto microseconds = [](std::chrono::steady_clock::time_point from) {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - from).count();
    };
    QElapsedTimer timer;
    timer.start();
    const CpuTimes cpuBefore = cpuTimes();
    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&] {
                if (background) {
                    ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
                } else if (lowest) {
                    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_LOWEST);
                    MEMORY_PRIORITY_INFORMATION memory {MEMORY_PRIORITY_LOW};
                    ::SetThreadInformation(::GetCurrentThread(), ThreadMemoryPriority, &memory, sizeof memory);
                }
                for (;;) {
                    const std::size_t i = next.fetch_add(1);
                    if (i >= files.size())
                        break;
                    auto at = std::chrono::steady_clock::now();
                    const std::wstring path = index.wpath(files[i]);
                    pathUs += microseconds(at);
                    at = std::chrono::steady_clock::now();
                    const auto text = ws::ContentIndexer::readFile(path, limits.of(path), {});
                    readUs += microseconds(at);
                    switch (text.outcome) {
                    case ws::ContentIndexer::Outcome::Indexed:
                        ++indexed;
                        pairs += text.keys.size();
                        withGrams += text.keys.empty() ? 0 : 1;
                        break;
                    case ws::ContentIndexer::Outcome::Empty:
                        ++empty;
                        break;
                    case ws::ContentIndexer::Outcome::Skipped:
                        ++skipped;
                        continue;
                    }
                    if (!noAdd) {
                        at = std::chrono::steady_clock::now();
                        if (!content.add(
                                files[i], text.keys, text.outcome == ws::ContentIndexer::Outcome::Empty, text.stamp, 0))
                            ++refused;
                        addUs += microseconds(at);
                    }
                    if ((i + 1) % 20000 == 0)
                        std::printf("  %zu files, %.0f s\n", i + 1, msSince(timer) / 1000.0);
                }
            });
        }
    }
    const double buildMs = msSince(timer);
    const CpuTimes cpuAfter = cpuTimes();
    std::printf("read         %.1f s (%.0f files/s): %zu read, %zu with grams, %zu empty/too large, %zu skipped%s\n",
        buildMs / 1000.0, static_cast<double>(files.size()) * 1000.0 / buildMs, indexed.load(), withGrams.load(),
        empty.load(), skipped.load(),
        refused.load() > 0 ? qPrintable(u", %1 REFUSED by the index"_s.arg(refused.load())) : "");
    // Most of what a read costs is the antivirus scanning the file in its own
    // process: the whole computer's processor time tells what it costs.
    const double wallS = buildMs / 1000.0;
    std::printf("processors   whole computer %.1f of %u busy on average, this process %.1f\n",
        (cpuAfter.system - cpuBefore.system) / wallS, std::thread::hardware_concurrency(),
        (cpuAfter.process - cpuBefore.process) / wallS);
    const double perFile = static_cast<double>(std::max<std::size_t>(files.size(), 1));
    std::printf(
        "per file     path %.1f us, read %.1f us, add %.1f us (thread time: %.0f%% of %d threads' wall time)%s\n",
        static_cast<double>(pathUs) / perFile, static_cast<double>(readUs) / perFile,
        static_cast<double>(addUs) / perFile,
        100.0 * static_cast<double>(pathUs + readUs + addUs) / (buildMs * 1000.0 * threads), threads,
        noAdd ? ", nothing added" : "");
    if (readOnly || noAdd) {
        QDir(directory).removeRecursively();
        return 0;
    }
    timer.restart();
    content.merge(mergeThreads);
    const double mergeMs = msSince(timer);
    ::HeapCompact(::GetProcessHeap(), 0);
    const auto stats = content.stats();
    std::printf("index        %.1f MB on disk (%zu documents, %zu distinct, %.1f M gram-document pairs, %.2f bytes each), merge %.0f ms\n",
        static_cast<double>(stats.segmentBytes) / (1024.0 * 1024.0), stats.documents, stats.contents,
        static_cast<double>(pairs.load()) / 1e6, static_cast<double>(stats.segmentBytes) / static_cast<double>(std::max<std::uint64_t>(pairs, 1)),
        mergeMs);
    std::printf("             %.2f M distinct grams (tables %.1f MB), postings %.1f MB\n",
        static_cast<double>(stats.grams) / 1e6,
        static_cast<double>(stats.segmentBytes - stats.postingBytes) / (1024.0 * 1024.0),
        static_cast<double>(stats.postingBytes) / (1024.0 * 1024.0));
    std::printf("memory       %.1f MB private since the start (documents table, read buffers, heap slack), peak +%.1f MB\n",
        privateMB() - privateBefore, peakCommitMB() - privateBefore);
    if (args.contains(u"--merge-times"_s)) {
        // Everything merged again (one segment into one) on fewer and fewer
        // threads: how long each takes. Each starts from the order the one
        // before left, so the files differ a little.
        for (const int t : {16, 8, 4, 1}) {
            timer.restart();
            const bool merged = content.merge(t);
            const double ms = msSince(timer);
            std::printf("merge again  %2d threads: %6.0f ms, %.1f MB%s\n", t, ms,
                static_cast<double>(content.stats().segmentBytes) / (1024.0 * 1024.0), merged ? "" : " FAILED");
        }
    }
    std::vector<std::uint64_t> segments;
    std::vector<ws::EntryId> identity(index.slotCount());
    std::iota(identity.begin(), identity.end(), ws::EntryId {0});
    std::printf("state        %.1f MB in the snapshot\n",
        static_cast<double>(content.serialize(identity, segments).size()) / (1024.0 * 1024.0));

    // Lookups, and the candidate walk a content search does with them.
    const QStringList needles = {u"的"_s, u"中文"_s, u"合同"_s, u"配置"_s, u"季度报告"_s, u"快速搜索"_s, u"日本語"_s,
        u"测试用例"_s, u"错误"_s, u"注意事项"_s, u"the"_s, u"error"_s, u"function"_s, u"WinShun"_s,
        u"localhost:8080"_s, u"2026-10"_s, u"std::vector"_s, u"getElementById"_s, u"ab"_s,
        u"const auto lock = index->readLock();"_s, u"printf(\"hello\")"_s, u"for (int i = 0; i < n; ++i)"_s,
        u"import numpy as np"_s, u"return nullptr;"_s, u"the error"_s};
    ws::WorkerPool pool(ws::WorkerPool::defaultThreadCount()); // as SearchEngine has
    const auto lock = index.readLock();
    for (const QString& needle : needles) {
        double lookupMs = 1e9;
        double walkMs = 1e9;
        std::size_t matched = 0;
        std::size_t unknown = 0;
        std::size_t ruledOut = 0;
        for (int run = 0; run < 3; ++run) {
            timer.restart();
            const auto lookup = content.lookup(needle);
            lookupMs = std::min(lookupMs, msSince(timer));
            timer.restart();
            // As SearchEngine::runContentSearch walks: a chunk per worker.
            std::atomic<std::size_t> m {0}, u {0}, r {0};
            constexpr std::uint32_t kMatch = ws::ContentIndex::Lookup::kMatch;
            pool.parallelFor(index.chunkCount(), [&](std::size_t c) {
                const auto first = static_cast<ws::EntryId>(c << ws::FileIndex::kChunkBits);
                auto k = static_cast<std::size_t>(std::lower_bound(lookup.known.begin(), lookup.known.end(), first,
                    [](std::uint32_t known, ws::EntryId id) { return (known & ~kMatch) < id; }) - lookup.known.begin());
                std::size_t mc = 0, uc = 0, rc = 0;
                const auto entries = index.chunk(c);
                for (std::size_t i = 0; i < entries.size(); ++i) {
                    if (!filter.accepts(index, entries[i]))
                        continue;
                    const auto id = static_cast<ws::EntryId>(first + i);
                    while (k < lookup.known.size() && (lookup.known[k] & ~kMatch) < id)
                        ++k;
                    if (k < lookup.known.size() && (lookup.known[k] & ~kMatch) == id)
                        ++(!lookup.usable || (lookup.known[k] & kMatch) ? mc : rc); // no grams: no file ruled out
                    else
                        ++uc;
                }
                m += mc;
                u += uc;
                r += rc;
            });
            matched = m;
            unknown = u;
            ruledOut = r;
            walkMs = std::min(walkMs, msSince(timer));
        }
        std::printf("%s%*s lookup %6.1f ms, walk %5.0f ms: %7zu files may contain it, %7zu ruled out, %6zu unknown\n",
            needle.toUtf8().constData(), std::max(0, static_cast<int>(16 - needle.size() * (needle[0].unicode() < 0x80 ? 1 : 2))), "", lookupMs, walkMs, matched,
            ruledOut, unknown);
    }
    // --verify: read every file the index rules out; any that contains the
    // phrase is a miss (a bug, or a file written to since it was indexed).
    if (args.contains(u"--verify"_s)) {
        for (const QString& needle : {u"WinShun"_s, u"std::vector"_s, u"localhost:8080"_s, u"getElementById"_s,
                 u"error"_s, u"季度报告"_s, u"配置"_s, u"const auto lock = index->readLock();"_s,
                 u"for (int i = 0; i < n; ++i)"_s, u"return nullptr;"_s}) {
            const auto lookup = content.lookup(needle);
            std::vector<ws::EntryId> ruledOut;
            for (const std::uint32_t k : lookup.known) {
                if (!(k & ws::ContentIndex::Lookup::kMatch))
                    ruledOut.push_back(k);
            }
            const ws::ContentScanner scanner(needle);
            std::atomic<std::size_t> at {0};
            std::atomic<std::size_t> misses {0};
            std::mutex printMutex;
            timer.restart();
            {
                std::vector<std::jthread> workers;
                for (int t = 0; t < threads; ++t) {
                    workers.emplace_back([&] {
                        for (std::size_t i = at.fetch_add(1); i < ruledOut.size(); i = at.fetch_add(1)) {
                            const std::wstring path = index.wpath(ruledOut[i]);
                            if (!scanner.scanFile(path, limits.of(path), {}))
                                continue;
                            if (misses.fetch_add(1) < 5) {
                                std::lock_guard printLock(printMutex);
                                std::printf("    miss: %s\n", QString::fromStdWString(path).toUtf8().constData());
                            }
                        }
                    });
                }
            }
            std::printf("verify %s: %zu ruled out, %zu of them contain it (%.0f s)\n", needle.toUtf8().constData(),
                ruledOut.size(), misses.load(), msSince(timer) / 1000.0);
        }
    }
    if (!args.contains(u"--keep"_s))
        QDir(directory).removeRecursively();
    return 0;
}

// ---- a content search with the app's own index (--content-search) -----------
//
// Loads the content index the app keeps, as the app restores it (from copies of
// its segment files: restoring deletes the files a state does not refer to),
// then for each needle does what a content search does before it opens a file
// and reads the files it would read, as 内容 reads them (16 threads, the
// ones the index says may contain it first). --unknown reads the files the
// index does not know too. Run it twice: the antivirus remembers files it has
// scanned for a few minutes.
int runContentSearchBench(const ws::FileIndex& index, std::span<const char> state, const QString& contentDir, QStringList args)
{
    ws::Settings settings;
    settings.load();
    const bool all = args.contains(u"--all"_s) || settings.contentInLowPriority;
    const bool readUnknown = args.contains(u"--unknown"_s);
    const bool show = args.contains(u"--show"_s); // the first files found, with their lines
    const ws::ContentFilter filter {ws::ExtensionFilter(settings.contentExtensions), all};
    const ws::ContentSizeLimits limits = settings.contentSizeLimits();
    QStringList needles;
    for (qsizetype i = args.indexOf(u"--content-search"_s) + 1; i < args.size() && !args[i].startsWith(u"--"); ++i)
        needles.append(args[i]);

    QTemporaryDir copy;
    for (const QFileInfo& f : QDir(contentDir).entryInfoList({u"*.grams"_s, u"*.texts"_s}, QDir::Files))
        QFile::copy(f.absoluteFilePath(), copy.path() + u'/' + f.fileName());
    ws::ContentIndex content(copy.path());
    QElapsedTimer timer;
    timer.start();
    if (!content.restore(state, index.slotCount())) {
        std::printf("no content index in the snapshot (or of another version)\n");
        return 1;
    }
    const auto stats = content.stats();
    std::printf("content index %zu documents (%zu contents, %zu to read again), %zu segments, %.1f MB; restored in %.0f ms\n",
        stats.documents, stats.contents, stats.pending, stats.segments,
        static_cast<double>(stats.segmentBytes) / (1024.0 * 1024.0), msSince(timer));

    const auto lock = index.readLock();
    const ws::EntryId profile = index.findPath(ws::win32::expandEnvironment(L"%USERPROFILE%"));
    for (const QString& needle : needles) {
        timer.restart();
        const QStringList terms = ws::contentTerms(needle); // as the app splits what was typed
        const ws::ContentIndex::Lookup lookup = content.lookup(terms);
        const double lookupMs = msSince(timer);
        struct File {
            ws::EntryId id;
            int order; // as SearchEngine: likely 0..3 by class, then unknown 4..7
            std::uint64_t size;
        };
        std::vector<File> files;
        std::size_t ruledOut = 0;
        constexpr std::uint32_t kMatch = ws::ContentIndex::Lookup::kMatch;
        QElapsedTimer walkTimer; // the candidate walk (SearchEngine does it in parallel)
        walkTimer.start();
        std::size_t accepted = 0;
        for (std::size_t c = 0; c < index.chunkCount(); ++c) {
            const auto entries = index.chunk(c);
            for (std::size_t i = 0; i < entries.size(); ++i)
                accepted += filter.accepts(index, entries[i]);
        }
        std::printf("  filter pass: %zu files accepted in %.2f ms (one thread)\n", accepted, msSince(walkTimer));
        std::size_t k = 0;
        for (std::size_t c = 0; c < index.chunkCount(); ++c) {
            const auto entries = index.chunk(c);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const ws::Entry& e = entries[i];
                if (!filter.accepts(index, e))
                    continue;
                const auto id = static_cast<ws::EntryId>((c << ws::FileIndex::kChunkBits) + i);
                bool likely = false;
                if (lookup.usable) {
                    while (k < lookup.known.size() && (lookup.known[k] & ~kMatch) < id)
                        ++k;
                    if (k < lookup.known.size() && (lookup.known[k] & ~kMatch) == id) {
                        if (!(lookup.known[k] & kMatch)) {
                            ++ruledOut;
                            continue;
                        }
                        likely = true;
                    }
                }
                int cls = 1;
                if (e.flags & ws::EntryFlag::LowPriority)
                    cls = 3;
                else if (e.flags & ws::EntryFlag::Hidden)
                    cls = 2;
                else
                    for (ws::EntryId p = e.parent; p != ws::kNoEntry; p = index.entry(p).parent)
                        if (p == profile) {
                            cls = 0;
                            break;
                        }
                files.push_back({id, likely ? cls : 4 + cls, 0});
            }
        }
        std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.order < b.order; });
        const auto likelyEnd = static_cast<std::size_t>(
            std::find_if(files.begin(), files.end(), [](const File& f) { return f.order >= 4; }) - files.begin());
        std::uint64_t likelyBytes = 0;
        std::vector<std::wstring> paths(files.size());
        for (std::size_t i = 0; i < files.size(); ++i) {
            paths[i] = index.wpath(files[i].id);
            if (i < likelyEnd) {
                WIN32_FILE_ATTRIBUTE_DATA data {}; // no antivirus scan: the file is not opened for its data
                if (::GetFileAttributesExW(ws::win32::longPath(paths[i]).c_str(), GetFileExInfoStandard, &data))
                    files[i].size = (std::uint64_t {data.nFileSizeHigh} << 32) | data.nFileSizeLow;
                likelyBytes += files[i].size;
            }
        }
        std::printf("\n\"%s\"  lookup %.1f ms (%s): %zu may contain it (%.1f MB), %zu ruled out, %zu unknown\n",
            needle.toUtf8().constData(), lookupMs, !lookup.usable ? "no grams" : lookup.decisive ? "decisive" : "filter",
            likelyEnd, static_cast<double>(likelyBytes) / (1024.0 * 1024.0), ruledOut, files.size() - likelyEnd);
        std::vector<std::size_t> biggest(likelyEnd);
        std::iota(biggest.begin(), biggest.end(), std::size_t {0});
        std::sort(biggest.begin(), biggest.end(), [&](std::size_t a, std::size_t b) { return files[a].size > files[b].size; });
        for (std::size_t i = 0; i < std::min<std::size_t>(biggest.size(), 5); ++i)
            std::printf("    %7.1f MB  %s\n", static_cast<double>(files[biggest[i]].size) / (1024.0 * 1024.0),
                QString::fromStdWString(paths[biggest[i]]).toUtf8().constData());

        // Read as 内容 does: 16 threads, in order, the likely ones first.
        const std::size_t end = readUnknown ? files.size() : likelyEnd;
        const ws::ContentScanner scanner(terms);
        std::atomic<std::size_t> next {0};
        std::atomic<std::size_t> found {0};
        std::atomic<std::size_t> foundLikely {0};
        std::atomic<std::int64_t> firstUs {-1};
        std::atomic<std::int64_t> likelyDoneUs {0};
        std::atomic<std::size_t> likelyLeft {likelyEnd};
        std::mutex shownMutex;
        std::vector<std::pair<std::size_t, ws::ContentMatch>> shown;
        timer.restart();
        {
            std::vector<std::jthread> workers;
            for (int t = 0; t < 16; ++t) {
                workers.emplace_back([&] {
                    for (std::size_t i = next.fetch_add(1); i < end; i = next.fetch_add(1)) {
                        if (const auto match = scanner.scanFile(paths[i], limits.of(paths[i]), {})) {
                            ++found;
                            if (show) {
                                std::lock_guard lock(shownMutex);
                                shown.emplace_back(i, *match);
                            }
                            foundLikely += i < likelyEnd ? 1 : 0;
                            std::int64_t none = -1;
                            firstUs.compare_exchange_strong(none, timer.nsecsElapsed() / 1000);
                        }
                        if (i < likelyEnd && likelyLeft.fetch_sub(1) == 1)
                            likelyDoneUs = timer.nsecsElapsed() / 1000;
                    }
                });
            }
        }
        std::printf("  read %zu files in %.0f ms: %zu contain it (%zu of the likely ones); first after %.0f ms, likely ones done after %.0f ms\n",
            end, msSince(timer), found.load(), foundLikely.load(), static_cast<double>(firstUs.load()) / 1000.0,
            static_cast<double>(likelyDoneUs.load()) / 1000.0);
        // As the launcher lists them: by relevance, a tie in reading order.
        const std::int64_t now = QDateTime::currentSecsSinceEpoch();
        std::vector<std::pair<int, std::size_t>> ranked; // relevance, index into shown
        for (std::size_t k = 0; k < shown.size(); ++k) {
            const auto& [i, m] = shown[k];
            const QString path = QString::fromStdWString(paths[i]);
            const ws::FileTime modified = index.modified(files[i].id);
            ranked.emplace_back(ws::contentRelevance(m, terms, QStringView(path).mid(path.lastIndexOf(u'\\') + 1),
                                    files[i].order & 3, modified ? now - static_cast<std::int64_t>(modified) : -1),
                k);
        }
        std::ranges::sort(ranked, [&](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : shown[a.second].first < shown[b.second].first;
        });
        for (std::size_t r = 0; r < ranked.size() && r < 10; ++r) {
            const auto& [i, m] = shown[ranked[r].second];
            QString marked = m.snippet; // the terms in [brackets]
            for (auto it = m.matches.crbegin(); it != m.matches.crend(); ++it)
                marked.insert(it->first + it->second, u']').insert(it->first, u'[');
            std::printf("    %3d %d/%lld%s%s  %s:%d  %s\n", ranked[r].first, m.termsOnLine,
                static_cast<long long>(terms.size()), m.wordStarts == 0 ? "" : " w", m.longLine ? " long" : "",
                QString::fromStdWString(paths[i]).toUtf8().constData(), m.line, marked.left(120).toUtf8().constData());
        }
    }
    return 0;
}

// ---- documents (--extract) -------------------------------------------------

// Reads documents as WinShunExtract.exe does (here, in this process): those
// given (files, or folders searched through), else every document a content
// search looks in (the snapshot's, outside system and program folders).
// Reports by type what came out, how long it took, the slowest files and
// those that failed; --dump prints the text of each file with its places.
int runExtractBench(const ws::FileIndex* index, QStringList args)
{
    const bool dump = args.removeAll(u"--dump"_s) > 0;
    // Through WinShunExtract.exe, as the app reads them (restricted, low integrity).
    const bool sandbox = args.removeAll(u"--sandbox"_s) > 0;
    // ... at background priority, as the content indexer's are.
    const bool background = args.removeAll(u"--background"_s) > 0;
    // In this process: the whole file read into memory first, rather than
    // read where the parser asks.
    const bool memory = args.removeAll(u"--memory"_s) > 0;
    int threads = 4;
    if (const qsizetype at = args.indexOf(u"--threads"_s); at >= 0 && at + 1 < args.size()) {
        threads = std::clamp(args[at + 1].toInt(), 1, 64);
        args.remove(at, 2);
    }
    args.removeAll(u"--extract"_s);
    const QStringList& extensions = ws::documentExtensions();
    std::vector<std::wstring> files;
    if (!args.isEmpty()) {
        QStringList filters;
        for (const QString& ext : extensions)
            filters.append(u"*."_s + ext);
        for (const QString& arg : args) {
            const QFileInfo info(arg);
            if (info.isDir()) {
                QDirIterator it(arg, filters, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
                while (it.hasNext())
                    files.push_back(QDir::toNativeSeparators(it.next()).toStdWString());
            } else {
                files.push_back(QDir::toNativeSeparators(info.absoluteFilePath()).toStdWString());
            }
        }
    } else if (index) {
        const ws::ContentFilter filter {ws::ExtensionFilter(extensions), false};
        const auto lock = index->readLock();
        for (std::size_t c = 0; c < index->chunkCount(); ++c) {
            const auto entries = index->chunk(c);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (filter.accepts(*index, entries[i]))
                    files.push_back(index->wpath(static_cast<ws::EntryId>((c << ws::FileIndex::kChunkBits) + i)));
            }
        }
    }
    std::printf("%zu documents, %d threads%s%s\n", files.size(), threads,
        sandbox ? (background ? ", in WinShunExtract.exe at background priority" : ", in WinShunExtract.exe") : "",
        !sandbox && memory ? ", each read into memory first" : "");
    std::unique_ptr<ws::DocExtractor> extractor;
    if (sandbox) {
        extractor = std::make_unique<ws::DocExtractor>(
            threads, background ? ws::DocExtractor::Priority::Background : ws::DocExtractor::Priority::Normal);
    }
    const auto pathOf = [&](std::size_t i) {
        return narrow(std::u16string_view(reinterpret_cast<const char16_t*>(files[i].data()), files[i].size()));
    };

    struct Result {
        ws::extractproto::Status status = ws::extractproto::Status::Failed;
        double ms = 0;
        std::uint64_t size = 0;
        std::size_t text = 0;
        std::size_t packed = 0;
        std::size_t places = 0;
        bool truncated = false;
    };
    std::vector<Result> results(files.size());
    std::atomic<std::size_t> next {0};
    std::atomic<std::size_t> done {0};
    std::mutex printMutex;
    QElapsedTimer total;
    total.start();
    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&] {
                for (std::size_t i = next.fetch_add(1); i < files.size(); i = next.fetch_add(1)) {
                    Result& r = results[i];
                    const ws::win32::UniqueHandle file(::CreateFileW(ws::win32::longPath(files[i]).c_str(),
                        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                        nullptr));
                    LARGE_INTEGER size {};
                    if (!file.valid() || !::GetFileSizeEx(file.get(), &size))
                        continue;
                    r.size = static_cast<std::uint64_t>(size.QuadPart);
                    QElapsedTimer timer;
                    timer.start();
                    ws::doctext::DocText text;
                    if (extractor) {
                        ws::DocExtractor::Result read = extractor->extract(file.get(), r.size, {});
                        r.status = read.retry ? ws::extractproto::Status::Failed : read.status;
                        text = std::move(read.text);
                    } else if (memory) {
                        std::string bytes(static_cast<std::size_t>(r.size), '\0');
                        ws::extract::HandleSource(file.get(), r.size).readExact(0, bytes.data(), bytes.size());
                        ws::extract::MemorySource source(bytes);
                        r.status = ws::extract::extract(source, {}, text);
                    } else {
                        ws::extract::HandleSource source(file.get(), r.size);
                        r.status = ws::extract::extract(source, {}, text);
                    }
                    r.ms = msSince(timer);
                    r.text = text.text.size();
                    r.places = text.places.size();
                    r.truncated = text.truncated;
                    if (r.status == ws::extractproto::Status::Ok) {
                        const std::string blob = ws::doctext::serialize(text);
                        if (const auto back = ws::doctext::deserialize(blob, ws::DocExtractor::kMaxText);
                            !back || !(*back == text)) {
                            std::lock_guard lock(printMutex);
                            std::printf("  does not read back (%zu places): %s\n", text.places.size(), pathOf(i).c_str());
                        }
                        r.packed = static_cast<std::size_t>(
                            qCompress(reinterpret_cast<const uchar*>(blob.data()), static_cast<qsizetype>(blob.size()))
                                .size());
                    }
                    if (dump) {
                        std::lock_guard lock(printMutex);
                        std::printf("\n==== %s  [%s, %.1f ms, %zu bytes, %zu places%s]\n", pathOf(i).c_str(),
                            ws::extract::statusName(r.status), r.ms, r.text, r.places, r.truncated ? ", cut" : "");
                        std::uint32_t line = 1;
                        std::size_t start = 0;
                        while (start < text.text.size()) {
                            std::size_t end = text.text.find('\n', start);
                            if (end == std::string::npos)
                                end = text.text.size();
                            const ws::doctext::Location where = ws::doctext::locate(text, line);
                            const char* kind = where.kind == ws::doctext::PlaceKind::Page ? "page"
                                : where.kind == ws::doctext::PlaceKind::Slide             ? "slide"
                                : where.kind == ws::doctext::PlaceKind::Row               ? "row"
                                                                                          : "-";
                            std::printf("%5u %s %s%s%u | %.*s\n", line, kind, where.sheet.c_str(),
                                where.sheet.empty() ? "" : "!", where.number,
                                static_cast<int>(std::min<std::size_t>(end - start, 300)), text.text.data() + start);
                            start = end + 1;
                            ++line;
                        }
                    }
                    const std::size_t n = done.fetch_add(1) + 1;
                    if (!dump && n % 500 == 0) {
                        std::lock_guard lock(printMutex);
                        std::printf("  %zu / %zu  (%.0f s)\n", n, files.size(), msSince(total) / 1000);
                    }
                }
            });
        }
    }
    const double wall = msSince(total);

    struct Row {
        std::size_t count = 0;
        std::array<std::size_t, 6> statuses {};
        double ms = 0;
        double maxMs = 0;
        std::uint64_t size = 0;
        std::uint64_t text = 0;
        std::uint64_t packed = 0;
    };
    std::map<std::string, Row> rows;
    Row all;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string ext = pathOf(i);
        ext = ext.substr(ext.rfind('.') + 1);
        for (char& ch : ext)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (Row* row : {&rows[ext], &all}) {
            const Result& r = results[i];
            ++row->count;
            ++row->statuses[static_cast<std::size_t>(r.status)];
            row->ms += r.ms;
            row->maxMs = std::max(row->maxMs, r.ms);
            row->size += r.size;
            row->text += r.text;
            row->packed += r.packed;
        }
    }
    std::printf("\n%-6s %6s %6s %6s %6s %6s %6s %6s %9s %8s %9s %9s %9s\n", "type", "files", "ok", "notext", "locked",
        "unsup", "broken", "failed", "avg ms", "max ms", "file MB", "text MB", "packed MB");
    const auto print = [](const std::string& name, const Row& r) {
        std::printf("%-6s %6zu %6zu %6zu %6zu %6zu %6zu %6zu %9.1f %8.0f %9.1f %9.1f %9.1f\n", name.c_str(), r.count,
            r.statuses[0], r.statuses[1], r.statuses[2], r.statuses[3], r.statuses[4], r.statuses[5],
            r.count ? r.ms / static_cast<double>(r.count) : 0.0, r.maxMs, static_cast<double>(r.size) / 1048576.0,
            static_cast<double>(r.text) / 1048576.0, static_cast<double>(r.packed) / 1048576.0);
    };
    for (const auto& [name, row] : rows)
        print(name, row);
    print("all", all);
    std::printf("wall %.1f s, peak commit of this process %.0f MB\n", wall / 1000, peakCommitMB());

    std::vector<std::size_t> order(files.size());
    std::iota(order.begin(), order.end(), std::size_t {0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return results[a].ms > results[b].ms; });
    std::printf("\nslowest:\n");
    for (std::size_t k = 0; k < std::min<std::size_t>(order.size(), 12); ++k) {
        const Result& r = results[order[k]];
        std::printf("  %8.0f ms  %7.1f MB  %8zu text%s  %s\n", r.ms, static_cast<double>(r.size) / 1048576.0, r.text,
            r.truncated ? " (cut)" : "", pathOf(order[k]).c_str());
    }
    for (const auto status : {ws::extractproto::Status::Broken, ws::extractproto::Status::Unsupported,
             ws::extractproto::Status::Failed, ws::extractproto::Status::Encrypted, ws::extractproto::Status::NoText}) {
        std::size_t shown = 0;
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (results[i].status != status)
                continue;
            if (shown++ == 0)
                std::printf("\n%s:\n", ws::extract::statusName(status));
            if (shown <= 25)
                std::printf("  %s\n", pathOf(i).c_str());
        }
        if (shown > 25)
            std::printf("  ... %zu more\n", shown - 25);
    }
    return 0;
}

// Process heap: bytes handed out vs committed (the gap is free memory the
// heap keeps instead of returning it to the OS).
void printHeap(const char* label)
{
    HEAP_SUMMARY hs {};
    hs.cb = sizeof hs;
    if (::HeapSummary(::GetProcessHeap(), 0, &hs))
        std::printf("%-12s heap allocated %.1f MB, committed %.1f MB\n", label,
            static_cast<double>(hs.cbAllocated) / (1024.0 * 1024.0), static_cast<double>(hs.cbCommitted) / (1024.0 * 1024.0));
}

// ---- places (--places) -----------------------------------------------------

int runPlaces(const QStringList& queries)
{
    QElapsedTimer timer;
    timer.start();
    const ws::PlaceList places = ws::loadPlaces({});
    std::printf("places       %zu, read in %.0f ms\n", places.size(), static_cast<double>(timer.nsecsElapsed()) / 1e6);

    QFile extras(u":/winshun/places.txt"_s);
    if (extras.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ws::PlaceList copy = places;
        for (const QString& header : ws::applyPlaceExtras(copy, QString::fromUtf8(extras.readAll()), true))
            std::printf("places.txt: no place here opens [%s]\n", qUtf8Printable(header));
    }

    if (queries.isEmpty()) {
        for (const ws::PlaceInfo& p : places) {
            std::printf("%s | %s | %zu + %zu keywords\n", qUtf8Printable(p.name), qUtf8Printable(p.command),
                p.folded.size(), p.ownFolded.size());
        }
        return 0;
    }
    for (const QString& text : queries) {
        const ws::ParsedQuery query = ws::parseQuery(text);
        const ws::NameMatcher matcher(query);
        timer.restart();
        const std::vector<ws::PlaceHit> hits = ws::searchPlaces(places, query, matcher, {});
        const double us = static_cast<double>(timer.nsecsElapsed()) / 1e3;
        std::printf("\n%s  (%zu, %.0f us)\n", qUtf8Printable(text), hits.size(), us);
        for (std::size_t i = 0; i < std::min<std::size_t>(hits.size(), 8); ++i) {
            const ws::PlaceInfo& p = places[hits[i].index];
            std::printf("  %4d  %s | %s\n", hits[i].score, qUtf8Printable(p.name), qUtf8Printable(p.command));
        }
    }
    return 0;
}

} // namespace

// ---- network shares (--share) ------------------------------------------------

const char* problemName(ws::share::Problem p)
{
    using P = ws::share::Problem;
    switch (p) {
    case P::None: return "none";
    case P::Offline: return "offline";
    case P::Dropped: return "dropped";
    case P::NotFound: return "not found";
    case P::NeedsSignIn: return "needs sign-in";
    case P::GuestBlocked: return "guest blocked";
    case P::Signing: return "signing";
    case P::Smb1Only: return "SMB1 only";
    case P::Denied: return "denied";
    case P::Other: return "other";
    }
    return "?";
}

const char* stateName(ws::ShareIndexer::State s)
{
    using S = ws::ShareIndexer::State;
    switch (s) {
    case S::Waiting: return "waiting";
    case S::Building: return "building";
    case S::Verifying: return "verifying";
    case S::Online: return "online";
    case S::Problem: return "problem";
    case S::Suspect: return "suspect";
    }
    return "?";
}

// Reads a share as the ShareIndexer does: how fast, with what problems.
int runShare(QStringList args)
{
    int threads = 16;
    int rounds = 2;
    int watchSeconds = 0;
    ws::share::Lister::Options options;
    options.excludedFiles = {L"._*", L".DS_Store"};
    if (const qsizetype at = args.indexOf(u"--threads"_s); at >= 0 && at + 1 < args.size()) {
        threads = args[at + 1].toInt();
        args.remove(at, 2);
    }
    if (const qsizetype at = args.indexOf(u"--rounds"_s); at >= 0 && at + 1 < args.size()) {
        rounds = args[at + 1].toInt();
        args.remove(at, 2);
    }
    if (const qsizetype at = args.indexOf(u"--buffer"_s); at >= 0 && at + 1 < args.size()) {
        options.bufferBytes = static_cast<std::size_t>(args[at + 1].toInt()) * 1024;
        args.remove(at, 2);
    }
    if (const qsizetype at = args.indexOf(u"--watch"_s); at >= 0 && at + 1 < args.size()) {
        watchSeconds = args[at + 1].toInt();
        args.remove(at, 2);
    }
    options.shortNames = args.removeAll(u"--short-names"_s) > 0;
    const bool lifecycle = args.removeAll(u"--indexer"_s) > 0;
    if (args.isEmpty()) {
        std::printf("wsbench --share \\\\host\\share [--threads N] [--rounds N] [--buffer KB] [--short-names] [--watch S] [--indexer]\n");
        return 1;
    }
    const std::wstring path = ws::unc::normalize(args.first().toStdWString());
    if (path.empty()) {
        std::printf("not a share: %ls\n", qUtf16Printable(args.first()));
        return 1;
    }
    const std::wstring host(ws::unc::host(path));

    QElapsedTimer t;
    t.start();
    const ws::share::Answer answer = ws::share::probe(host, std::chrono::milliseconds(1500), true);
    std::printf("probe %ls:445  %s  %.1f ms\n", host.c_str(),
        answer == ws::share::Answer::Smb2       ? "reachable"
            : answer == ws::share::Answer::Smb1Only ? "reachable, SMB1 only"
                                                    : "NOT reachable",
        msSince(t));
    t.restart();
    unsigned long shareError = 0;
    const std::vector<std::wstring> shares = ws::share::listShares(host, &shareError);
    std::printf("shares on %ls (%.1f ms, error %lu):", host.c_str(), msSince(t), shareError);
    for (const std::wstring& s : shares)
        std::printf(" %ls", s.c_str());
    std::printf("\n");

    if (lifecycle) {
        // The whole thing: an index of its own, the share's thread, statuses as they change.
        auto index = std::make_shared<ws::FileIndex>();
        ws::ShareIndexer::Options o;
        o.shares = {{path, {}}};
        o.rules = ws::CrawlRules {{}, {L"#recycle", L"@eaDir", L"@Recycle", L"$RECYCLE.BIN", L"System Volume Information"}, {}, {}};
        o.excludedFiles = options.excludedFiles;
        o.threads = threads;
        std::mutex printMutex;
        ws::ShareIndexer::Callbacks callbacks;
        callbacks.reading = [&](const std::wstring& p, bool reading) {
            std::lock_guard lock(printMutex);
            std::printf("%8.0f ms  %s %ls\n", msSince(t), reading ? "reading" : "done reading", p.c_str());
        };
        ws::ShareIndexer indexer({[&] { return index; }, [] { return true; }}, o, callbacks);
        t.restart();
        indexer.start();
        ws::ShareIndexer::Status last;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(30);
        int passes = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto st = indexer.statuses();
            if (st.empty())
                break;
            if (!(st[0] == last)) {
                last = st[0];
                std::lock_guard lock(printMutex);
                SYSTEMTIME now;
                ::GetLocalTime(&now); // to line up with changes made elsewhere
                std::printf("%8.0f ms  %02u:%02u:%02u.%03u  %-9s problem=%s error=%lu items=%zu verified=%u missing=%zu watching=%d\n",
                    msSince(t), now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, stateName(last.state), problemName(last.problem), last.error, last.items,
                    last.lastVerified, last.missing, last.watching ? 1 : 0);
                if (last.state == ws::ShareIndexer::State::Online && ++passes < rounds)
                    indexer.verifyNow(path);
            }
            if (passes >= rounds && watchSeconds == 0)
                break;
            if (passes >= rounds && msSince(t) > 1000.0 * watchSeconds)
                break;
        }
        indexer.stop();
        const auto lock = index->readLock();
        const ws::FileIndex::MemoryUse use = index->memoryUse();
        std::printf("index: %zu live entries, %.1f MB (entries %.1f, times %.1f, names %.1f, folders %.1f, tables %.1f)\n",
            index->liveCount(),
            (use.entries + use.times + use.names + use.folderRecords + use.childTables + use.intern) / 1048576.0,
            use.entries / 1048576.0, use.times / 1048576.0, use.names / 1048576.0, use.folderRecords / 1048576.0,
            use.childTables / 1048576.0);
        // The awkward names: the longest path, the deepest, those not in ASCII.
        ws::EntryId longest = ws::kNoEntry, deepest = ws::kNoEntry;
        std::size_t longestLength = 0;
        int deepestDepth = 0, wide = 0;
        for (ws::EntryId id = 0; id < index->slotCount(); ++id) {
            if (index->entry(id).isDeleted())
                continue;
            const std::wstring p = index->wpath(id);
            if (p.size() > longestLength)
                longestLength = p.size(), longest = id;
            if (const int d = index->depth(id); d > deepestDepth)
                deepestDepth = d, deepest = id;
            if (std::ranges::any_of(index->name(id), [](char c) { return static_cast<unsigned char>(c) >= 0x80; })
                && wide++ < 8)
                std::printf("  non-ASCII: %ls (found again: %s)\n", p.c_str(), index->findPath(p) == id ? "yes" : "NO");
        }
        if (longest != ws::kNoEntry)
            std::printf("  longest (%zu chars): %ls\n", longestLength, index->wpath(longest).c_str());
        if (deepest != ws::kNoEntry)
            std::printf("  deepest (%d levels): %ls\n", deepestDepth, index->wpath(deepest).c_str());
        return 0;
    }

    // Rounds of Crawler::sync with the share lister: the first builds, the
    // others verify (removals deferred, as the indexer does).
    const ws::Crawler crawler(ws::CrawlRules {{}, {L"#recycle", L"@eaDir", L"@Recycle", L"$RECYCLE.BIN", L"System Volume Information"}, {}, {}});
    ws::FileIndex index;
    ws::EntryId root = ws::kNoEntry;
    {
        auto lock = index.writeLock();
        root = ws::ShareIndexer::ensureRoot(index, path);
    }
    for (int r = 0; r < rounds; ++r) {
        std::stop_source stop;
        ws::share::Lister lister(crawler, path, options, stop);
        std::vector<ws::EntryId> gone;
        ws::Crawler::SyncStats stats;
        t.restart();
        crawler.sync(index, {{root, path, ws::EntryFlag::Remote}}, threads, false, stop.get_token(),
            [&](const ws::Crawler::Root& dir, ws::DirListing& out) { return lister(dir, out); }, &gone, &stats);
        const double ms = msSince(t);
        std::printf("round %d: %.0f ms  folders %zu (%.0f/s, %.2f ms each)  items %zu  failed %zu  denied %zu  gone %zu"
                    "  problem %s (error %lu)%s\n",
            r + 1, ms, stats.listed, stats.listed / (ms / 1000.0), ms / std::max<std::size_t>(1, stats.listed),
            stats.items, stats.failed, lister.deniedFolders(), gone.size(), problemName(lister.problem()),
            lister.firstError(), lister.stoppedPass() ? "  [pass stopped]" : "");
    }
    {
        const auto lock = index.readLock();
        std::printf("index: %zu live entries; findPath(root) %s\n", index.liveCount(),
            index.findPath(path) == root ? "ok" : "MISSING");
    }

    if (watchSeconds > 0) {
        std::printf("watching %ls for %d s...\n", path.c_str(), watchSeconds);
        ws::ChangeWatcher watcher([](std::vector<ws::FsChange>&& changes) {
            for (const ws::FsChange& c : changes)
                std::printf("  change %d  %ls\n", static_cast<int>(c.kind), c.path.c_str());
        });
        watcher.setRoots({path});
        std::printf("watching: %s\n", watcher.roots().empty() ? "NO (the server does not tell)" : "yes");
        std::this_thread::sleep_for(std::chrono::seconds(watchSeconds));
    }
    return 0;
}

// ---- a plugin, as the app runs it (--plugin) --------------------------------

// Checks FOLDER's plugin.json, runs `query` through the plugin Host (the
// runner starts the plugin as it would in the app), prints the first
// answer's items and the timings.
int runPlugin(QStringList args)
{
    QString folder;
    QString query;
    int runs = 20;
    if (const qsizetype at = args.indexOf(u"--plugin"_s); at >= 0 && at + 1 < args.size()) {
        folder = args[at + 1];
        args.remove(at, 2);
    }
    if (const qsizetype at = args.indexOf(u"--query"_s); at >= 0 && at + 1 < args.size()) {
        query = args[at + 1];
        args.remove(at, 2);
    }
    if (const qsizetype at = args.indexOf(u"--runs"_s); at >= 0 && at + 1 < args.size()) {
        runs = std::max(0, args[at + 1].toInt());
        args.remove(at, 2);
    }
    QJsonObject pluginSettings; // --setting key=value: as the settings page would hand them over
    for (qsizetype at = args.indexOf(u"--setting"_s); at >= 0 && at + 1 < args.size(); at = args.indexOf(u"--setting"_s)) {
        const QString pair = args[at + 1];
        const qsizetype eq = pair.indexOf(u'=');
        if (eq > 0) {
            const QString value = pair.mid(eq + 1);
            pluginSettings.insert(pair.left(eq), value == u"true" ? QJsonValue(true) : value == u"false" ? QJsonValue(false) : QJsonValue(value));
        }
        args.remove(at, 2);
    }
    QString actionId; // --action ID: run it on the first item of the first answer
    if (const qsizetype at = args.indexOf(u"--action"_s); at >= 0 && at + 1 < args.size()) {
        actionId = args[at + 1];
        args.remove(at, 2);
    }
    if (folder.isEmpty() || query.isEmpty()) {
        std::printf("usage: wsbench --plugin FOLDER --query \"TEXT\" [--runs N] [--setting KEY=VALUE ...] [--action ID]\n");
        return 1;
    }
    const QFileInfo info(folder);
    if (!info.isDir() || !QFileInfo::exists(info.filePath() + u"/plugin.json"_s)) {
        std::printf("no plugin.json in %s\n", qUtf8Printable(QDir::toNativeSeparators(folder)));
        return 1;
    }
    QString error;
    const std::optional<ws::plugin::Manifest> manifest = ws::plugin::Manifest::load(info.filePath() + u"/plugin.json"_s, &error);
    if (!manifest) {
        std::printf("plugin.json: %s\n", qUtf8Printable(error));
        return 1;
    }
    if (manifest->id != info.fileName()) {
        std::printf("the folder is named %s but the manifest's id is %s: they must agree\n", qUtf8Printable(info.fileName()),
            qUtf8Printable(manifest->id));
        return 1;
    }
    std::printf("plugin %s %s (%s): %s, %lld command(s)\n", qUtf8Printable(manifest->id), qUtf8Printable(manifest->version),
        qUtf8Printable(manifest->name.pick(u"zh"_s)),
        !manifest->runtime ? "zero-code" : manifest->runtime->mode == ws::plugin::RunMode::Resident ? "resident" : "once",
        static_cast<long long>(manifest->commands.size()));

    QTemporaryDir temp;
    ws::plugin::Host::Options options;
    options.userPluginDir = temp.path() + u"/plugins"_s;
    options.bundledPluginDir = info.absolutePath();
    options.dataRoot = temp.path() + u"/data"_s;
    options.runnerExe = QCoreApplication::applicationDirPath() + u"/WinShunPluginHost.exe"_s;
    options.version = QStringLiteral(WINSHUN_VERSION "-bench");
    ws::plugin::Host host(options);
    host.setEnabled(manifest->id, true);
    if (!pluginSettings.isEmpty())
        host.setPluginSettings(manifest->id, pluginSettings);
    QObject::connect(&host, &ws::plugin::Host::logged, [](const QString& line) { std::printf("  log: %s\n", qUtf8Printable(line)); });
    QObject::connect(&host, &ws::plugin::Host::queryFailed, [](std::uint64_t id, const QString&, const QString& why) {
        std::printf("  query %llu failed: %s\n", static_cast<unsigned long long>(id), qUtf8Printable(why));
    });
    if (host.catalog().match(query).empty()) {
        QStringList keywords;
        for (const ws::plugin::Command& c : manifest->commands)
            keywords += host.catalog().keywordsOf(manifest->id, c.id);
        std::printf("\"%s\" triggers no command of this plugin (keywords: %s)\n", qUtf8Printable(query), qUtf8Printable(keywords.join(u", "_s)));
        return 1;
    }

    // One query at a time, each timed from query() to its final answer.
    std::uint64_t nextId = 1;
    ws::plugin::Items firstItems; // of the first answer, for --action
    QString firstCommand;
    const auto timed = [&](bool print) -> std::optional<double> {
        const std::uint64_t id = nextId++;
        std::optional<double> took;
        QElapsedTimer timer;
        timer.start();
        bool done = false;
        const QMetaObject::Connection results = QObject::connect(&host, &ws::plugin::Host::results,
            [&](std::uint64_t queryId, const QString&, const QString& command, const ws::plugin::Items& items, bool partial, bool) {
                if (queryId != id || partial)
                    return;
                took = msSince(timer);
                done = true;
                if (!print)
                    return;
                firstItems = items;
                firstCommand = command;
                QJsonArray array;
                for (const ws::plugin::Item& item : items)
                    array.push_back(ws::plugin::itemToJson(item));
                std::printf("%s: %lld item(s)\n%s\n", qUtf8Printable(command), static_cast<long long>(items.size()),
                    QJsonDocument(array).toJson(QJsonDocument::Indented).constData());
            });
        const QMetaObject::Connection failed = QObject::connect(&host, &ws::plugin::Host::queryFailed,
            [&](std::uint64_t queryId, const QString&, const QString&) {
                if (queryId == id)
                    done = true;
            });
        host.query(query, ws::Scope::All, id);
        const QDeadlineTimer deadline(15000);
        while (!done && !deadline.hasExpired())
            QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
        QObject::disconnect(results);
        QObject::disconnect(failed);
        if (!done)
            std::printf("  query %llu: no answer in 15 s\n", static_cast<unsigned long long>(id));
        return took;
    };

    const std::optional<double> cold = timed(true);
    const std::optional<double> first = timed(false);
    std::vector<double> rest;
    for (int i = 0; i < runs; ++i) {
        if (const std::optional<double> t = timed(false))
            rest.push_back(*t);
    }
    std::sort(rest.begin(), rest.end());
    const auto show = [](const std::optional<double>& ms) { return ms ? QString::number(*ms, 'f', 1) + u" ms"_s : u"failed"_s; };
    std::printf("\ncold start (runner + plugin, to the first answer): %s\n", qUtf8Printable(show(cold)));
    std::printf("first round trip after it:                        %s\n", qUtf8Printable(show(first)));
    if (!rest.empty()) {
        std::printf("steady state (%zu): median %.2f ms  p95 %.2f ms  min %.2f ms  max %.2f ms\n", rest.size(), rest[rest.size() / 2],
            rest[std::min(rest.size() - 1, static_cast<std::size_t>(rest.size() * 95 / 100))], rest.front(), rest.back());
    }
    const ws::plugin::Host::State state = host.state(manifest->id);
    std::printf("plugin state: %d, pid %lu%s\n", static_cast<int>(state.status), static_cast<unsigned long>(state.pid),
        state.error.isEmpty() ? "" : qUtf8Printable(u" (%1)"_s.arg(state.error)));
    if (state.pid != 0) {
        // The process's token, read out (an elevated wsbench can open any):
        // the integrity level the sandbox gave it, whether it is elevated,
        // and whether it is in a job.
        const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, state.pid);
        HANDLE token = nullptr;
        if (process && ::OpenProcessToken(process, TOKEN_QUERY, &token)) {
            DWORD rid = 0;
            DWORD got = 0;
            alignas(8) char buffer[64];
            if (::GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof buffer, &got)) {
                const auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
                rid = *::GetSidSubAuthority(label->Label.Sid, *::GetSidSubAuthorityCount(label->Label.Sid) - 1);
            }
            TOKEN_ELEVATION elevation {};
            ::GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &got);
            BOOL inJob = FALSE;
            ::IsProcessInJob(process, nullptr, &inJob);
            std::printf("plugin process: integrity %s (%lu), elevated %s, in a job %s\n",
                rid >= 12288 ? "high" : rid >= 8192 ? "medium" : rid >= 4096 ? "LOW" : "untrusted", static_cast<unsigned long>(rid),
                elevation.TokenIsElevated ? "YES" : "no", inJob ? "yes" : "no");
            ::CloseHandle(token);
        } else
            std::printf("plugin process: cannot open its token (error %lu; run wsbench elevated to read it)\n", ::GetLastError());
        if (process)
            ::CloseHandle(process);
    }
    bool actionOk = true;
    if (!actionId.isEmpty()) {
        // The action on the first row, as Enter (or the row's menu) would send it.
        if (firstItems.isEmpty()) {
            std::printf("no item to run the action on\n");
            actionOk = false;
        } else {
            bool done = false;
            QElapsedTimer timer;
            timer.start();
            const QMetaObject::Connection finished = QObject::connect(&host, &ws::plugin::Host::actionDone,
                [&](std::uint64_t, const ws::plugin::Effects& effects, bool ok, const QString& error) {
                    done = true;
                    actionOk = ok;
                    std::printf("action %s on \"%s\": %s in %.1f ms\n", qUtf8Printable(actionId), qUtf8Printable(firstItems.first().title),
                        ok ? "done" : qUtf8Printable(u"failed: "_s + error), msSince(timer));
                    for (const ws::plugin::Effect& e : effects) {
                        const char* kind = e.kind == ws::plugin::Effect::Kind::Hide ? "hide" : e.kind == ws::plugin::Effect::Kind::Keep ? "keep"
                            : e.kind == ws::plugin::Effect::Kind::SetQuery                                                            ? "setQuery"
                            : e.kind == ws::plugin::Effect::Kind::Notify                                                              ? "notify"
                            : e.kind == ws::plugin::Effect::Kind::Copy                                                                ? "copy"
                            : e.kind == ws::plugin::Effect::Kind::Open                                                                ? "open"
                                                                                                                                      : "replace";
                        std::printf("  effect %s %s%s\n", kind, qUtf8Printable(e.text), qUtf8Printable(e.open.target));
                    }
                });
            const std::vector<ws::plugin::Match> matches = host.catalog().match(query);
            host.action(1, manifest->id, firstCommand, actionId, firstItems.first(), matches.empty() ? query : matches.front().text);
            const QDeadlineTimer deadline(15000);
            while (!done && !deadline.hasExpired())
                QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
            QObject::disconnect(finished);
            if (!done) {
                std::printf("action: no answer in 15 s\n");
                actionOk = false;
            }
        }
    }
    host.shutdown();
    return cold && actionOk ? 0 : 1;
}

// ---- the launcher's list, headless (--launcher-sim) ---------------------------

namespace {

// The list as plugin::Merge writes it (ResultModel in the app), with the
// keyboard on the first row, as it is once rows are shown.
struct SimList : ws::plugin::RowSink {
    ws::SearchResults rows;
    int count() const override { return static_cast<int>(rows.size()); }
    const ws::SearchResult& row(int index) const override { return rows[index]; }
    int currentRow() const override { return rows.isEmpty() ? -1 : 0; }
    void assign(ws::SearchResults r) override { rows = std::move(r); }
    void append(const ws::SearchResults& r) override { rows.append(r); }
    void insert(int index, const ws::SearchResult& r) override { rows.insert(index, r); }
    void update(int index, const ws::SearchResult& r) override { rows[index] = r; }
    void remove(int index) override { rows.removeAt(index); }
};

QString rowSource(const ws::SearchResult& r)
{
    if (r.isPlugin())
        return u"plugin:"_s + r.plugin->pluginId;
    if (r.isWeb())
        return u"web"_s;
    if (r.isPlace())
        return u"place"_s;
    if (r.isApp())
        return u"app"_s;
    if (r.line > 0)
        return u"content"_s;
    return r.isDir ? u"folder"_s : u"file"_s;
}

QString rowLine(int index, const ws::SearchResult& r)
{
    QString marks;
    if (r.recent)
        marks += u" recent"_s;
    if (r.exact)
        marks += u" exact"_s;
    const QString detail = r.isPlugin() ? r.plugin->subtitle : r.isApp() ? r.target : r.path;
    return u"%1  %2  %3  [%4]%5  score %6"_s.arg(index, 2).arg(rowSource(r), -16).arg(r.name, detail).arg(marks).arg(r.score);
}

} // namespace

// What Launcher::search does for each key, over a copy of the index and the
// test plugins: the name search and the plugins asked together, their rows
// merged as the launcher merges them. Prints the final list of each text.
int runLauncherSim(QStringList args)
{
    QString pluginsDir = QDir::cleanPath(QCoreApplication::applicationDirPath() + u"/../../tests/plugins"_s);
    QString indexFolder;
    int keyMs = 80;
    if (const qsizetype at = args.indexOf(u"--plugins"_s); at >= 0 && at + 1 < args.size()) {
        pluginsDir = args[at + 1];
        args.remove(at, 2);
    }
    if (const qsizetype at = args.indexOf(u"--index"_s); at >= 0 && at + 1 < args.size()) {
        indexFolder = args[at + 1];
        args.remove(at, 2);
    }
    if (const qsizetype at = args.indexOf(u"--key-ms"_s); at >= 0 && at + 1 < args.size()) {
        keyMs = std::max(0, args[at + 1].toInt());
        args.remove(at, 2);
    }
    ws::Settings settings;
    settings.load();
    // --web KEYWORD: a [WebSearch] entry for this run (the user's file is not
    // touched), to see a plugin's keyword set aside for it.
    for (qsizetype at = args.indexOf(u"--web"_s); at >= 0 && at + 1 < args.size(); at = args.indexOf(u"--web"_s)) {
        ws::WebShortcut shortcut;
        shortcut.keyword = args[at + 1];
        shortcut.name = u"Web "_s + args[at + 1];
        shortcut.url = u"https://example.com/search?q=%s"_s;
        settings.webShortcuts.append(shortcut);
        args.remove(at, 2);
    }
    QStringList texts = args;
    if (texts.isEmpty())
        texts = {u"kill note"_s, u"#ff8800"_s, u"report"_s, u"gh winshun"_s};
    QTemporaryDir temp;
    if (indexFolder.isEmpty()) {
        // The app's files, copied: it goes on writing its own.
        const QString from = QDir::fromNativeSeparators(settings.indexDir());
        indexFolder = temp.path() + u"/index"_s;
        QDir().mkpath(indexFolder + u"/content"_s);
        if (!QFile::copy(from + u"/index.bin"_s, indexFolder + u"/index.bin"_s)) {
            std::printf("no index at %s\n", qUtf8Printable(QDir::toNativeSeparators(from)));
            return 1;
        }
        for (const QFileInfo& file : QDir(from + u"/content"_s).entryInfoList(QDir::Files))
            QFile::copy(file.filePath(), indexFolder + u"/content/"_s + file.fileName());
    }

    // The plugins: copies of the test folders, each enabled, plugin_fake.exe
    // put in where a manifest asks for it (the app's bundled folder is read
    // in place; here nothing of the source tree is written to).
    const QString userPlugins = temp.path() + u"/plugins"_s;
    QDir().mkpath(userPlugins);
    const QString fake = QCoreApplication::applicationDirPath() + u"/plugin_fake.exe"_s;
    QStringList ids;
    for (const QFileInfo& folder : QDir(pluginsDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (!QFileInfo::exists(folder.filePath() + u"/plugin.json"_s))
            continue;
        const QString to = userPlugins + u'/' + folder.fileName();
        QDir().mkpath(to);
        for (const QFileInfo& file : QDir(folder.filePath()).entryInfoList(QDir::Files))
            QFile::copy(file.filePath(), to + u'/' + file.fileName());
        QString error;
        const std::optional<ws::plugin::Manifest> manifest = ws::plugin::Manifest::load(to + u"/plugin.json"_s, &error);
        if (!manifest) {
            std::printf("%s: %s\n", qUtf8Printable(folder.fileName()), qUtf8Printable(error));
            continue;
        }
        if (manifest->runtime && manifest->runtime->program == ws::plugin::Program::Exe
            && manifest->runtime->programPath.compare(u"plugin_fake.exe"_s, Qt::CaseInsensitive) == 0
            && !QFileInfo::exists(to + u"/plugin_fake.exe"_s))
            QFile::copy(fake, to + u"/plugin_fake.exe"_s);
        ids.append(manifest->id);
    }
    ws::plugin::Host::Options options;
    options.userPluginDir = userPlugins;
    options.dataRoot = temp.path() + u"/data"_s;
    options.logPath = temp.path() + u"/plugins.log"_s;
    options.runnerExe = QCoreApplication::applicationDirPath() + u"/WinShunPluginHost.exe"_s;
    options.version = QStringLiteral(WINSHUN_VERSION "-bench");
    ws::plugin::Host host(options);
    for (const QString& id : ids)
        host.setEnabled(id, true);
    QList<std::pair<QString, QString>> web;
    for (const ws::WebShortcut& w : settings.webShortcuts)
        web.append({w.keyword, w.name});
    host.setWebKeywords(web);
    std::printf("plugins: %s\n", qUtf8Printable(ids.join(u", "_s)));
    for (const ws::plugin::KeywordConflict& c : host.catalog().conflicts())
        std::printf("  keyword %s of %s/%s is set aside: taken by %s\n", qUtf8Printable(c.keyword), qUtf8Printable(c.pluginId),
            qUtf8Printable(c.commandId), qUtf8Printable(c.takenBy));
    QObject::connect(&host, &ws::plugin::Host::logged, [](const QString& line) { std::printf("  log: %s\n", qUtf8Printable(line)); });
    host.preload(); // as the app does ten seconds after starting

    // The index and the catalogs, as the app has them.
    ws::IndexService::Options indexOptions;
    indexOptions.rules = settings.crawlRules();
    indexOptions.rescanOnStartup = false;
    indexOptions.content.enabled = false;
    ws::IndexService service(indexFolder, indexOptions);
    ws::AppCatalog apps;
    ws::SystemCatalog places;
    ws::SearchEngine engine(&service, &apps, &places);
    service.start();
    apps.refresh(true);
    places.refresh(true);
    QElapsedTimer ready;
    ready.start();
    bool appsIn = false;
    bool placesIn = false;
    QObject::connect(&apps, &ws::AppCatalog::changed, [&] { appsIn = true; });
    QObject::connect(&places, &ws::SystemCatalog::changed, [&] { placesIn = true; });
    while ((service.state() != ws::IndexService::State::Ready || !appsIn || !placesIn) && ready.elapsed() < 60'000)
        QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
    std::printf("index %s (%zu items), apps %s, places %s, after %.0f ms\n\n",
        service.state() == ws::IndexService::State::Ready ? "ready" : "NOT READY", service.itemCount(), appsIn ? "in" : "NOT IN",
        placesIn ? "in" : "NOT IN", msSince(ready));

    // One key: the launcher's search() for `text`, waited for.
    SimList list;
    ws::plugin::Merge merge(list, host.catalog());
    std::uint64_t queryId = 0;
    quint64 requestId = 0;
    bool namesIn = false;
    QSet<QString> pending;
    double namesMs = -1;
    double pluginFirstMs = -1;
    double pluginDoneMs = -1;
    QElapsedTimer key;
    // The engine emits from its worker thread: with the host (this thread's)
    // as context the slot is queued here, as the launcher's is.
    QObject::connect(&engine, &ws::SearchEngine::resultsReady, &host,
        [&](quint64 id, const ws::SearchResults& results, qint64, qint64) {
            if (id != requestId)
                return;
            list.assign(results);
            namesIn = true;
            namesMs = msSince(key);
            merge.namesShown(); // a plugin that answered first: its rows show now
            if (pluginFirstMs < 0 && merge.pluginRows() > 0)
                pluginFirstMs = msSince(key);
        });
    QObject::connect(&host, &ws::plugin::Host::queryTaken, [&](std::uint64_t id, const QString& plugin) {
        if (id == queryId)
            pending.insert(plugin);
    });
    QObject::connect(&host, &ws::plugin::Host::results,
        [&](std::uint64_t id, const QString& plugin, const QString& command, const ws::plugin::Items& items, bool partial, bool replace) {
            if (id != queryId)
                return;
            const int before = merge.pluginRows();
            merge.results(id, plugin, command, items, partial, replace);
            // Keyword mode: the answer replaced the rows (as many as before, maybe).
            if (pluginFirstMs < 0 && (merge.keywordMode() || merge.pluginRows() > before))
                pluginFirstMs = msSince(key);
        });
    const auto settled = [&](std::uint64_t id, const QString& plugin) {
        if (id != queryId)
            return;
        pending.remove(plugin);
        if (pending.isEmpty())
            pluginDoneMs = msSince(key);
    };
    QObject::connect(&host, &ws::plugin::Host::queryFinished, settled);
    QObject::connect(&host, &ws::plugin::Host::queryFailed, [&](std::uint64_t id, const QString& plugin, const QString& why) {
        std::printf("  %s failed: %s\n", qUtf8Printable(plugin), qUtf8Printable(why));
        settled(id, plugin);
    });

    const auto press = [&](const QString& text, bool withPlugins) {
        const QString trimmed = text.trimmed();
        key.start();
        namesIn = false;
        pending.clear();
        namesMs = pluginFirstMs = pluginDoneMs = -1;
        QString keywordPlugin;
        QString keywordCommand;
        // As typed, as the launcher asks: the space after a keyword is what
        // opens its command (Catalog::match).
        if (withPlugins && !trimmed.isEmpty()) {
            for (const ws::plugin::Match& m : host.catalog().match(text)) {
                if (m.kind == ws::plugin::Match::Kind::Keyword) {
                    keywordPlugin = m.entry->id;
                    keywordCommand = m.command->id;
                    break;
                }
            }
        }
        merge.begin(++queryId, text, keywordPlugin, keywordCommand);
        if (keywordPlugin.isEmpty()) {
            ws::SearchEngine::Request request;
            request.text = text;
            request.scope = ws::Scope::All;
            request.kindOrder = settings.foldersFirst ? ws::KindOrder::FoldersFirst : ws::KindOrder::FilesFirst;
            request.web = settings.webShortcuts;
            requestId = engine.submit(std::move(request));
        } else {
            engine.cancel();
            requestId = 0;
            namesIn = true;
        }
        if (withPlugins)
            host.query(text, ws::Scope::All, queryId);
        else
            host.cancelAll();
        return !keywordPlugin.isEmpty();
    };
    const auto waitSettled = [&](int ms) {
        const QDeadlineTimer deadline(ms);
        while ((!namesIn || !pending.isEmpty()) && !deadline.hasExpired())
            QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 10);
        return namesIn && pending.isEmpty();
    };
    const auto printList = [&] {
        for (int i = 0; i < list.count(); ++i)
            std::printf("  %s\n", qUtf8Printable(rowLine(i, list.rows[i])));
        if (list.count() == 0)
            std::printf("  (no rows)\n");
    };

    int failures = 0;
    for (const QString& text : texts) {
        std::printf("== \"%s\"\n", qUtf8Printable(text));
        // Typed key by key: each key cancels the last query's plugins, as
        // in the launcher; only the last key is waited for and shown.
        bool keyword = false;
        for (qsizetype n = 1; n <= text.size(); ++n) {
            keyword = press(text.left(n), true);
            if (n < text.size()) {
                const QDeadlineTimer pause(keyMs);
                while (!pause.hasExpired())
                    QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 5);
            }
        }
        const bool done = waitSettled(8000);
        std::printf("%s; names %s ms, plugin rows after %s ms, plugins done after %s ms%s\n",
            keyword ? "keyword mode (no name search)" : "push mode", namesMs < 0 ? "-" : qUtf8Printable(QString::number(namesMs, 'f', 1)),
            pluginFirstMs < 0 ? "-" : qUtf8Printable(QString::number(pluginFirstMs, 'f', 1)),
            pluginDoneMs < 0 ? "-" : qUtf8Printable(QString::number(pluginDoneMs, 'f', 1)), done ? "" : "  (NOT SETTLED in 8 s)");
        printList();
        if (!done)
            ++failures;
        if (keyword || merge.pluginRows() > 0 || !host.catalog().match(text).empty() || host.catalog().hint(text))
            continue;
        // No plugin answered: the list must be the one without plugins, row for row.
        const ws::SearchResults withPlugins = list.rows;
        press(text, false);
        waitSettled(8000);
        bool same = withPlugins.size() == list.rows.size();
        for (qsizetype i = 0; same && i < withPlugins.size(); ++i)
            same = withPlugins[i] == list.rows[i];
        std::printf("without plugins: %lld rows, %s\n", static_cast<long long>(list.rows.size()), same ? "the same rows" : "DIFFERENT");
        if (!same) {
            ++failures;
            printList();
        }
    }
    host.shutdown();
    service.shutdown();
    return failures == 0 ? 0 : 1;
}

// ---- the MCP server, headless (--mcp-host) ----------------------------------

// Win顺's MCP tools on a pipe, as the app serves them, over a copy of the
// index: for trying real AI clients (and WinShunMcp.exe) while the app
// itself runs. No shell and no clipboard here, so their tools are not
// offered. Each call is printed.
int runMcpHost(QStringList args)
{
    std::wstring pipeName = ws::pipe::defaultName() + L".bench";
    if (const qsizetype at = args.indexOf(u"--pipe"_s); at >= 0 && at + 1 < args.size()) {
        pipeName = args[at + 1].toStdWString();
        args.remove(at, 2);
    }
    QString indexFolder;
    if (const qsizetype at = args.indexOf(u"--index"_s); at >= 0 && at + 1 < args.size()) {
        indexFolder = args[at + 1];
        args.remove(at, 2);
    }
    QString pluginsDir;
    if (const qsizetype at = args.indexOf(u"--plugins"_s); at >= 0 && at + 1 < args.size()) {
        pluginsDir = QDir(args[at + 1]).absolutePath();
        args.remove(at, 2);
    }
    ws::Settings settings;
    settings.load();
    QTemporaryDir temp;
    if (indexFolder.isEmpty()) {
        // The app's files, copied: it goes on writing its own, and this
        // service saves its snapshot into the copy on exit.
        const QString from = QDir::fromNativeSeparators(settings.indexDir());
        indexFolder = temp.path();
        QDir().mkpath(indexFolder + u"/content"_s);
        QElapsedTimer copyTimer;
        copyTimer.start();
        if (!QFile::copy(from + u"/index.bin"_s, indexFolder + u"/index.bin"_s)) {
            std::printf("no index at %s\n", qUtf8Printable(QDir::toNativeSeparators(from)));
            return 1;
        }
        for (const QFileInfo& file : QDir(from + u"/content"_s).entryInfoList(QDir::Files))
            QFile::copy(file.filePath(), indexFolder + u"/content/"_s + file.fileName());
        std::printf("index copied from %s in %.0f ms\n", qUtf8Printable(QDir::toNativeSeparators(from)), msSince(copyTimer));
    }

    ws::IndexService::Options options;
    options.rules = settings.crawlRules();
    options.rescanOnStartup = false;
    options.content.enabled = settings.contentIndex;
    options.content.extensions = settings.contentExtensions;
    options.content.documents = settings.contentDocuments;
    options.content.sizeLimits = settings.contentSizeLimits();
    ws::IndexService service(indexFolder, options);
    ws::SearchEngine engine(&service);
    ws::mcp::Services services;
    services.index = &service;
    services.engine = &engine;
    services.version = QStringLiteral(WINSHUN_VERSION "-bench");
    services.log = [](const QString& line) {
        std::printf("%s\n", qUtf8Printable(line));
        std::fflush(stdout);
    };
    ws::mcp::ToolHost host(std::move(services));
    ws::mcp::Policy policy;
    policy.contentExtensions = settings.contentExtensions;
    policy.contentSizeLimits = settings.contentSizeLimits();
    policy.contentDocuments = settings.contentDocuments;
    policy.contentInLowPriority = settings.contentInLowPriority;
    policy.kindOrder = settings.foldersFirst ? ws::KindOrder::FoldersFirst : ws::KindOrder::FilesFirst;
    policy.excludedPaths = settings.mcpExcludedPaths;
    host.setPolicy(policy);
    // The plugins of --plugins as tools: every one enabled, read in place
    // (as the app reads its bundled folder), their data in the temporary folder.
    std::unique_ptr<ws::plugin::Host> plugins;
    std::unique_ptr<ws::plugin::AiBridge> bridge;
    if (!pluginsDir.isEmpty()) {
        ws::plugin::Host::Options pluginOptions;
        pluginOptions.userPluginDir = temp.path() + u"/plugins"_s;
        pluginOptions.bundledPluginDir = pluginsDir;
        pluginOptions.dataRoot = temp.path() + u"/data"_s;
        pluginOptions.logPath = temp.path() + u"/plugins.log"_s;
        pluginOptions.runnerExe = QCoreApplication::applicationDirPath() + u"/WinShunPluginHost.exe"_s;
        pluginOptions.version = QStringLiteral(WINSHUN_VERSION "-bench");
        plugins = std::make_unique<ws::plugin::Host>(pluginOptions);
        QObject::connect(plugins.get(), &ws::plugin::Host::logged, [](const QString& line) { std::printf("  plugins: %s\n", qUtf8Printable(line)); });
        QStringList ids;
        for (const ws::plugin::Entry& e : plugins->catalog().entries()) {
            if (e.manifest) {
                plugins->setEnabled(e.id, true);
                ids.push_back(e.id);
            } else
                std::printf("plugin %s: %s\n", qUtf8Printable(e.id), qUtf8Printable(e.error));
        }
        bridge = std::make_unique<ws::plugin::AiBridge>(plugins.get(), &host);
        ws::plugin::AiBridge::Options bridgeOptions;
        bridgeOptions.enabled = true;
        bridgeOptions.actions = true;
        bridge->setOptions(bridgeOptions);
        std::printf("plugins: %s\n", qUtf8Printable(ids.join(u", "_s)));
    }
    ws::mcp::McpServer server(&host, QStringLiteral(WINSHUN_VERSION));
    DWORD error = 0;
    if (!server.listen(pipeName, &error)) {
        std::printf("cannot serve on %ls (error %lu)\n", pipeName.c_str(), error);
        return 1;
    }
    QObject::connect(&server, &ws::mcp::McpServer::sessionsChanged, [&] {
        QStringList clients;
        for (const ws::mcp::McpServer::SessionInfo& s : server.sessions())
            clients.push_back(s.client.isEmpty() ? u"(connecting)"_s : u"%1 (%2 calls)"_s.arg(s.client).arg(s.calls));
        std::printf("sessions: %s\n", clients.isEmpty() ? "none" : qUtf8Printable(clients.join(u", "_s)));
        std::fflush(stdout);
    });
    QTimer status;
    status.setInterval(1000);
    bool wasReady = false;
    QObject::connect(&status, &QTimer::timeout, [&] {
        const bool ready = service.state() == ws::IndexService::State::Ready;
        if (ready != wasReady) {
            wasReady = ready;
            std::printf("index %s: %zu items\n", ready ? "ready" : "not ready", service.itemCount());
            std::fflush(stdout);
        }
    });
    service.start();
    status.start();
    std::printf("serving MCP on \\\\.\\pipe\\%ls\n"
                "connect a client with: WinShunMcp.exe --pipe %ls\n"
                "tools: %s\nCtrl+C stops\n",
        pipeName.c_str(), pipeName.c_str(), qUtf8Printable(host.toolNames().join(u", "_s)));
    std::fflush(stdout);
    QCoreApplication::exec();
    server.stop();
    bridge.reset();
    if (plugins)
        plugins->shutdown();
    service.shutdown();
    return 0;
}

// ---- sites' icons (--site-icon) -----------------------------------------------

// Fetches each home page's icon as Win顺 does (siteicons::fetchNow): which
// addresses were asked, which won. With --save <folder>: <site>.png as it
// would be kept, and <site>-<px>[-dark].png as a row draws it at 32, 48 and
// 64 px in the light and the dark theme.
int runSiteIcon(QStringList args)
{
    QString folder;
    if (const qsizetype at = args.indexOf(u"--save"_s); at >= 0 && at + 1 < args.size()) {
        folder = args[at + 1];
        args.remove(at, 2);
        QDir().mkpath(folder);
    }
    if (args.isEmpty()) {
        std::printf("wsbench --site-icon <home page>... [--save <folder>]\n");
        return 1;
    }
    for (const QString& home : std::as_const(args)) {
        QElapsedTimer t;
        t.start();
        const ws::siteicons::Fetched f = ws::siteicons::fetchNow(home);
        const QString key = ws::siteIconKey(home);
        std::printf("%ls  (%.0f ms)\n", qUtf16Printable(key), msSince(t));
        for (const QString& line : f.tried)
            std::printf("    %ls\n", qUtf16Printable(line));
        if (f.image.isNull()) {
            std::printf("    => none\n");
            continue;
        }
        std::printf("    => %ls  %dx%d, kept %dx%d%s\n", qUtf16Printable(f.from), f.original.width(),
            f.original.height(), f.image.width(), f.image.height(), ws::siteicon::isTile(f.image) ? ", a tile" : "");
        if (!folder.isEmpty()) {
            f.image.save(folder + u'/' + key + u".png"_s, "PNG");
            for (int px : {32, 48, 64}) {
                for (bool dark : {false, true})
                    ws::siteicon::drawn(f.image, px, dark)
                        .save(folder + u'/' + key + u'-' + QString::number(px) + (dark ? u"-dark"_s : u""_s) + u".png"_s, "PNG");
            }
        }
    }
    return 0;
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    std::setlocale(LC_CTYPE, ".UTF-8"); // %ls of names in any language
    QStringList args = app.arguments().mid(1);
    if (const qsizetype at = args.indexOf(u"--out"_s); at >= 0 && at + 1 < args.size()) {
        // For runs started elevated, whose console cannot be redirected.
        FILE* reopened = nullptr;
        _wfreopen_s(&reopened, reinterpret_cast<const wchar_t*>(args[at + 1].utf16()), L"w", stdout);
        std::setvbuf(stdout, nullptr, _IONBF, 0); // keep what was written if it crashes
        args.remove(at, 2);
    }
    if (args.contains(u"--site-icon"_s)) {
        args.removeAll(u"--site-icon"_s);
        return runSiteIcon(args);
    }
    if (args.contains(u"--mft-time"_s)) {
        args.removeAll(u"--mft-time"_s);
        return runMftTiming(args, 3);
    }
    if (args.contains(u"--mft"_s)) {
        const bool compare = !args.contains(u"--no-compare"_s);
        ws::ntfs::ReadOptions options;
        options.skipFree = !args.contains(u"--no-skip"_s);
        if (const qsizetype at = args.indexOf(u"--depth"_s); at >= 0 && at + 1 < args.size()) {
            options.readsInFlight = args[at + 1].toInt();
            args.remove(at, 2);
        }
        for (const QString& flag : {u"--mft"_s, u"--no-compare"_s, u"--no-skip"_s})
            args.removeAll(flag);
        return runMftCheck(compare, args, options); // the volumes named ("D:"), or all
    }
    if (args.contains(u"--share"_s)) {
        args.removeAll(u"--share"_s);
        return runShare(args);
    }
    if (args.contains(u"--places"_s)) {
        args.removeAll(u"--places"_s);
        return runPlaces(args);
    }
    if (args.contains(u"--mcp-host"_s)) {
        args.removeAll(u"--mcp-host"_s);
        return runMcpHost(args);
    }
    if (args.contains(u"--launcher-sim"_s)) {
        args.removeAll(u"--launcher-sim"_s);
        return runLauncherSim(args);
    }
    if (args.contains(u"--plugin"_s))
        return runPlugin(args);
    if (args.contains(u"--extract"_s)) {
        QStringList paths = args;
        if (const qsizetype at = paths.indexOf(u"--threads"_s); at >= 0)
            paths.remove(at, std::min<qsizetype>(2, paths.size() - at));
        paths.removeIf([](const QString& a) { return a.startsWith(u"--"_s); });
        if (!paths.isEmpty() && !paths.first().endsWith(u".bin"_s))
            return runExtractBench(nullptr, args); // files or folders given: no snapshot needed
    }
    if (args.contains(u"--service"_s)) {
        args.removeAll(u"--service"_s);
        if (args.removeAll(u"--documents"_s) > 0)
            return runServiceDocuments(args);
        if (args.removeAll(u"--content"_s) > 0)
            return runServiceContent(args);
        return runService(args);
    }
    QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/WinShun/index.bin"_s;
    if (!args.isEmpty() && args.first().endsWith(u".bin"))
        path = args.takeFirst();
    if (args.isEmpty())
        args = {u"report"_s, u"a"_s, u"readme.md"_s, u"*.pdf"_s, u"ext:docx 合同"_s, u"src\\main"_s, u"zzzzqx"_s};

    const double before = privateMB();
    QElapsedTimer timer;
    timer.start();
    auto contents = ws::snapshot::load(path);
    ws::FileIndex* index = contents ? contents->index.get() : nullptr;
    if (!index) {
        std::printf("could not load %s\n", qPrintable(path));
        return 1;
    }
    const double loadMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    const double after = privateMB();
    const std::size_t chunks = index->chunkCount();
    std::printf("entries      %zu (slots %zu)\n", index->liveCount(), index->slotCount());
    std::printf("load         %.0f ms\n", loadMs);
    std::printf("memory       %.1f MB private (entries %.1f MB in %zu chunks, %.1f bytes/item overall)\n",
        after - before, static_cast<double>(chunks * ws::FileIndex::kChunkSize * sizeof(ws::Entry)) / (1024.0 * 1024.0),
        chunks, (after - before) * 1024.0 * 1024.0 / static_cast<double>(index->liveCount()));
    printHeap("after load");
    {
        const ws::FileIndex::MemoryUse use = index->memoryUse();
        const auto mb = [](std::size_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
        std::printf("index parts  entries %.1f, times %.1f, names %.1f (%.1f used), folder records %.1f, child tables %.1f, "
                    "intern %.1f MB\n",
            mb(use.entries), mb(use.times), mb(use.names), mb(use.nameBytes), mb(use.folderRecords), mb(use.childTables),
            mb(use.intern));
    }
    if (args.removeAll(u"--newest"_s) > 0) {
        // What the launcher lists with nothing typed and 最近修改 chosen: the
        // first 30 so, and how long finding the 5 takes.
        ws::WorkerPool pool(std::max(2u, std::thread::hardware_concurrency()));
        const auto now = static_cast<std::int64_t>(QDateTime::currentSecsSinceEpoch());
        const auto show = [&](const char* title, bool folders, std::size_t limit) {
            const ws::NewestOptions options {
                .limit = limit, .folders = folders, .notAfter = static_cast<ws::FileTime>(now + 86'400)};
            const ws::NewestOutput out = ws::newestFiles(*index, options, pool, {});
            std::printf("\n%s:\n", title);
            for (const ws::NameHit& hit : out.hits) {
                const QDateTime when = QDateTime::fromSecsSinceEpoch(hit.modified);
                std::printf("  %s  %s\n", when.toString(u"MM-dd HH:mm:ss"_s).toUtf8().constData(),
                    index->path(hit.id).toUtf8().constData());
            }
        };
        if (args.removeAll(u"--sim"_s) > 0) {
            // Typical moments, added to the real index in memory (nothing on
            // disk): what the 5 are after each, the ones before still there.
            const auto t = [&](std::int64_t ago) { return static_cast<ws::FileTime>(now - ago); };
            const auto add = [&](ws::EntryId parent, const std::string& name, bool dir, ws::FileTime when) {
                const ws::EntryId id = index->add(parent, name, dir ? ws::EntryFlag::Directory : 0);
                index->setModified(id, when);
                return id;
            };
            const ws::EntryId docs = index->findPath(L"C:\\Users\\young\\Documents");
            const ws::EntryId desktop = index->findPath(L"C:\\Users\\young\\Desktop");
            const ws::EntryId drive = index->findPath(L"F:\\");
            if (docs == ws::kNoEntry || desktop == ws::kNoEntry || drive == ws::kNoEntry) {
                std::printf("folders for the simulation not found\n");
                return 1;
            }
            const ws::EntryId sim = add(drive, "wsnewsim", true, t(5000));
            show("as it is", false, 5);

            add(docs, "报价单-v3.docx", false, t(600));
            add(desktop, "截图 2026-10-10.png", false, t(500));
            show("+ a document saved 10 min ago, a screenshot 8 min ago", false, 5);

            // git clone: a working tree, every file written now (.git is a dot-folder).
            const ws::EntryId repo = add(sim, "some-repo", true, t(120));
            add(repo, ".git", true, t(120));
            add(repo, "README.md", false, t(120));
            add(repo, "CMakeLists.txt", false, t(120));
            for (int d = 0; d < 12; ++d) {
                const ws::EntryId dir = add(repo, "module" + std::to_string(d), true, t(120));
                for (int f = 0; f < 40; ++f)
                    add(dir, "file" + std::to_string(f) + ".cpp", false, t(120 - (d * 40 + f) / 100));
            }
            show("+ git clone of 480 files 2 min ago", false, 5);

            // An archive unpacked by a tool that sets the times to now, deep.
            const ws::EntryId pack = add(sim, "大包解压", true, t(60));
            for (int a = 0; a < 10; ++a) {
                const ws::EntryId l1 = add(pack, "资料" + std::to_string(a), true, t(60));
                for (int b = 0; b < 10; ++b) {
                    const ws::EntryId l2 = add(l1, "第" + std::to_string(b) + "章", true, t(60));
                    for (int f = 0; f < 20; ++f)
                        add(l2, "图" + std::to_string(f) + ".jpg", false, t(60 - (a * 200 + b * 20 + f) / 100));
                }
            }
            show("+ 2000 pictures unpacked 1 min ago", false, 5);

            add(docs, "报价单-v4.docx", false, t(10));
            show("+ the document saved again 10 s ago", false, 5);
            show("folders now", true, 5);
            return 0;
        }
        show("newest files", false, 30);
        show("newest folders", true, 30);
        QElapsedTimer t;
        double best = 1e9;
        for (int run = 0; run < 10; ++run) {
            t.start();
            const ws::NewestOutput out = ws::newestFiles(*index, {.limit = 5}, pool, {});
            best = std::min(best, static_cast<double>(t.nsecsElapsed()) / 1e6);
        }
        std::printf("\nnewest 5 files: %.1f ms (best of 10)\n", best);
        // The list to scroll: a page of 256, 4 and 16 of them (the launcher's
        // kPage, kMaxPages; SearchEngine's caps per folder and tree).
        for (const std::size_t limit : {std::size_t {256}, std::size_t {1024}, std::size_t {4096}}) {
            best = 1e9;
            std::size_t listed = 0;
            std::uint64_t hash = 14695981039346656037ull; // of the ids in order: the same list or not
            for (int run = 0; run < 10; ++run) {
                t.start();
                const ws::NewestOutput out
                    = ws::newestFiles(*index, {.limit = limit, .perFolder = 10, .perTree = 30}, pool, {});
                best = std::min(best, static_cast<double>(t.nsecsElapsed()) / 1e6);
                listed = out.hits.size();
                hash = 14695981039346656037ull;
                for (const ws::NameHit& hit : out.hits)
                    hash = (hash ^ hit.id) * 1099511628211ull;
            }
            std::printf("newest %zu files: %zu listed (%016llx), %.1f ms (best of 10)\n", limit, listed,
                static_cast<unsigned long long>(hash), best);
        }
        return 0;
    }
    if (args.removeAll(u"--stats"_s) > 0) {
        // What the entries are like: how many distinct names, folders, deep.
        std::size_t dirs = 0, han = 0, low = 0, nameBytes = 0, maxDepth = 0, depthSum = 0;
        std::vector<std::uint32_t> offsets;
        offsets.reserve(index->slotCount());
        for (std::size_t i = 0; i < index->slotCount(); ++i) {
            const ws::Entry& e = index->entry(static_cast<ws::EntryId>(i));
            if (e.isDeleted())
                continue;
            dirs += e.isDir();
            han += (e.flags & ws::EntryFlag::Han) != 0;
            low += (e.flags & ws::EntryFlag::LowPriority) != 0;
            nameBytes += index->name(e).size();
            offsets.push_back(e.nameOffset);
            const auto d = static_cast<std::size_t>(index->depth(static_cast<ws::EntryId>(i)));
            depthSum += d;
            maxDepth = std::max(maxDepth, d);
        }
        std::sort(offsets.begin(), offsets.end());
        const std::size_t distinct
            = static_cast<std::size_t>(std::unique(offsets.begin(), offsets.end()) - offsets.begin());
        const std::size_t live = index->liveCount();
        std::printf("stats        %zu folders, %zu han, %zu low priority; names %.1f MB over all entries (%.1f B avg), "
                    "%zu distinct offsets; depth avg %.1f max %zu\n",
            dirs, han, low, static_cast<double>(nameBytes) / (1024.0 * 1024.0),
            static_cast<double>(nameBytes) / static_cast<double>(live), distinct,
            static_cast<double>(depthSum) / static_cast<double>(live), maxDepth);
    }

    if (args.contains(u"--content"_s))
        return runContentBench(*index, args);
    if (args.contains(u"--content-index"_s))
        return runContentIndexBench(*index, args);
    if (args.contains(u"--content-search"_s))
        return runContentSearchBench(*index, contents->attachment, QFileInfo(path).absolutePath() + u"/content"_s, args);
    if (const qsizetype at = args.indexOf(u"--resave"_s); at >= 0 && at + 1 < args.size()) {
        // Saved again as the app saves it (packed), and loaded from there.
        const QString out = args[at + 1];
        const auto lock = index->readLock();
        timer.restart();
        const bool saved = ws::snapshot::save(*index, contents->volumes, contents->journals,
            contents->rules.value_or(ws::CrawlRules {}), out,
            [&](const std::vector<ws::EntryId>&) { return contents->attachment; });
        const double saveMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        timer.restart();
        const auto again = ws::snapshot::load(out);
        const double againMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        std::printf("resave       %s: %.1f MB (was %.1f MB), saved in %.0f ms, loaded in %.0f ms, %zu entries\n",
            saved && again ? "ok" : "FAILED", static_cast<double>(QFileInfo(out).size()) / (1024.0 * 1024.0),
            static_cast<double>(QFileInfo(path).size()) / (1024.0 * 1024.0), saveMs, againMs,
            again ? again->index->liveCount() : 0);
        return saved && again && again->index->liveCount() == index->liveCount() ? 0 : 1;
    }
    if (args.contains(u"--extract"_s))
        return runExtractBench(index, args);

    if (args.contains(u"--sync"_s)) {
        // Re-walk all drives in place, as the app does after startup.
        args.removeAll(u"--sync"_s);
        // The app's rules (from the user's settings), so the walk matches the app's startup rescan.
        ws::Settings settings;
        settings.load();
        const ws::CrawlRules rules = settings.crawlRules();
        std::vector<ws::Crawler::Root> roots;
        for (const ws::EntryId r : index->roots()) {
            const std::string_view name = index->name(r);
            roots.push_back({r, std::wstring(name.begin(), name.end()), 0});
        }
        // Run twice: the first pass picks up changes since the snapshot was
        // saved; the second shows the cost of a no-change sync.
        for (int pass = 1; pass <= 2; ++pass) {
            const std::size_t slotsBefore = index->slotCount();
            timer.restart();
            index->setInterning(true);
            ws::Crawler(rules).sync(*index, roots, ws::WorkerPool::defaultThreadCount(), true, {});
            index->setInterning(false);
            const double syncMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
            ::HeapCompact(::GetProcessHeap(), 0);
            std::printf("sync pass %d  %.0f ms, slots %zu -> %zu, live %zu, private %.1f MB\n", pass, syncMs,
                slotsBefore, index->slotCount(), index->liveCount(), privateMB() - before);
            printHeap("after sync");
        }
    }

    if (args.contains(u"--compact"_s)) {
        // Remove every 8th file (as if deleted from disk), then compact.
        args.removeAll(u"--compact"_s);
        std::size_t removed = 0;
        for (std::size_t i = 0; i < index->slotCount(); i += 8) {
            const auto id = static_cast<ws::EntryId>(i);
            if (!index->entry(id).isDir() && !index->entry(id).isDeleted())
                removed += index->remove(id);
        }
        ::HeapCompact(::GetProcessHeap(), 0);
        const double withGarbage = privateMB() - before;
        timer.restart();
        const std::size_t freed = index->compact();
        const double compactMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        ::HeapCompact(::GetProcessHeap(), 0);
        std::printf("compact      removed %zu files, freed %zu slots in %.0f ms, private %.1f -> %.1f MB\n", removed,
            freed, compactMs, withGarbage, privateMB() - before);
        printHeap("after compact");
    }

    const bool showTop = args.removeAll(u"--top"_s) > 0; // list the first hits of each query
    const bool check = args.removeAll(u"--check"_s) > 0; // compare with matching every entry
    int failures = 0;
    ws::WorkerPool pool(ws::WorkerPool::defaultThreadCount());
    const auto lock = index->readLock();
    for (const QString& q : args) {
        const ws::NameMatcher matcher(ws::parseQuery(q));
        double best = 1e9;
        ws::NameSearchOutput out;
        for (int run = 0; run < 5; ++run) {
            timer.restart();
            out = ws::searchNames(*index, matcher, {.limit = 100}, pool, {});
            best = std::min(best, static_cast<double>(timer.nsecsElapsed()) / 1e6);
        }
        std::printf("%-16s %9zu matches  %6.1f ms   top: %s\n", qPrintable(q), out.totalMatches, best,
            out.hits.empty() ? "-" : qPrintable(index->path(out.hits.front().id)));
        if (check) {
            // Every entry through the matcher, as before names were tested first: the same matches?
            std::size_t brute = 0;
            ws::NameMatcher::Folders seen;
            for (std::size_t i = 0; i < index->slotCount(); ++i) {
                const ws::Entry& e = index->entry(static_cast<ws::EntryId>(i));
                if (!(e.flags & (ws::EntryFlag::Deleted | ws::EntryFlag::Root)) && matcher.match(*index, e, seen) >= 0)
                    ++brute;
            }
            std::printf("  check: %zu matches by every entry -> %s\n", brute, brute == out.totalMatches ? "same" : "DIFFERENT");
            failures += brute != out.totalMatches;
        }
        for (std::size_t i = 0; showTop && i < out.hits.size() && i < 10; ++i)
            std::printf("      %4d  %s\n", out.hits[i].score, index->path(out.hits[i].id).toUtf8().constData());
        // A path from some folder along the way: where it leads (SearchEngine puts that first).
        if (const ws::pathtext::RelativePath typed = ws::pathtext::splitRelative(q); !typed.names.isEmpty()) {
            std::vector<std::string> names;
            for (const QString& name : typed.names)
                names.push_back(ws::wtf8::fromUtf16(ws::wtf8::view(name)));
            double bestPath = 1e9;
            std::vector<ws::EntryId> led;
            for (int run = 0; run < 5; ++run) {
                timer.restart();
                led = ws::findRelativePath(*index, names, typed.folder, 200, pool, {});
                bestPath = std::min(bestPath, static_cast<double>(timer.nsecsElapsed()) / 1e6);
            }
            std::printf("  leads to %zu place(s) in %.1f ms\n", led.size(), bestPath);
            for (std::size_t i = 0; i < led.size() && i < 10; ++i)
                std::printf("      %s\n", index->path(led[i]).toUtf8().constData());
        }
    }
    return failures == 0 ? 0 : 1;
}
