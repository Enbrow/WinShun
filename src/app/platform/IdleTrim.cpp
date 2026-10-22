#include "IdleTrim.h"

#include <QDebug>
#include <QGuiApplication>

#include <algorithm>
#include <thread>

#include <windows.h>
#include <psapi.h>

namespace ws {

namespace {

// Since the last window lost the focus: one searches, opens what was found,
// and often searches again within seconds; emptied in between, every open
// would take the pages back.
constexpr int kAfterUseMs = 20'000;
constexpr int kSettleMs = 5'000; // after busy work ended (poke)
constexpr int kBusyRecheckMs = 15'000; // while the index loads, catches up or reads content
// While unused: background work brings pages back. The content indexer's
// passes (30 s after start, then every 15 minutes and after changes) go
// through the whole file index, ~90 MB; copies, journal changes and saves.
// A check reads one number; only a working set above kTrimAbove is emptied.
constexpr int kRecheckMs = 30'000;
constexpr SIZE_T kTrimAbove = 32 * 1024 * 1024; // private working set; ~5 MB is what idling itself touches

SIZE_T privateWorkingSet()
{
    PROCESS_MEMORY_COUNTERS_EX2 counters {};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)))
        return SIZE_MAX; // unknown: trim
    return counters.PrivateWorkingSetSize;
}

} // namespace

IdleTrim::IdleTrim(std::function<const char*()> busy, QObject* parent)
    : QObject(parent)
    , m_busy(std::move(busy))
{
    m_timer.setSingleShot(true);
    m_inactive.start();
    connect(&m_timer, &QTimer::timeout, this, &IdleTrim::check);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) {
            m_inactive.restart();
            m_timer.start(kAfterUseMs);
        }
    });
    m_timer.start(kAfterUseMs);
}

bool IdleTrim::inUse()
{
    // Qt's application state follows the focus events only; closing the
    // focused window sets the focus window to null without ever going
    // inactive, so the state alone stays Active for good. A window of ours has
    // the focus, or the foreground window (a native dialog too) is ours.
    if (QGuiApplication::applicationState() != Qt::ApplicationActive)
        return false;
    if (QGuiApplication::focusWindow())
        return true;
    DWORD owner = 0;
    if (const HWND foreground = ::GetForegroundWindow())
        ::GetWindowThreadProcessId(foreground, &owner);
    return owner == ::GetCurrentProcessId();
}

void IdleTrim::poke()
{
    // Not asking inUse() here: right after a window closes the foreground may
    // not have moved yet; check() asks again when the time is up.
    if (m_busy())
        return;
    startAfter(static_cast<int>(std::max<qint64>(kSettleMs, kAfterUseMs - m_inactive.elapsed())));
}

void IdleTrim::startAfter(int ms)
{
    if (!m_timer.isActive() || m_timer.remainingTime() > ms)
        m_timer.start(ms);
}

void IdleTrim::check()
{
    if (inUse()) {
        m_timer.start(kAfterUseMs);
        return;
    }
    if (const char* busy = m_busy()) {
        if (busy != m_waitingFor)
            qWarning() << "Idle trim: waiting," << busy;
        m_waitingFor = busy;
        m_timer.start(kBusyRecheckMs);
        return;
    }
    m_waitingFor = nullptr;
    if (const SIZE_T bytes = privateWorkingSet(); bytes > kTrimAbove) {
        qWarning() << "Idle trim:" << bytes / (1024 * 1024) << "MB";
        // ~10 ms per 100 MB (wsbench --trim): not on the GUI thread, so a
        // hotkey pressed meanwhile is not held up.
        m_trimmed = true;
        std::thread([] {
            ::SetProcessWorkingSetSizeEx(::GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
        }).detach();
    }
    m_timer.start(kRecheckMs);
}

} // namespace ws
