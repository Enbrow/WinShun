#include "FileManagers.h"

#include "FolderViewHit.h"
#include "Integrity.h"
#include "PathText.h"
#include "Shell.h"
#include "Win32Util.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QStringList>

#include <ole2.h> // before exdisp.h: WIN32_LEAN_AND_MEAN keeps it out of windows.h

#include <exdisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;
using namespace Qt::StringLiterals;

namespace ws::filemanager {

namespace {

constexpr UINT kTotalCommanderAsk = WM_USER + 50; // its interface for other programs
// XYplorer's WM_COPYDATA: dwData says what lpData is. A script to run (as
// its /script= switch takes it, "::" first); what its `copydata` command
// sends in mode 0, its answer to a window of ours.
constexpr ULONG_PTR kXYplorerScript = 0x00400001;
constexpr ULONG_PTR kXYplorerAnswer = 0x00400000;
constexpr DWORD kXYplorerWait = 3000; // ms for a script to finish: it answers while the message is being sent

// A window's own text, as Windows keeps it: no message to the program.
std::wstring caption(HWND hwnd)
{
    std::wstring text(static_cast<std::size_t>(::GetWindowTextLengthW(hwnd)) + 1, L'\0');
    text.resize(static_cast<std::size_t>(::GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()))));
    return text;
}

bool isFolder(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool isFile(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring quoted(const std::wstring& path)
{
    return L'"' + path + L'"';
}

std::wstring folderOf(const std::wstring& file)
{
    const std::size_t slash = file.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : file.substr(0, slash);
}

// The program file of the process a window belongs to.
std::wstring programOf(HWND window)
{
    DWORD pid = 0;
    if (!window || !::GetWindowThreadProcessId(window, &pid) || pid == 0)
        return {};
    const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!process.valid() || !::QueryFullProcessImageNameW(process.get(), 0, path.data(), &size))
        return {};
    path.resize(size);
    return path;
}

// A text value, %VARIABLES% expanded; empty when there is none. `flags`:
// which registry view (RRF_SUBKEY_WOW6432KEY for a 32-bit program's).
std::wstring registryText(HKEY root, const wchar_t* key, const wchar_t* value, DWORD flags = 0)
{
    flags |= RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    DWORD size = 0;
    if (::RegGetValueW(root, key, value, flags, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t))
        return {};
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(root, key, value, flags, nullptr, text.data(), &size) != ERROR_SUCCESS)
        return {};
    text.resize(::wcsnlen(text.c_str(), text.size()));
    return text;
}

std::wstring totalCommanderProgram()
{
    if (const HWND running = ::FindWindowW(L"TTOTAL_CMD", nullptr)) {
        if (std::wstring program = programOf(running); !program.empty())
            return program;
    }
    constexpr const wchar_t* kKey = L"Software\\Ghisler\\Total Commander";
    for (const std::wstring& dir : {registryText(HKEY_CURRENT_USER, kKey, L"InstallDir"),
             registryText(HKEY_LOCAL_MACHINE, kKey, L"InstallDir", RRF_SUBKEY_WOW6464KEY),
             registryText(HKEY_LOCAL_MACHINE, kKey, L"InstallDir", RRF_SUBKEY_WOW6432KEY)}) {
        if (dir.empty())
            continue;
        for (const wchar_t* name : {L"\\TOTALCMD64.EXE", L"\\TOTALCMD.EXE"}) {
            if (isFile(dir + name))
                return dir + name;
        }
    }
    return {};
}

std::wstring opusProgram()
{
    std::wstring dopus = programOf(::FindWindowW(L"dopus.lister", nullptr));
    constexpr const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\dopus.exe";
    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        if (dopus.empty())
            dopus = registryText(root, kKey, nullptr);
    }
    if (dopus.size() >= 2 && dopus.front() == L'"' && dopus.back() == L'"')
        dopus = dopus.substr(1, dopus.size() - 2);
    const std::wstring rt = folderOf(dopus) + L"\\dopusrt.exe";
    return !dopus.empty() && isFile(rt) ? rt : std::wstring();
}

// Starts `program` with the user's normal rights. `wait`: until it has
// handed its command over, so that the next one comes after it (a program
// that keeps running, a Total Commander started this way, is waited for a
// few seconds).
bool run(const std::wstring& program, const std::wstring& args, bool wait, const std::wstring& cwd = {})
{
    const win32::UniqueHandle process = shell::startUnelevated(program, args, cwd);
    if (!process.valid()) {
        qWarning() << "Could not start" << QString::fromStdWString(program);
        return false;
    }
    if (wait)
        ::WaitForSingleObject(process.get(), 3000);
    return true;
}

