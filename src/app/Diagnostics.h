#pragma once

#include "DiagnosticLog.h"

#include <QJsonObject>
#include <QString>

namespace ws::diagnostics {
struct Exported {
    QString path;
    QString error;
};
QJsonObject environment(); // no account names, paths, queries or settings secrets
Exported exportArchive(Log& log, const QString& dataFolder, const QString& serviceFolder, QJsonObject context);
} // namespace ws::diagnostics
