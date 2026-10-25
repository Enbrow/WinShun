#include "PasteAsFile.h"

#include "PasteFile.h"
#include "PasteOffer.h"

#include <windows.h>
#include <commctrl.h>
#include <ole2.h> // before exdisp.h: WIN32_LEAN_AND_MEAN keeps it out of windows.h
#include <exdisp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// TaskDialog is version 6 of the common controls', which a program gets by
// asking for it in its manifest (the linker writes it); comctl32.dll itself
// is loaded only when a message is shown (/DELAYLOAD, CMakeLists.txt).
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' "  \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                        "language='*'\"")

namespace ws::pasteasfile {

namespace {

using Microsoft::WRL::ComPtr;
namespace pf = ws::pastefile;

// What the user is told, in Win顺's language.
struct Texts {
    const wchar_t* app;
    const wchar_t* nothing;
    const wchar_t* nothingHint;
    const wchar_t* files;
    const wchar_t* filesHint;
    const wchar_t* busy;
    const wchar_t* busyHint;
    const wchar_t* noPlace;
    const wchar_t* noPlaceHint;
    const wchar_t* failed;
};

constexpr Texts kChinese {
    .app = L"Win顺",
    .nothing = L"剪贴板里没有文字或图片",
    .nothingHint = L"先复制一段文字或一张图片，再来粘贴为文件。",
    .files = L"剪贴板里是文件",
    .filesHint = L"用资源管理器的“粘贴”（Ctrl+V）就能把它们放到这里。",
    .busy = L"剪贴板正被别的程序占用",
    .busyHint = L"稍等一下再试。",
    .noPlace = L"这里放不了文件",
    .noPlaceHint = L"到一个文件夹里再粘贴为文件。",
    .failed = L"没能粘贴为文件",
};

constexpr Texts kEnglish {
    .app = L"WinShun",
    .nothing = L"There's no text or picture on the clipboard",
    .nothingHint = L"Copy some text or a picture first, then paste it as a file.",
    .files = L"The clipboard holds files",
    .filesHint = L"Explorer's Paste (Ctrl+V) puts them here.",
    .busy = L"Another program is using the clipboard",
    .busyHint = L"Wait a moment and try again.",
    .noPlace = L"Files can't go here",
    .noPlaceHint = L"Paste as a file in a folder instead.",
    .failed = L"Couldn't paste as a file",
};

std::wstring systemMessage(HRESULT hr)
{
    PWSTR text = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(hr), 0, reinterpret_cast<PWSTR>(&text), 0, nullptr);
    std::wstring message = n ? std::wstring(text, n) : std::wstring();
    ::LocalFree(text);
    while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' '))
        message.pop_back();
    if (message.empty()) {
        wchar_t code[16] {};
        std::swprintf(code, std::size(code), L"0x%08X", static_cast<unsigned>(hr));
        message = code;
    }
    return message;
}

// A message centred on the window it is about (or at the pointer), on that
// monitor: TaskDialog centres on its parent, so the parent is a window of
// ours over that one, never shown.
void tell(HWND about, const Texts& texts, const wchar_t* heading, const std::wstring& detail, bool warning)
{
    RECT area {};
    if (!about || !::GetWindowRect(about, &area)) {
        POINT pointer {};
        ::GetCursorPos(&pointer);
        area = {pointer.x, pointer.y, pointer.x + 1, pointer.y + 1};
    }
    const HWND anchor = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", texts.app, WS_POPUP, area.left, area.top,
        area.right - area.left, area.bottom - area.top, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    TASKDIALOGCONFIG config {sizeof config};
    config.hwndParent = anchor;
    config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = texts.app;
    config.pszMainIcon = warning ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    config.pszMainInstruction = heading;
    config.pszContent = detail.c_str();
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    ::TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
    if (anchor)
        ::DestroyWindow(anchor);
}

bool hasClass(HWND window, const wchar_t* name)
{
    wchar_t found[64] {};
    return window && ::GetClassNameW(window, found, static_cast<int>(std::size(found))) && std::wcscmp(found, name) == 0;
}

bool isDesktop(HWND root)
{
    return hasClass(root, L"Progman") || hasClass(root, L"WorkerW");
}