// The file name of a program's path, "XYplorer.exe".
std::wstring_view baseName(std::wstring_view path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

// Whether `window` is the main window of an XYplorer (a VB6 program: the
// form's class is any VB6 form's, so the program is checked too; the form
// is owned by the runtime's hidden ThunderRT6Main window, while XYplorer's
// own dialogs are owned by the form).
bool isXYplorerMain(HWND window)
{
    const HWND owner = ::GetWindow(window, GW_OWNER);
    wchar_t name[32] {};
    if (owner && (::GetClassNameW(owner, name, static_cast<int>(std::size(name))) == 0 || ::wcscmp(name, L"ThunderRT6Main") != 0))
        return false;
    return win32::equalsIgnoreCase(baseName(programOf(window)), L"XYplorer.exe");
}

// An XYplorer that runs, by the VB6 runtime's window of its process (there
// whether its main window is shown or in the tray); null if none.
HWND xyplorerRuntimeWindow()
{
    for (HWND hwnd = nullptr; (hwnd = ::FindWindowExW(nullptr, hwnd, L"ThunderRT6Main", L"XYplorer"));) {
        if (win32::equalsIgnoreCase(baseName(programOf(hwnd)), L"XYplorer.exe"))
            return hwnd;
    }
    return nullptr;
}

std::wstring xyplorerProgram()
{
    if (std::wstring program = programOf(xyplorerRuntimeWindow()); !program.empty())
        return program;
    constexpr const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\XYplorer.exe";
    for (std::wstring exe : {registryText(HKEY_CURRENT_USER, kKey, nullptr),
             registryText(HKEY_LOCAL_MACHINE, kKey, nullptr, RRF_SUBKEY_WOW6464KEY),
             registryText(HKEY_LOCAL_MACHINE, kKey, nullptr, RRF_SUBKEY_WOW6432KEY)}) {
        if (exe.size() >= 2 && exe.front() == L'"' && exe.back() == L'"')
            exe = exe.substr(1, exe.size() - 2);
        if (!exe.empty() && isFile(exe))
            return exe;
    }
    return {};
}

// The main window of an XYplorer in front of the others; null when none is open.
HWND xyplorerWindow()
{
    HWND found = nullptr;
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            if (!::IsWindowVisible(hwnd) || kindOf(hwnd) != Kind::XYplorer)
                return TRUE;
            *reinterpret_cast<HWND*>(param) = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// The window XYplorer answers to (askXYplorer): what it sends is kept in
// the std::wstring the window's user data points to.
LRESULT CALLBACK xyplorerAnswerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg != WM_COPYDATA)
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
    auto* answer = reinterpret_cast<std::wstring*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (answer && data && data->dwData == kXYplorerAnswer && data->lpData) {
        // UTF-16, cbData the bytes without a terminator (27.20); should one come, it goes.
        std::wstring_view text(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
        while (!text.empty() && text.back() == L'\0')
            text.remove_suffix(1);
        answer->append(text);
    }
    return TRUE;
}

struct XYplorerAsked {
    bool delivered = false; // the script reached it (it may still be at it: a slow folder)
    std::wstring answer; // what it sent to our window (copydata <ours>, …, 0), in order
};

// Runs a script in the XYplorer window `window`: `script` makes it from
// our window's handle (decimal), for its answers. XYplorer runs it while
// the message is being sent, and its answers arrive meanwhile. Not
// delivered: it is gone, hung, or runs elevated while Win顺 does not (UIPI
// refuses the message: told once).
XYplorerAsked askXYplorer(HWND window, const std::function<std::wstring(const std::wstring& ours)>& script)
{
    constexpr const wchar_t* kClass = L"WinShun.XYplorerAnswer";
    static std::once_flag registered;
    std::call_once(registered, [] {
        WNDCLASSEXW wc {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = xyplorerAnswerProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        ::RegisterClassExW(&wc);
    });
    XYplorerAsked asked;
    const HWND ours = ::CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!ours)
        return asked;
    ::SetWindowLongPtrW(ours, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&asked.answer));
    // XYplorer runs with the user's rights; Win顺 may be elevated (管理员直读).
    ::ChangeWindowMessageFilterEx(ours, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    std::wstring text = script(std::to_wstring(reinterpret_cast<std::uintptr_t>(ours)));
    COPYDATASTRUCT data {};
    data.dwData = kXYplorerScript;
    data.cbData = static_cast<DWORD>(text.size() * sizeof(wchar_t)); // no terminator: it would be read as a line
    data.lpData = text.data();
    DWORD_PTR result = 0;
    const bool sent = ::SendMessageTimeoutW(window, WM_COPYDATA, reinterpret_cast<WPARAM>(ours),
                          reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, kXYplorerWait, &result)
        != 0;
    const DWORD error = sent ? ERROR_SUCCESS : ::GetLastError();
    ::DestroyWindow(ours);
    asked.delivered = sent || error == ERROR_TIMEOUT;
    if (!asked.delivered) {
        static std::atomic<bool> warned {false};
        if (!warned.exchange(true))
            qWarning() << "XYplorer did not take a script (error" << error
                       << (error == ERROR_ACCESS_DENIED ? "): it runs as administrator while Win顺 does not" : ")");
    } else if (!sent) {
        qWarning() << "XYplorer took more than" << kXYplorerWait << "ms over a script";
    }
    return asked;
}

// The folders an XYplorer window shows, as pathtext::xyplorerTabs reads
// its answer: each pane's tabs (the active pane first, the tab on top
// first), the other pane's only while it is shown.
QList<pathtext::XYplorerTab> xyplorerTabs(HWND window)
{
    const XYplorerAsked asked = askXYplorer(window, [](const std::wstring& ours) {
        // pathreal(): a tab's path may be a special one ("Documents", the
        // user's name for the profile). Flag e: no empty token, whose
        // pathreal() would be the item focused.
        return L"::$a = ''; foreach($t, get('tabs_sf', '|', 'a'), '|', 'e') { $a = $a . '|' . pathreal($t); }"
               L" $b = ''; foreach($t, get('tabs_sf', '|', 'i'), '|', 'e') { $b = $b . '|' . pathreal($t); }"
               L" copydata "
            + ours + L", dualpane() . \"<crlf>\" . $a . \"<crlf>\" . $b, 0;";
    });
    if (!asked.delivered)
        return {};
    if (asked.answer.empty()) {
        qWarning() << "XYplorer did not say which folders it shows";
        return {};
    }
    QList<pathtext::XYplorerTab> tabs = pathtext::xyplorerTabs(QString::fromStdWString(asked.answer));
    tabs.removeIf([](const pathtext::XYplorerTab& tab) { return !isFolder(tab.path.toStdWString()); }); // archives, gone
    return tabs;
}

// A script that brings the tab showing `folder` in the active pane to the
// front, or opens one, and selects `names` there (none: nothing). Scripts
// run in order: the list is loaded by the time it is selected in.
std::wstring xyplorerGoScript(const QString& folder, const QStringList& names)
{
    QString key = folder.toLower();
    while (key.size() > 2 && key.endsWith(u'\\'))
        key.chop(1); // pathreal() gives "C:" for a drive
    QString script = u"::$p = %1; $k = 0; $n = 0; foreach($t, get('tabs', '|', 'a'), '|', 'e') { $n++;"
                     u" if ($k == 0 && recase(pathreal($t)) == $p) { $k = $n; } }"
                     u" if ($k > 0) { seltab $k; } else { tab('new', %2); }"_s.arg(
        pathtext::xyplorerQuoted(key), pathtext::xyplorerQuoted(folder));
    if (!names.isEmpty())
        script += u" selectitems %1, , 1;"_s.arg(pathtext::xyplorerQuoted(names.join(u'|')));
    return (script + u" focus 'XY';"_s).toStdWString();
}

// XYplorer's window to the front, as Total Commander and Opus come by
// themselves: the user just pressed Enter in Win顺, which may hand the front on.
void bringToFront(HWND window)
{
    if (::IsIconic(window))
        ::ShowWindowAsync(window, SW_RESTORE);
    if (::GetForegroundWindow() != window)
        ::SetForegroundWindow(window);
}

// The command lines of Kind::Custom (setCustomCommands).
std::mutex g_customMutex;
QString g_customOpen;
QString g_customReveal;

// Runs a custom command line for `path`; false when there is none, its
// program is not there, or it did not start.
bool runCustom(const QString& commandLine, const std::wstring& path, bool wait)
{
    const pathtext::CustomCommand command = pathtext::customCommand(commandLine, QString::fromStdWString(path));
    const QString program = customProgram(commandLine);
    if (command.program.isEmpty() || program.isEmpty()) {
        qWarning() << "The file manager command has no program to run:" << commandLine;
        return false;
    }
    const std::wstring exe = QDir::toNativeSeparators(program).toStdWString();
    return run(exe, command.arguments.toStdWString(), wait, folderOf(exe));
}

// The folder on disk that an Explorer tab shows; empty for Home, This PC, a
// search and the like.
std::wstring folderPath(IShellBrowser* browser)
{
    ComPtr<IShellView> view;
    ComPtr<IFolderView> folderView;
    ComPtr<IPersistFolder2> folder;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folderView))
        || FAILED(folderView->GetFolder(IID_PPV_ARGS(&folder))) || FAILED(folder->GetCurFolder(&pidl)) || !pidl)
        return {};
    std::wstring path;
    PWSTR name = nullptr;
    if (SUCCEEDED(::SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &name))) {
        path = name;
        ::CoTaskMemFree(name);
    }
    ::ILFree(pidl);
    return path;
}

