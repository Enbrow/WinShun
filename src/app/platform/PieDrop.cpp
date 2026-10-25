#include "PieDrop.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstddef>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace ws::piedrop {

namespace {

POINT pointOf(POINTL pt)
{
    return {pt.x, pt.y};
}

FORMATETC hdropFormat()
{
    return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
}

FORMATETC shellIdListFormat()
{
    static const auto format = static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
    return {format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
}

// A STGMEDIUM released however it was filled.
struct Medium {
    STGMEDIUM medium {};
    ~Medium() { ::ReleaseStgMedium(&medium); }
};

std::wstring folded(std::wstring text)
{
    if (!text.empty())
        ::CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

std::wstring withSlash(std::wstring path)
{
    if (!path.empty() && path.back() != L'\\')
        path += L'\\';
    return path;
}

// The temporary folder, lower-case with its backslash: as GetTempPath gives
// it (short names, perhaps) and in its long form.
const std::vector<std::wstring>& tempFolders()
{
    static const std::vector<std::wstring> folders = [] {
        std::vector<std::wstring> result;
        std::wstring raw(MAX_PATH + 1, L'\0');
        const DWORD length = ::GetTempPathW(static_cast<DWORD>(raw.size()), raw.data());
        if (length == 0 || length >= raw.size())
            return result;
        raw.resize(length);
        result.push_back(folded(withSlash(raw)));
        std::wstring full(32768, L'\0');
        const DWORD fullLength = ::GetLongPathNameW(raw.c_str(), full.data(), static_cast<DWORD>(full.size()));
        if (fullLength > 0 && fullLength < full.size()) {
            full.resize(fullLength);
            full = folded(withSlash(full));
            if (full != result.front())
                result.push_back(std::move(full));
        }
        return result;
    }();
    return folders;
}

// Gone when the drag is over, or not the item's real name: an archiver's
// unpacked copy in the temporary folder, a deleted file's $R… in the bin.
bool transient(const std::wstring& path)
{
    const std::wstring lower = folded(path);
    if (lower.find(L"\\$recycle.bin\\") != std::wstring::npos)
        return true;
    return std::ranges::any_of(tempFolders(), [&](const std::wstring& temp) { return lower.starts_with(temp); });
}

} // namespace

STDMETHODIMP DropTarget::DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    if (!effect)
        return E_INVALIDARG;
    m_inside = true;
    *effect = m_handler && data ? m_handler->dragEnter(data, keys, pointOf(pt), *effect) : DROPEFFECT_NONE;
    return S_OK;
}

STDMETHODIMP DropTarget::DragOver(DWORD keys, POINTL pt, DWORD* effect)
{
    if (!effect)
        return E_INVALIDARG;
    // Gone already: OLE moves on at the pointer's next move.
    *effect = m_inside && m_handler ? m_handler->dragOver(keys, pointOf(pt), *effect) : DROPEFFECT_NONE;
    return S_OK;
}

STDMETHODIMP DropTarget::DragLeave()
{
    if (std::exchange(m_inside, false) && m_handler)
        m_handler->dragLeave();
    return S_OK;
}

STDMETHODIMP DropTarget::Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    if (!effect)
        return E_INVALIDARG;
    const bool inside = std::exchange(m_inside, false);
    *effect = inside && m_handler && data ? m_handler->drop(data, keys, pointOf(pt), *effect) : DROPEFFECT_NONE;
    return S_OK;
}

bool hasFiles(IDataObject* data)
{
    FORMATETC format = hdropFormat();
    return data && data->QueryGetData(&format) == S_OK;
}

bool hasShellItems(IDataObject* data)
{
    FORMATETC format = shellIdListFormat();
    return data && data->QueryGetData(&format) == S_OK;
}