// ---- The clipboard -------------------------------------------------------

// Opened for reading. Another program may hold it for a moment (a clipboard
// manager reading what was just copied): tried for half a second.
bool openClipboard()
{
    for (int tries = 0; tries < 20; ++tries) {
        if (::OpenClipboard(nullptr))
            return true;
        ::Sleep(25);
    }
    return false;
}

std::vector<BYTE> clipboardBytes(UINT format)
{
    const HANDLE data = ::GetClipboardData(format);
    const auto* bytes = data ? static_cast<const BYTE*>(::GlobalLock(data)) : nullptr;
    if (!bytes)
        return {};
    std::vector<BYTE> copy(bytes, bytes + ::GlobalSize(data));
    ::GlobalUnlock(data);
    return copy;
}

std::wstring clipboardText()
{
    const HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    const auto* text = data ? static_cast<const wchar_t*>(::GlobalLock(data)) : nullptr;
    if (!text)
        return {};
    std::wstring copy(text, ::wcsnlen(text, ::GlobalSize(data) / sizeof(wchar_t)));
    ::GlobalUnlock(data);
    return copy;
}

// The "PNG" format's bytes up to the end of the picture (GlobalSize rounds
// up); empty when they are not a PNG.
std::vector<BYTE> pngFile(std::vector<BYTE> bytes)
{
    static constexpr BYTE kSignature[] {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (bytes.size() < sizeof kSignature || std::memcmp(bytes.data(), kSignature, sizeof kSignature) != 0)
        return {};
    for (std::size_t at = sizeof kSignature; at + 12 <= bytes.size();) {
        const std::uint32_t length = (std::uint32_t {bytes[at]} << 24) | (std::uint32_t {bytes[at + 1]} << 16)
            | (std::uint32_t {bytes[at + 2]} << 8) | bytes[at + 3];
        const std::size_t end = at + 12 + std::size_t {length};
        if (length > 0x7FFFFFFF || end > bytes.size())
            return {};
        if (std::memcmp(&bytes[at + 4], "IEND", 4) == 0) {
            bytes.resize(end);
            return bytes;
        }
        at = end;
    }
    return {};
}

// What the clipboard held, read at once and let go of: the file is made from
// this copy.
struct Contents {
    pf::Offer offer;
    std::optional<pf::Kind> kind;
    std::wstring text;
    std::vector<BYTE> png; // "PNG" as copied, its transparency kept
    std::vector<BYTE> dib; // else CF_DIB (Windows makes it from a bitmap or a CF_DIBV5)
};

std::optional<Contents> readClipboard()
{
    if (!openClipboard())
        return std::nullopt;
    static const UINT csv = ::RegisterClipboardFormatW(L"Csv");
    static const UINT png = ::RegisterClipboardFormatW(L"PNG");
    Contents contents;
    contents.offer = pf::clipboardOffer();
    pf::Clip clip;
    if (contents.offer.text)
        clip.text = clipboardText();
    clip.spreadsheet = csv && ::IsClipboardFormatAvailable(csv);
    clip.image = contents.offer.image;
    contents.kind = pf::choose(clip);
    if (contents.kind == pf::Kind::Picture) {
        contents.png = pngFile(clipboardBytes(png));
        if (contents.png.empty())
            contents.dib = clipboardBytes(CF_DIB);
    }
    ::CloseClipboard();
    contents.text = std::move(clip.text);
    return contents;
}

// ---- The file ------------------------------------------------------------

// A CF_DIB as a .bmp file, for WIC to read: its header says where the pixels
// start, after the DIB's header, BI_BITFIELDS's masks and the colour table.
std::vector<BYTE> bmpFile(const std::vector<BYTE>& dib)
{
    BITMAPINFOHEADER header {};
    if (dib.size() < sizeof header)
        return {};
    std::memcpy(&header, dib.data(), sizeof header);
    if (header.biSize < sizeof header || header.biSize > dib.size())
        return {};
    std::size_t offset = header.biSize;
    if (header.biSize == sizeof header && header.biCompression == BI_BITFIELDS)
        offset += 3 * sizeof(DWORD);
    else if (header.biSize == sizeof header && header.biCompression == 6) // BI_ALPHABITFIELDS
        offset += 4 * sizeof(DWORD);
    std::size_t colours = header.biClrUsed;
    if (colours == 0 && header.biBitCount <= 8)
        colours = std::size_t {1} << header.biBitCount;
    offset += colours * sizeof(RGBQUAD);
    if (offset > dib.size() || dib.size() > 0x7FFFFFFF)
        return {};
    BITMAPFILEHEADER file {};
    file.bfType = 0x4D42; // "BM"
    file.bfSize = static_cast<DWORD>(sizeof file + dib.size());
    file.bfOffBits = static_cast<DWORD>(sizeof file + offset);
    std::vector<BYTE> bmp(sizeof file + dib.size());
    std::memcpy(bmp.data(), &file, sizeof file);
    std::memcpy(bmp.data() + sizeof file, dib.data(), dib.size());
    return bmp;
}

// A .bmp in memory made a PNG. A 32-bit picture whose alpha bytes are all 0
// (as many programs put their screenshots) is opaque, not invisible; one
// with no transparency at all is written without an alpha channel, as
// screenshots are.
HRESULT encodePng(const std::vector<BYTE>& bmp, std::string& png)
{
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> in;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> bgra;
    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr))
        hr = wic->CreateStream(&in);
    if (SUCCEEDED(hr))
        hr = in->InitializeFromMemory(const_cast<BYTE*>(bmp.data()), static_cast<DWORD>(bmp.size()));
    if (SUCCEEDED(hr))
        hr = wic->CreateDecoderFromStream(in.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr))
        hr = wic->CreateFormatConverter(&bgra);
    if (SUCCEEDED(hr))
        hr = bgra->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
            WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr))
        hr = wic->CreateBitmapFromSource(bgra.Get(), WICBitmapCacheOnLoad, &bitmap);
    if (FAILED(hr))
        return hr;

    bool opaque = true;
    {
        ComPtr<IWICBitmapLock> lock;
        UINT size = 0;
        BYTE* pixels = nullptr;
        if (FAILED(hr = bitmap->Lock(nullptr, WICBitmapLockRead | WICBitmapLockWrite, &lock))
            || FAILED(hr = lock->GetDataPointer(&size, &pixels)))
            return hr;
        bool seen = false;
        for (UINT i = 3; i < size; i += 4) {
            seen |= pixels[i] != 0;
            opaque &= pixels[i] == 0xFF;
        }
        if (!seen) {
            for (UINT i = 3; i < size; i += 4)
                pixels[i] = 0xFF;
            opaque = true;
        }
    }

    ComPtr<IStream> out;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> target;
    ComPtr<IWICBitmapSource> source = bitmap;
    WICPixelFormatGUID format = opaque ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    UINT width = 0;
    UINT height = 0;
    hr = bitmap->GetSize(&width, &height);
    if (SUCCEEDED(hr))
        hr = ::CreateStreamOnHGlobal(nullptr, TRUE, &out);
    if (SUCCEEDED(hr))
        hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr))
        hr = encoder->Initialize(out.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr))
        hr = encoder->CreateNewFrame(&target, nullptr);
    if (SUCCEEDED(hr))
        hr = target->Initialize(nullptr);
    if (SUCCEEDED(hr))
        hr = target->SetSize(width, height);
    if (SUCCEEDED(hr))
        hr = target->SetPixelFormat(&format); // the encoder may answer with another
    if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA) {
        ComPtr<IWICFormatConverter> converter;
        hr = wic->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr))
            hr = converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0,
                WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr))
            source = converter;
    }
    if (SUCCEEDED(hr))
        hr = target->WriteSource(source.Get(), nullptr);
    if (SUCCEEDED(hr))
        hr = target->Commit();
    if (SUCCEEDED(hr))
        hr = encoder->Commit();
    HGLOBAL memory = nullptr;
    STATSTG stat {};
    if (SUCCEEDED(hr))
        hr = ::GetHGlobalFromStream(out.Get(), &memory);
    if (SUCCEEDED(hr))
        hr = out->Stat(&stat, STATFLAG_NONAME);
    if (SUCCEEDED(hr)) {
        const auto* bytes = static_cast<const char*>(::GlobalLock(memory));
        if (!bytes)
            return E_OUTOFMEMORY;
        png.assign(bytes, static_cast<std::size_t>(stat.cbSize.QuadPart));
        ::GlobalUnlock(memory);
    }
    return hr;
}

