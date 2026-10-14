#include "RecentFolders.h"

#include <QDir>
#include <QFileInfo>

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace Qt::StringLiterals;

namespace ws::recentfolders {

namespace {

// "C:\Users\me\file.txt" -> "C:\Users\me"; "C:\file.txt" -> "C:\".
QString parentOf(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(u'\\');
    return slash < 0 ? QString() : path.left(slash == 2 ? 3 : slash);
}

// The folders of what Windows lists as opened lately, newest first: the
// shortcuts in the Recent folder, which the file dialogs of all programs add
// to (what they open and save), and File Explorer too (the folders opened
// in it). Empty while Windows keeps no such list. Needs COM.
QStringList windowsRecentFolders(int max, const LocalDrives& drives)
{
    PWSTR dir = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_Recent, KF_FLAG_DONT_VERIFY, nullptr, &dir)))
        return {};
    const QString recent = QString::fromWCharArray(dir);
    ::CoTaskMemFree(dir);
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))
        || FAILED(link.As(&file)))
        return {};
    const QFileInfoList shortcuts = QDir(recent).entryInfoList({u"*.lnk"_s}, QDir::Files, QDir::Time); // newest first
    std::wstring target(32768, L'\0');
    QStringList folders;
    for (const QFileInfo& shortcut : shortcuts.first(std::min<qsizetype>(shortcuts.size(), 100))) {
        if (folders.size() >= max)
            break;
        const std::wstring name = QDir::toNativeSeparators(shortcut.filePath()).toStdWString();
        if (FAILED(file->Load(name.c_str(), STGM_READ))
            || link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, 0) != S_OK)
            continue; // S_FALSE: no file behind it (Control Panel and the like)
        const QString folder = folderOf(QString::fromWCharArray(target.c_str()), drives);
        if (!folder.isEmpty() && !folders.contains(folder, Qt::CaseInsensitive))
            folders.append(folder);
    }
    return folders;
}

QStringList interleaved(const QStringList& a, const QStringList& b, int max)
{
    QStringList out;
    for (qsizetype i = 0; out.size() < max && (i < a.size() || i < b.size()); ++i) {
        for (const QStringList* list : {&a, &b}) {
            if (i < list->size() && out.size() < max && !out.contains(list->at(i), Qt::CaseInsensitive))
                out.append(list->at(i));
        }
    }
    return out;
}

} // namespace

LocalDrives::LocalDrives()
{
    const DWORD present = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(present & (1u << i)))
            continue;
        const wchar_t root[] {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
        const UINT type = ::GetDriveTypeW(root);
        if (type != DRIVE_REMOTE && type != DRIVE_NO_ROOT_DIR && type != DRIVE_UNKNOWN)
            m_mask |= 1u << i;
    }
}

bool LocalDrives::contain(const QString& path) const
{
    if (path.size() < 3 || path[1] != u':' || path[2] != u'\\')
        return false;
    const char16_t letter = path[0].toUpper().unicode();
    return letter >= u'A' && letter <= u'Z' && (m_mask & (1u << (letter - u'A')));
}

QString folderOf(const QString& path, const LocalDrives& drives)
{
    const DWORD attributes
        = drives.contain(path) ? ::GetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16())) : INVALID_FILE_ATTRIBUTES;
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return {};
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? path : parentOf(path);
}

QStringList recent(const QStringList& history, int max)
{
    const LocalDrives drives;
    // The folders of what was opened through Win顺 (or the folders themselves).
    QStringList ours;
    for (const QString& item : history) {
        if (ours.size() >= max)
            break;
        const QString folder = folderOf(QDir::toNativeSeparators(item), drives);
        if (!folder.isEmpty() && !ours.contains(folder, Qt::CaseInsensitive))
            ours.append(folder);
    }
    return interleaved(windowsRecentFolders(max, drives), ours, max);
}

} // namespace ws::recentfolders
