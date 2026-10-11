#include "ShareIndexer.h"

#include "Unc.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QtGlobal>

#include <windows.h>

#include <algorithm>

using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr auto kStartupDelay = 15s; // after start, before shares read before are verified: let the network come up
constexpr auto kNetworkQuiet = 5s; // after the network changed, before servers are tried: DHCP, VPN routes
constexpr auto kFirstRetry = 1min; // an offline server, tried again; doubling up to
constexpr auto kLastRetry = 10min;
constexpr auto kProblemRetry = 30min; // signing in and the like: until the user does something
constexpr auto kSuspectRetry = 30min; // a share that came back nearly empty, read again
constexpr auto kForever = std::chrono::hours(24 * 365);
constexpr int kPassShare = 60; // verified at most every this many times as long as a pass takes
constexpr std::size_t kSuspectMin = 1000; // missing items that can be a mount not ready yet
constexpr std::size_t kRemoveBatch = 256; // removals per write lock
constexpr std::size_t kMaxPending = 100'000; // live changes queued; beyond, the share is verified instead

std::string toUtf8(std::wstring_view s)
{
    return wtf8::fromUtf16(wtf8::view(s));
}

std::wstring wideName(const FileIndex& index, EntryId id)
{
    std::u16string wide;
    wtf8::decodeAppend(wide, index.name(id));
    return std::wstring(wide.begin(), wide.end());
}

FileTime now() noexcept
{
    FILETIME t;
    ::GetSystemTimeAsFileTime(&t);
    return fileTimeOf(win32::ticks(t));
}

// Entries in the subtree of `id`, itself included. Read lock held.
std::size_t subtreeSize(const FileIndex& index, EntryId id)
{
    if (id >= index.slotCount() || index.entry(id).isDeleted())
        return 0;
    std::size_t n = 0;
    std::vector<EntryId> stack {id};
    while (!stack.empty()) {
        const EntryId cur = stack.back();
        stack.pop_back();
        ++n;
        for (EntryId c = index.entry(cur).firstChild; c != kNoEntry; c = index.entry(c).nextSibling)
            stack.push_back(c);
    }
    return n;
}

bool isUnder(const FileIndex& index, EntryId id, EntryId root)
{
    for (EntryId cur = id; cur != kNoEntry; cur = index.entry(cur).parent) {
        if (cur == root)
            return true;
    }
    return false;
}

// Shares inside another share (or the same twice) are left out: their files
// would be in the index twice.
std::vector<ShareConfig> distinct(std::vector<ShareConfig> shares)
{
    std::vector<ShareConfig> out;
    for (ShareConfig& s : shares) {
        if (s.path.empty())
            continue;
        const auto overlaps = [&](const ShareConfig& o) { return unc::contains(o.path, s.path) || unc::contains(s.path, o.path); };
        if (std::none_of(out.begin(), out.end(), overlaps))
            out.push_back(std::move(s));
    }
    return out;
}

bool unreachable(share::Problem p)
{
    return p == share::Problem::Offline || p == share::Problem::Dropped;
}

} // namespace

struct ShareIndexer::Share {
    ShareConfig config;
    Status status;
    std::condition_variable_any cv;
    bool wake = false;
    bool verifyRequested = false;
    bool acceptMissing = false;
    bool passRunning = false;
    bool removed = false;
    std::vector<FsChange> pending; // live changes, applied between passes
    std::size_t suspectMissing = 0; // what the first suspect pass did not find
    Clock::time_point suspectAt {};
    std::atomic<std::size_t> progress {0}; // items read by the pass under way
    std::jthread thread;
};

ShareIndexer::ShareIndexer(Source source, Options options, Callbacks callbacks)
    : m_source(std::move(source))
    , m_callbacks(std::move(callbacks))
    , m_options(std::move(options))
{
    m_options.shares = distinct(std::move(m_options.shares));
    for (const ShareConfig& c : m_options.shares) {
        auto s = std::make_unique<Share>();
        s->config = c;
        s->status.path = c.path;
        s->status.name = c.name;
        m_shares.push_back(std::move(s));
    }
}