std::vector<std::wstring> filePaths(IDataObject* data)
{
    std::vector<std::wstring> paths;
    FORMATETC format = hdropFormat();
    Medium m;
    if (!data || FAILED(data->GetData(&format, &m.medium)) || m.medium.tymed != TYMED_HGLOBAL)
        return paths;
    const auto drop = static_cast<HDROP>(::GlobalLock(m.medium.hGlobal));
    if (!drop)
        return paths;
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    paths.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(length, L'\0');
        if (length > 0 && ::DragQueryFileW(drop, i, path.data(), length + 1) == length)
            paths.push_back(std::move(path));
    }
    ::GlobalUnlock(m.medium.hGlobal);
    return paths;
}

DragItems readItems(IDataObject* data)
{
    DragItems items;
    if (!data)
        return items;
    bool fromBin = false;
    if (hasShellItems(data)) {
        items.fromIdList = true;
        FORMATETC format = shellIdListFormat();
        Medium m;
        if (FAILED(data->GetData(&format, &m.medium)) || m.medium.tymed != TYMED_HGLOBAL)
            return items;
        const SIZE_T size = ::GlobalSize(m.medium.hGlobal);
        const auto* ida = static_cast<const CIDA*>(::GlobalLock(m.medium.hGlobal));
        if (!ida)
            return items;
        // The folder's ID list at aoffset[0], then each item's relative to it.
        const auto base = reinterpret_cast<const BYTE*>(ida);
        const SIZE_T table = offsetof(CIDA, aoffset) + sizeof(UINT) * (static_cast<SIZE_T>(ida->cidl) + 1);
        const auto at = [&](UINT i) -> const void* {
            return ida->aoffset[i] + sizeof(USHORT) <= size ? base + ida->aoffset[i] : nullptr;
        };
        if (size >= offsetof(CIDA, aoffset) + sizeof(UINT) && table <= size && at(0)) {
            const auto parent = static_cast<PCIDLIST_ABSOLUTE>(at(0));
            PIDLIST_ABSOLUTE bin = nullptr;
            if (SUCCEEDED(::SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &bin)) && ::ILIsEqual(parent, bin))
                fromBin = true;
            ::CoTaskMemFree(bin);
            items.count = ida->cidl;
            for (UINT i = 0; i < ida->cidl; ++i) {
                const void* child = at(i + 1);
                PIDLIST_ABSOLUTE full = child ? ::ILCombine(parent, static_cast<PCUIDLIST_RELATIVE>(child)) : nullptr;
                ComPtr<IShellItem> item;
                SFGAOF attributes = 0;
                PWSTR path = nullptr;
                if (full && SUCCEEDED(::SHCreateItemFromIDList(full, IID_PPV_ARGS(&item)))
                    && SUCCEEDED(item->GetAttributes(SFGAO_FILESYSTEM, &attributes)) && (attributes & SFGAO_FILESYSTEM)
                    && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path && *path)
                    items.paths.emplace_back(path);
                ::CoTaskMemFree(path);
                ::ILFree(full);
            }
        }
        ::GlobalUnlock(m.medium.hGlobal);
    } else {
        items.paths = filePaths(data);
        items.count = items.paths.size();
    }
    items.plain = items.count > 0 && !fromBin && items.paths.size() == items.count
        && std::ranges::none_of(items.paths, transient);
    return items;
}

ComPtr<IAgileReference> startAsync(IDataObject* data)
{
    ComPtr<IDataObjectAsyncCapability> async;
    BOOL on = FALSE;
    if (!data || FAILED(data->QueryInterface(IID_PPV_ARGS(&async))) || FAILED(async->GetAsyncMode(&on)) || !on)
        return nullptr;
    ComPtr<IAgileReference> reference;
    if (FAILED(::RoGetAgileReference(AGILEREFERENCE_DEFAULT, __uuidof(IDataObjectAsyncCapability), async.Get(), &reference))
        || FAILED(async->StartOperation(nullptr)))
        return nullptr;
    return reference;
}

void endAsync(IAgileReference* reference, HRESULT result)
{
    ComPtr<IDataObjectAsyncCapability> async;
    if (reference && SUCCEEDED(reference->Resolve(IID_PPV_ARGS(&async))))
        async->EndOperation(result, nullptr, DROPEFFECT_NONE); // nothing for the source to delete: moved here already
}

