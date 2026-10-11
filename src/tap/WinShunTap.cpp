// WinShunTap.dll: a XAML diagnostics "TAP" that Windows loads into explorer.exe
// for Win顺 (platform/TaskbarStartGap asks for it). It does one thing: puts a
// right Margin on the taskbar's Start button (Taskbar.ExperienceToggleButton,
// AutomationId "StartButton") of the PRIMARY taskbar, so the app icons move
// right and the search box has room next to Start. The margin comes in over
// a shared-memory channel (TapProtocol.h); it goes back to the default when
// Win顺 asks, or by itself when the Win顺 process ends.
//
// Entered by Windows on the XAML thread it was attached to (SetSite), then by
// the visual tree callbacks on that thread. Its own worker thread only waits
// on the channel and on Win顺's process handle and hands work to the XAML
// thread through its dispatcher queue: XAML objects are touched on that
// thread alone. Every entry point is guarded (C++ and SEH): nothing of ours
// may bring Explorer down. Never unloads (DllCanUnloadNow: S_FALSE).
//
// What the sources say (WinUI's LiveVisualTree.cpp, DebugTool.cpp): the TAP
// is loaded and sited on the XAML thread; the callback's strings are handed
// over to it; the runtime enumerates the tree with IFC_RETURN on the callback,
// so it must always answer S_OK; AdviseVisualTreeChange marshals to the XAML
// thread itself, so it is called from another thread (as TranslucentTB and
// the Windhawk styler do), else it can hang.

#include <unknwn.h>
#include <windows.h>

#include <ocidl.h> // IObjectWithSite
#include <xamlOM.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

#undef GetCurrentTime // windows.h's macro collides with Windows.UI.Xaml.Media.Animation
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "TapProtocol.h"

namespace wux = winrt::Windows::UI::Xaml;
namespace wuxh = winrt::Windows::UI::Xaml::Hosting;

namespace {

HMODULE g_module = nullptr;

void trace(const wchar_t* format, ...)
{
    wchar_t line[512];
    va_list args;
    va_start(args, format);
    std::vswprintf(line, 512, format, args);
    va_end(args);
    std::wstring out = L"[WinShunTap] ";
    out += line;
    out += L"\n";
    ::OutputDebugStringW(out.c_str());
}

// SEH round a C++ callable: an access violation in our code becomes an
// HRESULT, not a dead Explorer. (No C++ objects with destructors may live in
// a function with __try, hence the split.)
template <typename F> HRESULT guarded(F& f)
{
    __try {
        return f();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return HRESULT_FROM_WIN32(ERROR_UNHANDLED_EXCEPTION);
    }
}

template <typename F> HRESULT guard(F&& f) noexcept
{
    auto wrapped = [&f]() -> HRESULT {
        try {
            return f();
        } catch (...) {
            return winrt::to_hresult();
        }
    };
    return guarded(wrapped);
}

std::wstring className(HWND window)
{
    wchar_t name[64] {};
    if (window)
        ::GetClassNameW(window, name, 64);
    return name;
}

// A value from the initialization data ("ep=3"): the endpoint index.
int endpointFrom(const wchar_t* data)
{
    if (!data)
        return 0;
    const wchar_t* at = std::wcsstr(data, L"ep=");
    return at ? std::wcstol(at + 3, nullptr, 10) : 0;
}

struct Tap : winrt::implements<Tap, IObjectWithSite, IVisualTreeServiceCallback2, winrt::non_agile> {
    ~Tap() { stop(); }

