#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <memory>
#include <mutex>
#include <vector>

namespace ws::diagnostics {

// Only the three owned journal files and the last exported archive are
// cleaned. User files in the same folder are never enumerated or removed.
class Log {
public:
    struct Limits {
        qint64 fileBytes = 1 << 20;
        qint64 entryBytes = 8 << 10;
        int days = 7;
    };
    struct File {
        QString name;
        QByteArray bytes;
    };
    explicit Log(QString folder, Limits limits = {});
    bool append(const QString& event, QJsonObject details = {});
    std::vector<File> snapshot(); // bounded copies, consistent with rotation
    QString folder() const { return m_folder; }
    QString archivePath() const;
    void prune();

private:
    void pruneLocked();
    QString path(int generation) const;
    const QString m_folder;
    const Limits m_limits;
    std::mutex m_mutex;
};

void initialize(const QString& dataFolder); // empty disables it (isolated tests)
std::shared_ptr<Log> current();
bool enabled();
void record(const QString& event, QJsonObject details = {});

} // namespace ws::diagnostics
