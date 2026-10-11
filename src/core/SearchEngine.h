#pragma once

#include "ContentIndex.h"
#include "Favorites.h"
#include "SearchTypes.h"
#include "WebShortcut.h"
#include "WorkerPool.h"

#include <QObject>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace ws {

class AppCatalog;
class DocExtractor;
class IndexService;
class RecentOnDisk;
class SystemCatalog;

// Runs searches on a worker thread. Only the newest request matters: a new
// submit() cancels whatever is running, so typing never queues up work.
// Name searches also go through the installed apps, the places in Windows
// (`apps` and `places` may be null) and the web shortcuts.
// Signals are emitted from the worker thread (queued to the receivers).
// Whether content match `a` goes before `b`: the better match, or by when
// last written (a time not known last either way, the better match on a tie).
bool rankedBefore(const SearchResult& a, const SearchResult& b, RankBy by) noexcept;

// What a content scan passed over, told once it has looked at every
// candidate (SearchEngine::contentLeftOut): the launcher offers each part
// after the last row, to be searched this once (Request::contentSet).
struct ContentLeftOut {
    QStringList large; // over the size limit, by path (the content index knew some, the rest were opened)
    int lowPriority = 0; // of a listed type, in system, program and tool folders (not asked for: contentInLowPriority)
    int otherTypes = 0; // text of a kind not listed (OtherTextTypes)
    QStringList otherExtensions; // ... the most numerous few of those kinds, "vue", "kt"
    friend bool operator==(const ContentLeftOut&, const ContentLeftOut&) = default;
};

// A removable drive (a USB stick, a memory card) with media in it that the
// index leaves out (Settings: includeRemovableDrives), as a name search
// found it (SearchEngine::removableDrives). The launcher offers to walk it
// for the query (Request::walkDrive).
struct RemovableDrive {
    QString root; // "E:"
    QString label; // its volume label; may be empty
    friend bool operator==(const RemovableDrive&, const RemovableDrive&) = default;
};
using RemovableDrives = QList<RemovableDrive>;

class SearchEngine : public QObject {
    Q_OBJECT

public:
    // Which files a content search looks in (its candidates): those the
    // settings say, or, asked for after a scan said how many it passed over
    // (ContentLeftOut), one of those parts alone. Those are read this once:
    // the content index is neither asked about them nor given what is read
    // (its own pass keeps to the settings and would drop them again; and a
    // file over the limit has nearly every gram, so it would be read for
    // nearly every phrase).
    enum class ContentSet : std::uint8_t {
        Listed, // the extensions set, and documents if asked; not system folders unless asked
        LowPriority, // only the files of listed types in system, program and tool folders
        OtherTypes, // only the files of text types not listed (OtherTextTypes), where the scan looked
        Given, // only the files named in contentOnly, as they are now
    };

    struct Request {
        QString text;
        Scope scope = Scope::All;
        // 文件 with a type chosen (图片…): only files with one of these
        // extensions (without the dot, any case), typed or not; with ext:
        // typed too, those of both. Empty: any file or folder.
        QStringList extensions;
        // Set: not a name search but a walk of this drive ("E:", one the
        // index leaves out: RemovableDrive), on the disk, for the files and
        // folders named as `text` says (scope, extensions and rankBy as for
        // names). The rows come in batches (driveResults), with how far it is
        // (driveProgress); a new request stops it, as any search.
        QString walkDrive;
        KindOrder kindOrder = KindOrder::FilesFirst; // 全部, 文件
        // Files and folders, and content matches (全部: its apps, places and
        // web shortcuts stay first, by match).
        RankBy rankBy = RankBy::Match;
        StartList startList = StartList::Opened; // nothing typed: what was opened, or written, last
        // Names; with nothing typed, the files written last. Asked again with
        // a higher one (the launcher, as its user scrolls down), the list
        // starts as the shorter one did, more after it; so do content matches.
        int limit = 100;
        QStringList history; // newest first
        QStringList contentExtensions; // e.g. {"txt"}
        ContentSizeLimits contentSizeLimits; // by kind of file
        bool contentDocuments = true; // and Word, Excel, PowerPoint, PDF (documentExtensions())
        int maxContentResults = 300;
        // Also inside system, program and tool folders (EntryFlag::LowPriority).
        bool contentInLowPriority = false;
        // Files read at once (each open waits for the antivirus, so more
        // than processors). Asked for (内容): many; 全部's own: a few.
        int contentThreads = 4;
        ContentSet contentSet = ContentSet::Listed;
        // ContentSet::Given: the files, by path (the ones a scan before left
        // out for being over the size limit, ContentLeftOut::large, asked for
        // after all). The limits given still apply: the caller raises them.
        QStringList contentOnly;
        // 全部: a keyword typed first puts its page or search on top; names find them too.
        WebShortcuts web;
        // Listed first with nothing typed, the first few that match first
        // when typed, a favorite's keyword alone on top (after a web one).
        FavoriteList favorites;
    };