ShareIndexer::~ShareIndexer()
{
    stop();
}

void ShareIndexer::start()
{
    std::lock_guard lock(m_mutex);
    if (m_started || m_stopped)
        return;
    m_started = true;
    for (auto& s : m_shares)
        launch(*s);
}

void ShareIndexer::launch(Share& share)
{
    share.thread = std::jthread([this, &share](std::stop_token stop) { run(stop, share); });
}

void ShareIndexer::stop()
{
    std::vector<std::unique_ptr<Share>> all;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopped)
            return;
        m_stopped = true;
        for (auto& s : m_shares)
            s->thread.request_stop();
        for (auto& s : m_retired)
            s->thread.request_stop();
    }
    // Joined without the lock: the threads take it on their way out.
    for (auto& s : m_shares) {
        if (s->thread.joinable())
            s->thread.join();
    }
    for (auto& s : m_retired) {
        if (s->thread.joinable())
            s->thread.join();
    }
    std::lock_guard watch(m_watchMutex);
    m_watcher.reset();
}

void ShareIndexer::setOptions(Options options)
{
    options.shares = distinct(std::move(options.shares));
    std::vector<std::unique_ptr<Share>> removed;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopped)
            return;
        const bool rulesChanged = !(options.rules == m_options.rules) || options.excludedFiles != m_options.excludedFiles;
        // Shares no longer wanted: their threads end and take their entries with them.
        for (auto it = m_shares.begin(); it != m_shares.end();) {
            const auto kept = std::find_if(options.shares.begin(), options.shares.end(),
                [&](const ShareConfig& c) { return win32::equalsIgnoreCase(c.path, (*it)->config.path); });
            if (kept == options.shares.end()) {
                (*it)->removed = true;
                (*it)->thread.request_stop();
                removed.push_back(std::move(*it));
                it = m_shares.erase(it);
                continue;
            }
            Share& s = **it;
            s.config.name = kept->name;
            s.status.name = kept->name;
            if (rulesChanged) {
                s.verifyRequested = true; // folders no longer excluded come back (newly excluded ones are gone already)
                s.wake = true;
                s.cv.notify_all();
            }
            ++it;
        }
        // New ones.
        for (const ShareConfig& c : options.shares) {
            const bool known = std::any_of(m_shares.begin(), m_shares.end(),
                [&](const auto& s) { return win32::equalsIgnoreCase(s->config.path, c.path); });
            if (known)
                continue;
            auto s = std::make_unique<Share>();
            s->config = c;
            s->status.path = c.path;
            s->status.name = c.name;
            if (m_started)
                launch(*s);
            m_shares.push_back(std::move(s));
        }
        // In the order given.
        std::stable_sort(m_shares.begin(), m_shares.end(), [&](const auto& a, const auto& b) {
            const auto at = [&](const Share& s) {
                return std::find_if(options.shares.begin(), options.shares.end(),
                           [&](const ShareConfig& c) { return win32::equalsIgnoreCase(c.path, s.config.path); })
                    - options.shares.begin();
            };
            return at(*a) < at(*b);
        });
        m_options = std::move(options);
        for (auto& r : removed)
            m_retired.push_back(std::move(r));
    }
    if (m_callbacks.changed)
        m_callbacks.changed();
}

ShareIndexer::Options ShareIndexer::options() const
{
    std::lock_guard lock(m_mutex);
    return m_options;
}

void ShareIndexer::verifyNow(const std::wstring& path)
{
    std::lock_guard lock(m_mutex);
    for (auto& s : m_shares) {
        if (win32::equalsIgnoreCase(s->config.path, path)) {
            s->verifyRequested = true;
            s->wake = true;
            s->cv.notify_all();
        }
    }
}

