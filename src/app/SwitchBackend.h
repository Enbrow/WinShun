#pragma once

#include "SwitchIntent.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace ws {

// One system switch as its row in the launcher shows it
// (docs/design-system-switches.md 4.4). A backend's read() fills all but
// `busy` and `error`, which are Switches' own.
struct SwitchState {
    enum class Availability : quint8 {
        Unknown, // not read yet (Win顺 has just started)
        Ready, // read and set from here: the row has its switch
        NoDevice, // nothing to switch on this computer ("这台电脑没有蓝牙"): `note` says so
        Denied, // not allowed ("让应用控制设备无线电" is off): `note` says so
        Blocked, // held elsewhere (high contrast, a policy): `note` says so
        // No backend for it in this build (a switch of places.txt whose
        // phase has not come): an ordinary place, Enter opens its page as
        // before. Never changes while Win顺 runs.
        Unsupported,
    };
    // The switch's position; the numbers are the doc's (ResultModel's switchOn).
    enum class Value : qint8 {
        Unknown = -1, // not read yet, or the system could not say
        Off = 0,
        On = 1, // on, dark, shown, audible (the volume's switch is "sound on")
        Mixed = 2, // part on, part off: dark mode with the apps and the system unlike ("自定义")
    };

    Availability availability = Availability::Unknown;
    // While `busy`, where it is going (the knob is there already); otherwise
    // as last read back. Before the first read, Unknown.
    Value on = Value::Unknown;
    qint8 level = -1; // the volume, 0–100; -1 for the other switches
    // Being written: shown at the target until the write is confirmed (the
    // row fades it a little if that takes over 150 ms).
    bool busy = false;
    // The second line: a device's name, "自定义：应用浅色，系统深色", "到 16:42",
    // or why the switch is not ready. Empty: the row's own source there.
    QString note;
    // The second line in red: why the last write failed, in the user's
    // language (codes go to the log). Cleared by the next write, a change
    // made elsewhere, or Switches::refresh once it is 30 s old.
    QString error;
    QDateTime until; // keep awake: when it ends (wall clock); invalid otherwise
    // Not ready: the page Enter opens instead of the row's own
    // (ms-settings:easeofaccess-highcontrast for dark mode under high
    // contrast); empty: the row's own command.
    QString openInstead;
    // Mixed: where a toggle goes (true: On), as the backend decides. Dark
    // mode: the opposite of the apps' setting, both written (design 5.1).
    bool mixedToggleOn = false;
    // WLAN: the internet goes through it now, so turning it off leaves the
    // computer offline (the footer says "会断网").
    bool carriesInternet = false;

    [[nodiscard]] bool ready() const noexcept { return availability == Availability::Ready; }
    friend bool operator==(const SwitchState&, const SwitchState&) = default;
};

// What a write did (SwitchBackend::write).
struct SwitchWrite {
    bool ok = false; // the switch is as asked now (whether or not it was written)
    bool wrote = false; // the system was changed: false when it already was so, or nothing could be done
    QString error; // !ok and ready: why, for the row (the user's language; codes go to the log)
    SwitchState after; // read back afterwards; not ready: the reason the write was not tried
};

// The system's side of one or more switches (platform/SwitchTheme,
// SwitchExplorer, SwitchRadios, SwitchAudio), as Switches drives it; tests put in a fake. Made and
// destroyed on the GUI thread. read() and write() run on the switch thread
// only (ComWorker, multithreaded apartment), one call at a time; close()
// runs there last. watch() and written() run on the GUI thread.
class SwitchBackend {
public:
    using Notify = std::function<void(const QString& id)>;

    virtual ~SwitchBackend() = default;

    // The switch ids it answers for (places.txt "switch =", PlaceInfo::switchId).
    [[nodiscard]] virtual QStringList ids() const = 0;

    // GUI thread, once, before watch(): requested as the switch thread ends
    // (Win顺 exits). A read or write that waits on the system (a radio being
    // switched) looks at it now and then and gives up, so that the exit does
    // not wait for it.
    virtual void setStopToken(std::stop_token stop) { (void)stop; }

    // GUI thread, once, before the first read: from now on `changed` is told
    // when a switch may have been changed elsewhere (another program, the
    // Settings app, Win顺's own write), and Switches reads it again. It may
    // be called on any thread, until close() (sources on other threads stop
    // there) or the backend is destroyed (those on the GUI thread).
    virtual void watch(Notify changed) { (void)changed; }

    // Switch thread: the state now (all but busy and error). Quick: a few
    // registry values, an object asked; never waits on another program.
    [[nodiscard]] virtual SwitchState read(const QString& id) = 0;

    // Switch thread: sets the switch as `intent` says, always explicit (On,
    // Off; SetLevel, LevelOnly and the keep-awake kinds for their backends),
    // never Toggle or OpenSettings: Switches turns a toggle into On or Off on
    // the GUI thread, from the state shown, and a step into the level it
    // reaches (a StepLevel comes only while the level is not known yet).
    // Reads first and writes nothing when it already is so (ok, not wrote);
    // reads back afterwards.
    [[nodiscard]] virtual SwitchWrite write(const QString& id, const SwitchIntent& intent) = 0;

    // GUI thread, after a write that changed the system (SwitchWrite::wrote):
    // what has to happen there. Dark mode: Win顺's own windows told at once,
    // then every other program from a thread of its own (SettingBroadcast).
    virtual void written(const QString& id) { (void)id; }

    // Switch thread, as it ends: let go of what was made there (COM and WinRT
    // objects, their callbacks). No read or write comes after it.
    virtual void close() { }
};

using SwitchBackends = std::vector<std::unique_ptr<SwitchBackend>>;

// For a backend whose sources call on threads of their own, at any time (a
// radio's StateChanged, Core Audio's callbacks): passes their calls on to
// SwitchBackend::watch's `changed` until close(), and drops those that come
// later. The callbacks share it, as they may outlive the backend. Without
// watch() (wsbench), nobody is told.
class SwitchRelay {
public:
    void watch(SwitchBackend::Notify changed)
    {
        std::lock_guard lock(m_mutex);
        m_changed = std::move(changed);
    }
    // Before the sources are let go: a call under way has told Switches, or
    // tells nobody. Never from within one of the calls.
    void close()
    {
        std::lock_guard lock(m_mutex);
        m_changed = nullptr;
    }
    void operator()(const QString& id)
    {
        std::lock_guard lock(m_mutex); // a moment: `changed` only posts to the GUI thread
        if (m_changed)
            m_changed(id);
    }

private:
    std::mutex m_mutex;
    SwitchBackend::Notify m_changed;
};

} // namespace ws
