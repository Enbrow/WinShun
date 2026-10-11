#include "SearchEngine.h"

#include "AppCatalog.h"
#include "ContentIndex.h"
#include "ContentScanner.h"
#include "DocExtractor.h"
#include "Documents.h"
#include "IndexService.h"
#include "NameSearch.h"
#include "PathText.h"
#include "Query.h"
#include "SystemCatalog.h"
#include "ShareLister.h"
#include "TextUtil.h"
#include "Unc.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimeZone>

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <unordered_map>
#include <variant>

using namespace std::chrono_literals;

namespace ws {

// Whether recent items are still on disk, asked on a thread per drive. A drive
// can take long to answer: a disk spinning up, a USB stick in trouble, a
// network share whose server is gone (seconds, or minutes). A search waiting
// for it would leave the last query's rows on screen all that time, so a
// search waits a moment only. A drive still busy then answers in its own
// time; until it does, its items keep the answer from before, or are left
// out. The threads hold only the state they share: a drive that never answers
// holds up nothing else, quitting included.
class RecentOnDisk {
public:
    struct Item {
        bool isDir = false;
        std::uint32_t modified = 0; // seconds since 1970, UTC
    };

    RecentOnDisk() = default;
    RecentOnDisk(const RecentOnDisk&) = delete;
    RecentOnDisk& operator=(const RecentOnDisk&) = delete;
    ~RecentOnDisk()
    {
        std::lock_guard lock(m_state->mutex);
        m_state->quitting = true;
        m_state->work.notify_all();
    }

    // For each of `paths`, in its place: what is there, or nothing (not there,
    // or no answer in time and none from before). `known`, if given: for each,
    // whether the disk said (so nothing means not there): not when its server
    // did not answer, or it would not say (no access, no disk in the drive).
    std::vector<std::optional<Item>> look(const QStringList& paths, std::vector<bool>* known = nullptr)
    {
        std::vector<std::optional<Item>> found(static_cast<std::size_t>(paths.size()));
        if (known)
            known->assign(found.size(), false);
        if (paths.isEmpty())
            return found;
        State& s = *m_state;
        std::unique_lock lock(s.mutex);
        const auto now = Clock::now();
        std::vector<qsizetype> awaited;
        std::vector<quint64> since(found.size()); // the answer wanted: from a look begun at this one or later
        for (qsizetype i = 0; i < paths.size(); ++i) {
            Drive& drive = driveOf(paths[i]);
            if (drive.busySince && drive.busyPath == paths[i]) {
                since[static_cast<std::size_t>(i)] = drive.busyLook; // being looked at: that answer will do
            } else {
                since[static_cast<std::size_t>(i)] = s.looks + 1;
                if (std::find(drive.queue.cbegin(), drive.queue.cend(), paths[i]) == drive.queue.cend())
                    drive.queue.push_back(paths[i]);
            }
            // Slow today (still on an earlier one after the wait, or the last
            // one took longer): not waited for, until it answers in time again.
            if ((!drive.busySince || now - *drive.busySince < kWait) && drive.lastTook < kWait)
                awaited.push_back(i);
        }
        s.work.notify_all();
        const auto answered = [&](qsizetype i) {
            const auto it = s.answers.find(paths[i]);
            return it != s.answers.end() && it->second.look >= since[static_cast<std::size_t>(i)];
        };
        s.answered.wait_until(lock, now + kWait, [&] { return std::all_of(awaited.cbegin(), awaited.cend(), answered); });
        for (qsizetype i = 0; i < paths.size(); ++i) {
            if (const auto it = s.answers.find(paths[i]); it != s.answers.end()) {
                found[static_cast<std::size_t>(i)] = it->second.item;
                if (known)
                    (*known)[static_cast<std::size_t>(i)] = it->second.item || it->second.gone;
            }
        }
        return found;
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kWait = 100ms; // a drive that is awake answers in well under a millisecond

    struct Answer {
        quint64 look = 0; // State::looks when it was begun
        std::optional<Item> item; // nothing: not there, or could not be told
        bool gone = false; // nothing, and the disk said so (no such file or path)
    };
    struct Drive {
        std::deque<QString> queue;
        std::optional<Clock::time_point> busySince; // looking at busyPath
        QString busyPath;
        quint64 busyLook = 0;
        Clock::duration lastTook {}; // logged when it grows long
    };
    struct State {
        std::mutex mutex;
        std::condition_variable work; // for the drives' threads
        std::condition_variable answered; // for the searches
        std::unordered_map<QString, Drive> drives; // "C:", "\\server\share"; nodes stay put
        std::unordered_map<QString, Answer> answers; // by path
        quint64 looks = 0; // begun so far
        bool quitting = false;
    };

    // Caller holds the lock.
    Drive& driveOf(const QString& path)
    {
        const QString key = driveKey(path);
        const auto [it, added] = m_state->drives.try_emplace(key);
        if (added)
            std::thread(&RecentOnDisk::run, m_state, key).detach();
        return it->second;
    }

    static QString driveKey(const QString& path)
    {
        if (path.size() >= 2 && path[1] == u':')
            return path.left(2).toUpper();
        if (path.startsWith(u"\\\\")) {
            const qsizetype server = path.indexOf(u'\\', 2);
            const qsizetype share = server < 0 ? -1 : path.indexOf(u'\\', server + 1);
            return (share < 0 ? path : path.left(share)).toLower();
        }
        return {};
    }

    // With "\\?\" when longer than Windows takes otherwise.
    static std::wstring longPath(const QString& path)
    {
        if (path.size() < MAX_PATH - 12 || path.startsWith(u"\\\\?\\"))
            return path.toStdWString();
        if (path.startsWith(u"\\\\"))
            return L"\\\\?\\UNC\\" + path.mid(2).toStdWString();
        return L"\\\\?\\" + path.toStdWString();
    }

    static void run(std::shared_ptr<State> state, QString key)
    {
        State& s = *state;
        std::unique_lock lock(s.mutex);
        Drive& drive = s.drives[key];
        for (;;) {
            s.work.wait(lock, [&] { return s.quitting || !drive.queue.empty(); });
            if (s.quitting)
                return;
            const QString path = drive.queue.front();
            drive.queue.pop_front();
            const quint64 look = ++s.looks;
            const auto began = Clock::now();
            drive.busySince = began;
            drive.busyPath = path;
            drive.busyLook = look;
            lock.unlock();

            std::optional<Item> item;
            bool gone = false;
            WIN32_FILE_ATTRIBUTE_DATA data {};
            if (::GetFileAttributesExW(longPath(path).c_str(), GetFileExInfoStandard, &data)) {
                const auto written = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
                const auto seconds = static_cast<qint64>(written / 10'000'000) - 11'644'473'600; // since 1601 → since 1970
                item = Item {bool(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY),
                    static_cast<std::uint32_t>(std::clamp<qint64>(seconds, 0, std::numeric_limits<std::uint32_t>::max()))};
            } else {
                // Not "the server is not there" (ERROR_BAD_NETPATH and the like), "access denied", "no disk".
                const DWORD error = ::GetLastError();
                gone = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
            }
            const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began);

            lock.lock();
            drive.busySince.reset();
            drive.busyPath.clear();
            s.answers[path] = {look, item, gone};
            s.answered.notify_all();
            // Once while it stays slow: a share whose server is gone takes as long every time.
            if (took > 2s && drive.lastTook <= 2s) {
                qWarning("WinShun: drive %ls took %lld ms to say whether a recent item is still there",
                    qUtf16Printable(key), static_cast<long long>(took.count()));
            }
            drive.lastTook = took;
        }
    }

