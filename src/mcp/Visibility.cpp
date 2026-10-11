#include "Visibility.h"

#include "Win32Util.h"

#include <QDir>
#include <QElapsedTimer>

#include <aclapi.h>

namespace ws::mcp {

Visibility::Visibility(HANDLE token)
    : m_token(token ? token : defaultToken())
{
}

HANDLE Visibility::defaultToken()
{
    static const HANDLE token = [] {
        HANDLE process = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process))
            return static_cast<HANDLE>(nullptr);
        HANDLE result = nullptr;
        DWORD size = 0;
        TOKEN_ELEVATION_TYPE elevation = TokenElevationTypeDefault;
        if (::GetTokenInformation(process, TokenElevationType, &elevation, sizeof elevation, &size)
            && elevation == TokenElevationTypeFull) {
            // Elevated: the linked token is the same user without the
            // administrator group and privileges (identification level,
            // which AccessCheck is content with).
            TOKEN_LINKED_TOKEN linked {};
            if (::GetTokenInformation(process, TokenLinkedToken, &linked, sizeof linked, &size))
                result = linked.LinkedToken;
        }
        if (!result) {
            ::DuplicateTokenEx(process, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE, nullptr, SecurityImpersonation,
                TokenImpersonation, &result);
        }
        ::CloseHandle(process);
        return result;
    }();
    return token;
}

QString Visibility::parentOf(const QString& path)
{
    QString p = QDir::toNativeSeparators(path);
    while (p.size() > 3 && p.endsWith(u'\\'))
        p.chop(1);
    if (p.size() <= 3) // "C:\", "C:"
        return p.size() == 2 ? p + u'\\' : p;
    const qsizetype at = p.lastIndexOf(u'\\');
    if (at < 0)
        return p;
    QString parent = p.left(at);
    if (parent.size() == 2 && parent[1] == u':')
        parent.append(u'\\');
    return parent;
}

bool Visibility::check(const QString& path, DWORD desired)
{
    if (!m_token)
        return false;
    QElapsedTimer timer;
    timer.start();
    ++m_checks;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    // The label too: AccessCheck weighs the integrity policy when it is there.
    const DWORD error = ::GetNamedSecurityInfoW(win32::longPath(path.toStdWString()).c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &descriptor);
    bool allowed = false;
    if (error == ERROR_SUCCESS) {
        GENERIC_MAPPING mapping {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        DWORD access = desired;
        ::MapGenericMask(&access, &mapping);
        PRIVILEGE_SET privileges {};
        DWORD privilegesSize = sizeof privileges;
        DWORD granted = 0;
        BOOL status = FALSE;
        allowed = ::AccessCheck(descriptor, m_token, access, &mapping, &privileges, &privilegesSize, &granted, &status) && status;
        ::LocalFree(descriptor);
    }
    // Else: not even its security descriptor can be read (a protected system
    // folder): not shown.
    m_us += timer.nsecsElapsed() / 1000;
    return allowed;
}

bool Visibility::canList(const QString& folder)
{
    const QString key = QDir::toNativeSeparators(folder).toCaseFolded();
    if (const auto it = m_folders.constFind(key); it != m_folders.cend())
        return it.value();
    const bool allowed = check(folder, FILE_LIST_DIRECTORY);
    m_folders.insert(key, allowed);
    return allowed;
}

bool Visibility::canRead(const QString& file)
{
    return check(file, FILE_READ_DATA);
}

bool Visibility::canReadAttributes(const QString& path)
{
    return check(path, FILE_READ_ATTRIBUTES);
}

bool Visibility::sees(const QString& path)
{
    return canList(parentOf(path));
}

} // namespace ws::mcp
