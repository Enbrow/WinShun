#pragma once

#include <QByteArrayView>
#include <QString>

#include <string>
#include <string_view>
#include <vector>

namespace ws::wsl {

enum class State { Unknown, Running, Stopped, NotRegistered };
struct Distribution {
    std::wstring name;
    bool rootUser = false;
};

// Registry only: safe on the UI thread, and never starts a distribution.
std::vector<Distribution> distributions();
QString executable();

// wsl.exe writes UTF-16LE when redirected; newer versions may use UTF-8.
std::vector<std::wstring> parseNames(QByteArrayView output);

// Runs wsl --list --running --quiet with a bounded wait. Background only.
// One cached answer is shared by all folders/distributions for five seconds.
State probe(std::wstring_view path, bool fresh = false);
// Last answer only; never starts a process or waits for a probe in progress.
State cachedState(std::wstring_view path);

} // namespace ws::wsl
