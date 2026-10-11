#include "Credentials.h"

#include "Unc.h"

#include <combaseapi.h>
#include <wincred.h>
#include <winnetwk.h>

#include <vector>

namespace ws::credentials {

namespace {

// The connections this logon session has to `host` ("\\host\..."), by name.
std::vector<std::wstring> connectionsTo(std::wstring_view host)
{
    std::vector<std::wstring> found;
    HANDLE e = nullptr;
    if (::WNetOpenEnumW(RESOURCE_CONNECTED, RESOURCETYPE_ANY, 0, nullptr, &e) != NO_ERROR)
        return found;
    const std::wstring prefix = L"\\\\" + std::wstring(host) + L"\\";
    std::vector<std::byte> buffer(16 * 1024);
    for (;;) {
        DWORD count = static_cast<DWORD>(-1);
        DWORD size = static_cast<DWORD>(buffer.size());
        if (::WNetEnumResourceW(e, &count, buffer.data(), &size) != NO_ERROR)
            break;
        const auto* items = reinterpret_cast<const NETRESOURCEW*>(buffer.data());
        for (DWORD i = 0; i < count; ++i) {
            const std::wstring_view remote = items[i].lpRemoteName ? items[i].lpRemoteName : L"";
            if (remote.size() > prefix.size() && ::_wcsnicmp(remote.data(), prefix.c_str(), prefix.size()) == 0)
                found.emplace_back(remote);
        }
    }
    ::WNetCloseEnum(e);
    return found;
}

} // namespace

Entered::~Entered()
{
    ::SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
}

bool prompt(HWND owner, const std::wstring& caption, const std::wstring& message, unsigned long error,
    std::wstring_view user, Entered& entered)
{
    // The name to start with goes in packed, as Windows' own dialogs take it.
    std::vector<std::byte> in;
    const std::wstring startUser(user);
    if (!startUser.empty()) {
        DWORD size = 0;
        ::CredPackAuthenticationBufferW(0, const_cast<LPWSTR>(startUser.c_str()), const_cast<LPWSTR>(L""), nullptr, &size);
        in.resize(size);
        if (size == 0
            || !::CredPackAuthenticationBufferW(0, const_cast<LPWSTR>(startUser.c_str()), const_cast<LPWSTR>(L""),
                reinterpret_cast<PBYTE>(in.data()), &size))
            in.clear();
    }

    CREDUI_INFOW info {sizeof(info)};
    info.hwndParent = owner;
    info.pszCaptionText = caption.c_str();
    info.pszMessageText = message.c_str();
    ULONG package = 0;
    void* out = nullptr;
    ULONG outSize = 0;
    BOOL save = TRUE; // ticked: it saves typing it in File Explorer as well
    const DWORD result = ::CredUIPromptForWindowsCredentialsW(&info, error, &package, in.empty() ? nullptr : in.data(),
        static_cast<ULONG>(in.size()), &out, &outSize, &save, CREDUIWIN_GENERIC | CREDUIWIN_CHECKBOX);
    if (result != ERROR_SUCCESS || !out)
        return false;

    std::wstring name(CREDUI_MAX_USERNAME_LENGTH + 1, L'\0');
    std::wstring domain(CREDUI_MAX_DOMAIN_TARGET_LENGTH + 1, L'\0');
    std::wstring password(CREDUI_MAX_PASSWORD_LENGTH + 1, L'\0');
    DWORD nameLength = static_cast<DWORD>(name.size());
    DWORD domainLength = static_cast<DWORD>(domain.size());
    DWORD passwordLength = static_cast<DWORD>(password.size());
    const bool unpacked = ::CredUnPackAuthenticationBufferW(0, out, outSize, name.data(), &nameLength, domain.data(),
        &domainLength, password.data(), &passwordLength);
    ::SecureZeroMemory(out, outSize);
    ::CoTaskMemFree(out);
    if (unpacked) {
        name.resize(wcsnlen(name.c_str(), name.size()));
        domain.resize(wcsnlen(domain.c_str(), domain.size()));
        entered.user = domain.empty() ? name : domain + L"\\" + name;
        entered.password.assign(password.c_str(), wcsnlen(password.c_str(), password.size()));
        entered.save = save != FALSE;
    }
    ::SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    return unpacked && !entered.user.empty();
}

unsigned long connect(std::wstring_view path, const Entered& entered)
{
    const std::wstring_view host = unc::host(path);
    if (host.empty())
        return ERROR_BAD_NETPATH;
    // The share itself (a folder in it is no resource), or the server's IPC$.
    std::wstring remote(unc::shareRoot(path));
    if (remote.empty())
        remote = L"\\\\" + std::wstring(host) + L"\\IPC$";
    NETRESOURCEW resource {};
    resource.dwType = RESOURCETYPE_ANY;
    resource.lpRemoteName = remote.data();
    DWORD error = ::WNetAddConnection2W(&resource, entered.password.c_str(), entered.user.c_str(), 0);
    if (error == ERROR_SESSION_CREDENTIAL_CONFLICT) {
        // Windows holds one name per server and session: the connections
        // under the other one go first (made by Win顺, or mirrored from
        // Explorer's; Explorer's own are not touched).
        for (const std::wstring& c : connectionsTo(host))
            ::WNetCancelConnection2W(c.c_str(), 0, TRUE);
        error = ::WNetAddConnection2W(&resource, entered.password.c_str(), entered.user.c_str(), 0);
    }
    if (error != NO_ERROR || !entered.save)
        return error;

    // What Explorer keeps when "remember" is ticked: a Windows credential
    // for the server, named as the path names it (an IP address stays one).
    const std::wstring target(host);
    CREDENTIALW credential {};
    credential.Type = CRED_TYPE_DOMAIN_PASSWORD;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.UserName = const_cast<LPWSTR>(entered.user.c_str());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(entered.password.data()));
    credential.CredentialBlobSize = static_cast<DWORD>(entered.password.size() * sizeof(wchar_t));
    credential.Persist = CRED_PERSIST_ENTERPRISE;
    ::CredWriteW(&credential, 0); // connected anyway; next time it is asked again
    return NO_ERROR;
}

} // namespace ws::credentials