// The words as their file holds them (pf::Encoding).
std::string encoded(const pf::TextFile& file)
{
    switch (file.encoding) {
    case pf::Encoding::Utf8:
        break;
    case pf::Encoding::Utf8Bom:
        return "\xEF\xBB\xBF" + pf::utf8(file.text);
    case pf::Encoding::Utf16Bom: {
        std::string bytes("\xFF\xFE");
        bytes.append(reinterpret_cast<const char*>(file.text.data()), file.text.size() * sizeof(wchar_t));
        return bytes;
    }
    case pf::Encoding::OemOrUtf8:
        if (file.text.size() < 0x7FFFFFFF) {
            const int length = static_cast<int>(file.text.size());
            BOOL lost = FALSE;
            const int n = ::WideCharToMultiByte(CP_OEMCP, WC_NO_BEST_FIT_CHARS, file.text.data(), length, nullptr, 0,
                nullptr, &lost);
            if (n > 0 && !lost) {
                std::string bytes(static_cast<std::size_t>(n), '\0');
                ::WideCharToMultiByte(CP_OEMCP, WC_NO_BEST_FIT_CHARS, file.text.data(), length, bytes.data(), n,
                    nullptr, nullptr);
                return bytes;
            }
        }
        break;
    }
    return pf::utf8(file.text);
}

