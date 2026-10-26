#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

// What a backup of the settings holds and how it is written (GitHub #17):
// one zip a backup, "WinShun-2026-10-13-194700.zip", in a folder the user
// chose. Inside, "WinShun.ini" (the settings), "data/…" (files of the data
// folder, below) and "backup.json" (when, which version, a hash of the rest).
// Qt Core and miniz only: the backup service (Backup.h) and the tests use it.
namespace ws::backupfiles {

inline constexpr int kKept = 10; // backups kept in the folder; older ones go
inline constexpr qint64 kMaxFileBytes = 16 << 20; // a plugin's data file larger than this is left out

struct File {
    QString name; // in the zip: "WinShun.ini", "data/favorites.json"
    QString path; // on disk
    friend bool operator==(const File&, const File&) = default;
};

// The files of a backup: the settings file, and from the data folder the
// favorites, the dialog pins, the pictures chosen for web shortcuts, and
// the plugins added (no logs, nothing over kMaxFileBytes). Not the indexes
// (rebuilt from the disks), not the clipboard or search history (private,
// and changing all the time), not window places, not the plugins' caches
// ("<id>.data"). Plugins' passwords stay in the Credential Manager.
QList<File> collect(const QString& settingsFile, const QString& dataFolder);

// The files packed, with backup.json; and the hash of their names and
// contents (the same files give the same hash). Empty when one cannot be read.
struct Packed {
    QByteArray zip;
    QString hash;
};
// A transfer leaves out this computer's backup/index locations and saved
// Windows integration state. On import, those are kept from the target PC.
Packed pack(const QList<File>& files, const QString& version, const QDateTime& when, bool forTransfer = false);

struct Found {
    QString path;
    QDateTime when; // from the name
};
// The backups in `folder`, newest first.
QList<Found> list(const QString& folder);
// The hash the backup at `path` was written with; empty when it is no backup of ours.
QString hashOf(const QString& path);
// "WinShun-2026-10-13-194700.zip"
QString fileName(const QDateTime& when);

// Writes `packed` atomically into `folder` as the backup of `when`, using
// the next free second on a name collision, then removes all but the newest
// kKept. Empty on success, else why.
QString write(const QString& folder, const Packed& packed, const QDateTime& when);

// A single, user-named export, saved atomically without pruning its folder.
QString exportTo(const QString& path, const Packed& packed);

// Unpacks the backup at `path` into `folder` (made here): "WinShun.ini"
// and "data\…". Only those names are taken; a path that leaves the folder
// fails the whole backup. Empty on success, else why.
QString unpack(const QString& path, const QString& folder);

// Puts an unpacked backup (`staged`, as unpack() left it) in place: its
// WinShun.ini over `settingsFile`, its data\… into `dataFolder`, file by
// file (what the backup lacks stays). A failed replacement rolls back the
// files already replaced; the staged backup is kept on failure. Empty on
// success, else why. `staged` is removed only after success.
QString apply(const QString& staged, const QString& settingsFile, const QString& dataFolder);

} // namespace ws::backupfiles