    // ---- IObjectWithSite (the XAML thread) ----

    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override
    {
        return guard([&]() -> HRESULT {
            if (!site) {
                stop();
                m_site = nullptr;
                return S_OK;
            }
            // Loaded for good: Windows keeps a reference to us anyway, and an
            // unload with a thread of ours still running would crash Explorer.
            HMODULE pinned = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&g_module), &pinned);
            m_site.copy_from(site);
            m_diag = m_site.as<IXamlDiagnostics>();
            m_tree = m_diag.as<IVisualTreeService3>();
            m_thread = ::GetCurrentThreadId();
            const HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
            DWORD trayProcess = 0;
            const DWORD trayThread = tray ? ::GetWindowThreadProcessId(tray, &trayProcess) : 0;

            wchar_t* data = nullptr; // BSTR
            m_diag->GetInitializationData(&data);
            m_endpoint = endpointFrom(data);
            ::SysFreeString(data);

            if (!openChannel())
                return S_OK; // nothing to talk to us through: stay idle
            m_shared->explorerPid = ::GetCurrentProcessId();
            m_shared->xamlThreadId = m_thread;
            m_shared->taskbarThreadId = trayThread;
            m_shared->endpoint = static_cast<uint32_t>(m_endpoint);
            if (trayProcess != ::GetCurrentProcessId() || trayThread != m_thread) {
                // Another XAML thread of explorer (a folder window's island, the
                // input switcher...): not ours. Idle for good, says so.
                m_shared->state = static_cast<uint32_t>(wstap::State::WrongCore);
                trace(L"SetSite on thread %lu, the taskbar's is %lu: wrong core (endpoint %d)", m_thread, trayThread, m_endpoint);
                return S_OK;
            }
            // How we get back onto this thread later.
            m_queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
            if (!m_queue) {
                ::IInspectable* raw = nullptr; // the ABI's IInspectable, not C++/WinRT's
                if (SUCCEEDED(m_diag->GetDispatcher(&raw)) && raw) {
                    winrt::com_ptr<::IInspectable> dispatcher;
                    dispatcher.attach(raw);
                    m_dispatcher = dispatcher.try_as<winrt::Windows::UI::Core::CoreDispatcher>();
                }
            }
            if (!m_queue && !m_dispatcher) {
                m_shared->state = static_cast<uint32_t>(wstap::State::Failed);
                m_shared->lastError = E_UNEXPECTED;
                trace(L"no dispatcher on the taskbar thread");
                return S_OK;
            }
            m_stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            m_worker = std::thread([self = get_strong()] { self->work(); });
            trace(L"attached on the taskbar thread %lu (endpoint %d)", m_thread, m_endpoint);
            return S_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** out) noexcept override
    {
        return guard([&]() -> HRESULT {
            if (!m_site)
                return E_FAIL;
            return m_site.as(riid, out);
        });
    }

    // ---- IVisualTreeServiceCallback2 (the XAML thread of the element) ----

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation relation, VisualElement element, VisualMutationType mutation) noexcept override
    {
        // The strings are handed over to us: freed here, whatever happens.
        struct Strings {
            VisualElement& e;
            ~Strings()
            {
                ::SysFreeString(e.SrcInfo.FileName);
                ::SysFreeString(e.SrcInfo.Hash);
                ::SysFreeString(e.Type);
                ::SysFreeString(e.Name);
            }
        } strings {element};
        // Always S_OK: one failing answer would abort the runtime's enumeration
        // of the tree, and with it the advise. Failures are counted instead.
        const HRESULT hr = guard([&]() -> HRESULT {
            if (::GetCurrentThreadId() != m_thread || !m_shared)
                return S_OK; // another XAML thread's element
            if (mutation == Remove) {
                removed(element.Handle);
                return S_OK;
            }
            ++m_shared->adds;
            const std::wstring_view type = element.Type ? element.Type : L"";
            if (type == L"Windows.UI.Xaml.Hosting.DesktopWindowXamlSource") {
                m_sources.insert(element.Handle);
            } else if (type == L"Taskbar.ExperienceToggleButton") {
                consider(element.Handle, relation.Parent);
            }
            if (!m_pending.empty())
                retryPending();
            return S_OK;
        });
        if (FAILED(hr) && m_shared) {
            m_shared->lastCallbackError = hr;
            ++m_shared->callbackErrors;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) noexcept override
    {
        return S_OK;
    }

private:
    enum class Where { Unknown, Primary, Elsewhere };

    // ---- the channel ----

    bool openChannel()
    {
        const DWORD pid = ::GetCurrentProcessId();
        m_section = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(wstap::Shared),
            wstap::sectionName(pid, m_endpoint).c_str());
        if (!m_section)
            return false;
        const bool fresh = ::GetLastError() != ERROR_ALREADY_EXISTS;
        m_shared = static_cast<wstap::Shared*>(::MapViewOfFile(m_section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(wstap::Shared)));
        if (!m_shared)
            return false;
        if (fresh)
            *m_shared = wstap::Shared {};
        m_request = ::CreateEventW(nullptr, FALSE, FALSE, wstap::requestEventName(pid, m_endpoint).c_str());
        m_ack = ::CreateEventW(nullptr, FALSE, FALSE, wstap::ackEventName(pid, m_endpoint).c_str());
        return m_request && m_ack;
    }