// The file made at `path` (none there yet) with these bytes; gone again when
// they could not all be written.
HRESULT writeNew(const std::wstring& path, std::string_view bytes)
{
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(::GetLastError());
    HRESULT hr = S_OK;
    while (!bytes.empty() && SUCCEEDED(hr)) {
        DWORD written = 0;
        if (::WriteFile(file, bytes.data(), static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1u << 30)),
                &written, nullptr))
            bytes.remove_prefix(written);
        else
            hr = HRESULT_FROM_WIN32(::GetLastError());
    }
    ::CloseHandle(file);
    if (FAILED(hr))
        ::DeleteFileW(path.c_str());
    return hr;
}

// Written straight into a folder on disk, the quickest way: under the first
// free name, " (2)", " (3)"... after one taken, as Explorer's 新建 numbers.
// `path` is where it went.
HRESULT writeInto(const std::wstring& folder, const std::wstring& base, std::wstring_view extension,
    std::string_view bytes, std::wstring& path)
{
    const std::wstring prefix = folder.back() == L'\\' ? folder : folder + L'\\';
    for (int n = 1; n < 1000; ++n) {
        path = prefix + base + (n > 1 ? L" (" + std::to_wstring(n) + L')' : std::wstring()) + std::wstring(extension);
        const HRESULT hr = writeNew(path, bytes);
        if (hr != HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) && hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
            return hr;
    }
    return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
}

// The user's list separator, which Excel splits a .csv's lines by.
wchar_t listSeparator()
{
    wchar_t separator[4] {};
    return ::GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SLIST, separator, 4) == 2 ? separator[0] : L',';
}

// A folder of its own in %TEMP% to write the file in under its name, for
// the shell to copy it from (copyInto).
std::wstring tempFolder()
{
    wchar_t temp[MAX_PATH + 1] {};
    const DWORD n = ::GetTempPathW(MAX_PATH + 1, temp);
    if (n == 0 || n > MAX_PATH)
        return {};
    for (ULONGLONG tries = 0; tries < 10; ++tries) {
        const std::wstring folder = std::wstring(temp, n) + L"WinShun-paste-" + std::to_wstring(::GetCurrentProcessId())
            + L'-' + std::to_wstring(::GetTickCount64() + tries);
        if (::CreateDirectoryW(folder.c_str(), nullptr))
            return folder;
    }
    return {};
}

// ---- Where it goes -------------------------------------------------------

