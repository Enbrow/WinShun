#include "MissingFiles.h"

#include "Win32Util.h"

#include <QTimer>

#include <windows.h>

#include <algorithm>

using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr std::chrono::milliseconds kStartWait = 30s; // the history is loaded, the start out of the way
constexpr std::chrono::milliseconds kInterval = 10min;

// For testing: WINSHUN_MISSING_SECONDS=20 waits 20 s instead, after the
// start, between passes and before an entry is cleaned out.
std::optional<std::chrono::milliseconds> testWait()
{
    bool ok = false;
    const int seconds = qEnvironmentVariableIntValue("WINSHUN_MISSING_SECONDS", &ok);
    if (!ok || seconds < 0)
        return std::nullopt;
    return std::chrono::seconds(seconds);
}

// The drive letter of "C:\…"; none for any other path, a share's included.
std::optional<char16_t> driveLetter(const QString& path)
{
    if (path.size() < 3 || path[1] != u':' || path[2] != u'\\')
        return std::nullopt;
    const char16_t letter = path[0].toUpper().unicode();
    return letter >= u'A' && letter <= u'Z' ? std::optional(letter) : std::nullopt;
}

} // namespace

MissingFiles::Mode MissingFiles::modeOf(QStringView name)
{
    return name == u"hide" ? Mode::Hide : name == u"clean" ? Mode::Clean : Mode::Keep;
}

MissingFiles::Disk MissingFiles::Disk::windows()
{
    return {
        .driveType = [](const std::wstring& root) -> std::uint32_t { return ::GetDriveTypeW(root.c_str()); },
        .lookUp = [](const std::wstring& path) -> std::uint32_t {
            return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ? ERROR_SUCCESS : ::GetLastError();
        },
    };
}

MissingFiles::Judge::Judge(Disk disk)
    : m_disk(std::move(disk))
{
}

MissingFiles::Presence MissingFiles::Judge::file(const QString& path)
{
    // Drive letters only: a share that does not answer would hold the pass
    // for half a minute.
    const std::optional<char16_t> letter = driveLetter(path);
    if (!letter || !driveAnswers(*letter))
        return Presence::Unknown;
    switch (m_disk.lookUp(win32::longPath(path.toStdWString()))) {
    case ERROR_SUCCESS:
        return Presence::Present;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: // its folder is gone too
        return Presence::Gone;
    default: // not allowed to look, the drive not ready, ...
        return Presence::Unknown;
    }
}

bool MissingFiles::Judge::driveAnswers(char16_t letter)
{
    std::optional<bool>& answers = m_drives[letter - u'A'];
    if (!answers) {
        // A mapped share is as slow as the share. No drive by that letter (a
        // USB stick pulled out, a disk not connected), or one that is not
        // ready (a card reader without its card): its files may well exist.
        const std::wstring root {static_cast<wchar_t>(letter), L':', L'\\'};
        const std::uint32_t type = m_disk.driveType(root);
        answers = type != DRIVE_REMOTE && type != DRIVE_NO_ROOT_DIR && type != DRIVE_UNKNOWN
            && m_disk.lookUp(root) == ERROR_SUCCESS;
    }
    return *answers;
}

bool MissingFiles::Judge::allGone(const QStringList& paths)
{
    return !paths.isEmpty()
        && std::ranges::all_of(paths, [this](const QString& path) { return file(path) == Presence::Gone; });
}

std::vector<qint64> MissingFiles::Sightings::update(
    std::span<const qint64> gone, Clock::time_point now, Clock::duration wait)
{
    QHash<qint64, Clock::time_point> since;
    std::vector<qint64> forGood;
    for (const qint64 id : gone) {
        const auto it = m_since.constFind(id);
        if (it == m_since.cend()) {
            since.insert(id, now); // first seen gone
            continue;
        }
        since.insert(id, *it);
        if (now - *it >= wait)
            forGood.push_back(id);
    }
    m_since = std::move(since);
    return forGood;
}