void ShareIndexer::verifyAll()
{
    std::lock_guard lock(m_mutex);
    for (auto& s : m_shares) {
        s->verifyRequested = true;
        s->wake = true;
        s->cv.notify_all();
    }
}

void ShareIndexer::acceptMissing(const std::wstring& path)
{
    std::lock_guard lock(m_mutex);
    for (auto& s : m_shares) {
        if (win32::equalsIgnoreCase(s->config.path, path)) {
            s->acceptMissing = true;
            s->verifyRequested = true;
            s->wake = true;
            s->cv.notify_all();
        }
    }
}

void ShareIndexer::networkChanged()
{
    std::lock_guard lock(m_mutex);
    m_networkChangedAt = Clock::now();
    for (auto& s : m_shares) {
        // Offline ones try again; online ones make sure they still are.
        s->wake = true;
        s->cv.notify_all();
    }
}

std::vector<ShareIndexer::Status> ShareIndexer::statuses() const
{
    std::lock_guard lock(m_mutex);
    std::vector<Status> out;
    for (const auto& s : m_shares) {
        out.push_back(s->status);
        if (s->status.state == State::Building)
            out.back().items = s->progress.load(std::memory_order_relaxed);
    }
    return out;
}

bool ShareIndexer::offline(std::wstring_view path) const
{
    std::lock_guard lock(m_mutex);
    for (const auto& s : m_shares) {
        if (unc::contains(s->config.path, path))
            return s->status.state == State::Problem && unreachable(s->status.problem);
    }
    return false;
}

std::size_t ShareIndexer::dropUnknownRoots(FileIndex& index, const std::vector<ShareConfig>& shares)
{
    std::size_t removed = 0;
    const std::vector<EntryId> roots = index.roots();
    for (const EntryId r : roots) {
        if (!index.name(r).starts_with("\\\\"))
            continue;
        const std::wstring name = wideName(index, r);
        const bool wanted = std::any_of(
            shares.begin(), shares.end(), [&](const ShareConfig& c) { return win32::equalsIgnoreCase(c.path, name); });
        if (!wanted)
            removed += index.remove(r);
    }
    return removed;
}

EntryId ShareIndexer::ensureRoot(FileIndex& index, const std::wstring& path)
{
    EntryId root = kNoEntry;
    for (const EntryId r : index.roots()) {
        if (index.name(r).starts_with("\\\\") && win32::equalsIgnoreCase(wideName(index, r), path)) {
            root = r;
            break;
        }
    }
    if (root == kNoEntry)
        root = index.addRoot(toUtf8(path));
    // Not kept in the snapshot for roots (FileIndex::addStored): set every time.
    index.setFlags(root, EntryFlag::Remote);
    return root;
}

void ShareIndexer::setStatus(Share& share, const std::function<void(Status&)>& change)
{
    bool changed = false;
    {
        std::lock_guard lock(m_mutex);
        Status before = share.status;
        change(share.status);
        changed = !(before == share.status);
    }
    if (changed && m_callbacks.changed)
        m_callbacks.changed();
}

bool ShareIndexer::wait(std::stop_token stop, Share& share, Clock::duration timeout)
{
    std::unique_lock lock(m_mutex);
    const auto until = Clock::now() + std::min<Clock::duration>(timeout, kForever);
    share.cv.wait_until(lock, stop, until, [&] { return share.wake; });
    share.wake = false;
    return !stop.stop_requested();
}