struct Place {
    ComPtr<IShellItem> folder; // where the file goes
    ComPtr<IShellBrowser> browser; // the view showing it, where the file is selected; none when not found
    HWND window = nullptr; // the window in front then: the name is edited while it still is; messages go over it
};

std::wstring displayName(IShellItem* item, SIGDN form)
{
    PWSTR name = nullptr;
    if (!item || FAILED(item->GetDisplayName(form, &name)))
        return {};
    std::wstring copy(name);
    ::CoTaskMemFree(name);
    return copy;
}

// The item, by its path when it is a folder on disk: the desktop's view
// shows the namespace's root, which is the user's Desktop folder there.
ComPtr<IShellItem> onDisk(ComPtr<IShellItem> item)
{
    ComPtr<IShellItem> disk;
    if (const std::wstring path = displayName(item.Get(), SIGDN_FILESYSPATH); !path.empty())
        ::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&disk));
    return disk ? disk : item;
}

ComPtr<IShellItem> shownFolder(IShellBrowser* browser)
{
    ComPtr<IShellView> view;
    ComPtr<IFolderView> folderView;
    ComPtr<IPersistFolder2> folder;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(browser->QueryActiveShellView(&view)) || !view || FAILED(view.As(&folderView))
        || FAILED(folderView->GetFolder(IID_PPV_ARGS(&folder))) || FAILED(folder->GetCurFolder(&pidl)) || !pidl)
        return nullptr;
    ComPtr<IShellItem> item;
    ::SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item));
    ::ILFree(pidl);
    return item ? onDisk(item) : nullptr;
}

// An Explorer tab's browser: among the shell's windows, the one whose
// browser has that window (FileManagers.cpp's findTab).
ComPtr<IShellBrowser> tabBrowser(HWND tab)
{
    ComPtr<IShellWindows> windows;
    long count = 0;
    if (!tab || FAILED(::CoCreateInstance(__uuidof(ShellWindows), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))
        || FAILED(windows->get_Count(&count)))
        return nullptr;
    for (long i = 0; i < count; ++i) {
        VARIANT index {};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> item;
        ComPtr<IServiceProvider> services;
        ComPtr<IShellBrowser> browser;
        HWND window = nullptr;
        if (windows->Item(index, &item) == S_OK && item && SUCCEEDED(item.As(&services))
            && SUCCEEDED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))
            && SUCCEEDED(browser->GetWindow(&window)) && window == tab)
            return browser;
    }
    return nullptr;
}

// The desktop's browser, which is not counted among the shell's windows.
ComPtr<IShellBrowser> desktopBrowser()
{
    ComPtr<IShellWindows> windows;
    ComPtr<IDispatch> dispatch;
    ComPtr<IServiceProvider> services;
    ComPtr<IShellBrowser> browser;
    VARIANT desktop {};
    desktop.vt = VT_I4;
    desktop.lVal = CSIDL_DESKTOP;
    VARIANT none {};
    long window = 0;
    if (FAILED(::CoCreateInstance(__uuidof(ShellWindows), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))
        || windows->FindWindowSW(&desktop, &none, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, &dispatch) != S_OK
        || !dispatch || FAILED(dispatch.As(&services))
        || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))))
        return nullptr;
    return browser;
}

bool sameItem(IShellItem* one, IShellItem* other)
{
    int order = 1;
    return one && other
        && SUCCEEDED(one->Compare(other, SICHINT_CANONICAL | SICHINT_TEST_FILESYSPATH_IF_NOT_EQUAL, &order))
        && order == 0;
}

// Ctrl+V: the folder the view shows.
Place placeOf(HWND view)
{
    Place place;
    place.window = ::GetAncestor(view, GA_ROOT);
    place.browser = isDesktop(place.window) ? desktopBrowser() : tabBrowser(view);
    if (place.browser)
        place.folder = shownFolder(place.browser.Get());
    return place;
}