MissingFiles::MissingFiles(ClipStore* store, Mode mode, Disk disk, QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_mode(mode)
    , m_disk(std::move(disk))
{
    const std::optional<std::chrono::milliseconds> test = testWait();
    m_interval = test.value_or(kInterval);
    m_store->setJudged(mode != Mode::Keep);
    QTimer::singleShot(test.value_or(kStartWait), this, [this] {
        m_started = true;
        check();
    });
}

void MissingFiles::setMode(Mode mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    m_store->setJudged(mode != Mode::Keep);
    if (mode == Mode::Keep) {
        // Everything is listed again at once; a pass under way is let go of.
        m_pending.reset();
        m_sightings.clear();
        if (!m_hidden.isEmpty()) {
            m_hidden.clear();
            m_store->setHidden({});
            emit changed();
        }
        return;
    }
    // Turned on, or from hiding to cleaning: a pass now rather than in ten
    // minutes (before the one after the start, that one).
    if (m_started)
        check();
}

void MissingFiles::setListShown(bool shown)
{
    if (shown == m_listShown)
        return;
    m_listShown = shown;
    if (shown)
        return;
    if (m_pending)
        apply(*std::exchange(m_pending, std::nullopt));
    if (m_started && due())
        check();
}

bool MissingFiles::due() const
{
    return !m_lastPass || Clock::now() - *m_lastPass >= m_interval;
}

void MissingFiles::check()
{
    if (m_mode == Mode::Keep || m_checking)
        return;
    // A list of files cut short in memory (thousands of them) is not all of it.
    struct Entry {
        qint64 id = 0;
        qint64 used = 0;
        QStringList files;
    };
    std::vector<Entry> entries;
    for (const Clip& c : m_store->clips()) {
        if (c.kind == ClipKind::Files && c.group == 0 && !c.truncated())
            entries.push_back({c.id, c.used, c.files()});
    }
    const Clock::time_point at = Clock::now();
    m_checking = true;
    m_lastPass = at;
    m_worker = std::jthread([this, entries = std::move(entries), disk = m_disk, at](std::stop_token stop) {
        Judge judge(disk);
        Pass pass {.at = at};
        for (const Entry& e : entries) {
            if (stop.stop_requested())
                return; // Win顺 is quitting
            if (judge.allGone(e.files))
                pass.gone.push_back({e.id, e.used});
        }
        QMetaObject::invokeMethod(
            this, [this, pass = std::move(pass)]() mutable { finished(std::move(pass)); }, Qt::QueuedConnection);
    });
}

void MissingFiles::finished(Pass pass)
{
    m_checking = false;
    if (m_mode == Mode::Keep)
        return; // turned off meanwhile
    if (m_listShown)
        m_pending = std::move(pass); // not from under the user: once the window is put away
    else
        apply(pass);
}

void MissingFiles::apply(const Pass& pass)
{
    // As the entries are now: one removed, put in a group, or copied or
    // pasted again (its files back, maybe) since the pass is left alone.
    std::vector<qint64> gone;
    QHash<qint64, qint64> hidden;
    for (const Found& found : pass.gone) {
        const Clip* c = m_store->clip(found.id);
        if (!c || c->group != 0 || c->used != found.used)
            continue;
        gone.push_back(found.id);
        hidden.insert(found.id, found.used);
    }
    // Gone for good: out of the history when cleaning; hidden till then.
    std::vector<qint64> forGood = m_sightings.update(gone, pass.at, m_interval);
    if (m_mode != Mode::Clean)
        forGood.clear();
    for (const qint64 id : forGood)
        hidden.remove(id);
    const bool hiddenChanged = hidden != m_hidden;
    if (hiddenChanged) {
        m_hidden = hidden;
        m_store->setHidden(std::move(hidden));
    }
    if (!forGood.empty()) {
        // As the user removes entries, without the undo: what is on disk goes too.
        m_store->remove(forGood);
        m_store->discardImages(forGood);
    }
    if (hiddenChanged || !forGood.empty())
        emit changed();
}

} // namespace ws
