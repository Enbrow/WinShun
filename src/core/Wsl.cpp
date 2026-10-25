#include "Wsl.h"

#include "Unc.h"
#include "Win32Util.h"

#include <QDir>
#include <QProcess>
#include <QStringDecoder>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws::wsl {
namespace {

using Clock = std::chrono::steady_clock;
struct Snapshot {
    std::vector<Distribution> registered;
    std::vector<std::wstring> running;
    bool valid = false;
    Clock::time_point at;
};
std::atomic<std::shared_ptr<const Snapshot>> current;
std::mutex probeMutex; // only background callers wait here; cachedState never does

bool includes(const std::vector<std::wstring>& names, std::wstring_view name)
{
    return std::ranges::any_of(names, [&](const auto& n) { return win32::equalsIgnoreCase(n, name); });
}

State stateOf(const Snapshot& snapshot, std::wstring_view name)
{
    if (name.empty())
        return State::Unknown;
    if (std::ranges::none_of(
            snapshot.registered, [&](const Distribution& d) { return win32::equalsIgnoreCase(d.name, name); }))
        return State::NotRegistered;
    if (!snapshot.valid)
        return State::Unknown;
    return includes(snapshot.running, name) ? State::Running : State::Stopped;
}

} // namespace

std::vector<Distribution> distributions()
{
    win32::UniqueKey key;
    if (::RegOpenKeyExW(
            HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, key.out())
        != ERROR_SUCCESS)
        return {};
    std::vector<Distribution> names;
    wchar_t subkey[256];
    for (DWORD i = 0;; ++i) {
        DWORD length = static_cast<DWORD>(std::size(subkey));
        const auto result = ::RegEnumKeyExW(key.get(), i, subkey, &length, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS)
            break;
        if (result != ERROR_SUCCESS)
            continue;
        wchar_t name[256];
        DWORD bytes = sizeof(name);
        if (::RegGetValueW(key.get(), subkey, L"DistributionName", RRF_RT_REG_SZ, nullptr, name, &bytes)
                == ERROR_SUCCESS
            && name[0] != L'\0') {
            DWORD uid = ~0u;
            DWORD uidBytes = sizeof(uid);
            ::RegGetValueW(key.get(), subkey, L"DefaultUid", RRF_RT_REG_DWORD, nullptr, &uid, &uidBytes);
            names.push_back({name, uid == 0});
        }
    }
    std::ranges::sort(names, {}, &Distribution::name);
    return names;
}

QString executable()
{
    // Use the system binary, not a program with this name in the current directory.
    wchar_t system[MAX_PATH];
    const UINT length = ::GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
    return length != 0 && length < std::size(system)
        ? QDir::toNativeSeparators(QString::fromWCharArray(system, static_cast<int>(length)) + u"/wsl.exe"_s)
        : QString();
}

std::vector<std::wstring> parseNames(QByteArrayView output)
{
    const bool utf16 = output.contains('\0') || output.startsWith(QByteArrayView("\xFF\xFE", 2));
    const QString text = utf16 ? QStringDecoder(QStringDecoder::Utf16LE)(output) : QString::fromUtf8(output);
    std::vector<std::wstring> names;
    for (QString line : text.split(u'\n', Qt::SkipEmptyParts)) {
        line.remove(QChar(0xFEFF));
        line = line.trimmed();
        if (!line.isEmpty())
            names.push_back(line.toStdWString());
    }
    return names;
}

State cachedState(std::wstring_view path)
{
    const auto snapshot = current.load();
    return snapshot ? stateOf(*snapshot, unc::wslDistribution(path)) : State::Unknown;
}

State probe(std::wstring_view path, bool fresh)
{
    const auto distro = unc::wslDistribution(path);
    if (distro.empty())
        return State::Unknown;
    std::lock_guard lock(probeMutex);
    if (const auto snapshot = current.load(); snapshot && !fresh && Clock::now() - snapshot->at < 5s)
        return stateOf(*snapshot, distro);
    auto next = std::make_shared<Snapshot>();
    next->registered = distributions();
    if (!next->registered.empty()) {
        QProcess process;
        process.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
        process.start(executable(), {u"--list"_s, u"--running"_s, u"--quiet"_s});
        if (process.waitForStarted(1000) && process.waitForFinished(2000)
            && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
            next->running = parseNames(process.readAllStandardOutput());
            next->valid = true;
        }
        if (process.state() != QProcess::NotRunning) {
            process.kill(); // our read-only CLI, never the distribution
            process.waitForFinished(1000);
        }
    }
    next->at = Clock::now();
    const State answer = stateOf(*next, distro);
    current.store(std::move(next));
    return answer;
}

} // namespace ws::wsl