struct ExplorerTab {
    HWND window;
    HWND tab;
    std::wstring path;
};

// Each tab of each Explorer window that shows a folder on disk.
std::vector<ExplorerTab> explorerTabs()
{
    ComPtr<IShellWindows> windows;
    long count = 0;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))
        || FAILED(windows->get_Count(&count)))
        return {};
    std::vector<ExplorerTab> tabs;
    for (long i = 0; i < count; ++i) {
        VARIANT index {};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> item;
        ComPtr<IServiceProvider> services;
        ComPtr<IShellBrowser> browser;
        HWND tab = nullptr;
        if (windows->Item(index, &item) != S_OK || !item || FAILED(item.As(&services))
            || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))
            || FAILED(browser->GetWindow(&tab)) || !tab)
            continue;
        std::wstring path = folderPath(browser.Get());
        if (!path.empty())
            tabs.push_back({::GetAncestor(tab, GA_ROOT), tab, std::move(path)});
    }
    return tabs;
}

// The shell's window (an IWebBrowser2) of an Explorer tab, and its browser.
bool findTab(HWND tab, ComPtr<IWebBrowser2>& web, ComPtr<IShellBrowser>& browser)
{
    ComPtr<IShellWindows> windows;
    long count = 0;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))
        || FAILED(windows->get_Count(&count)))
        return false;
    for (long i = 0; i < count; ++i) {
        VARIANT index {};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> item;
        ComPtr<IServiceProvider> services;
        HWND window = nullptr;
        if (windows->Item(index, &item) != S_OK || !item || FAILED(item.As(&services))
            || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))
            || FAILED(browser->GetWindow(&window)) || window != tab)
            continue;
        return SUCCEEDED(item.As(&web));
    }
    return false;
}

