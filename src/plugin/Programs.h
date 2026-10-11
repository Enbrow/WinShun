#pragma once

// Where the interpreters a manifest names are (the plugin design, §5.4),
// and the command line and environment each one wants.

#include "Manifest.h"

#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

namespace ws::plugin::programs {

// The program's path; empty when it is not installed (`hint` says what
// to install). `python` is python.exe itself, never py.exe: the launcher
// starts python.exe as a child, which a job allowing one process forbids.
QString find(Program program, QString* hint = nullptr);
void setOverride(Program program, const QString& path); // the settings' choice; empty: look again

struct Launch {
    QString exe;
    QStringList args;
    QString cwd;
    QList<std::pair<QString, QString>> env; // what the interpreter needs (PYTHONIOENCODING …)
};
// How to start `runtime` in `pluginDir`; nothing with `error` when the
// program is missing.
std::optional<Launch> launchFor(const Runtime& runtime, const QString& pluginDir, QString* error);

} // namespace ws::plugin::programs
