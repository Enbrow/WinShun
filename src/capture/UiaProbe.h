#pragma once

#include <QObject>
#include <QPoint>
#include <QRect>

#include <windows.h>

#include <functional>
#include <memory>
#include <vector>

namespace ws::capture {

// The controls under a point that are not child windows (WinUI, XAML,
// Firefox; not Chromium's, whose windows are never asked), asked of UI Automation on a thread of its own so the hover never
// waits for it: the question for the latest point is answered when it is, the
// answer comes back on the thread that made the probe. From the window's
// element the tree is walked down one level at a time, keeping the child
// that holds the point (the overlay is over the point, so the element
// "from point" would be ours); every call is limited to kBudgetMs, and the
// whole walk is timed so the owner can drop a slow window's answers
// (UiaGate). Made when the first question is asked, let go of with the
// capture; a walk still under way at that point finishes by itself.
class UiaProbe {
public:
    struct Answer {
        quintptr window = 0;
        QPoint point; // physical, virtual desktop
        std::vector<QRect> chain; // the elements holding the point, from the outermost in
        qint64 elapsedMs = 0;
        bool ok = false; // the walk ran (false: no UI Automation, or the window gave nothing)
    };
    using Done = std::function<void(const Answer&)>;

    // `done` is called on `context`'s thread.
    UiaProbe(QObject* context, Done done);
    ~UiaProbe();
    UiaProbe(const UiaProbe&) = delete;
    UiaProbe& operator=(const UiaProbe&) = delete;

    // Asks for the controls holding `point` in `window`; a question not yet
    // started is replaced.
    void ask(HWND window, QPoint point);

    struct Shared; // the thread's and this object's, shared

private:
    std::shared_ptr<Shared> m_shared;
};

} // namespace ws::capture
