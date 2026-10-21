#pragma once

#include "ClipStore.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace ws {

// Entries of the clipboard history for copied files that were deleted or
// moved since (GitHub #54): left as they are (the row says 已不存在), hidden
// from the lists, or cleaned out of the history (Settings::
// clipboardMissingFiles). Only files copied as files: a path copied as text
// is still text worth pasting. Never an entry in "固定" or a group, which
// the user keeps on purpose.
//
// A pass asks the disk on a thread of its own, never as the window opens:
// a little after the start, when the window is put away (ten minutes after
// the last pass at the soonest), and at once when the setting is turned on.
// What it finds waits while the window is up: no row goes from under the
// user. A hidden entry still counts toward the history's limits; a search
// still finds it (ClipStore::setHidden). One is cleaned out only once two
// passes ten minutes apart found its files gone: a file cut and pasted
// elsewhere, a document saved by replacing it, one put back from the
// Recycle Bin are back by then. GUI thread.
class MissingFiles : public QObject {
    Q_OBJECT

public:
    enum class Mode { Keep, Hide, Clean };
    static Mode modeOf(QStringView name); // "keep", "hide" or "clean", as the settings keep it

    using Clock = std::chrono::steady_clock;

    // What looking up a file says.
    enum class Presence { Present, Gone, Unknown };

    // The disk as asked: Windows, or a test's made-up one.
    struct Disk {
        // GetDriveTypeW of a root such as "C:\": DRIVE_FIXED, DRIVE_REMOTE...
        std::function<std::uint32_t(const std::wstring& root)> driveType;
        // GetFileAttributesW of a path: ERROR_SUCCESS if it is there, else
        // GetLastError() right after.
        std::function<std::uint32_t(const std::wstring& path)> lookUp;

        static Disk windows();
    };

    // Judges files for one pass, asking about each drive once.
    class Judge {
    public:
        explicit Judge(Disk disk);

        // Gone only when Windows says there is no such file or folder.
        // Unknown for what it cannot be sure of: a share, a mapped network
        // drive, a drive that is not there (a USB stick pulled out), a file
        // it may not look at (Win顺 runs unelevated).
        Presence file(const QString& path);
        // Every one of an entry's files gone; stops at the first that is not.
        bool allGone(const QStringList& paths);

    private:
        bool driveAnswers(char16_t letter);

        Disk m_disk;
        std::array<std::optional<bool>, 26> m_drives; // by letter, as asked this pass
    };

    // Entries found gone pass after pass.
    class Sightings {
    public:
        // A pass at `now` found the entries `gone` gone; any other entry is
        // forgotten. Returns those an earlier pass, at least `wait` before,
        // found gone as well: gone for good.
        std::vector<qint64> update(std::span<const qint64> gone, Clock::time_point now, Clock::duration wait);
        void clear() { m_since.clear(); }

    private:
        QHash<qint64, Clock::time_point> m_since; // the first of the passes in a row that found it gone
    };

    // `store` is the history's, open or not; the first pass comes a little
    // after this (if `mode` is not Keep by then).
    MissingFiles(ClipStore* store, Mode mode, Disk disk = Disk::windows(), QObject* parent = nullptr);

    void setMode(Mode mode);
    // The clipboard window is up (Clipboard::active): what a pass finds
    // waits until it is put away, which starts a pass if one is due.
    void setListShown(bool shown);
    bool checking() const { return m_checking; } // a pass is under way

signals:
    // Entries were hidden, shown again or cleaned out: the lists are to be
    // made again.
    void changed();

private:
    struct Found { // an entry whose files were all gone, as it was then
        qint64 id = 0;
        qint64 used = 0;
    };
    struct Pass {
        std::vector<Found> gone;
        Clock::time_point at;
    };

    void check(); // a pass, unless one is under way or the mode is Keep
    void finished(Pass pass);
    void apply(const Pass& pass);
    bool due() const;

    ClipStore* m_store;
    Mode m_mode;
    Disk m_disk;
    std::chrono::milliseconds m_interval {}; // between passes, and between the two that clean an entry out
    bool m_started = false; // the wait after the start is over
    bool m_listShown = false;
    bool m_checking = false;
    std::optional<Clock::time_point> m_lastPass;
    std::optional<Pass> m_pending; // found while the window was up
    Sightings m_sightings;
    QHash<qint64, qint64> m_hidden; // as last given to the store
    std::jthread m_worker; // the pass; last, so stopped and joined first
};

} // namespace ws
