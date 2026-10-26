#pragma once

#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

namespace ws {

// Backing up the settings (GitHub #17; what a backup holds: BackupFiles.h)
// into a folder the user chose. One in a synced folder (OneDrive, 坚果云, a
// WebDAV drive mounted with alist, RaiDrive or rclone) takes them to the
// cloud as well: Win顺 itself speaks no WebDAV. Automatic while a folder is
// set: a few minutes after start, a minute after the settings change, and
// every half hour; each time only when something changed since the newest
// backup there. Restoring unpacks a backup beside the data and restarts
// Win顺, which puts it in place before reading anything (applyRestored).
// GUI thread; the files are read and written on a thread of their own.
class Backup : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(QString folder READ folder NOTIFY changed FINAL) // empty: no backups
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(QDateTime last READ last NOTIFY changed FINAL) // of the newest backup there; invalid when none
    Q_PROPERTY(QString problem READ problem NOTIFY changed FINAL) // why the last one failed
    Q_PROPERTY(QString message READ message NOTIFY changed FINAL) // result of an explicit operation

public:
    explicit Backup(QObject* parent = nullptr);

    void setFolder(const QString& folder); // from the settings
    QString folder() const { return m_folder; }
    bool busy() const { return m_busy; }
    QDateTime last() const { return m_last; }
    QString problem() const { return m_problem; }
    QString message() const { return m_message; }

    void soon(); // the settings changed: a backup in a minute
    Q_INVOKABLE void backUpNow();
    Q_INVOKABLE void exportBackup(QWindow* window);
    Q_INVOKABLE void openFolder();
    // Asks which backup (a dialog owned by `window`), backs up what there is
    // now, then has Win顺 restart with the one chosen.
    Q_INVOKABLE void restore(QWindow* window);

    // At start, before anything reads the settings: a backup being restored
    // is put in place.
    static void applyRestored();

signals:
    void changed();
    void restartRequested();

private:
    // A backup into the folder (or, with nowhere chosen, the data folder's
    // "backups"), then `restoring`, if given, unpacked to be applied.
    void run(const QString& restoring = {}, const QString& exporting = {});
    void readLast();

    QString m_folder;
    bool m_busy = false;
    QDateTime m_last;
    QString m_problem;
    QString m_message;
    QTimer m_timer;
};

} // namespace ws
