#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

namespace ws::taskbar {

// Room right of the Start button, made by WinShunTap.dll inside Explorer
// (src/tap): the DLL puts a right margin on the taskbar's Start button and
// the app icons move right, as Windows' own search box has it. This is the
// DLL's client. It stages the DLL where Explorer may load it from, finds a
// copy already resident in Explorer or attaches one (Windows' XAML
// diagnostics, InitializeXamlDiagnosticsEx, called from here with Explorer's
// pid), asks it for a margin in DIPs, and says how that went. The TAP clears
// the margin by itself when this process ends, however it ends.
//
// Its own thread: nothing here waits on the box's or the GUI's. The box
// (SearchBox) decides the DIPs from the form it wants and verifies the gap by
// measuring; the status is for the settings page.
class StartGap {
public:
    enum class State {
        Off, // nothing asked for, or the default asked for and back
        Attaching, // staging the DLL, looking for conflicts, attaching
        Ready, // a TAP on the primary taskbar, no Start button seen by it yet
        Applied, // the margin asked for is on the Start button
        NoGapSeen, // ... but the box measured no such gap (noGapSeen): it fell back
        Conflict, // another program has Explorer's XAML diagnostics: `reason` names it
        Incompatible, // a TAP of another protocol version is resident: an Explorer restart replaces it
        Failed, // `reason` has what failed
    };
    struct Status {
        State state = State::Off;
        std::wstring reason; // a program's name, an HRESULT, a step: for the settings page, English
        double dips = 0; // what the TAP has on the Start button now
    };
    using Changed = std::function<void(const Status&)>; // called on StartGap's thread

    explicit StartGap(Changed changed);
    ~StartGap(); // asks for the default back if it can (the TAP does that anyway once we are gone)

    StartGap(const StartGap&) = delete;
    StartGap& operator=(const StartGap&) = delete;

    // Any thread.
    void set(double dips); // 0: the default back; idempotent; kept and sent again after reattach()
    void reattach(); // Explorer started again (TaskbarCreated): a new pid, attached again
    void noGapSeen(); // the box saw no gap after an answer: NoGapSeen until the next set()
    Status status() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl; // shared with its thread, which may outlive us while stuck in a call
};

} // namespace ws::taskbar