// The tab goes to `folder`, as going there in it would (its Back button
// comes back).
bool navigate(IWebBrowser2* web, const std::wstring& folder)
{
    VARIANT target {};
    target.vt = VT_BSTR;
    target.bstrVal = ::SysAllocString(folder.c_str());
    VARIANT empty {};
    const HRESULT hr = web->Navigate2(&target, &empty, &empty, &empty, &empty);
    ::VariantClear(&target);
    return SUCCEEDED(hr);
}

// Whether the thread of the Explorer window `root` has the keyboard on its
// files and folders (a DirectUIHWND under SHELLDLL_DefView in that window).
bool filesFocused(HWND root)
{
    GUITHREADINFO info {sizeof(GUITHREADINFO)};
    if (!::GetGUIThreadInfo(::GetWindowThreadProcessId(root, nullptr), &info) || !info.hwndFocus)
        return false;
    const folderview::Hit hit = folderview::classify(info.hwndFocus, {});
    return hit.surface == folderview::Surface::Explorer && hit.root == root;
}

bool waitFor(std::predicate auto done, std::chrono::milliseconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// The folders a Total Commander window shows: the panel with the focus first.
std::vector<std::wstring> totalCommanderFolders(HWND window)
{
    DWORD_PTR active = 0; // 1: the left panel, 2: the right one
    if (!::SendMessageTimeoutW(window, kTotalCommanderAsk, 1000, 0, SMTO_ABORTIFHUNG, 1000, &active))
        return {};
    std::vector<std::wstring> folders;
    // The text over the left file list (9), the right one (10): "c:\Windows\*.*".
    for (const WPARAM side : active == 2 ? std::initializer_list<WPARAM> {10, 9} : std::initializer_list<WPARAM> {9, 10}) {
        DWORD_PTR label = 0;
        if (!::SendMessageTimeoutW(window, kTotalCommanderAsk, side, 0, SMTO_ABORTIFHUNG, 1000, &label) || !label)
            continue;
        const std::wstring folder = pathtext::folderFromTotalCommander(
            QString::fromStdWString(caption(reinterpret_cast<HWND>(label)))).toStdWString();
        if (!folder.empty() && isFolder(folder)) // not inside an archive
            folders.push_back(folder);
    }
    return folders;
}

// The tabs of Directory Opus's windows; none while it has no window open:
// asking would start it.
QList<pathtext::OpusTab> opusTabs()
{
    if (!::FindWindowW(L"dopus.lister", nullptr))
        return {};
    const std::wstring rt = opusProgram();
    wchar_t temp[MAX_PATH + 1] {};
    const DWORD n = ::GetTempPathW(MAX_PATH + 1, temp);
    if (rt.empty() || n == 0 || n > MAX_PATH)
        return {};
    static std::atomic<unsigned> asked {0};
    const std::wstring file = std::wstring(temp, n) + L"WinShun-opus-" + std::to_wstring(::GetCurrentProcessId()) + L'-'
        + std::to_wstring(++asked) + L".xml";
    // It answers in a file, written by the time dopusrt.exe ends (some 50 ms).
    const win32::UniqueHandle process = shell::startUnelevated(rt, L"/info " + quoted(file) + L",paths");
    if (!process.valid())
        return {};
    if (::WaitForSingleObject(process.get(), 3000) != WAIT_OBJECT_0) {
        ::TerminateProcess(process.get(), 1);
        ::DeleteFileW(file.c_str());
        qWarning() << "Directory Opus did not say which folders it shows";
        return {};
    }
    QFile answer(QString::fromStdWString(file));
    const QByteArray xml = answer.open(QIODevice::ReadOnly) ? answer.readAll() : QByteArray();
    answer.close();
    answer.remove();
    QList<pathtext::OpusTab> tabs = pathtext::opusTabs(xml);
    tabs.removeIf([](const pathtext::OpusTab& tab) { return !isFolder(tab.path.toStdWString()); }); // archives
    return tabs;
}

} // namespace