DWORD dropOnFolder(const std::wstring& folder, IDataObject* data, bool move, POINT pt, DWORD allowed, HRESULT* result)
{
    const DWORD wanted = allowed & (move ? DROPEFFECT_MOVE : DROPEFFECT_COPY);
    const DWORD key = move ? MK_SHIFT : MK_CONTROL;
    const POINTL at {pt.x, pt.y};
    ComPtr<IShellItem> item;
    ComPtr<IDropTarget> target;
    HRESULT hr = ::SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (SUCCEEDED(hr))
        hr = item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target));
    DWORD effect = DROPEFFECT_NONE;
    if (SUCCEEDED(hr) && wanted) {
        // As a drag would go over it: entered and moved over with the
        // button held, then let go with the key that says move or copy.
        effect = wanted;
        hr = target->DragEnter(data, MK_LBUTTON | key, at, &effect);
        if (SUCCEEDED(hr) && (effect & wanted)) {
            effect = wanted;
            hr = target->DragOver(MK_LBUTTON | key, at, &effect);
        }
        if (SUCCEEDED(hr) && (effect & wanted)) {
            effect = wanted;
            hr = target->Drop(data, key, at, &effect);
        } else {
            target->DragLeave();
            effect = DROPEFFECT_NONE;
            if (SUCCEEDED(hr))
                hr = E_ABORT; // the folder does not take these
        }
    }
    if (result)
        *result = wanted ? hr : E_INVALIDARG;
    return SUCCEEDED(hr) ? effect & wanted : DROPEFFECT_NONE;
}

FileOperations::~FileOperations()
{
    std::lock_guard lock(m_mutex);
    m_running.clear(); // each jthread joins: a copy under way is let finish
}

void FileOperations::start(std::vector<std::wstring> items, std::wstring folder, bool move,
    ComPtr<IAgileReference> async, std::function<void(HRESULT result, bool aborted)> done)
{
    std::lock_guard lock(m_mutex);
    std::erase_if(m_running, [](const std::unique_ptr<Operation>& op) { return op->finished.load(); });
    auto op = std::make_unique<Operation>();
    Operation* const self = op.get();
    op->thread = std::jthread([self, items = std::move(items), folder = std::move(folder), move, async = std::move(async),
                                  done = std::move(done)]() mutable {
        const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        HRESULT hr = init;
        BOOL aborted = FALSE;
        if (SUCCEEDED(init)) {
            ComPtr<IFileOperation> operation;
            ComPtr<IShellItem> destination;
            hr = ::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation));
            if (SUCCEEDED(hr))
                hr = ::SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&destination));
            if (SUCCEEDED(hr)) {
                // Undoable, and on File Explorer's undo list as its own moves
                // are; Windows asks about names taken and shows the progress.
                hr = operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOFX_ADDUNDORECORD);
            }
            bool queued = false;
            for (const std::wstring& path : items) {
                if (FAILED(hr))
                    break;
                ComPtr<IShellItem> item;
                if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))))
                    continue; // gone meanwhile: the others still go
                const HRESULT queuedHr = move ? operation->MoveItem(item.Get(), destination.Get(), nullptr, nullptr)
                                              : operation->CopyItem(item.Get(), destination.Get(), nullptr, nullptr);
                queued = queued || SUCCEEDED(queuedHr);
            }
            if (SUCCEEDED(hr) && !queued)
                hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            if (SUCCEEDED(hr))
                hr = operation->PerformOperations();
            if (operation)
                operation->GetAnyOperationsAborted(&aborted);
        } // released before COM is let go
        if (SUCCEEDED(init))
            endAsync(async.Get(), hr);
        async.Reset(); // likewise
        if (done)
            done(hr, aborted != FALSE);
        if (SUCCEEDED(init))
            ::CoUninitialize();
        self->finished = true;
    });
    m_running.push_back(std::move(op));
}

} // namespace ws::piedrop
