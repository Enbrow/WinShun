#include "IdleTrim.h"

#include <QGuiApplication>

#include <windows.h>
#include <psapi.h>

namespace ws {

namespace {

constexpr int kAfterUseMs = 60'000; // since the last window lost the focus
constexpr int kRecheckMs = 120'000; // while unused: copies, journal changes and saves bring pages back
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

IdleTrim::IdleTrim(std::function<bool()> busy, QObject* parent)
    : QObject(parent)
    , m_busy(std::move(busy))
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &IdleTrim::check);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive)
            m_timer.stop();
        else
            m_timer.start(kAfterUseMs);
    });
    if (QGuiApplication::applicationState() != Qt::ApplicationActive)
        m_timer.start(kAfterUseMs);
}

void IdleTrim::check()
{
    if (QGuiApplication::applicationState() == Qt::ApplicationActive)
        return; // the state change starts the timer again
    if (!m_busy() && privateWorkingSet() > kTrimAbove)
        ::SetProcessWorkingSetSizeEx(::GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
    m_timer.start(kRecheckMs);
}

} // namespace ws
