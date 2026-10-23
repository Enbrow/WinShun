#pragma once

#include <windows.h>
// after windows.h
#include <ole2.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ws::piedrop {

// What the drop pie's OLE drop target asks of its owner (DropPie), on the
// GUI thread, the source waiting: each answer must come at once. Points are
// screen pixels (physical); `allowed` is what the source allows
// (DROPEFFECT_*); the answers are the effect to tell it.
class Handler {
public:
    virtual DWORD dragEnter(IDataObject* data, DWORD keys, POINT pt, DWORD allowed) = 0;
    virtual DWORD dragOver(DWORD keys, POINT pt, DWORD allowed) = 0;
    virtual void dragLeave() = 0;
    virtual DWORD drop(IDataObject* data, DWORD keys, POINT pt, DWORD allowed) = 0;

protected:
    ~Handler() = default;
};

// The pie window's IDropTarget, registered in place of Qt's (RevokeDragDrop,
// then RegisterDragDrop: Qt never registers again unless the window is made
// anew). Every call is passed to the drag-image helper too, without which
// the dragged items' picture (SysDragImage) vanishes over the pie: only a
// target that calls IDropTargetHelper gets it drawn.
class DropTarget
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropTarget> {
public:
    DropTarget(Handler* handler, HWND window);

    // The owner is going: no more calls reach it.
    void detach() noexcept { m_handler = nullptr; }
    // The pie went away while the drag is still over the window (the
    // pointer left the ring): the picture's helper is told now, and the
    // DragLeave that OLE sends at its next move is not passed on again.
    void leaveEarly();

    STDMETHODIMP DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
    STDMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
    STDMETHODIMP DragLeave() override;
    STDMETHODIMP Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;

private:
    Handler* m_handler;
    HWND m_window;
    Microsoft::WRL::ComPtr<IDropTargetHelper> m_helper;
    bool m_inside = false; // between DragEnter and DragLeave or Drop, and not left early
};

// The file-system paths in a drag (CF_HDROP); none for shell items that
// are not files (a phone's, a zip file's contents).
std::vector<std::wstring> filePaths(IDataObject* data);
bool hasFiles(IDataObject* data); // CF_HDROP offered
bool hasShellItems(IDataObject* data); // CFSTR_SHELLIDLIST offered

// Hands shell items that are not files to the folder's own drop target, as
// File Explorer does when they are dropped on a folder (the shell copies
// them, its own progress and questions included): MK_SHIFT to move,
// MK_CONTROL to copy. The effect it reports, or DROPEFFECT_NONE. On the
// thread the data object belongs to (the GUI thread, inside Drop).
DWORD dropOnFolder(const std::wstring& folder, IDataObject* data, bool move, POINT pt, DWORD allowed, HRESULT* result);

// Copies and moves files with IFileOperation, each on a thread of its own
// (a single-threaded apartment): Windows' own progress window and questions
// (replace, skip, administrator rights), and an entry in File Explorer's
// undo (Ctrl+Z). Never waits on the caller's thread; the destructor waits
// for those still running, so no copy is cut off halfway.
class FileOperations {
public:
    FileOperations() = default;
    ~FileOperations();
    FileOperations(const FileOperations&) = delete;
    FileOperations& operator=(const FileOperations&) = delete;

    // `done` runs on the operation's thread: whether it worked (S_OK), and
    // whether the user cancelled part of it.
    void start(std::vector<std::wstring> items, std::wstring folder, bool move,
        std::function<void(HRESULT result, bool aborted)> done);

private:
    struct Operation {
        std::jthread thread;
        std::atomic<bool> finished {false};
    };
    std::mutex m_mutex;
    std::list<std::unique_ptr<Operation>> m_running;
};

} // namespace ws::piedrop
