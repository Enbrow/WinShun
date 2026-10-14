#pragma once

#include <QString>
#include <QStringList>

#include <windows.h>

// The folders the user was in lately, for the bar beside file dialogs and the
// folder menu (GitHub #22).
namespace ws::recentfolders {

// The drives of this computer, as they are now. Paths on the others are not
// looked at: a network drive whose server is gone could take long to answer
// (and hold up all that comes after).
class LocalDrives {
public:
    LocalDrives();
    bool contain(const QString& path) const;

private:
    DWORD m_mask = 0;
};

// A folder, or a file's folder; empty when it is not there (or not on one of `drives`).
QString folderOf(const QString& path, const LocalDrives& drives);

// Newest first, at most `max`: the folders of what Windows lists as opened
// lately and those of `history` (Win顺's, newest first: files and folders
// opened through it), taken in turns. Needs COM; looks at the disk.
QStringList recent(const QStringList& history, int max);

} // namespace ws::recentfolders