    std::shared_ptr<State> m_state = std::make_shared<State>();
};

namespace {

constexpr int kRecentOnEmptyQuery = 5; // listed with nothing typed (the history keeps more, for ranking)
constexpr int kRecentPromoted = 3;
// Places in 全部: the best few. Windows lists over a thousand, and words like
// 设置 or 显示 are in hundreds of their names.
constexpr std::size_t kPlacesShown = 5;
constexpr int kTypedKeywordScore = 1000; // a web keyword's row: above every app, and every plugin row
// Where a path typed from some folder along the way leads ("src/app/"): the
// places looked at, and the best few put first.
constexpr std::size_t kPathsSearched = 200;
constexpr std::size_t kPathsLed = 3;
constexpr std::size_t kFirstContentBatch = 8; // the most rows the launcher shows (maxRows in Main.qml)

std::shared_ptr<const AppList> appsOf(const AppCatalog* catalog)
{
    static const auto kNone = std::make_shared<const AppList>();
    return catalog ? catalog->apps() : kNone;
}

std::shared_ptr<const PlaceList> placesOf(const SystemCatalog* catalog)
{
    static const auto kNone = std::make_shared<const PlaceList>();
    return catalog ? catalog->places() : kNone;
}

const AppInfo* findApp(const AppList& apps, const QString& launchPath)
{
    const QString id = appIdOf(launchPath);
    if (id.isEmpty())
        return nullptr;
    const auto it = std::find_if(apps.begin(), apps.end(), [&](const AppInfo& a) { return a.id == id; });
    return it == apps.end() ? nullptr : &*it;
}

const PlaceInfo* findPlace(const PlaceList& places, const QString& path)
{
    const QString key = placeKeyOf(path);
    if (key.isEmpty())
        return nullptr;
    const auto it = std::find_if(places.begin(), places.end(), [&](const PlaceInfo& p) { return p.key == key; });
    return it == places.end() ? nullptr : &*it;
}

// Found by one of its other names ("计算器" for Calculator): shown by that.
SearchResult fromApp(const AppInfo& app, bool recent, int otherName = -1)
{
    SearchResult r;
    r.name = otherName >= 0 && otherName < app.otherNames.size() ? app.otherNames[otherName] : app.name;
    r.path = app.launchPath();
    r.app = app.kind;
    r.elevatable = app.elevatable;
    r.target = app.target;
    r.recent = recent;
    return r;
}

SearchResult fromPlace(const PlaceInfo& place, bool recent)
{
    SearchResult r;
    r.name = place.name;
    r.path = place.path();
    r.app = place.kind;
    r.target = place.command;
    r.icon = place.icon;
    r.recent = recent;
    return r;
}

// Its site, or with `words` a search on it.
SearchResult fromWeb(const WebShortcut& web, const QString& words)
{
    SearchResult r;
    r.name = web.name;
    r.path = webPath(web.keyword, words);
    r.app = AppKind::Web;
    r.target = words.isEmpty() ? web.homeUrl() : web.searchUrl(words);
    r.keyword = web.keyword;
    r.words = words;
    r.icon = web.iconRef(); // its site's icon (or its user's), whichever page it opens
    return r;
}

// A file row that only repeats an app row: the app's program, or its
// shortcut in the Start menu.
bool duplicatesApp(const SearchResult& file, const SearchResult& app)
{
    if (app.isWeb())
        return false; // a page on the web, not anything on this computer
    if (app.app == AppKind::Desktop && !app.target.isEmpty() && file.path.compare(app.target, Qt::CaseInsensitive) == 0)
        return true;
    return file.name.size() == app.name.size() + 4 && file.name.endsWith(u".lnk", Qt::CaseInsensitive)
        && file.name.startsWith(app.name, Qt::CaseInsensitive)
        && file.path.contains(u"\\Start Menu\\Programs\\", Qt::CaseInsensitive);
}

SearchResult fromIndex(const FileIndex& index, EntryId id)
{
    const Entry& e = index.entry(id);
    SearchResult r;
    r.name = wtf8::toQString(index.name(e));
    r.path = index.path(id);
    r.isDir = e.isDir();
    return r;
}

// A recent item that is not among the matches found in the index (or one
// listed with nothing typed).
SearchResult fromDisk(const QString& path, const RecentOnDisk::Item& item)
{
    SearchResult r;
    r.path = QDir::toNativeSeparators(path);
    r.name = r.path.mid(r.path.lastIndexOf(u'\\') + 1);
    if (r.name.isEmpty())
        r.name = r.path;
    r.isDir = item.isDir;
    r.recent = true;
    r.modified = item.modified;
    return r;
}

// A row as it is, or a path to look up on disk first (RecentOnDisk): the
// recent items are taken in rounds, the paths of a round looked up together.
using RecentItem = std::variant<SearchResult, QString>;

// Whether entries lie under one folder, cached by parent folder (files next
// to each other share it).
class UnderFolder {
public:
    UnderFolder(const FileIndex& index, EntryId folder)
        : m_index(index)
        , m_folder(folder)
    {
        m_cache.fill({kNoEntry, false});
    }
    bool operator()(EntryId id)
    {
        const EntryId parent = m_index.entry(id).parent;
        if (m_folder == kNoEntry || parent == kNoEntry)
            return false;
        Slot& slot = m_cache[parent & (m_cache.size() - 1)];
        if (slot.parent != parent) {
            bool under = false;
            for (EntryId cur = parent; cur != kNoEntry && !under; cur = m_index.entry(cur).parent)
                under = cur == m_folder;
            slot = {parent, under};
        }
        return slot.under;
    }

private:
    struct Slot {
        EntryId parent;
        bool under;
    };
    const FileIndex& m_index;
    EntryId m_folder;
    std::array<Slot, 4096> m_cache {};
};

// Recently opened items that match jump to the top (at most a few).
// Returns how many did.
qsizetype promoteHistory(
    SearchResults& results, const SearchEngine::Request& request, const NameMatcher& matcher, RecentOnDisk& onDisk)
{
    const bool foldersOnly = request.scope == Scope::Folders;
    SearchResults promoted;
    qsizetype next = 0;
    while (promoted.size() < kRecentPromoted && next < request.history.size()) {
        std::vector<RecentItem> round;
        QStringList paths;
        while (next < request.history.size() && promoted.size() + std::ssize(round) < kRecentPromoted) {
            const QString& path = request.history[next++];
            if (isAppLaunchPath(path) || isPlacePath(path))
                continue; // apps and places rank by their own rules (searchApps, searchPlaces)
            const auto it = std::find_if(results.begin(), results.end(),
                [&](const SearchResult& r) { return r.path.compare(path, Qt::CaseInsensitive) == 0; });
            if (it != results.end()) {
                SearchResult r = *it;
                r.recent = true;
                results.erase(it);
                round.emplace_back(std::move(r));
                continue;
            }
            if (matcher.matchPath(path, false) < 0 && matcher.matchPath(path, true) < 0)
                continue; // cheap name check before asking the disk
            round.emplace_back(path);
            paths.push_back(path);
        }
        const auto found = onDisk.look(paths);
        std::size_t k = 0;
        for (RecentItem& item : round) {
            if (auto* r = std::get_if<SearchResult>(&item)) {
                promoted.push_back(std::move(*r));
                continue;
            }
            const QString& path = std::get<QString>(item);
            const std::optional<RecentOnDisk::Item>& there = found[k++];
            if (there && (!foldersOnly || there->isDir) && matcher.matchPath(path, there->isDir) >= 0)
                promoted.push_back(fromDisk(path, *there));
        }
    }
    const qsizetype count = promoted.size();
    if (count > 0) {
        promoted.append(std::move(results));
        results = std::move(promoted);
    }
    return count;
}

// Ranked by time, the recent ones stay where they are, marked.
void markRecent(SearchResults& results, const QStringList& history)
{
    for (SearchResult& r : results) {
        r.recent = std::any_of(history.cbegin(), history.cend(),
            [&](const QString& path) { return r.path.compare(path, Qt::CaseInsensitive) == 0; });
    }
}

// The files and folders found, in their order: by match, a few opened
// lately first (promoteHistory), then those named just what was typed
// ("readme" finds README.md and the folder readme), then the files and the
// folders apart, as asked, each best first. By time, the two kinds apart,
// newest first.
void arrange(SearchResults& results, const SearchEngine::Request& request, const NameMatcher& matcher, RankBy rankBy,
    RecentOnDisk& onDisk)
{
    const qsizetype promoted = rankBy == RankBy::Match ? promoteHistory(results, request, matcher, onDisk) : 0;
    auto rest = results.begin() + promoted; // after promoteHistory: it puts `results` together anew
    if (rankBy == RankBy::Match) {
        const auto exactBegin = rest;
        rest = std::stable_partition(rest, results.end(),
            [&](const SearchResult& r) { return matcher.isExactName(wtf8::fromUtf16(wtf8::view(r.name)), r.isDir); });
        for (auto it = exactBegin; it != rest; ++it)
            it->exact = true; // a plugin's rows keep below them (plugin::Merge)
    } else {
        markRecent(results, request.history);
    }
    const bool foldersFirst = request.kindOrder == KindOrder::FoldersFirst;
    std::stable_partition(rest, results.end(), [&](const SearchResult& r) { return r.isDir == foldersFirst; });
    if (results.size() > request.limit)
        results.resize(request.limit);
}

constexpr qsizetype kFavoritesPromoted = 5; // of those that match, the first put first

void markFavorite(SearchResult& r, const Favorite& f)
{
    r.favorite = true;
    r.favoriteKeyword = f.keyword;
}

// Favorites' rows as things are now, in their order: their app or place, or
// what is at their path (asked of the disk as recent items are). One known
// to be gone is there all the same, `missing`; one that cannot be told yet
// (the apps or places not read yet) is left out. On a share that cannot be
// reached now: `offline`, the disk not asked.
SearchResults favoriteRows(const QList<const Favorite*>& favorites, const AppList& apps, const PlaceList& places,
    RecentOnDisk& onDisk, const IndexService* index)
{
    std::vector<std::optional<SearchResult>> rows(static_cast<std::size_t>(favorites.size()));
    QStringList paths;
    std::vector<std::size_t> asked;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Favorite& f = *favorites[static_cast<qsizetype>(i)];
        SearchResult r;
        if (f.kind == Favorite::Kind::App) {
            if (const AppInfo* app = findApp(apps, f.path))
                r = fromApp(*app, false);
            else if (apps.empty())
                continue;
            else
                r = {.path = f.path, .missing = true, .app = AppKind::Desktop};
        } else if (f.kind == Favorite::Kind::Place) {
            if (const PlaceInfo* place = findPlace(places, f.path))
                r = fromPlace(*place, false);
            else if (places.empty())
                continue;
            else
                r = {.path = f.path, .missing = true, .app = AppKind::Tool};
        } else {
            r.path = f.path;
            r.isDir = f.kind == Favorite::Kind::Folder;
            if (index && r.path.startsWith(u"\\\\") && index->shares().offline(r.path.toStdWString())) {
                r.offline = true;
            } else {
                paths.push_back(f.path);
                asked.push_back(i);
            }
        }
        r.name = f.name;
        markFavorite(r, f);
        rows[i] = std::move(r);
    }
    std::vector<bool> known;
    const auto found = onDisk.look(paths, &known);
    for (std::size_t k = 0; k < asked.size(); ++k) {
        SearchResult& r = *rows[asked[k]];
        if (found[k]) {
            r.isDir = found[k]->isDir;
            r.modified = found[k]->modified;
        } else if (known[k]) {
            r.missing = true;
        }
    }
    SearchResults out;
    for (std::optional<SearchResult>& r : rows) {
        if (r)
            out.push_back(std::move(*r));
    }
    return out;
}

// Favorites among the matches: marked; one whose keyword is all that was
// typed on top (below a web shortcut's typed keyword); with `promote`, the
// first few that match (in their order) put first, those the index does not
// hold (a folder left out of it) looked for on disk. Adds to `total` what it
// adds.
void promoteFavorites(SearchResults& results, qint64& total, const SearchEngine::Request& request,
    const NameMatcher& matcher, bool promote, const AppList& apps, const PlaceList& places, RecentOnDisk& onDisk,
    const IndexService* index)
{
    const FavoriteList& favorites = request.favorites;
    if (favorites.isEmpty())
        return;
    const auto nameMatches = [&](const QString& name) { return matcher.matchName(wtf8::fromUtf16(wtf8::view(name))) >= 0; };
    for (SearchResult& r : results) {
        const qsizetype i = r.isWeb() || r.isPlugin() ? -1 : indexOfFavorite(favorites, r.path);
        if (i < 0)
            continue;
        markFavorite(r, favorites[i]);
        if (nameMatches(favorites[i].name))
            r.name = favorites[i].name; // its user's name for it, when that is what was typed
    }

    QList<const Favorite*> wanted;
    const qsizetype keyword = typedFavoriteKeyword(favorites, request.text);
    if (keyword >= 0 && scopeShows(request.scope, favorites[keyword].kind))
        wanted.push_back(&favorites[keyword]);
    const bool keyed = !wanted.isEmpty();
    for (qsizetype i = 0; promote && i < favorites.size() && wanted.size() < kFavoritesPromoted + (keyed ? 1 : 0); ++i) {
        const Favorite& f = favorites[i];
        if (i == keyword || !scopeShows(request.scope, f.kind))
            continue;
        const bool found = std::any_of(results.cbegin(), results.cend(),
            [&](const SearchResult& r) { return r.favorite && r.path.compare(f.path, Qt::CaseInsensitive) == 0; });
        if (found || nameMatches(f.name)
            || (f.isFileOrFolder() && matcher.matchPath(f.path, f.kind == Favorite::Kind::Folder) >= 0)) {
            wanted.push_back(&f);
        }
    }
    if (wanted.isEmpty())
        return;

    // Those found taken out as they are, the others asked of the disk.
    SearchResults rows;
    QList<const Favorite*> unfound;
    std::vector<qsizetype> unfoundAt;
    for (const Favorite* f : std::as_const(wanted)) {
        const auto it = std::find_if(results.begin(), results.end(),
            [&](const SearchResult& r) { return r.favorite && r.path.compare(f->path, Qt::CaseInsensitive) == 0; });
        if (it != results.end()) {
            rows.push_back(std::move(*it));
            results.erase(it);
            --total;
        } else {
            unfound.push_back(f);
            unfoundAt.push_back(rows.size());
            rows.push_back({});
        }
    }
    const SearchResults looked = favoriteRows(unfound, apps, places, onDisk, index);
    SearchResults put;
    for (qsizetype i = 0, k = 0; i < rows.size(); ++i) {
        if (k < std::ssize(unfoundAt) && unfoundAt[static_cast<std::size_t>(k)] == i) {
            const QString& path = unfound[k++]->path;
            const auto it = std::find_if(looked.cbegin(), looked.cend(), [&](const SearchResult& r) { return r.path == path; });
            if (it != looked.cend())
                put.push_back(*it);
            continue;
        }
        put.push_back(std::move(rows[i]));
    }
    if (keyed && !put.isEmpty() && put.first().path.compare(favorites[keyword].path, Qt::CaseInsensitive) == 0) {
        put.first().score = kTypedKeywordScore; // as a web keyword's row: nothing a plugin pushes goes above it
        put.first().exact = true;
    }
    total += put.size();
    // Below a web shortcut's keyword typed (the same word cannot be both; one typed with words can).
    const qsizetype at = !results.isEmpty() && results.first().isWeb() && results.first().score == kTypedKeywordScore ? 1 : 0;
    for (qsizetype i = 0; i < put.size(); ++i)
        results.insert(at + i, std::move(put[i]));
    if (results.size() > request.limit)
        results.resize(request.limit);
}

} // namespace

