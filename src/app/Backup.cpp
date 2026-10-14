#include "Backup.h"

#include "BackupFiles.h"
#include "Settings.h"
#include "platform/Shell.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QThreadPool>
#include <QWindow>

#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr auto kFirst = 3min; // after start: login settles first
constexpr auto kAfterEdit = 1min; // several changes in a row make one backup
constexpr auto kEvery = 30min;

QString stagedFolder()
{
    return QDir(Settings::dataDir()).filePath(u"restore"_s);
}

} // namespace

Backup::Backup(QObject* parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        m_timer.start(kEvery);
        if (!m_folder.isEmpty())
            run();
    });
}

void Backup::setFolder(const QString& folder)
{
    if (folder == m_folder)
        return;
    const bool first = m_folder.isEmpty();
    m_folder = folder;
    m_problem.clear();
    readLast();
    if (m_folder.isEmpty())
        m_timer.stop();
    else if (first)
        m_timer.start(kFirst);
    emit changed();
}

void Backup::soon()
{
    if (!m_folder.isEmpty())
        m_timer.start(kAfterEdit);
}

void Backup::backUpNow()
{
    run();
}

void Backup::openFolder()
{
    if (!m_folder.isEmpty())
        shell::open(m_folder);
}

void Backup::restore(QWindow* window)
{
    if (m_busy)
        return;
    const HWND owner = window ? reinterpret_cast<HWND>(window->winId()) : nullptr;
    const QString file = shell::pickFile(owner, tr("选择要恢复的备份"),
        {{tr("Win顺 备份"), u"WinShun-*.zip"_s}, {tr("所有文件"), u"*.*"_s}});
    if (!file.isEmpty())
        run(file);
}

void Backup::run(const QString& restoring)
{
    if (m_busy)
        return;
    m_busy = true;
    emit changed();
    const QString folder
        = m_folder.isEmpty() ? QDir(Settings::dataDir()).filePath(u"backups"_s) : QDir::fromNativeSeparators(m_folder);
    const QString settingsFile = Settings::filePath();
    const QString dataFolder = Settings::dataDir();
    const QString version = QCoreApplication::applicationVersion();
    const QString staged = stagedFolder();
    QThreadPool::globalInstance()->start([self = QPointer(this), folder, settingsFile, dataFolder, version, restoring, staged] {
        // Nothing written when nothing changed since the newest backup.
        const QDateTime now = QDateTime::currentDateTime();
        const backupfiles::Packed packed = backupfiles::pack(backupfiles::collect(settingsFile, dataFolder), version, now);
        const QList<backupfiles::Found> existing = backupfiles::list(folder);
        QString problem;
        if (packed.zip.isEmpty())
            problem = QCoreApplication::translate("ws::Backup", "有文件读不出来");
        else if (existing.isEmpty() || backupfiles::hashOf(existing.first().path) != packed.hash)
            problem = backupfiles::write(folder, packed, now);
        // A backup to restore: unpacked beside the data, in place once
        // complete, applied by the next start.
        bool restart = false;
        if (!restoring.isEmpty() && problem.isEmpty()) {
            const QString part = staged + u".part"_s;
            problem = backupfiles::unpack(restoring, part);
            QDir(staged).removeRecursively();
            if (problem.isEmpty() && !QDir().rename(part, staged))
                problem = QCoreApplication::translate("ws::Backup", "写不进 %1").arg(QDir::toNativeSeparators(dataFolder));
            if (!problem.isEmpty())
                QDir(part).removeRecursively();
            restart = problem.isEmpty();
        }
        const QList<backupfiles::Found> found = backupfiles::list(folder);
        const QDateTime last = found.isEmpty() ? QDateTime() : found.first().when;
        QMetaObject::invokeMethod(qApp, [self, folder, problem, restart, last] {
            if (!self)
                return;
            self->m_busy = false;
            self->m_problem = problem;
            if (QDir::fromNativeSeparators(self->m_folder) == folder)
                self->m_last = last;
            emit self->changed();
            if (restart)
                emit self->restartRequested();
        });
    });
}

// Off the GUI thread: the folder may be on a drive that answers slowly.
void Backup::readLast()
{
    m_last = {};
    if (m_folder.isEmpty())
        return;
    QThreadPool::globalInstance()->start([self = QPointer(this), folder = m_folder] {
        const QList<backupfiles::Found> found = backupfiles::list(folder);
        const QDateTime last = found.isEmpty() ? QDateTime() : found.first().when;
        QMetaObject::invokeMethod(qApp, [self, folder, last] {
            if (!self || self->m_folder != folder || self->m_last == last)
                return;
            self->m_last = last;
            emit self->changed();
        });
    });
}

void Backup::applyRestored()
{
    const QString staged = stagedFolder();
    QDir(staged + u".part"_s).removeRecursively(); // one cut short
    if (QFileInfo(staged).isDir())
        backupfiles::apply(staged, Settings::filePath(), Settings::dataDir());
}

} // namespace ws
