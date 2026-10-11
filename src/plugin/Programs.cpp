#include "Programs.h"

#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QSettings>
#include <QStandardPaths>

#include <map>

using namespace Qt::StringLiterals;

namespace ws::plugin::programs {

namespace {

QMutex g_mutex;
std::map<Program, QString> g_overrides;
std::map<Program, QString> g_found; // looked up once

QString onPath(const QString& name)
{
    // The Store's python.exe under WindowsApps is a stub that opens the
    // Store (or, with Python from the Store, an app-execution alias that
    // starts python as a child): not what a job allowing one process wants.
    const QString found = QStandardPaths::findExecutable(name);
    if (found.isEmpty() || found.contains(u"\\WindowsApps\\"_s, Qt::CaseInsensitive)
        || found.contains(u"/WindowsApps/"_s, Qt::CaseInsensitive))
        return {};
    return QDir::toNativeSeparators(found);
}

QString pythonFromRegistry()
{
    QString best;
    QString bestVersion;
    for (const QString& root : {u"HKEY_CURRENT_USER\\Software\\Python\\PythonCore"_s, u"HKEY_LOCAL_MACHINE\\Software\\Python\\PythonCore"_s}) {
        const QSettings core(root, QSettings::NativeFormat);
        for (const QString& version : core.childGroups()) {
            const QSettings install(root + u'\\' + version + u"\\InstallPath"_s, QSettings::NativeFormat);
            QString exe = install.value(u"ExecutablePath"_s).toString();
            if (exe.isEmpty()) {
                const QString dir = install.value(u"Default"_s).toString();
                if (!dir.isEmpty())
                    exe = QDir(dir).filePath(u"python.exe"_s);
            }
            if (exe.isEmpty() || !QFileInfo::exists(exe))
                continue;
            if (best.isEmpty() || compareVersions(version, bestVersion) > 0) {
                best = QDir::toNativeSeparators(exe);
                bestVersion = version;
            }
        }
    }
    return best;
}

QString lookUp(Program program)
{
    switch (program) {
    case Program::Python: {
        const QString registry = pythonFromRegistry();
        return registry.isEmpty() ? onPath(u"python"_s) : registry;
    }
    case Program::Pwsh: {
        QString found = onPath(u"pwsh"_s);
        if (found.isEmpty()) {
            const QString fixed = qEnvironmentVariable("ProgramFiles") + u"\\PowerShell\\7\\pwsh.exe"_s;
            if (QFileInfo::exists(fixed))
                found = fixed;
        }
        return found;
    }
    case Program::PowerShell: {
        const QString fixed = qEnvironmentVariable("SystemRoot") + u"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"_s;
        return QFileInfo::exists(fixed) ? fixed : onPath(u"powershell"_s);
    }
    case Program::Node: {
        QString found = onPath(u"node"_s);
        if (found.isEmpty()) {
            const QString fixed = qEnvironmentVariable("ProgramFiles") + u"\\nodejs\\node.exe"_s;
            if (QFileInfo::exists(fixed))
                found = fixed;
        }
        return found;
    }
    case Program::Exe:
        break;
    }
    return {};
}

} // namespace

QString find(Program program, QString* hint)
{
    const QMutexLocker lock(&g_mutex);
    if (const auto it = g_overrides.find(program); it != g_overrides.end() && !it->second.isEmpty())
        return it->second;
    auto it = g_found.find(program);
    if (it == g_found.end())
        it = g_found.emplace(program, lookUp(program)).first;
    if (it->second.isEmpty() && hint) {
        switch (program) {
        case Program::Python: *hint = u"需要 Python 3：https://www.python.org/downloads/"_s; break;
        case Program::Pwsh: *hint = u"需要 PowerShell 7：https://aka.ms/powershell"_s; break;
        case Program::PowerShell: *hint = u"找不到 Windows PowerShell"_s; break;
        case Program::Node: *hint = u"需要 Node.js：https://nodejs.org/"_s; break;
        case Program::Exe: break;
        }
    }
    return it->second;
}

void setOverride(Program program, const QString& path)
{
    const QMutexLocker lock(&g_mutex);
    g_overrides[program] = path;
    g_found.erase(program);
}

std::optional<Launch> launchFor(const Runtime& runtime, const QString& pluginDir, QString* error)
{
    Launch launch;
    const QDir dir(pluginDir);
    launch.cwd = QDir::toNativeSeparators(runtime.cwd.isEmpty() ? dir.absolutePath() : dir.absoluteFilePath(runtime.cwd));
    const QString script = runtime.script.isEmpty() ? QString() : QDir::toNativeSeparators(dir.absoluteFilePath(runtime.script));
    if (runtime.program == Program::Exe) {
        launch.exe = QDir::toNativeSeparators(dir.absoluteFilePath(runtime.programPath));
        if (!QFileInfo::exists(launch.exe)) {
            if (error)
                *error = u"插件里没有 %1"_s.arg(runtime.programPath);
            return std::nullopt;
        }
        if (!script.isEmpty())
            launch.args.push_back(script);
    } else {
        QString hint;
        launch.exe = find(runtime.program, &hint);
        if (launch.exe.isEmpty()) {
            if (error)
                *error = hint;
            return std::nullopt;
        }
        if (!QFileInfo::exists(script)) {
            if (error)
                *error = u"插件里没有 %1"_s.arg(runtime.script);
            return std::nullopt;
        }
        switch (runtime.program) {
        case Program::Python:
            // -u: lines reach the pipe as they are printed (a pipe is block
            // buffered otherwise, and a resident plugin's answers would
            // sit in the buffer). -X utf8: UTF-8 stdio whatever the code page.
            launch.args = {u"-u"_s, u"-X"_s, u"utf8"_s, script};
            launch.env = {{u"PYTHONIOENCODING"_s, u"utf-8"_s}, {u"PYTHONUTF8"_s, u"1"_s}, {u"PYTHONDONTWRITEBYTECODE"_s, u"1"_s},
                {u"PYTHONUNBUFFERED"_s, u"1"_s}};
            break;
        case Program::Pwsh:
        case Program::PowerShell:
            launch.args = {u"-NoProfile"_s, u"-NonInteractive"_s, u"-ExecutionPolicy"_s, u"Bypass"_s, u"-File"_s, script};
            break;
        case Program::Node:
            launch.args = {script};
            break;
        case Program::Exe:
            break;
        }
    }
    launch.args += runtime.args;
    return launch;
}

} // namespace ws::plugin::programs