    // The worker: advises (off the XAML thread), then waits for requests and
    // for the Win顺 process to end.
    void work()
    {
        const HRESULT advised = m_tree->AdviseVisualTreeChange(this);
        m_shared->lastError = advised;
        m_shared->state = static_cast<uint32_t>(SUCCEEDED(advised) ? wstap::State::Primary : wstap::State::Failed);
        m_advised = SUCCEEDED(advised);
        trace(L"AdviseVisualTreeChange: 0x%08X", advised);
        for (;;) {
            HANDLE handles[3] = {m_stop, m_request, m_client};
            const DWORD count = m_client ? 3 : 2;
            const DWORD woke = ::WaitForMultipleObjects(count, handles, FALSE, INFINITE);
            if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED)
                return;
            if (woke == WAIT_OBJECT_0 + 1) {
                takeRequest();
            } else if (woke == WAIT_OBJECT_0 + 2) {
                trace(L"Win顺 (pid %lu) ended: margin back to the default", m_clientPid);
                dropClient();
                post([self = get_strong()] { self->apply(0, 0); });
            }
        }
    }

    void takeRequest()
    {
        // A copy: Win顺 may write again while this one is handled.
        const uint32_t seq = m_shared->requestSeq;
        const int32_t centi = m_shared->marginCentiDips;
        const uint32_t pid = m_shared->clientPid;
        const uint64_t handle = m_shared->clientHandle;
        if (m_shared->magic != wstap::kMagic || centi < 0 || centi > wstap::kMaxMarginCentiDips) {
            trace(L"bad request %u refused", seq);
            m_shared->applyError = E_INVALIDARG; // answered, so the client can tell "refused" from "dead"
            m_shared->appliedSeq = seq;
            ::SetEvent(m_ack);
            return;
        }
        if (pid != m_clientPid || handle != m_clientHandleValue) {
            dropClient();
            m_clientPid = pid;
            m_clientHandleValue = handle;
            if (handle)
                m_client = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
            else if (pid)
                m_client = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        }
        post([self = get_strong(), seq, centi] { self->apply(seq, centi); });
    }

    void dropClient()
    {
        if (m_client)
            ::CloseHandle(m_client);
        m_client = nullptr;
        m_clientPid = 0;
        m_clientHandleValue = 0;
    }

    // To the XAML thread.
    template <typename F> void post(F&& f)
    {
        if (m_queue) {
            m_queue.TryEnqueue(winrt::Windows::System::DispatcherQueueHandler(std::forward<F>(f)));
        } else if (m_dispatcher) {
            m_dispatcher.RunAsync(
                winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, winrt::Windows::UI::Core::DispatchedHandler(std::forward<F>(f)));
        }
    }

    void stop()
    {
        if (m_stop)
            ::SetEvent(m_stop);
        if (m_worker.joinable()) {
            if (::WaitForSingleObject(m_worker.native_handle(), 3000) == WAIT_OBJECT_0)
                m_worker.join();
            else
                m_worker.detach();
        }
        if (m_advised && m_tree && ::GetCurrentThreadId() == m_thread) {
            apply(0, 0);
            m_tree->UnadviseVisualTreeChange(this);
            m_advised = false;
        }
        dropClient();
        if (m_shared)
            ::UnmapViewOfFile(m_shared);
        m_shared = nullptr;
        for (HANDLE* h : {&m_section, &m_request, &m_ack, &m_stop}) {
            if (*h)
                ::CloseHandle(*h);
            *h = nullptr;
        }
    }

    // ---- the Start button (the XAML thread) ----

    template <typename T> T fromHandle(InstanceHandle handle)
    {
        ::IInspectable* raw = nullptr;
        winrt::check_hresult(m_diag->GetIInspectableFromHandle(handle, &raw));
        winrt::com_ptr<::IInspectable> object;
        object.attach(raw);
        return object.as<T>();
    }

    // A Taskbar.ExperienceToggleButton was added: the Start button of the
    // primary taskbar is remembered and gets the margin wanted.
    void consider(InstanceHandle handle, InstanceHandle)
    {
        const wux::FrameworkElement element = fromHandle<wux::FrameworkElement>(handle);
        if (wux::Automation::AutomationProperties::GetAutomationId(element) != L"StartButton")
            return;
        switch (where(element)) {
        case Where::Primary:
            m_startHandle = handle;
            m_start = element;
            m_shared->startSeen = 1;
            trace(L"StartButton of the primary taskbar: handle %llx", static_cast<unsigned long long>(handle));
            apply(0, m_wantedCentiDips); // seq 0: no ack, nobody asked just now
            break;
        case Where::Elsewhere:
            break;
        case Where::Unknown: // its island not known yet: again on the next change
            m_pending.push_back(handle);
            break;
        }
    }

    void retryPending()
    {
        std::vector<InstanceHandle> again;
        again.swap(m_pending);
        for (const InstanceHandle handle : again) {
            try {
                consider(handle, 0);
            } catch (...) {
                // gone, or not an element: dropped
            }
        }
    }

    void removed(InstanceHandle handle)
    {
        m_sources.erase(handle);
        std::erase(m_pending, handle);
        if (handle == m_startHandle) {
            m_startHandle = 0;
            m_start = nullptr;
            m_shared->startSeen = 0;
            trace(L"StartButton removed");
        }
    }

    // Which taskbar an element is on: through its XAML root to the
    // DesktopWindowXamlSource whose content that root is, to the island's
    // window, to its top-level window's class: Shell_TrayWnd is the primary,
    // Shell_SecondaryTrayWnd another monitor's.
    Where where(const wux::FrameworkElement& element)
    {
        winrt::com_ptr<IUnknown> root;
        try {
            if (const wux::XamlRoot xamlRoot = element.XamlRoot())
                root = xamlRoot.Content().as<IUnknown>();
        } catch (...) {
        }
        if (!root) { // the old way: up the tree
            wux::DependencyObject at = element;
            for (wux::DependencyObject up = wux::Media::VisualTreeHelper::GetParent(at); up; up = wux::Media::VisualTreeHelper::GetParent(at))
                at = up;
            root = at.as<IUnknown>();
        }
        for (const InstanceHandle source : m_sources) {
            try {
                const wuxh::DesktopWindowXamlSource xamlSource = fromHandle<wuxh::DesktopWindowXamlSource>(source);
                const wux::UIElement content = xamlSource.Content();
                if (!content || content.as<IUnknown>() != root)
                    continue;
                HWND island = nullptr;
                winrt::check_hresult(xamlSource.as<IDesktopWindowXamlSourceNative>()->get_WindowHandle(&island));
                const std::wstring top = className(::GetAncestor(island, GA_ROOT));
                ::wcsncpy_s(m_shared->rootClass, top.c_str(), _TRUNCATE);
                return top == L"Shell_TrayWnd" ? Where::Primary : Where::Elsewhere;
            } catch (...) {
                continue; // a source of another thread, or one gone: the others still count
            }
        }
        return Where::Unknown;
    }

    // Puts `centi` hundredths of a DIP of right margin on the Start button
    // (0: the local value cleared, the style's own back), answers Win顺.
    void apply(uint32_t seq, int32_t centi)
    {
        m_wantedCentiDips = centi;
        HRESULT hr = S_OK;
        if (const wux::FrameworkElement start = m_start.get()) {
            try {
                if (centi > 0)
                    start.Margin(wux::Thickness {0, 0, centi / 100.0, 0});
                else
                    start.ClearValue(wux::FrameworkElement::MarginProperty());
            } catch (...) {
                hr = winrt::to_hresult();
            }
        }
        if (!m_shared)
            return;
        m_shared->applyError = hr;
        m_shared->appliedCentiDips = SUCCEEDED(hr) && m_start.get() ? centi : 0;
        if (seq) {
            m_shared->appliedSeq = seq;
            ::SetEvent(m_ack);
        }
        trace(L"apply %d centi-DIPs (seq %u): 0x%08X", centi, seq, hr);
    }

    winrt::com_ptr<IUnknown> m_site;
    winrt::com_ptr<IXamlDiagnostics> m_diag;
    winrt::com_ptr<IVisualTreeService3> m_tree;
    DWORD m_thread = 0;
    int m_endpoint = 0;
    bool m_advised = false;
    winrt::Windows::System::DispatcherQueue m_queue {nullptr};
    winrt::Windows::UI::Core::CoreDispatcher m_dispatcher {nullptr};

    HANDLE m_section = nullptr;
    wstap::Shared* m_shared = nullptr;
    HANDLE m_request = nullptr;
    HANDLE m_ack = nullptr;
    HANDLE m_stop = nullptr;
    std::thread m_worker;
    HANDLE m_client = nullptr; // Win顺's process
    DWORD m_clientPid = 0;
    uint64_t m_clientHandleValue = 0;

    // The XAML thread's alone.
    std::unordered_set<InstanceHandle> m_sources; // DesktopWindowXamlSource elements seen
    std::vector<InstanceHandle> m_pending; // Start buttons whose island is not known yet
    InstanceHandle m_startHandle = 0;
    winrt::weak_ref<wux::FrameworkElement> m_start;
    int32_t m_wantedCentiDips = 0;
};

struct Factory : winrt::implements<Factory, IClassFactory> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** out) noexcept override
    {
        return guard([&]() -> HRESULT {
            *out = nullptr;
            if (outer)
                return CLASS_E_NOAGGREGATION;
            return winrt::make<Tap>().as(riid, out);
        });
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        ::DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

_Use_decl_annotations_ STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
{
    return guard([&]() -> HRESULT {
        *out = nullptr;
        if (clsid != wstap::kTapClsid)
            return CLASS_E_CLASSNOTAVAILABLE;
        return winrt::make<Factory>().as(riid, out);
    });
}

STDAPI DllCanUnloadNow() { return S_FALSE; } // resident until Explorer ends
