#include "ShellMenu.h"

#include "AutostartRun.h" // runCommandProgram: the program a command line starts

#include <QCoreApplication>
#include <QDir>

#include <windows.h>
#include <shlobj.h>

#include <string>
#include <string_view>

namespace ws::shellmenu {

namespace {

// Where the verb goes: a folder, a folder's empty space (%V is the folder
// shown), a drive's root.
constexpr const wchar_t* kKeys[] {
    L"Software\\Classes\\Directory\\shell\\WinShun.SearchHere",
    L"Software\\Classes\\Directory\\Background\\shell\\WinShun.SearchHere",
    L"Software\\Classes\\Drive\\shell\\WinShun.SearchHere",
};

std::wstring readString(HKEY root, const std::wstring& key, const wchar_t* name)
{
    DWORD bytes = 0;
    if (::RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0)
        return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
        return {};
    value.resize(::wcsnlen(value.c_str(), value.size()));
    return value;
}

// Whether it was written (false when it was so already, or could not be).
bool writeString(const std::wstring& key, const wchar_t* name, const std::wstring& value)
{
    if (readString(HKEY_CURRENT_USER, key, name) == value)
        return false;
    return ::RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
               static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)))
        == ERROR_SUCCESS;
}

// Whether it was there and is now gone.
bool deleteValue(const std::wstring& key, const wchar_t* name)
{
    return ::RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), name) == ERROR_SUCCESS;
}

} // namespace

void apply(bool on, const QString& title)
{
    const std::wstring folder = QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toStdWString();
    const std::wstring stub = folder + L"\\WinShunSearch.exe";
    const std::wstring command = L"\"" + stub + L"\" --search-here \"%V\"";
    if (on && ::GetFileAttributesW(stub.c_str()) == INVALID_FILE_ATTRIBUTES)
        return; // a build without it (run from the build folder)
    bool changed = false;
    for (const wchar_t* key : kKeys) {
        const std::wstring commandKey = std::wstring(key) + L"\\command";
        if (!on) {
            // Only ours: a portable copy elsewhere may have set them since.
            // A copy that is gone (a portable folder deleted) left a verb
            // that does nothing: that goes too.
            const std::wstring current = readString(HKEY_CURRENT_USER, commandKey, nullptr);
            const std::wstring program = autostart::runCommandProgram(current);
            if (!current.empty()
                && (current == command
                    || (!program.empty() && ::GetFileAttributesW(program.c_str()) == INVALID_FILE_ATTRIBUTES)))
                changed |= ::RegDeleteTreeW(HKEY_CURRENT_USER, key) == ERROR_SUCCESS;
            continue;
        }
        changed |= writeString(key, L"MUIVerb", title.toStdWString());
        changed |= writeString(key, L"Icon", folder + L"\\WinShun.exe,0");
        // Empty space has nothing selected, so "Single" hides the verb there
        // (0.8.0 wrote it everywhere; take it back off).
        if (std::wstring_view(key).find(L"\\Background\\") == std::wstring_view::npos)
            changed |= writeString(key, L"MultiSelectModel", L"Single");
        else
            changed |= deleteValue(key, L"MultiSelectModel");
        changed |= writeString(commandKey, nullptr, command);
    }
    if (changed)
        ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

} // namespace ws::shellmenu