void ShareIndexer::run(std::stop_token stop, Share& share)
{
    const std::wstring path = share.config.path;
    const std::wstring host(unc::host(path));
    const auto end = [&] {
        bool removed = false;
        {
            std::lock_guard lock(m_mutex);
            removed = share.removed;
        }
        if (!removed)
            return;
        // Taken out of the search: its entries go too.
        if (const auto index = m_source.index()) {
            auto lock = index->writeLock();
            for (const EntryId r : std::vector<EntryId>(index->roots())) {
                if (index->name(r).starts_with("\\\\") && win32::equalsIgnoreCase(wideName(*index, r), path))
                    index->remove(r);
            }
        }
        if (m_callbacks.dirty)
            m_callbacks.dirty();
        updateWatches();
    };

    // The index first: loaded from the snapshot, or the local disks read.
    while (!m_source.ready()) {
        if (!wait(stop, share, 500ms))
            return end();
    }
    const auto index = m_source.index();
    FileTime last = 0;
    std::size_t items = 0;
    {
        auto lock = index->writeLock();
        const EntryId root = ensureRoot(*index, path);
        last = index->modified(root);
        items = subtreeSize(*index, root) - 1;
    }
    setStatus(share, [&](Status& s) {
        s.lastVerified = last;
        s.items = items;
    });

    // Read before: verified once a little after start, for what changed
    // while we were not running. Asking for it (or a new share) goes first.
    auto nextPass = Clock::now() + (last != 0 ? kStartupDelay : 0s);
    auto retry = std::chrono::duration_cast<Clock::duration>(kFirstRetry);
    while (!stop.stop_requested()) {
        // Wait out a network change before trying a server.
        Clock::time_point changedAt;
        {
            std::lock_guard lock(m_mutex);
            changedAt = m_networkChangedAt;
        }
        const bool justChanged = Clock::now() - changedAt < kNetworkQuiet;
        if (justChanged && !wait(stop, share, changedAt + kNetworkQuiet - Clock::now()))
            break;

        if (!share::reachable(host, 1500ms, justChanged)) {
            setStatus(share, [](Status& s) {
                s.state = State::Problem;
                s.problem = share::Problem::Offline;
                s.error = ERROR_BAD_NETPATH;
                s.watching = false;
            });
            updateWatches();
            if (!wait(stop, share, retry))
                break;
            retry = std::min<Clock::duration>(retry * 2, kLastRetry);
            continue;
        }

        bool due = false;
        bool building = false;
        {
            std::lock_guard lock(m_mutex);
            building = share.status.lastVerified == 0;
            due = building || share.verifyRequested || Clock::now() >= nextPass
                || (share.status.state == State::Problem); // came back: read what we missed
            share.verifyRequested = false;
        }
        if (due) {
            const auto passStart = Clock::now();
            const bool read = pass(stop, share, building);
            const auto passTime = Clock::now() - passStart;
            if (stop.stop_requested())
                break;
            const Options o = options();
            Status st;
            {
                std::lock_guard lock(m_mutex);
                st = share.status;
            }
            if (read) {
                retry = kFirstRetry;
                const auto every = st.watching ? o.verifyEvery : o.verifyEveryUnwatched;
                // A share that takes long to read is not read most of the time.
                nextPass = Clock::now()
                    + (every.count() > 0 ? std::max<Clock::duration>(every, passTime * kPassShare) : kForever);
            } else if (st.state == State::Suspect) {
                nextPass = Clock::now() + kSuspectRetry;
            } else if (st.state == State::Problem) {
                const bool away = unreachable(st.problem);
                if (away && !share::reachable(host, 1500ms, true))
                    continue; // gone: the probe says when it is back
                // Still answering, but not to us (signing in, a connection it
                // drops each time): not again before a while.
                if (!wait(stop, share, away ? retry : std::chrono::duration_cast<Clock::duration>(kProblemRetry)))
                    break;
                if (away)
                    retry = std::min<Clock::duration>(retry * 2, kLastRetry);
                continue;
            }
        }
        updateWatches();
        applyPending(stop, share);

        // Until the next pass, or something to do (live changes, asked to).
        if (!wait(stop, share, nextPass - Clock::now()))
            break;
    }
    end();
}