std::optional<Kind> kindOf(HWND window)
{
    wchar_t name[32] {};
    if (!window || ::GetClassNameW(window, name, static_cast<int>(std::size(name))) == 0)
        return std::nullopt;
    if (::wcscmp(name, L"CabinetWClass") == 0)
        return Kind::Explorer;
    if (::wcscmp(name, L"TTOTAL_CMD") == 0)
        return Kind::TotalCommander;
    if (::wcscmp(name, L"dopus.lister") == 0)
        return Kind::DirectoryOpus;
    if (::wcscmp(name, L"ThunderRT6FormDC") == 0 && isXYplorerMain(window))
        return Kind::XYplorer;
    return std::nullopt;
}

std::vector<Folder> openFolders()
{
    struct Search {
        std::vector<ExplorerTab> explorer;
        QList<pathtext::OpusTab> opus;
        std::vector<Folder> onTop; // each window's folder in front
        std::vector<Folder> behind; // the others
    } search {explorerTabs(), opusTabs(), {}, {}};
    // Top-level windows come front to back, and so do a window's children.
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(param);
            // Explorer: Windows 11 has a child per tab (all of them visible), the tab on top first.
            bool first = true;
            for (HWND child = ::FindWindowExW(hwnd, nullptr, L"ShellTabWindowClass", nullptr); child;
                child = ::FindWindowExW(hwnd, child, L"ShellTabWindowClass", nullptr)) {
                for (const ExplorerTab& tab : search.explorer) {
                    if (tab.tab == child)
                        (first ? search.onTop : search.behind).push_back({tab.path, Kind::Explorer});
                }
                first = false;
            }
            for (const ExplorerTab& tab : search.explorer) { // a window without such children
                if (tab.window == hwnd && tab.tab == hwnd)
                    search.onTop.push_back({tab.path, Kind::Explorer});
            }
            const std::optional<Kind> kind = kindOf(hwnd);
            if (kind == Kind::TotalCommander && ::IsWindowVisible(hwnd)) {
                first = true;
                for (std::wstring& folder : totalCommanderFolders(hwnd))
                    (std::exchange(first, false) ? search.onTop : search.behind).push_back({std::move(folder), *kind});
            } else if (kind == Kind::DirectoryOpus) {
                // The tab in front on the side with the focus, the other side's, the tabs behind.
                for (const int state : {1, 2, 0}) {
                    for (const pathtext::OpusTab& tab : std::as_const(search.opus)) {
                        if (tab.lister == reinterpret_cast<quintptr>(hwnd) && tab.state == state)
                            (state == 1 ? search.onTop : search.behind).push_back({tab.path.toStdWString(), *kind});
                    }
                }
            } else if (kind == Kind::XYplorer && ::IsWindowVisible(hwnd)) {
                // Likewise; asked window by window (a window in the tray is not shown: not asked).
                const QList<pathtext::XYplorerTab> tabs = xyplorerTabs(hwnd);
                for (const int state : {1, 2, 0}) {
                    for (const pathtext::XYplorerTab& tab : tabs) {
                        if (tab.state == state)
                            (state == 1 ? search.onTop : search.behind).push_back({tab.path.toStdWString(), *kind});
                    }
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    std::vector<Folder> folders;
    for (const auto* list : {&search.onTop, &search.behind}) {
        for (const Folder& folder : *list) {
            if (std::ranges::none_of(folders, [&](const Folder& f) { return ::_wcsicmp(f.path.c_str(), folder.path.c_str()) == 0; }))
                folders.push_back(folder);
        }
    }
    return folders;
}

std::wstring explorerFolder(HWND tab)
{
    ComPtr<IWebBrowser2> web;
    ComPtr<IShellBrowser> browser;
    return findTab(tab, web, browser) ? folderPath(browser.Get()) : std::wstring();
}

bool goTo(HWND tab, const std::wstring& folder)
{
    ComPtr<IWebBrowser2> web;
    ComPtr<IShellBrowser> browser;
    return findTab(tab, web, browser) && navigate(web.Get(), folder);
}

bool goToKeepingFocus(HWND tab, const std::wstring& folder)
{
    const HWND root = ::GetAncestor(tab, GA_ROOT);
    const auto inFront = [root] { return ::GetForegroundWindow() == root && filesFocused(root); };
    // Just handed the front, the window has not given its files the keyboard
    // back yet (docs/pitfalls.md: the file dialogs' "刚交还前台就操作"); gone
    // on from there, Explorer gives the new folder's files the focus too.
    waitFor(inFront, std::chrono::milliseconds(500));
    ComPtr<IWebBrowser2> web;
    ComPtr<IShellBrowser> browser;
    if (!findTab(tab, web, browser) || !navigate(web.Get(), folder))
        return false;
    const QString wanted = QString::fromStdWString(folder);
    waitFor([&] { return pathtext::sameFolder(QString::fromStdWString(folderPath(browser.Get())), wanted); },
        std::chrono::milliseconds(2000));
    // The new view takes the focus as it comes; should it not have it (the
    // window was not in front with its files focused), it is given to it.
    if (::GetForegroundWindow() == root && !waitFor(inFront, std::chrono::milliseconds(300))) {
        ComPtr<IShellView> view;
        if (SUCCEEDED(browser->QueryActiveShellView(&view)) && view)
            view->UIActivate(SVUIA_ACTIVATE_FOCUS);
    }
    return true;
}

std::wstring program(Kind kind)
{
    switch (kind) {
    case Kind::TotalCommander:
        return totalCommanderProgram();
    case Kind::DirectoryOpus:
        return opusProgram();
    case Kind::XYplorer:
        return xyplorerProgram();
    case Kind::Custom: {
        const std::lock_guard lock(g_customMutex);
        return QDir::toNativeSeparators(customProgram(g_customOpen)).toStdWString();
    }
    case Kind::Explorer:
        break;
    }
    return {};
}

bool installed(Kind kind)
{
    return kind == Kind::Explorer || kind == Kind::Custom || !program(kind).empty();
}

bool openFolder(Kind kind, const std::wstring& folder)
{
    if (kind == Kind::Custom) {
        QString command;
        {
            const std::lock_guard lock(g_customMutex);
            command = g_customOpen;
        }
        return runCustom(command, folder, false);
    }
    const std::wstring exe = program(kind);
    if (exe.empty())
        return false;
    switch (kind) {
    case Kind::TotalCommander: // in the copy that runs (/O), the panel with the focus (/S: /L is that one)
        return run(exe, L"/O /T /S /L=" + quoted(folder), false);
    case Kind::DirectoryOpus:
        return run(exe, L"/acmd Go " + quoted(folder) + L" NEWTAB=findexisting,tofront", false);
    case Kind::XYplorer: {
        if (const HWND window = xyplorerWindow()) {
            const std::wstring script = xyplorerGoScript(QString::fromStdWString(folder), {});
            if (!askXYplorer(window, [&](const std::wstring&) { return script; }).delivered)
                return false;
            bringToFront(window);
            return true;
        }
        // Not running: started on it. "D:\" bare: a backslash before the closing quote would escape it.
        return run(exe, folder.ends_with(L'\\') ? folder : quoted(folder), false);
    }
    case Kind::Custom:
    case Kind::Explorer:
        break;
    }
    return false;
}

bool goToInOpus(const std::wstring& folder)
{
    const std::wstring exe = opusProgram();
    return !exe.empty() && run(exe, L"/acmd Go " + quoted(folder), false);
}

std::wstring frontFolder(HWND window)
{
    const std::optional<Kind> kind = kindOf(window);
    if (!kind)
        return {};
    switch (*kind) {
    case Kind::Explorer: {
        const HWND tab = ::FindWindowExW(window, nullptr, L"ShellTabWindowClass", nullptr); // the one on top first
        return explorerFolder(tab ? tab : window);
    }
    case Kind::TotalCommander: {
        const std::vector<std::wstring> folders = totalCommanderFolders(window);
        return folders.empty() ? std::wstring() : folders.front();
    }
    case Kind::DirectoryOpus:
        for (const pathtext::OpusTab& tab : opusTabs()) {
            if (tab.lister == reinterpret_cast<quintptr>(window) && tab.state == 1)
                return tab.path.toStdWString();
        }
        return {};
    case Kind::XYplorer:
        for (const pathtext::XYplorerTab& tab : xyplorerTabs(window)) {
            if (tab.state == 1)
                return tab.path.toStdWString();
        }
        return {};
    case Kind::Custom:
        break;
    }
    return {};
}

bool goToInFront(Kind kind, const std::wstring& folder)
{
    switch (kind) {
    case Kind::DirectoryOpus:
        return goToInOpus(folder);
    case Kind::TotalCommander: {
        // The copy that runs (/O), the panel with the focus (/S), its
        // current tab: without /T, which would open a new one.
        const std::wstring exe = totalCommanderProgram();
        return !exe.empty() && run(exe, L"/O /S /L=" + quoted(folder), false);
    }
    case Kind::XYplorer:
        return openFolder(kind, folder);
    case Kind::Explorer:
    case Kind::Custom:
        break;
    }
    return false;
}

bool openOpusPreferences()
{
    const std::wstring exe = opusProgram();
    return !exe.empty() && run(exe, L"/acmd Prefs", false);
}

std::wstring opusPrefsFile()
{
    const std::wstring file = qEnvironmentVariable("APPDATA").toStdWString()
        + L"\\GPSoftware\\Directory Opus\\ConfigFiles\\prefs.oxc";
    return isFile(file) ? file : std::wstring();
}

namespace {

// Directory Opus's process in this session, 0 if none.
DWORD opusProcess()
{
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    const win32::UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid())
        return 0;
    PROCESSENTRY32W entry {sizeof entry};
    for (BOOL more = ::Process32FirstW(snapshot.get(), &entry); more; more = ::Process32NextW(snapshot.get(), &entry)) {
        DWORD processSession = 0;
        if (::_wcsicmp(entry.szExeFile, L"dopus.exe") == 0 && ::ProcessIdToSessionId(entry.th32ProcessID, &processSession)
            && processSession == session)
            return entry.th32ProcessID;
    }
    return 0;
}

} // namespace

bool opusRunning()
{
    return opusProcess() != 0;
}

PrefsEdit editOpusPrefs(const std::function<std::optional<QByteArray>(const QByteArray&)>& edit)
{
    const std::wstring rt = opusProgram();
    const std::wstring file = opusPrefsFile();
    if (rt.empty() || file.empty())
        return PrefsEdit::Failed;
    // Closed as its own Exit would close it (it may ask about a copy still going).
    const DWORD pid = opusProcess();
    const bool hadWindow = ::FindWindowW(L"dopus.lister", nullptr) != nullptr;
    if (pid) {
        const win32::UniqueHandle process(::OpenProcess(SYNCHRONIZE, FALSE, pid));
        if (!process.valid() || !run(rt, L"/cmd Close PROGRAM", true)
            || ::WaitForSingleObject(process.get(), 15000) != WAIT_OBJECT_0) {
            qWarning() << "Directory Opus did not close";
            return PrefsEdit::OpusStayed;
        }
    }
    bool done = false;
    QFile in(QString::fromStdWString(file));
    if (in.open(QIODevice::ReadOnly)) {
        const QByteArray before = in.readAll();
        in.close();
        if (const std::optional<QByteArray> after = edit(before); !after) {
            done = true;
        } else {
            QSaveFile out(in.fileName());
            done = out.open(QIODevice::WriteOnly) && out.write(*after) == after->size() && out.commit();
        }
    }
    if (!done)
        qWarning() << "Could not change" << in.fileName();
    if (pid && run(folderOf(rt) + L"\\dopus.exe", {}, false)) {
        // Its windows come back and to the front: done once one is there.
        // One that ran with no window (in the tray) opens its default lister
        // as it starts, and no switch keeps that shut: closed again.
        for (int waited = 0; waited < 3000; waited += 100) {
            if (!hadWindow) {
                if (::FindWindowW(L"dopus.lister", nullptr)) {
                    for (HWND lister = nullptr; (lister = ::FindWindowExW(nullptr, lister, L"dopus.lister", nullptr));)
                        ::PostMessageW(lister, WM_CLOSE, 0, 0);
                    break;
                }
            } else {
                wchar_t name[32] {};
                const HWND front = ::GetAncestor(::GetForegroundWindow(), GA_ROOT);
                if (front && ::GetClassNameW(front, name, static_cast<int>(std::size(name)))
                    && ::wcscmp(name, L"dopus.lister") == 0)
                    break;
            }
            ::Sleep(100);
        }
    }
    return done ? PrefsEdit::Done : PrefsEdit::Failed;
}

namespace {

// Each item's folder in XYplorer with the items selected: a drive, nothing
// above it, is opened. Items grouped by folder, in the order the folders
// first come up (one script each).
bool revealInXYplorer(const std::wstring& exe, const std::vector<std::wstring>& paths)
{
    struct Group {
        QString folder;
        QStringList names;
    };
    std::vector<Group> groups;
    for (const std::wstring& path : paths) {
        const QFileInfo info(QString::fromStdWString(path));
        const QString folder = info.isRoot() ? QDir::toNativeSeparators(info.filePath()) : QDir::toNativeSeparators(info.path());
        auto it = std::ranges::find_if(groups, [&](const Group& g) { return g.folder.compare(folder, Qt::CaseInsensitive) == 0; });
        if (it == groups.end())
            it = groups.insert(groups.end(), {folder, {}});
        if (!info.isRoot())
            it->names.append(info.fileName());
    }
    const HWND window = xyplorerWindow();
    if (!window) {
        // Not running: started with the first item selected (/select), or on the drive.
        const Group& first = groups.front();
        const std::wstring folder = first.folder.toStdWString();
        return run(exe, first.names.isEmpty() ? folder : L"/select=" + quoted((first.folder + u'\\' + first.names.first()).toStdWString()), false);
    }
    bool shown = false;
    for (const Group& group : groups) {
        const std::wstring script = xyplorerGoScript(group.folder, group.names);
        if (!askXYplorer(window, [&](const std::wstring&) { return script; }).delivered)
            return shown;
        shown = true;
    }
    bringToFront(window);
    return shown;
}

// Each item's folder opened by the custom open command (no reveal command).
bool openFoldersOf(const QString& openCommand, const std::vector<std::wstring>& paths)
{
    std::vector<std::wstring> folders;
    for (const std::wstring& path : paths) {
        const QFileInfo info(QString::fromStdWString(path));
        std::wstring folder = QDir::toNativeSeparators(info.isRoot() ? info.filePath() : info.path()).toStdWString();
        if (std::ranges::none_of(folders, [&](const std::wstring& f) { return ::_wcsicmp(f.c_str(), folder.c_str()) == 0; }))
            folders.push_back(std::move(folder));
    }
    bool shown = false;
    for (const std::wstring& folder : folders) {
        if (!runCustom(openCommand, folder, folders.size() > 1))
            return shown;
        shown = true;
    }
    return shown;
}

} // namespace

bool reveal(Kind kind, const std::vector<std::wstring>& paths)
{
    if (paths.empty())
        return false;
    if (kind == Kind::Custom) {
        QString open;
        QString reveal;
        {
            const std::lock_guard lock(g_customMutex);
            open = g_customOpen;
            reveal = g_customReveal;
        }
        if (reveal.trimmed().isEmpty())
            return openFoldersOf(open, paths);
        bool shown = false;
        for (const std::wstring& path : paths) {
            if (!runCustom(reveal, path, paths.size() > 1))
                return shown;
            shown = true;
        }
        return shown;
    }
    const std::wstring exe = program(kind);
    if (exe.empty() || kind == Kind::Explorer)
        return false;
    if (kind == Kind::XYplorer)
        return revealInXYplorer(exe, paths);
    std::vector<std::wstring> folders; // shown already (Total Commander)
    bool shown = false;
    for (const std::wstring& path : paths) {
        const QFileInfo info(QString::fromStdWString(path));
        const bool root = info.isRoot(); // a drive: nothing above it
        std::wstring args;
        if (kind == Kind::TotalCommander) {
            const std::wstring folder = QDir::toNativeSeparators(info.path()).toStdWString();
            if (std::ranges::any_of(folders, [&](const std::wstring& f) { return ::_wcsicmp(f.c_str(), folder.c_str()) == 0; }))
                continue;
            folders.push_back(folder);
            // A file: its folder with the cursor on it. A folder: /P, the one above with the cursor on it.
            args = std::wstring(L"/O /T /S ") + (info.isDir() && !root ? L"/P " : L"") + L"/L=" + quoted(path);
        } else {
            args = L"/acmd Go " + quoted(path) + (root ? L"" : L" OPENCONTAINER") + L" NEWTAB=findexisting,tofront";
        }
        if (!run(exe, args, paths.size() > 1))
            return shown;
        shown = true;
    }
    return shown;
}

void setCustomCommands(const QString& open, const QString& reveal)
{
    const std::lock_guard lock(g_customMutex);
    g_customOpen = open;
    g_customReveal = reveal;
}

QString customProgram(const QString& commandLine)
{
    const QString program = pathtext::customCommand(commandLine, u"x"_s).program;
    if (program.isEmpty())
        return {};
    if (program.contains(u'\\') || program.contains(u'/')) {
        const QFileInfo info(program);
        return info.isFile() ? QDir::toNativeSeparators(info.absoluteFilePath()) : QString();
    }
    std::wstring found(32768, L'\0');
    const DWORD n = ::SearchPathW(nullptr, program.toStdWString().c_str(), L".exe", static_cast<DWORD>(found.size()),
        found.data(), nullptr);
    if (n == 0 || n >= found.size())
        return {};
    found.resize(n);
    return QString::fromStdWString(found);
}

} // namespace ws::filemanager