SearchEngine::SearchEngine(IndexService* index, AppCatalog* apps, SystemCatalog* places, QObject* parent)
    : QObject(parent)
    , m_index(index)
    , m_documents(std::make_unique<DocExtractor>(4, DocExtractor::Priority::Normal))
    , m_apps(apps)
    , m_places(places)
    , m_onDisk(std::make_unique<RecentOnDisk>())
{
    qRegisterMetaType<ws::SearchResults>();
    m_worker = std::jthread([this](std::stop_token stop) { workerLoop(stop); });
}

SearchEngine::~SearchEngine()
{
    {
        std::lock_guard lock(m_mutex);
        m_pending.reset();
        m_latest.store(~quint64 {0}); // marks any running job stale
    }
    m_worker.request_stop();
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

quint64 SearchEngine::submit(Request request)
{
    std::lock_guard lock(m_mutex);
    const quint64 id = ++m_counter;
    m_latest.store(id);
    m_pending = Job {id, std::move(request), std::chrono::steady_clock::now()};
    m_cv.notify_one();
    return id;
}

void SearchEngine::cancel()
{
    std::lock_guard lock(m_mutex);
    m_latest.store(++m_counter);
    m_pending.reset();
}

void SearchEngine::workerLoop(std::stop_token stop)
{
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            if (!m_cv.wait(lock, stop, [this] { return m_pending.has_value(); }))
                return;
            job = std::move(*m_pending);
            m_pending.reset();
        }
        if (isStale(job.id))
            continue;
        if (job.request.scope == Scope::Content)
            runContentSearch(job);
        else
            runNameSearch(job);
    }
}

