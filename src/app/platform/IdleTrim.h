#pragma once

#include <QObject>
#include <QTimer>

#include <functional>

namespace ws {

// Hands Win顺's memory back to Windows while nobody uses it. Unused, the app
// touches ~5 MB, yet everything it ever touched (the file index, the windows'
// graphics) stays in its working set: Windows trims a process only when memory
// runs short, so Task Manager shows ~230 MB. A while after the last window
// lost the focus, the working set is emptied: the pages go to the standby
// list, counted as available memory, and come back by soft faults when used
// (measured 2026-10-12: a first name search ~10 ms instead of ~2.3 ms).
// Nothing is freed or rebuilt, so nothing has to load again.
class IdleTrim : public QObject {
public:
    // `busy` says work is under way that would touch the pages again at once
    // (the index loading or being built, files being read for content).
    explicit IdleTrim(std::function<bool()> busy, QObject* parent = nullptr);

private:
    void check();

    std::function<bool()> m_busy;
    QTimer m_timer;
};

} // namespace ws