// The right-click: the folder named, selected in the tab on top of the window
// in front (or the desktop) when that is the one showing it.
Place placeFor(const std::wstring& folder)
{
    Place place;
    place.window = ::GetForegroundWindow();
    ComPtr<IShellItem> item;
    if (SUCCEEDED(::SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item))))
        place.folder = onDisk(item);
    ComPtr<IShellBrowser> browser;
    if (isDesktop(place.window)) {
        browser = desktopBrowser();
    } else if (hasClass(place.window, L"CabinetWClass") || hasClass(place.window, L"ExploreWClass")) {
        // Windows 11 has a child per tab, the one on top first among them.
        const HWND tab = ::FindWindowExW(place.window, nullptr, L"ShellTabWindowClass", nullptr);
        browser = tabBrowser(tab ? tab : place.window);
    }
    if (browser && sameItem(shownFolder(browser.Get()).Get(), place.folder.Get()))
        place.browser = browser;
    return place;
}

// A folder on disk, or storage that takes files (a phone's, a library); not
// This PC, Home or a search, nor a file opened as a folder: a .zip or .cab
// is a folder and a stream at once, and Bandizip's or 7-Zip's turns a copy
// into it down ("目录名称无效", measured).
bool holdsFiles(IShellItem* folder)
{
    SFGAOF attributes = 0;
    return SUCCEEDED(folder->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM | SFGAO_STORAGE | SFGAO_STREAM, &attributes))
        && (attributes & SFGAO_FOLDER) && !(attributes & SFGAO_STREAM)
        && (attributes & (SFGAO_FILESYSTEM | SFGAO_STORAGE));
}

// ---- Copied there and selected -------------------------------------------

// Where the shell put the file (another name when that one was taken).
class CopySink : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                     IFileOperationProgressSink> {
public:
    ComPtr<IShellItem> made;

    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT result, IShellItem* created) override
    {
        if (SUCCEEDED(result))
            made = created;
        return S_OK;
    }
    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
};

// Copied in by the shell, as Explorer's Paste copies, where writing it
// straight in does not do: a place not on disk (a .zip, a phone) is written
// as Explorer writes it, a folder that needs an administrator asks for one.
// From a file of that name in %TEMP%; another name when it is taken there.
// No progress window (FOF_SILENT): it took the front from Explorer for a
// moment, so the name was not edited. (Explorer's Ctrl+Z undoes only its own
// operations: its undo list is in its process.) `made` stays null when the
// file did not go: cancelled, or failed, which the shell has said.
HRESULT copyInto(IShellItem* folder, const std::wstring& name, std::string_view bytes, ComPtr<IShellItem>& made)
{
    const std::wstring temp = tempFolder();
    if (temp.empty())
        return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    const std::wstring file = temp + L'\\' + name;
    const HRESULT hr = writeNew(file, bytes);
    ComPtr<IFileOperation> operation;
    ComPtr<IShellItem> source;
    const ComPtr<CopySink> sink = Microsoft::WRL::Make<CopySink>();
    BOOL aborted = FALSE;
    if (SUCCEEDED(hr) && sink
        && SUCCEEDED(::CoCreateInstance(__uuidof(FileOperation), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation)))
        && SUCCEEDED(::SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&source)))
        && SUCCEEDED(operation->SetOperationFlags(FOF_SILENT | FOF_RENAMEONCOLLISION | FOF_NOCONFIRMMKDIR))
        && SUCCEEDED(operation->CopyItem(source.Get(), folder, nullptr, sink.Get()))
        && SUCCEEDED(operation->PerformOperations()) && SUCCEEDED(operation->GetAnyOperationsAborted(&aborted))
        && !aborted)
        made = sink->made;
    ::DeleteFileW(file.c_str());
    ::RemoveDirectoryW(temp.c_str());
    return hr;
}

// Whether the view's focused item is `made`.
bool focusedIs(IShellView* view, IShellItem* made)
{
    ComPtr<IFolderView2> folderView;
    ComPtr<IShellItem> focused;
    int index = -1;
    return SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&folderView))) && folderView->GetFocusedItem(&index) == S_OK
        && index >= 0 && SUCCEEDED(folderView->GetItem(index, IID_PPV_ARGS(&focused))) && sameItem(focused.Get(), made);
}