bool ShareIndexer::pass(std::stop_token stop, Share& share, bool building)
{
    const Options o = options();
    const Crawler crawler(o.rules);
    const auto index = m_source.index();
    const std::wstring& path = share.config.path;
    EntryId root = kNoEntry;
    FileIndex::IdPin pin; // the ids held across write locks stay valid
    {
        auto lock = index->writeLock();
        root = ensureRoot(*index, path);
        pin = index->pinIds();
    }
    {
        std::lock_guard lock(m_mutex);
        share.passRunning = true;
        share.progress = 0;
    }
    const auto done = [&] {
        std::lock_guard lock(m_mutex);
        share.passRunning = false;
    };
    setStatus(share, [&](Status& s) { s.state = building ? State::Building : State::Verifying; });
    if (m_callbacks.reading)
        m_callbacks.reading(path, true);

    std::stop_source passStop;
    const std::stop_callback forward(stop, [&] { passStop.request_stop(); });
    share::Lister lister(crawler, path, {o.excludedFiles, false, 64 * 1024, &share.progress}, passStop);
    std::vector<EntryId> gone;
    Crawler::SyncStats stats;
    const int threads = building ? o.threads : std::max(2, o.threads / 2);
    crawler.sync(*index, {{root, path, EntryFlag::Remote}}, threads, false, passStop.get_token(),
        [&](const Crawler::Root& dir, DirListing& out) { return lister(dir, out); }, &gone, &stats);
    if (m_callbacks.reading)
        m_callbacks.reading(path, false);
    if (stop.stop_requested()) {
        done();
        return false; // what it added stays; nothing removed
    }
    if (lister.firstError() != 0) {
        share::Problem problem = lister.problem();
        unsigned long error = lister.firstError();
        // Windows hangs up on an SMB 1 server (64, "no longer available")
        // unless SMB 1 was installed again: the probe knows why.
        if ((unreachable(problem) || problem == share::Problem::Other)
            && share::probe(unc::host(path), 1500ms, true) == share::Answer::Smb1Only) {
            problem = share::Problem::Smb1Only;
            error = ERROR_SMB1_NOT_AVAILABLE;
        }
        qWarning("WinShun: cannot read %ls (error %lu)", path.c_str(), error);
        setStatus(share, [&](Status& s) {
            s.state = State::Problem;
            s.problem = problem;
            s.error = error;
            s.watching = false;
        });
        done();
        return false;
    }

    // Believe what is gone? Not when it is much of the share at once.
    std::size_t missing = 0;
    std::size_t live = 0;
    {
        const auto lock = index->readLock();
        for (const EntryId id : gone)
            missing += subtreeSize(*index, id);
        live = subtreeSize(*index, root) - 1;
    }
    bool accept = false;
    {
        std::lock_guard lock(m_mutex);
        // The same finding (within a tenth) a while after the first: believed.
        // Read again sooner (by hand), it keeps the time of the first.
        const bool same = share.suspectMissing != 0 && missing * 10 >= share.suspectMissing * 9
            && missing * 10 <= share.suspectMissing * 11;
        const bool agrees = same && Clock::now() - share.suspectAt >= kSuspectRetry - 5min;
        accept = std::exchange(share.acceptMissing, false) || building || agrees
            || missing <= std::max(kSuspectMin, live / 2);
        if (accept) {
            share.suspectMissing = 0;
        } else if (!same) {
            share.suspectMissing = missing;
            share.suspectAt = Clock::now();
        }
    }
    if (!accept) {
        qWarning("WinShun: %ls has %zu of %zu items no more: not believed yet", path.c_str(), missing, live);
        setStatus(share, [&](Status& s) {
            s.state = State::Suspect;
            s.problem = share::Problem::None;
            s.error = 0;
            s.items = live; // all still in the index
            s.missing = missing;
        });
        done();
        return false;
    }

    for (std::size_t i = 0; i < gone.size(); i += kRemoveBatch) {
        auto lock = index->writeLock();
        for (std::size_t k = i; k < std::min(gone.size(), i + kRemoveBatch); ++k) {
            const EntryId id = gone[k];
            // Removed or moved away by a live change meanwhile: leave it be.
            if (id < index->slotCount() && !index->entry(id).isDeleted() && isUnder(*index, id, root))
                index->remove(id);
        }
    }
    const FileTime verified = now();
    {
        auto lock = index->writeLock();
        index->setModified(root, verified);
        live = subtreeSize(*index, root) - 1;
    }
    setStatus(share, [&](Status& s) {
        s.state = State::Online;
        s.problem = share::Problem::None;
        s.error = 0;
        s.items = live;
        s.lastVerified = verified;
        s.missing = 0;
    });
    done();
    if (m_callbacks.dirty)
        m_callbacks.dirty();
    return true;
}

