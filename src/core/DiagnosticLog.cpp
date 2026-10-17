#include "DiagnosticLog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include <algorithm>
#include <atomic>

using namespace Qt::StringLiterals;

namespace ws::diagnostics {
namespace {
std::atomic<std::shared_ptr<Log>> g_log;
constexpr int kBackups = 2;
} // namespace

Log::Log(QString folder, Limits limits)
    : m_folder(QDir::cleanPath(std::move(folder)))
    , m_limits {std::max<qint64>(256, limits.fileBytes),
          std::clamp<qint64>(limits.entryBytes, 256, std::max<qint64>(256, limits.fileBytes)), std::max(1, limits.days)}
{
    QDir().mkpath(m_folder);
    prune();
}

QString Log::path(int generation) const
{
    return QDir(m_folder).filePath(generation == 0 ? u"events.jsonl"_s : u"events.%1.jsonl"_s.arg(generation));
}

QString Log::archivePath() const
{
    return QDir(m_folder).filePath(u"WinShun-diagnostics.zip"_s);
}

void Log::pruneLocked()
{
    const QDateTime oldest = QDateTime::currentDateTimeUtc().addDays(-m_limits.days);
    for (int i = 0; i <= kBackups; ++i) {
        const QFileInfo file(path(i));
        if (file.exists() && file.lastModified().toUTC() < oldest)
            QFile::remove(file.filePath());
    }
    const QFileInfo archive(archivePath());
    if (archive.exists() && archive.lastModified().toUTC() < oldest)
        QFile::remove(archive.filePath());
}

void Log::prune()
{
    const std::lock_guard lock(m_mutex);
    pruneLocked();
}

bool Log::append(const QString& event, QJsonObject details)
{
    QJsonObject entry {{u"time"_s, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {u"event"_s, event.left(128)}, {u"details"_s, details}};
    QByteArray bytes = QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > m_limits.entryBytes) {
        // Keep valid UTF-8 and valid JSON even for a giant Qt error message.
        const QString message = details.value(u"message"_s).toString();
        entry[u"details"_s] = QJsonObject {{u"truncated"_s, true}};
        if (!message.isEmpty())
            entry[u"details"_s] = QJsonObject {{u"truncated"_s, true}, {u"message"_s, message.left(512)}};
        bytes = QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
        if (bytes.size() > m_limits.entryBytes) {
            entry[u"details"_s] = QJsonObject {{u"truncated"_s, true}};
            entry[u"event"_s] = u"truncated-event"_s;
            bytes = QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
        }
    }
    const std::lock_guard lock(m_mutex);
    if (!QDir().mkpath(m_folder))
        return false;
    pruneLocked();
    for (int i = 0; i <= kBackups; ++i) {
        const QFileInfo target(path(i));
        if (target.exists() && !target.isFile())
            return false;
    }
    if (QFileInfo(path(0)).size() + bytes.size() > m_limits.fileBytes) {
        QFile::remove(path(kBackups));
        for (int i = kBackups - 1; i >= 0; --i) {
            if (QFileInfo::exists(path(i)) && !QFile::rename(path(i), path(i + 1)))
                return false; // do not grow an unrotatable file without bound
        }
    }
    QFile file(path(0));
    return file.open(QIODevice::WriteOnly | QIODevice::Append) && file.write(bytes) == bytes.size();
}

std::vector<Log::File> Log::snapshot()
{
    const std::lock_guard lock(m_mutex);
    pruneLocked();
    std::vector<File> files;
    for (int i = kBackups; i >= 0; --i) {
        QFile file(path(i));
        if (file.open(QIODevice::ReadOnly))
            files.push_back({QFileInfo(file).fileName(), file.read(m_limits.fileBytes)});
    }
    return files;
}

void initialize(const QString& dataFolder)
{
    g_log.store(dataFolder.isEmpty() ? nullptr : std::make_shared<Log>(QDir(dataFolder).filePath(u"diagnostics"_s)));
}

std::shared_ptr<Log> current()
{
    return g_log.load();
}

bool enabled()
{
    return static_cast<bool>(g_log.load());
}

void record(const QString& event, QJsonObject details)
{
    if (const auto log = current())
        log->append(event, std::move(details));
}
} // namespace ws::diagnostics
