#include "PieDrop.h"

#include <shellapi.h>
#include <shlobj.h>

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

} // namespace

DropTarget::DropTarget(Handler* handler, HWND window)
    : m_handler(handler)
    , m_window(window)
{
    // Absent (no shell, a stripped-down Windows): the drag goes on without its picture.
    ::CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_helper));
}

void DropTarget::leaveEarly()
{
    if (!std::exchange(m_inside, false))
        return;
    if (m_helper)
        m_helper->DragLeave();
}

STDMETHODIMP DropTarget::DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    if (!effect)
        return E_INVALIDARG;
    m_inside = true;
    POINT at = pointOf(pt);
    *effect = m_handler && data ? m_handler->dragEnter(data, keys, at, *effect) : DROPEFFECT_NONE;
    if (m_helper && data && m_inside) // the handler may have gone at once (a drag of text)
        m_helper->DragEnter(m_window, data, &at, *effect);
    return S_OK;
}

STDMETHODIMP DropTarget::DragOver(DWORD keys, POINTL pt, DWORD* effect)
{
    if (!effect)
        return E_INVALIDARG;
    POINT at = pointOf(pt);
    if (!m_inside) { // gone already: OLE moves on at the pointer's next move
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    *effect = m_handler ? m_handler->dragOver(keys, at, *effect) : DROPEFFECT_NONE;
    if (m_helper && m_inside) // the handler may have left early just now
        m_helper->DragOver(&at, *effect);
    return S_OK;
}

STDMETHODIMP DropTarget::DragLeave()
{
    if (!std::exchange(m_inside, false))
        return S_OK;
    if (m_helper)
        m_helper->DragLeave();
    if (m_handler)
        m_handler->dragLeave();
    return S_OK;
}

STDMETHODIMP DropTarget::Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    if (!effect)
        return E_INVALIDARG;
    POINT at = pointOf(pt);
    if (!std::exchange(m_inside, false)) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    *effect = m_handler && data ? m_handler->drop(data, keys, at, *effect) : DROPEFFECT_NONE;
    if (m_helper && data)
        m_helper->Drop(data, &at, *effect);
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
    std::function<void(HRESULT result, bool aborted)> done)
{
    std::lock_guard lock(m_mutex);
    std::erase_if(m_running, [](const std::unique_ptr<Operation>& op) { return op->finished.load(); });
    auto op = std::make_unique<Operation>();
    Operation* const self = op.get();
    op->thread = std::jthread([self, items = std::move(items), folder = std::move(folder), move, done = std::move(done)] {
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
        if (done)
            done(hr, aborted != FALSE);
        if (SUCCEEDED(init))
            ::CoUninitialize();
        self->finished = true;
    });
    m_running.push_back(std::move(op));
}

} // namespace ws::piedrop