void ShareIndexer::applyPending(std::stop_token stop, Share& share)
{
    std::vector<FsChange> changes;
    {
        std::lock_guard lock(m_mutex);
        changes.swap(share.pending);
    }
    if (changes.empty())
        return;
    const Options o = options();
    const Crawler crawler(o.rules);
    std::erase_if(changes, [&](const FsChange& c) { return crawler.isExcludedPath(c.path); });
    const auto index = m_source.index();
    FileIndex::IdPin pin;
    {
        const auto lock = index->readLock();
        pin = index->pinIds();
    }
    std::stop_source passStop;
    const std::stop_callback forward(stop, [&] { passStop.request_stop(); });
    share::Lister lister(crawler, share.config.path, {o.excludedFiles}, passStop);
    applyChanges(*index, changes, crawler, passStop.get_token(),
        [&](const Crawler::Root& dir, DirListing& out) { return lister(dir, out); });
    std::size_t items = 0;
    {
        const auto lock = index->readLock();
        items = subtreeSize(*index, index->findPath(share.config.path)) - 1;
    }
    setStatus(share, [&](Status& s) { s.items = items; });
    if (m_callbacks.dirty)
        m_callbacks.dirty();
}

void ShareIndexer::updateWatches()
{
    std::vector<std::wstring> roots;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopped)
            return;
        for (const auto& s : m_shares) {
            const bool read = s->status.lastVerified != 0 && m_options.watch;
            if (read && (s->status.state == State::Online || s->status.state == State::Verifying))
                roots.push_back(s->config.path);
        }
    }
    std::vector<std::wstring> watched;
    {
        std::lock_guard watch(m_watchMutex);
        if (!m_watcher) {
            if (roots.empty())
                return;
            m_watcher = std::make_unique<ChangeWatcher>([this](std::vector<FsChange>&& c) { onChanges(std::move(c)); });
        }
        if (m_watcher->roots() != roots)
            m_watcher->setRoots(roots); // a server that does not tell is not in roots() after
        watched = m_watcher->roots();
    }
    std::vector<Share*> toldChanged;
    {
        std::lock_guard lock(m_mutex);
        for (auto& s : m_shares) {
            const bool watching = std::find(watched.begin(), watched.end(), s->config.path) != watched.end();
            if (s->status.watching != watching) {
                s->status.watching = watching;
                toldChanged.push_back(s.get());
            }
        }
    }
    if (!toldChanged.empty() && m_callbacks.changed)
        m_callbacks.changed();
}

void ShareIndexer::onChanges(std::vector<FsChange>&& changes)
{
    std::lock_guard lock(m_mutex);
    for (FsChange& c : changes) {
        const auto it = std::find_if(m_shares.begin(), m_shares.end(),
            [&](const auto& s) { return unc::contains(s->config.path, c.path); });
        if (it == m_shares.end())
            continue;
        Share& s = **it;
        // Lost events (or the server went away): only reading it again tells.
        if (c.kind == FsChange::Kind::Overflow || s.pending.size() >= kMaxPending) {
            s.verifyRequested = true;
            s.pending.clear();
        } else {
            s.pending.push_back(std::move(c));
        }
        s.wake = true;
        s.cv.notify_all();
    }
}

} // namespace ws