void SearchEngine::runNameSearch(const Job& job)
{
    QElapsedTimer timer;
    timer.start();
    const Request& request = job.request;
    const ParsedQuery query = parseQuery(request.text);
    SearchResults results;
    qint64 total = 0;
    const auto apps = appsOf(m_apps);
    const auto places = placesOf(m_places);

    // For the log, when it takes long.
    using Clock = std::chrono::steady_clock;
    const auto began = Clock::now();
    Clock::duration lockWait {};
    Clock::duration names {};

    const bool foldersOnly = request.scope == Scope::Folders;
    if (query.isEmpty() && request.startList == StartList::Modified) {
        // The files written last (in 文件夹, the folders they are in), newest first.
        // A day ahead at most: later is a clock gone wrong, not new work.
        const auto now = static_cast<std::int64_t>(QDateTime::currentSecsSinceEpoch());
        const NewestOptions options {.limit = kRecentOnEmptyQuery,
            .folders = foldersOnly,
            .notAfter = static_cast<FileTime>(std::clamp<std::int64_t>(now + 86'400, 0, 0xFFFF'FFFF))};
        const auto index = m_index->index();
        const auto lock = index->readLock();
        lockWait = Clock::now() - began;
        const NewestOutput out = newestFiles(*index, options, m_pool, [&] { return isStale(job.id); });
        if (out.cancelled || isStale(job.id))
            return;
        for (const NameHit& hit : out.hits) {
            SearchResult r = fromIndex(*index, hit.id);
            r.modified = hit.modified;
            results.push_back(std::move(r));
        }
        markRecent(results, request.history);
        total = results.size();
    } else if (query.isEmpty()) {
        qsizetype next = 0;
        while (results.size() < kRecentOnEmptyQuery && next < request.history.size() && !isStale(job.id)) {
            std::vector<RecentItem> round;
            QStringList paths;
            while (next < request.history.size() && results.size() + std::ssize(round) < kRecentOnEmptyQuery) {
                const QString& path = request.history[next++];
                if (indexOfFavorite(request.favorites, path) >= 0)
                    continue; // listed above them
                if (isAppLaunchPath(path)) {
                    const AppInfo* app = request.scope == Scope::All ? findApp(*apps, path) : nullptr;
                    if (app)
                        round.emplace_back(fromApp(*app, true));
                    continue;
                }
                if (isPlacePath(path)) {
                    const PlaceInfo* place = request.scope == Scope::All ? findPlace(*places, path) : nullptr;
                    if (place)
                        round.emplace_back(fromPlace(*place, true));
                    continue;
                }
                round.emplace_back(path);
                paths.push_back(path);
            }
            const auto found = m_onDisk->look(paths);
            std::size_t k = 0;
            for (RecentItem& item : round) {
                if (auto* r = std::get_if<SearchResult>(&item)) {
                    results.push_back(std::move(*r));
                    continue;
                }
                const std::optional<RecentOnDisk::Item>& there = found[k++];
                if (there && (!foldersOnly || there->isDir))
                    results.push_back(fromDisk(std::get<QString>(item), *there));
            }
        }
        total = results.size();
    } else {
        const NameMatcher matcher(query);
        const auto index = m_index->index();
        const RankBy rankBy = request.scope == Scope::All ? RankBy::Match : request.rankBy;
        // A path typed or pasted ("C:\Users\me\Tencent Files\x"): what that
        // folder holds and is so named, from the disk as Explorer lists it,
        // then what lies deeper in it and is so named, from the index.
        const pathtext::TypedPath typedPath = pathtext::splitTyped(request.text);
        {
            const auto lock = index->readLock();
            lockWait = Clock::now() - began;
            const NameSearchOptions options {.limit = static_cast<std::size_t>(request.limit),
                .foldersOnly = foldersOnly,
                .rankBy = rankBy,
                .kindsApart = true};
            const NameSearchOutput out
                = searchNames(*index, matcher, options, m_pool, [&] { return isStale(job.id); });
            names = Clock::now() - began - lockWait;
            if (out.cancelled || isStale(job.id))
                return;
            total = static_cast<qint64>(out.totalMatches);
            results.reserve(static_cast<qsizetype>(out.hits.size()));
            // The folders of a typed path match names anywhere along the way:
            // only what really lies under the folder typed.
            const EntryId folder = typedPath.folder.isEmpty() ? kNoEntry : index->findPath(typedPath.folder.toStdWString());
            UnderFolder under(*index, folder);
            for (const NameHit& hit : out.hits) {
                if (folder != kNoEntry && !under(hit.id)) {
                    --total;
                    continue;
                }
                SearchResult r = fromIndex(*index, hit.id);
                r.modified = hit.modified;
                results.push_back(std::move(r));
            }
        }
        arrange(results, request, matcher, rankBy, *m_onDisk);

        // A network folder is listed only when its server answers: one that
        // is not there would hold the search up for the half minute Windows
        // waits. Known to be away (a share searched), or not answering now.
        const bool listable = !unc::isUnc(typedPath.folder.toStdWString())
            || (!m_index->shares().offline(typedPath.folder.toStdWString())
                && share::reachable(unc::host(typedPath.folder.toStdWString())));
        if (!typedPath.folder.isEmpty() && listable) {
            // The ones named just so first; with nothing more typed than the
            // folder, the folder itself (listTyped puts it first).
            SearchResults rows = pathtext::listTyped(typedPath, foldersOnly, static_cast<int>(request.limit));
            std::stable_partition(rows.begin(), rows.end(),
                [&](const SearchResult& r) { return matcher.isExactName(wtf8::fromUtf16(wtf8::view(r.name)), r.isDir); });
            total -= results.removeIf([&](const SearchResult& deeper) {
                return std::any_of(rows.cbegin(), rows.cend(),
                    [&](const SearchResult& r) { return r.path.compare(deeper.path, Qt::CaseInsensitive) == 0; });
            });
            total += rows.size();
            rows.append(std::move(results));
            if (rows.size() > request.limit)
                rows.resize(request.limit);
            results = std::move(rows);
        }
        // On a share that cannot be reached now: shown all the same, dimmed.
        for (SearchResult& r : results) {
            if (r.path.startsWith(u"\\\\"))
                r.offline = m_index->shares().offline(r.path.toStdWString());
        }

        // 全部: a web shortcut's keyword typed first ("gh", "gh WinShun")
        // puts its site or search on top. Then every app found, the best few
        // places and the web shortcuts by name, best first; then the files
        // and folders.
        if (request.scope == Scope::All && typedPath.folder.isEmpty()) {
            struct Ranked {
                int score;
                SearchResult result;
            };
            std::vector<Ranked> ranked;
            for (const AppHit& hit : searchApps(*apps, query, matcher, request.history)) {
                ranked.push_back({hit.score, fromApp((*apps)[hit.index], false, hit.otherName)});
                ranked.back().result.exact = matcher.isExactName((*apps)[hit.index].nameUtf8, false);
            }
            const std::size_t appCount = ranked.size();
            for (const PlaceHit& hit : searchPlaces(*places, query, matcher, request.history)) {
                if (ranked.size() - appCount >= kPlacesShown)
                    break;
                const PlaceInfo& place = (*places)[hit.index];
                // An app of that name (控制面板) is the same thing.
                const bool isApp = std::any_of(ranked.cbegin(), ranked.cbegin() + appCount,
                    [&](const Ranked& r) { return r.result.name.compare(place.name, Qt::CaseInsensitive) == 0; });
                if (!isApp)
                    ranked.push_back({hit.score, fromPlace(place, false)});
            }
            const std::optional<TypedKeyword> typed = typedKeyword(request.web, request.text);
            for (const WebHit& hit : searchWebShortcuts(request.web, matcher)) {
                if (!typed || hit.index != typed->index) // else on top already
                    ranked.push_back({hit.score, fromWeb(request.web[hit.index], {})});
            }
            // Equal scores: apps first.
            std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
            SearchResults top;
            if (typed) {
                top.push_back(fromWeb(request.web[typed->index], typed->words));
                top.back().score = kTypedKeywordScore; // nothing a plugin pushes goes above it
                top.back().exact = true;
            }
            total += static_cast<qint64>(ranked.size()) + top.size();
            for (Ranked& r : ranked) {
                if (top.size() >= request.limit)
                    break;
                r.result.score = r.score;
                top.push_back(std::move(r.result));
            }
            if (!top.isEmpty()) {
                total -= results.removeIf([&](const SearchResult& file) {
                    return std::any_of(top.cbegin(), top.cend(), [&](const SearchResult& app) { return duplicatesApp(file, app); });
                });
                top.append(std::move(results));
                if (top.size() > request.limit)
                    top.resize(request.limit);
                results = std::move(top);
            }
        }

        // A path typed from some folder along the way ("tools/out/covers/",
        // "src\app\App.cpp"): where it leads first, so Enter goes there, not
        // to the best of what lies in it or is named like it. Several such
        // places: the one opened last, then your own, then the shallowest.
        if (const pathtext::RelativePath typed = typedPath.folder.isEmpty() ? pathtext::splitRelative(request.text)
                                                                            : pathtext::RelativePath();
            !typed.names.isEmpty()) {
            std::vector<std::string> names;
            for (const QString& name : typed.names)
                names.push_back(wtf8::fromUtf16(wtf8::view(name)));
            struct Led {
                SearchResult row;
                qsizetype opened; // in the history; its size when not there
                bool aside; // under system, program or tool folders, or hidden
                int depth;
            };
            std::vector<Led> led;
            {
                const auto lock = index->readLock();
                const std::vector<EntryId> ids = findRelativePath(*index, names, foldersOnly || typed.folder,
                    kPathsSearched, m_pool, [&] { return isStale(job.id); });
                for (const EntryId id : ids) {
                    SearchResult row = fromIndex(*index, id);
                    row.modified = index->modified(id);
                    const qsizetype opened = request.history.indexOf(row.path, 0, Qt::CaseInsensitive);
                    const bool aside = index->entry(id).flags & (EntryFlag::LowPriority | EntryFlag::Hidden);
                    led.push_back({std::move(row), opened < 0 ? request.history.size() : opened, aside, index->depth(id)});
                }
            }
            if (isStale(job.id))
                return;
            const auto keep = std::min(led.size(), kPathsLed);
            std::ranges::partial_sort(led, led.begin() + static_cast<std::ptrdiff_t>(keep), [](const Led& a, const Led& b) {
                if (a.opened != b.opened)
                    return a.opened < b.opened;
                if (a.aside != b.aside)
                    return !a.aside;
                if (a.depth != b.depth)
                    return a.depth < b.depth;
                return a.row.modified > b.row.modified;
            });
            led.resize(keep);
            if (!led.empty()) {
                total -= results.removeIf([&](const SearchResult& r) {
                    return std::ranges::any_of(
                        led, [&](const Led& l) { return l.row.path.compare(r.path, Qt::CaseInsensitive) == 0; });
                });
                SearchResults rows;
                for (Led& l : led)
                    rows.push_back(std::move(l.row));
                total += rows.size();
                rows.append(std::move(results));
                if (rows.size() > request.limit)
                    rows.resize(request.limit);
                results = std::move(rows);
            }
        }

        // Favorites first, but not before what a typed path leads to, nor
        // among rows ranked by time (marked there only).
        const bool promote = rankBy == RankBy::Match && typedPath.folder.isEmpty()
            && pathtext::splitRelative(request.text).names.isEmpty();
        promoteFavorites(results, total, request, matcher, promote, *apps, *places, *m_onDisk, m_index);
    }

    // Nothing typed: every favorite first, in its user's order; the recent
    // items after them (not twice).
    if (query.isEmpty() && !request.favorites.isEmpty() && !isStale(job.id)) {
        QList<const Favorite*> shown;
        for (const Favorite& f : request.favorites) {
            if (scopeShows(request.scope, f.kind))
                shown.push_back(&f);
        }
        SearchResults rows = favoriteRows(shown, *apps, *places, *m_onDisk, m_index);
        results.removeIf([&](const SearchResult& r) { return indexOfFavorite(request.favorites, r.path) >= 0; });
        total = rows.size() + results.size();
        rows.append(std::move(results));
        results = std::move(rows);
    }

    // The rows of the query before stay on screen (dimmed) until these come.
    const auto ms = [](Clock::duration d) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    };
    const auto end = Clock::now();
    if (end - job.submitted > 2s) {
        qWarning("WinShun: a search took %lld ms (%lld waiting for the search before, %lld for the index, "
                 "%lld finding names, %lld the rest)",
            ms(end - job.submitted), ms(began - job.submitted), ms(lockWait), ms(names), ms(end - began - lockWait - names));
    }

    if (!isStale(job.id))
        emit resultsReady(job.id, results, total, timer.nsecsElapsed() / 1000);
}

