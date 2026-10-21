#pragma once

#include <cwctype>
#include <string>

namespace ws::autostart {

// The autostart that stands in for the logon task when Task Scheduler will not
// take it (GitHub #52: the name "WinShun" belongs to another account's task on
// this computer, or a policy forbids tasks): the value "WinShun" under
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run, `"<exe>" --background`.
constexpr wchar_t kRunValueName[] = L"WinShun";

inline std::wstring runCommand(const std::wstring& exe)
{
    return L"\"" + exe + L"\" --background";
}

// The program a Run value starts: the quoted path, else up to the first space.
inline std::wstring runCommandProgram(const std::wstring& command)
{
    std::size_t begin = 0;
    while (begin < command.size() && command[begin] == L' ')
        ++begin;
    if (begin >= command.size())
        return {};
    if (command[begin] == L'"') {
        const std::size_t close = command.find(L'"', begin + 1);
        return command.substr(begin + 1, close == std::wstring::npos ? std::wstring::npos : close - begin - 1);
    }
    const std::size_t space = command.find(L' ', begin);
    return command.substr(begin, space == std::wstring::npos ? std::wstring::npos : space - begin);
}

// Whether a Run value starts `exe` (a path compares without regard to case).
inline bool runCommandStarts(const std::wstring& command, const std::wstring& exe)
{
    const std::wstring program = runCommandProgram(command);
    if (program.empty() || program.size() != exe.size())
        return false;
    for (std::size_t i = 0; i < program.size(); ++i) {
        if (std::towlower(program[i]) != std::towlower(exe[i]))
            return false;
    }
    return true;
}

} // namespace ws::autostart
