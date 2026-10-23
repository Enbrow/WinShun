#include "UiaProbe.h"

#include "CaptureTools.h"

#include <QDebug>
#include <QPointer>

#include <ole2.h> // before uiautomation.h: WIN32_LEAN_AND_MEAN leaves it out of windows.h
#include <uiautomation.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace ws::capture {

namespace {

constexpr int kMaxDepth = 32; // Chromium nests a dozen wrapper panes before its content

struct Request {
    HWND window = nullptr;
    QPoint point;
};

// A program that draws its own controls (Chromium, so VS Code and the other Electron programs) builds the
// tree UI Automation reads only once some client listens for events (UiaClientsAreListening); a client that
// only asks gets the window's frame. This listens to the focus, and does nothing with it, while the probe lives.
class FocusSink : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IUIAutomationFocusChangedEventHandler> {
public:
    IFACEMETHODIMP HandleFocusChangedEvent(IUIAutomationElement*) override { return S_OK; }
};

} // namespace

struct UiaProbe::Shared {
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Request> request;
    bool stop = false;
    QPointer<QObject> context;
    Done done;
};

namespace {

// Where the walk is: the deadline, the elements still to be read, the longest chain found.
struct Dive {
    IUIAutomationCacheRequest* cache = nullptr;
    IUIAutomationCondition* all = nullptr;
    QPoint point;
    std::chrono::steady_clock::time_point deadline;
    int budget = 400; // elements read at most
    std::vector<QRect> chain; // the one being walked
    std::vector<QRect> best; // the deepest so far
};

// The children holding the point, the last (the one on top) first; Chromium wraps its content in a dozen
// panes of one size, some of them leading nowhere, so every one is tried and the deepest chain kept.
void dive(Dive& d, IUIAutomationElement* element, int depth)
{
    if (depth >= kMaxDepth || d.budget <= 0 || std::chrono::steady_clock::now() >= d.deadline)
        return;
    ComPtr<IUIAutomationElementArray> children;
    if (FAILED(element->FindAllBuildCache(TreeScope_Children, d.all, d.cache, &children)) || !children)
        return;
    int count = 0;
    children->get_Length(&count);
    struct Held {
        ComPtr<IUIAutomationElement> element;
        RECT rect;
    };
    std::vector<Held> holding;
    for (int i = count - 1; i >= 0; --i) {
        --d.budget;
        ComPtr<IUIAutomationElement> child;
        RECT rect {};
        BOOL offscreen = FALSE;
        if (FAILED(children->GetElement(i, &child)) || !child || FAILED(child->get_CachedBoundingRectangle(&rect))
            || FAILED(child->get_CachedIsOffscreen(&offscreen)) || offscreen)
            continue;
        if (d.point.x() >= rect.left && d.point.x() < rect.right && d.point.y() >= rect.top && d.point.y() < rect.bottom)
            holding.push_back({child, rect});
    }
    for (Held& h : holding) {
        d.chain.push_back(QRect(h.rect.left, h.rect.top, h.rect.right - h.rect.left, h.rect.bottom - h.rect.top));
        if (d.chain.size() > d.best.size())
            d.best = d.chain;
        dive(d, h.element.Get(), depth + 1);
        d.chain.pop_back();
        if (d.budget <= 0 || std::chrono::steady_clock::now() >= d.deadline)
            return;
    }
}

bool walk(IUIAutomation* automation, IUIAutomationCacheRequest* cache, IUIAutomationCondition* all, const Request& r,
    std::chrono::steady_clock::time_point deadline, std::vector<QRect>* chain)
{
    ComPtr<IUIAutomationElement> element;
    if (FAILED(automation->ElementFromHandleBuildCache(r.window, cache, &element)) || !element)
        return false;
    Dive d;
    d.cache = cache;
    d.all = all;
    d.point = r.point;
    d.deadline = deadline;
    dive(d, element.Get(), 0);
    *chain = std::move(d.best);
    return true;
}

void run(std::shared_ptr<UiaProbe::Shared> shared)
{
    const bool com = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationCacheRequest> cache;
    ComPtr<IUIAutomationCondition> all;
    if (SUCCEEDED(::CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))
        || SUCCEEDED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
        ComPtr<IUIAutomation2> timed;
        if (SUCCEEDED(automation.As(&timed))) {
            timed->put_ConnectionTimeout(UiaGate::kBudgetMs);
            timed->put_TransactionTimeout(UiaGate::kBudgetMs);
        }
        if (SUCCEEDED(automation->CreateCacheRequest(&cache))) {
            cache->AddProperty(UIA_BoundingRectanglePropertyId);
            cache->AddProperty(UIA_IsOffscreenPropertyId);
            cache->put_TreeScope(TreeScope_Element);
        }
        automation->CreateTrueCondition(&all);
    }
    ComPtr<IUIAutomationFocusChangedEventHandler> sink;
    if (automation) {
        sink = Make<FocusSink>();
        if (sink)
            automation->AddFocusChangedEventHandler(nullptr, sink.Get());
    }
    const bool ready = automation && cache && all;
    for (;;) {
        Request request;
        {
            std::unique_lock lock(shared->mutex);
            shared->wake.wait(lock, [&] { return shared->stop || shared->request.has_value(); });
            if (shared->stop)
                break;
            request = *shared->request;
            shared->request.reset();
        }
        UiaProbe::Answer answer;
        answer.window = reinterpret_cast<quintptr>(request.window);
        answer.point = request.point;
        const auto began = std::chrono::steady_clock::now();
        if (ready)
            answer.ok = walk(automation.Get(), cache.Get(), all.Get(), request, began + std::chrono::milliseconds(UiaGate::kBudgetMs),
                &answer.chain);
        answer.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        std::lock_guard lock(shared->mutex);
        if (shared->stop)
            break;
        if (QObject* context = shared->context.data()) {
            QMetaObject::invokeMethod(context, [shared, answer] {
                if (shared->done)
                    shared->done(answer);
            }, Qt::QueuedConnection);
        }
    }
    if (automation && sink)
        automation->RemoveAllEventHandlers();
    sink.Reset();
    automation.Reset();
    cache.Reset();
    all.Reset();
    if (com)
        ::CoUninitialize();
}

} // namespace

UiaProbe::UiaProbe(QObject* context, Done done)
    : m_shared(std::make_shared<Shared>())
{
    m_shared->context = context;
    m_shared->done = std::move(done);
    std::thread(run, m_shared).detach();
}

UiaProbe::~UiaProbe()
{
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->stop = true;
        m_shared->done = nullptr; // an answer already posted finds nothing to call
    }
    m_shared->wake.notify_all();
}

void UiaProbe::ask(HWND window, QPoint point)
{
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->request = Request {window, point};
    }
    m_shared->wake.notify_one();
}

} // namespace ws::capture
