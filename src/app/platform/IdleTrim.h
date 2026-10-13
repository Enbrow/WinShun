#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <functional>

namespace ws {

// Hands Win顺's memory back to Windows while nobody uses it. Unused, the app
// touches ~5 MB, yet everything it ever touched (the file index, the windows'
// graphics) stays in its working set: Windows trims a process only when memory
// runs short, so Task Manager shows ~230 MB. A while after the last window
// lost the focus, the working set is emptied: the pages go to the standby
// list, counted as available memory, and come back by soft faults when used
// (measured 2026-10-12: a first name search ~10 ms instead of ~2.3 ms; the
// launcher warms the index when shown, see FileIndex::warm).
// Nothing is freed or rebuilt, so nothing has to load again.
class IdleTrim : public QObject {
public:
    // `busy` names work under way that would touch the pages again at once
    // (the index loading or being built, files being read for content), or
    // returns null.
    explicit IdleTrim(std::function<const char*()> busy, QObject* parent = nullptr);

    // Work `busy` reports may have ended (the index is loaded, a catch-up is
    // done): emptied a few seconds later, not at the next recheck.
    void poke();
    // Whether the working set was emptied since last asked.
    bool takeTrimmed() noexcept { return m_trimmed.exchange(false); }

private:
    void check();
    static bool inUse(); // a window of Win顺's has the focus
    void startAfter(int ms);

    std::function<const char*()> m_busy;
    const char* m_waitingFor = nullptr; // logged once per reason
    QTimer m_timer;
    QElapsedTimer m_inactive; // since the last window lost the focus
    std::atomic<bool> m_trimmed {false};
};

} // namespace ws