// The file made, selected in the view (scrolled to, the rest deselected),
// its name ready to be typed while the window is still in front, as 新建
// leaves a new file. A view takes the selection of an item it does not list
// yet for later, when the shell has told it (a few hundred milliseconds for a
// file from another process), but not the editing: that waits until the item
// selected has the focus (asked every 25 ms, three seconds at most).
void select(IShellBrowser* browser, IShellItem* made, HWND window)
{
    PIDLIST_ABSOLUTE pidl = nullptr;
    ComPtr<IShellView> view;
    if (FAILED(::SHGetIDListFromObject(made, &pidl)))
        return;
    const PCUITEMID_CHILD child = ::ILFindLastID(pidl);
    constexpr UINT kSelect = SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED;
    if (SUCCEEDED(browser->QueryActiveShellView(&view)) && view && SUCCEEDED(view->SelectItem(child, kSelect))) {
        for (const ULONGLONG deadline = ::GetTickCount64() + 3000; ::GetTickCount64() < deadline; ::Sleep(25)) {
            if (focusedIs(view.Get(), made)) {
                if (::GetForegroundWindow() == window)
                    view->SelectItem(child, kSelect | SVSI_EDIT);
                break;
            }
        }
    }
    ::ILFree(pidl);
}

} // namespace

void paste(const Target& target, bool english)
{
    // Its messages sharp on any monitor.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const Texts& texts = english ? kEnglish : kChinese;
    const bool asked = !target.view; // the right-click is answered; Ctrl+V is silent where Explorer did nothing
    const Place place = asked ? placeFor(target.folder) : placeOf(target.view);
    if (!place.folder || !holdsFiles(place.folder.Get())) {
        if (asked)
            tell(place.window, texts, texts.noPlace, texts.noPlaceHint, false);
        return;
    }
    const std::optional<Contents> contents = readClipboard();
    if (!contents) {
        tell(place.window, texts, texts.busy, texts.busyHint, false);
        return;
    }
    if (!contents->kind) {
        if (asked) {
            const bool files = contents->offer.files || contents->offer.virtualFiles;
            tell(place.window, texts, files ? texts.files : texts.nothing, files ? texts.filesHint : texts.nothingHint,
                false);
        }
        return;
    }
    const pf::Kind kind = *contents->kind;

    std::string bytes;
    HRESULT hr = S_OK;
    if (kind != pf::Kind::Picture) {
        bytes = encoded(pf::textFile(kind, contents->text, listSeparator()));
    } else if (!contents->png.empty()) {
        bytes.assign(reinterpret_cast<const char*>(contents->png.data()), contents->png.size());
    } else {
        const std::vector<BYTE> bmp = bmpFile(contents->dib);
        hr = bmp.empty() ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) : encodePng(bmp, bytes);
    }

    SYSTEMTIME now {};
    ::GetLocalTime(&now);
    const std::wstring when = pf::stamp(now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    const std::wstring_view extension = pf::extension(kind);
    std::wstring onDiskAt = displayName(place.folder.Get(), SIGDN_FILESYSPATH);
    // A .zip has a path too, but is a file: the shell writes into it.
    if (const DWORD attributes = ::GetFileAttributesW(onDiskAt.c_str());
        attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        onDiskAt.clear();
    const std::wstring base = pf::fitName(
        pf::baseName(kind, contents->text, english ? pf::englishWords() : pf::Words {}, when), extension, onDiskAt.size());

    ComPtr<IShellItem> made;
    bool viaShell = onDiskAt.empty();
    if (SUCCEEDED(hr) && !viaShell) {
        std::wstring path;
        hr = writeInto(onDiskAt, base, extension, bytes, path);
        if (SUCCEEDED(hr)) {
            ::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&made));
            // Views hear late of a file another process made: told at once.
            ::SHChangeNotify(SHCNE_CREATE, SHCNF_PATH | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr);
        } else if (hr == E_ACCESSDENIED) {
            viaShell = true; // Program Files and the like: the shell asks for an administrator
            hr = S_OK;
        }
    }
    if (SUCCEEDED(hr) && viaShell)
        hr = copyInto(place.folder.Get(), base + std::wstring(extension), bytes, made);
    if (FAILED(hr)) {
        tell(place.window, texts, texts.failed, systemMessage(hr), true);
        return;
    }
    if (made && place.browser)
        select(place.browser.Get(), made.Get(), place.window);
}

} // namespace ws::pasteasfile