void SearchEngine::runContentSearch(const Job& job)
{
    QElapsedTimer timer;
    timer.start();
    const Request& request = job.request;
    const QStringList terms = contentTerms(request.text);
    emit resultsReady(job.id, {}, 0, 0);
    if (terms.isEmpty()) {
        emit contentProgress(job.id, 0, 0, true);
        return;
    }

    // Candidates: the files of the chosen types, not cloud placeholders. Only
    // their ids are kept (there can be millions: every .js under
    // node_modules); a file's path is looked up when it is read, and the ids
    // are pinned until then. The content index answers for the files it
    // knows without opening them: those it says match are read first (for
    // the line and snippet), then the ones it does not know, your own files
    // before the rest. Those are read whole and handed to the content index
    // (ContentIndexer::Intake): the next search knows them.
    struct Candidate {
        EntryId id;
        std::uint8_t order; // lower first
        bool indexed; // the content index knows it
    };
    const ContentFilter filter {ExtensionFilter(request.contentDocuments
                                                    ? request.contentExtensions + documentExtensions()
                                                    : request.contentExtensions),
        request.contentInLowPriority};
    const auto index = m_index->index();
    const auto content = m_index->contentIndex();
    const ContentIndex::SearchGuard searching(content.get()); // the content indexer waits meanwhile
    std::vector<Candidate> candidates;
    int ruledOut = 0;
    FileIndex::IdPin pin;
    {
        const auto lock = index->readLock();
        pin = index->pinIds(); // from here on the index is not compacted: the content index's ids stay its own
    }
    // Not under the lock: the content index may have to be read from disk
    // first (on a hard disk, one that may first have to spin up), and the
    // index would wait for that, and name searches behind it.
    QElapsedTimer lookupTimer;
    lookupTimer.start();
    const ContentIndex::Lookup lookup = content ? content->lookup(terms) : ContentIndex::Lookup();
    if (lookupTimer.elapsed() > 50) // a few milliseconds from the cache: it was read off the disk
        m_index->prewarmSoon(std::chrono::seconds(5));
    if (isStale(job.id))
        return;
    {
        // Every entry is looked at: a chunk of them per worker (on one
        // thread, 3 million take some 15 ms).
        const auto lock = index->readLock();
        constexpr std::uint32_t kMatch = ContentIndex::Lookup::kMatch;
        const EntryId profile = index->findPath(win32::expandEnvironment(L"%USERPROFILE%"));
        std::vector<std::vector<Candidate>> found(index->chunkCount());
        std::vector<int> ruledOutOf(index->chunkCount(), 0);
        m_pool.parallelFor(index->chunkCount(), [&](std::size_t c) {
            if (isStale(job.id))
                return;
            UnderFolder inProfile(*index, profile);
            const auto first = static_cast<EntryId>(c << FileIndex::kChunkBits);
            auto k = static_cast<std::size_t>(std::lower_bound(lookup.known.begin(), lookup.known.end(), first,
                [](std::uint32_t known, EntryId id) { return (known & ~kMatch) < id; }) - lookup.known.begin());
            const auto entries = index->chunk(c);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const Entry& e = entries[i];
                if (!filter.accepts(*index, e))
                    continue;
                const auto id = static_cast<EntryId>(first + i);
                bool indexed = false;
                bool likely = false;
                while (k < lookup.known.size() && (lookup.known[k] & ~kMatch) < id)
                    ++k;
                if (k < lookup.known.size() && (lookup.known[k] & ~kMatch) == id) {
                    indexed = true;
                    if (lookup.usable) {
                        if (!(lookup.known[k] & kMatch)) {
                            ++ruledOutOf[c];
                            continue;
                        }
                        likely = true;
                    }
                }
                std::uint8_t priority = 1;
                if (e.flags & EntryFlag::LowPriority)
                    priority = 3;
                else if (e.flags & EntryFlag::Hidden)
                    priority = 2;
                else if (inProfile(id))
                    priority = 0;
                found[c].push_back({id, static_cast<std::uint8_t>(likely ? priority : 4 + priority), indexed});
            }
        });
        if (isStale(job.id))
            return;
        std::size_t count = 0;
        for (const auto& f : found)
            count += f.size();
        candidates.reserve(count);
        for (std::size_t c = 0; c < found.size(); ++c) {
            candidates.insert(candidates.end(), found[c].begin(), found[c].end());
            ruledOut += ruledOutOf[c];
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.order < b.order; });

    const ContentScanner scanner(terms);
    const std::int64_t now = QDateTime::currentSecsSinceEpoch();
    ContentIndexer::Intake intake(m_index->contentIndexer()); // before the reads, gone once they are done
    const int total = static_cast<int>(candidates.size()) + ruledOut;
    std::atomic<std::size_t> next {0};
    std::atomic<int> scanned {ruledOut};
    std::atomic<int> found {0};
    std::mutex batchMutex;
    SearchResults batch;
    std::mutex doneMutex;
    std::condition_variable doneCv;
    const int threadCount = static_cast<int>(
        std::clamp<std::size_t>(candidates.size(), 1, static_cast<std::size_t>(std::clamp(request.contentThreads, 1, 64))));
    int running = threadCount;

    const auto stale = [&] { return isStale(job.id); };
    const auto flush = [&](bool finished) {
        SearchResults out;
        {
            std::lock_guard lock(batchMutex);
            out.swap(batch);
        }
        if (stale())
            return;
        // Best first (the launcher places them among the rows it has).
        std::ranges::stable_sort(out, std::ranges::greater {}, &SearchResult::relevance);
        if (!out.isEmpty())
            emit contentResults(job.id, out);
        emit contentProgress(job.id, scanned.load(), total, finished);
    };

    {
        std::vector<std::jthread> workers;
        workers.reserve(threadCount);
        for (int t = 0; t < threadCount; ++t) {
            workers.emplace_back([&] {
                ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                std::wstring path;
                while (!stale() && found.load() < request.maxContentResults) {
                    const std::size_t i = next.fetch_add(1);
                    if (i >= candidates.size())
                        break;
                    const Candidate& candidate = candidates[i];
                    bool gone = false;
                    FileTime modified = 0;
                    {
                        const auto lock = index->readLock();
                        gone = index->entry(candidate.id).isDeleted();
                        if (!gone) {
                            path = index->wpath(candidate.id); // as it is now: it may have moved
                            modified = index->modified(candidate.id);
                        }
                    }
                    if (gone) {
                        scanned.fetch_add(1);
                        continue;
                    }
                    const std::int64_t maxBytes = request.contentSizeLimits.of(path);
                    std::optional<ContentMatch> match;
                    doctext::Location location;
                    const bool document = isDocumentPath(path);
                    if (document) {
                        // Its text as the index keeps it; else read now (not
                        // indexed yet, changed since, or on a volume not
                        // followed), and kept for next time if it can be.
                        std::optional<doctext::DocText> text
                            = candidate.indexed ? content->textOf(candidate.id) : std::nullopt;
                        if (!text && intake.wants(path)) {
                            const std::uint64_t since = intake.since();
                            ContentIndexer::FileText read
                                = ContentIndexer::readFile(path, maxBytes, stale, m_documents.get());
                            intake.take(candidate.id, read, since);
                            text = std::move(read.document);
                        } else if (!text) {
                            DocExtractor::Result read = m_documents->extractFile(path, maxBytes, stale);
                            if (read.status == extractproto::Status::Ok)
                                text = std::move(read.text);
                        }
                        if (text) {
                            std::size_t at = 0;
                            const std::string& s = text->text;
                            match = scanner.scan(
                                [&](char* buffer, std::size_t capacity) {
                                    const std::size_t n = std::min(capacity, s.size() - at);
                                    std::memcpy(buffer, s.data() + at, n);
                                    at += n;
                                    return n;
                                },
                                ContentScanner::kChunkBytes, stale);
                            if (match)
                                location = doctext::locate(*text, static_cast<std::uint32_t>(match->line));
                        }
                    } else if (!candidate.indexed && intake.wants(path)) {
                        const std::uint64_t since = intake.since();
                        ContentIndexer::Scanned read = ContentIndexer::scanFile(path, maxBytes, scanner, stale);
                        intake.take(candidate.id, read.text, since);
                        match = std::move(read.match);
                    } else {
                        match = scanner.scanFile(path, maxBytes, stale);
                    }
                    scanned.fetch_add(1);
                    if (!match)
                        continue;
                    SearchResult r;
                    r.path = QString::fromStdWString(path);
                    r.name = r.path.mid(r.path.lastIndexOf(u'\\') + 1);
                    r.modified = modified;
                    r.line = match->line;
                    if (document) {
                        switch (location.kind) {
                        case doctext::PlaceKind::Page:
                            r.where = SearchResult::Where::Page;
                            break;
                        case doctext::PlaceKind::Slide:
                            r.where = SearchResult::Where::Slide;
                            break;
                        case doctext::PlaceKind::Row:
                            r.where = SearchResult::Where::Row;
                            r.sheet = QString::fromUtf8(location.sheet);
                            break;
                        case doctext::PlaceKind::None:
                            r.where = SearchResult::Where::Document;
                            break;
                        }
                        r.placeNumber = static_cast<int>(location.number);
                    }
                    r.snippet = match->snippet;
                    r.snippetMatches = std::move(match->matches);
                    r.relevance = contentRelevance(*match, terms, r.name, candidate.order & 3,
                        modified ? now - static_cast<std::int64_t>(modified) : -1);
                    std::lock_guard lock(batchMutex);
                    if (found.load() < request.maxContentResults) {
                        batch.push_back(std::move(r));
                        found.fetch_add(1);
                    }
                }
                {
                    std::lock_guard lock(doneMutex);
                    --running;
                }
                doneCv.notify_all();
            });
        }

        // The first matches go out once they fill the window or 50 ms have
        // passed (a new query's rows replace the last one's with them: one
        // row alone would flash), the rest every 100 ms.
        const auto start = std::chrono::steady_clock::now();
        bool first = true;
        for (;;) {
            std::unique_lock lock(doneMutex);
            if (doneCv.wait_for(lock, first ? 5ms : 100ms, [&] { return running == 0; }))
                break;
            lock.unlock();
            if (first) {
                std::size_t ready = 0;
                {
                    std::lock_guard batchLock(batchMutex);
                    ready = static_cast<std::size_t>(batch.size());
                }
                if (ready < kFirstContentBatch && std::chrono::steady_clock::now() - start < 50ms)
                    continue;
                first = false;
            }
            flush(false);
        }
    }
    flush(true);
    candidates = decltype(candidates)(); // free them before compacting (`= {}` keeps the capacity)
    ::HeapCompact(::GetProcessHeap(), 0); // return the scan's scratch memory to the OS
}

} // namespace ws