    explicit SearchEngine(
        IndexService* index, AppCatalog* apps = nullptr, SystemCatalog* places = nullptr, QObject* parent = nullptr);
    ~SearchEngine() override;

    quint64 submit(Request request);
    void cancel();

signals:
    // Name search, or the (empty) start of a content search.
    void resultsReady(quint64 requestId, const ws::SearchResults& results, qint64 totalMatches, qint64 elapsedUs);
    // Content search delivers matches incrementally.
    void contentResults(quint64 requestId, const ws::SearchResults& results);
    void contentProgress(quint64 requestId, int scanned, int total, bool finished);
    // Once a content scan of ContentSet::Listed has looked at every candidate
    // (not when it stopped at maxContentResults): what it passed over, before
    // the contentProgress that says it finished. The launcher offers each
    // part (Request::contentSet).
    void contentLeftOut(quint64 requestId, const ws::ContentLeftOut& leftOut);
    // After a name search's resultsReady, when something was typed and the
    // list is all there is (fewer than Request::limit): the removable drives
    // the index leaves out (none: an empty list). The launcher offers to
    // walk one (Request::walkDrive).
    void removableDrives(quint64 requestId, const ws::RemovableDrives& drives);
    // A walk (Request::walkDrive): its rows as they are found, best of each
    // batch first, and how many files and folders it has looked at.
    void driveResults(quint64 requestId, const ws::SearchResults& results);
    void driveProgress(quint64 requestId, int seen, bool finished);

private:
    struct Job {
        quint64 id = 0;
        Request request;
        std::chrono::steady_clock::time_point submitted;
    };

    void workerLoop(std::stop_token stop);
    void runNameSearch(const Job& job);
    void runContentSearch(const Job& job);
    void runDriveWalk(const Job& job);
    RemovableDrives unindexedRemovableDrives(); // worker thread: as of a moment ago (m_removable)
    bool isStale(quint64 id) const noexcept { return m_latest.load(std::memory_order_relaxed) != id; }

    IndexService* m_index;
    std::unique_ptr<DocExtractor> m_documents; // reads documents the content index has no text of
    AppCatalog* m_apps;
    SystemCatalog* m_places;
    std::unique_ptr<RecentOnDisk> m_onDisk; // whether recent items are still there
    // The removable drives as last looked at (unindexedRemovableDrives), and
    // when: asked of Windows every few seconds at most (the volume label is
    // read from the drive), worker thread only.
    RemovableDrives m_removable;
    std::chrono::steady_clock::time_point m_removableAt {};
    WorkerPool m_pool {WorkerPool::defaultThreadCount()};
    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::optional<Job> m_pending;
    quint64 m_counter = 0;
    std::atomic<quint64> m_latest {0};
    std::jthread m_worker; // last: started after everything above exists
};

} // namespace ws

Q_DECLARE_METATYPE(ws::ContentLeftOut)
Q_DECLARE_METATYPE(ws::RemovableDrive)
