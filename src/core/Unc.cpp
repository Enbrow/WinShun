#include "Unc.h"

#include "Win32Util.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace ws::unc {

bool isUnc(std::wstring_view path) noexcept
{
    return path.size() > 2 && path.starts_with(L"\\\\") && path[2] != L'?' && path[2] != L'.' && path[2] != L'\\';
}

std::wstring normalize(std::wstring_view path)
{
    std::wstring out;
    out.reserve(path.size());
    for (const wchar_t c : path) {
        const wchar_t s = c == L'/' ? L'\\' : c;
        if (s == L'\\' && out.size() >= 2 && out.back() == L'\\')
            continue; // "\\host\\share" (a path copied from source code)
        out.push_back(s);
    }
    while (out.size() > 2 && out.back() == L'\\')
        out.pop_back();
    if (!isUnc(out))
        return {};
    const std::size_t slash = out.find(L'\\', 2);
    if (slash == std::wstring::npos || slash == 2 || slash + 1 >= out.size())
        return {}; // no share
    if (std::any_of(out.begin(), out.end(), [](wchar_t c) { return c < 32 || std::wstring_view(L"<>\"|?*").find(c) != std::wstring_view::npos; }))
        return {};
    return out;
}

std::wstring_view host(std::wstring_view path) noexcept
{
    if (!isUnc(path))
        return {};
    const std::size_t slash = path.find(L'\\', 2);
    return path.substr(2, slash == std::wstring_view::npos ? std::wstring_view::npos : slash - 2);
}

std::wstring_view shareRoot(std::wstring_view path) noexcept
{
    if (!isUnc(path))
        return {};
    const std::size_t slash = path.find(L'\\', 2);
    if (slash == std::wstring_view::npos)
        return {};
    return path.substr(0, path.find(L'\\', slash + 1));
}

bool contains(std::wstring_view folder, std::wstring_view path) noexcept
{
    if (path.size() < folder.size() || (path.size() > folder.size() && path[folder.size()] != L'\\'))
        return false;
    return win32::equalsIgnoreCase(path.substr(0, folder.size()), folder);
}

namespace {

std::wstring readString(HKEY key, const wchar_t* subKey, const wchar_t* value)
{
    DWORD bytes = 0;
    if (::RegGetValueW(key, subKey, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < 2)
        return {};
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(key, subKey, value, RRF_RT_REG_SZ, nullptr, text.data(), &bytes) != ERROR_SUCCESS)
        return {};
    text.resize(::wcsnlen(text.c_str(), text.size()));
    return text;
}

// The names of a key's subkeys, the most recently written first.
std::vector<std::wstring> subKeys(HKEY parent, const wchar_t* path)
{
    win32::UniqueKey key;
    if (::RegOpenKeyExW(parent, path, 0, KEY_READ, key.out()) != ERROR_SUCCESS)
        return {};
    struct Sub {
        std::wstring name;
        std::int64_t written;
    };
    std::vector<Sub> subs;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD length = static_cast<DWORD>(std::size(name));
        FILETIME written {};
        const LSTATUS status = ::RegEnumKeyExW(key.get(), i, name, &length, nullptr, nullptr, nullptr, &written);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status == ERROR_SUCCESS)
            subs.push_back({std::wstring(name, length), win32::ticks(written)});
    }
    std::stable_sort(subs.begin(), subs.end(), [](const Sub& a, const Sub& b) { return a.written > b.written; });
    std::vector<std::wstring> names;
    for (Sub& s : subs)
        names.push_back(std::move(s.name));
    return names;
}

} // namespace

std::wstring fromMappedDrive(std::wstring_view path)
{
    if (path.size() < 2 || path[1] != L':' || !std::iswalpha(path[0]) || (path.size() > 2 && path[2] != L'\\'))
        return {};
    const std::wstring key = L"Network\\" + std::wstring(1, static_cast<wchar_t>(std::towupper(path[0])));
    const std::wstring remote = normalize(readString(HKEY_CURRENT_USER, key.c_str(), L"RemotePath"));
    if (remote.empty())
        return {};
    return normalize(remote + std::wstring(path.substr(2)));
}

std::vector<Known> knownShares()
{
    std::vector<Known> known;
    const auto add = [&](std::wstring path, std::wstring drive) {
        path = normalize(path);
        if (path.empty())
            return;
        const auto same = [&](const Known& k) { return win32::equalsIgnoreCase(k.path, path); };
        if (const auto it = std::find_if(known.begin(), known.end(), same); it != known.end()) {
            if (it->drive.empty())
                it->drive = std::move(drive);
            return;
        }
        known.push_back({std::move(path), std::move(drive)});
    };
    // Mapped drives: "Network\Z" with RemotePath.
    for (const std::wstring& letter : subKeys(HKEY_CURRENT_USER, L"Network")) {
        if (letter.size() != 1)
            continue;
        const std::wstring key = L"Network\\" + letter;
        add(readString(HKEY_CURRENT_USER, key.c_str(), L"RemotePath"), letter + L':');
    }
    // Shares Explorer has opened: "##host#share" ('#' for '\').
    for (std::wstring name : subKeys(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MountPoints2")) {
        if (!name.starts_with(L"##"))
            continue;
        std::replace(name.begin(), name.end(), L'#', L'\\');
        add(std::move(name), {});
    }
    return known;
}

} // namespace ws::unc
