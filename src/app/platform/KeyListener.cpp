#include "KeyListener.h"
#include "KeyListenerProtocol.h"
#include "OwnInput.h"

#include <algorithm>
#include <format>
#include <latch>
#include <optional>
#include <vector>

namespace ws {

namespace {

constexpr const wchar_t* kClassName = keyproto::kWindowClass;
constexpr USHORT kGenericDesktop = 0x01; // HID usage page
constexpr USHORT kMouse = 0x02; // HID usage
constexpr USHORT kKeyboard = 0x06;
constexpr USHORT kAnyButtonDown = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN
    | RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN;
// Ctrl+wheel zooms. A touchpad pinch is sent as Ctrl+wheel too, by Windows.
constexpr USHORT kWheel = RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL;
constexpr UINT_PTR kMouseOffTimer = 1;

using Key = DoubleTapDetector::Key;

// The key pressed while dragging: Ctrl switches between moving and copying,
// Alt drags a column selection or (AltSnap) the window.
bool mouseButtonHeld()
{
    for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) {
        if (::GetAsyncKeyState(button) < 0)
            return true;
    }
    return false;
}

// As long as a double click may take (Control Panel → Mouse), so a slow
// setting there is honoured; within limits, so two separate presses do not
// make one.
std::uint32_t maxGapMs()
{
    return std::clamp<std::uint32_t>(::GetDoubleClickTime(), 400, 900);
}

bool held(int vk)
{
    return ::GetAsyncKeyState(vk) < 0;
}

// Alt+double click is File Explorer's Properties, Ctrl and Shift select.
bool modifierHeld()
{
    return held(VK_SHIFT) || held(VK_CONTROL) || held(VK_MENU) || held(VK_LWIN) || held(VK_RWIN);
}

bool isDown(std::uint32_t vk)
{
    return held(static_cast<int>(vk));
}

DoubleTapDetector detectorFor(Key key)
{
    return DoubleTapDetector({.key = key, .isDown = &isDown});
}

bool takes(KeyListener::Keys keys, Key key)
{
    switch (keys) {
    case KeyListener::Keys::All:
        return true;
    case KeyListener::Keys::Ctrl:
        return key == Key::Ctrl;
    case KeyListener::Keys::Alt:
        return key == Key::Alt;
    case KeyListener::Keys::Shift:
        return key == Key::Shift;
    }
    return false;
}

bool keyboardRegistered(HWND window)
{
    // ShortcutCapture temporarily removes this registration while recording
    // a hotkey. Its Ctrl presses must not wake the launcher via the helper.
    UINT count = 0;
    if (::GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1))
        return false;
    std::vector<RAWINPUTDEVICE> devices(count);
    const UINT got = ::GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE));
    if (got == static_cast<UINT>(-1))
        return false;
    return std::any_of(devices.begin(), devices.begin() + got, [window](const RAWINPUTDEVICE& d) {
        return d.usUsagePage == kGenericDesktop && d.usUsage == kKeyboard && d.hwndTarget == window;
    });
}

// Raw Input names Ctrl, Shift and Alt without their side: the right Ctrl and
// Alt come with the E0 prefix, the right Shift with its own scan code.
std::uint32_t sidedKey(const RAWKEYBOARD& key)
{
    const bool e0 = key.Flags & RI_KEY_E0;
    switch (key.VKey) {
    case VK_CONTROL:
        return e0 ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return e0 ? VK_RMENU : VK_LMENU;
    case VK_SHIFT:
        return key.MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    default:
        return key.VKey;
    }
}

constexpr const char* kRecentNames[] {"", "LCtrl", "RCtrl", "LShift", "RShift", "LAlt", "RAlt", "LWin", "RWin", "key"};

std::uint8_t recentKey(std::uint32_t vk)
{
    switch (vk) {
    case VK_LCONTROL:
        return 1;
    case VK_RCONTROL:
        return 2;
    case VK_LSHIFT:
        return 3;
    case VK_RSHIFT:
        return 4;
    case VK_LMENU:
        return 5;
    case VK_RMENU:
        return 6;
    case VK_LWIN:
        return 7;
    case VK_RWIN:
        return 8;
    default:
        return 9;
    }
}

// WINSHUN_KEYLOG=1: every key event in the log (Callbacks::log).
bool keyLogWanted()
{
    wchar_t value[4] {};
    return ::GetEnvironmentVariableW(L"WINSHUN_KEYLOG", value, 4) == 1 && value[0] == L'1';
}

const char* keysName(KeyListener::Keys keys)
{
    switch (keys) {
    case KeyListener::Keys::Ctrl:
        return "Ctrl";
    case KeyListener::Keys::Alt:
        return "Alt";
    case KeyListener::Keys::Shift:
        return "Shift";
    case KeyListener::Keys::All:
        break;
    }
    return "Ctrl, Alt and Shift";
}

const char* sideName(DoubleTapDetector::Side side)
{
    return side == DoubleTapDetector::Side::Left ? "left" : side == DoubleTapDetector::Side::Right ? "right" : "both";
}

const char* yesNo(bool yes)
{
    return yes ? "yes" : "no";
}

// For the key log: a modifier (or KeyRouter's E8) with what Raw Input said
// of it, any other key only as "key" (nothing typed is kept); when, and from
// where.
std::string describeKey(const RAWKEYBOARD& key, std::uint32_t vk, bool up, bool device, bool sent, std::uint32_t time)
{
    const std::uint8_t name = recentKey(vk);
    std::string text = std::format("Key log: {}{}", kRecentNames[name], up ? "↑" : "↓");
    if (name != 9 || key.VKey == 0xE8)
        text += std::format(" (vk {:#x}, scan {:#x}, flags {:#x})", key.VKey, key.MakeCode, key.Flags);
    return text
        + std::format(" at {}, {}, extra {:#x}", time,
            device  ? "from a keyboard"
            : sent  ? "sent by a program"
                    : "sent by Win顺",
            key.ExtraInformation);
}

} // namespace

KeyListener::KeyListener(Callbacks callbacks)
    : m_callbacks(std::move(callbacks))
    , m_detectors {detectorFor(Key::Ctrl), detectorFor(Key::Alt), detectorFor(Key::Shift)}
    , m_keyLog(m_callbacks.log && keyLogWanted())
{
    std::latch ready(1);
    m_thread = std::jthread([this, &ready] {
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = &KeyListener::wndProc;
        wc.hInstance = instance;
        wc.lpszClassName = kClassName;
        ::RegisterClassExW(&wc);
        // Message-only: never shown, and still the target of raw input.
        const HWND hwnd = ::CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        m_window = hwnd;
        // Every keyboard, also while other programs have the focus; the mouse
        // only around a press of the key (listenToMouse), or all the time for
        // double clicks.
        bool active = hwnd != nullptr;
        if (hwnd && m_callbacks.doubleCtrl) {
            const RAWINPUTDEVICE keyboard {kGenericDesktop, kKeyboard, RIDEV_INPUTSINK, hwnd};
            active = ::RegisterRawInputDevices(&keyboard, 1, sizeof keyboard);
        }
        if (hwnd && m_callbacks.doubleClick) {
            listenToMouse(true);
            active = active && m_mouseOn;
        }
        m_active = active;
        ready.count_down();
        if (!hwnd)
            return;
        // Above normal: the second tap is timed from the message's own clock,
        // but it should not wait behind other work for long either.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
            ::DispatchMessageW(&msg);
    });
    ready.wait();
}

KeyListener::~KeyListener()
{
    if (const HWND hwnd = m_window.load())
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0); // the window ends the thread's message loop
    if (m_thread.joinable())
        m_thread.join();
}

LRESULT CALLBACK KeyListener::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<KeyListener*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case keyproto::kReady:
        if (self && wParam != 0 && wParam == self->m_servicePid.load()) {
            const LPARAM reports = lParam & (keyproto::kReportsAlt | keyproto::kReportsShift);
            if (self->m_keyLog && (self->m_readyPid.load() != wParam || self->m_serviceReports.load() != reports))
                self->log(std::format("Key log: UIAccess listener {} ready, it tells of Ctrl{}{}", wParam,
                    (reports & keyproto::kReportsAlt) ? ", Alt" : "", (reports & keyproto::kReportsShift) ? ", Shift" : ""));
            self->m_readyPid = static_cast<DWORD>(wParam);
            self->m_serviceReports = reports;
            self->m_heartbeat = ::GetTickCount64();
            self->followService();
        }
        return 0;
    case keyproto::kTap:
        if (self && static_cast<DWORD>(wParam >> 32) == self->m_servicePid.load()) {
            const auto key = static_cast<std::uint32_t>(wParam & 0xFF);
            const auto time = static_cast<std::uint32_t>(lParam);
            const bool taken = self->serviceTells() && keyboardRegistered(hwnd)
                && static_cast<DWORD>(::GetTickCount()) - time < 1000;
            if (self->m_keyLog)
                self->log(std::format("Key log: UIAccess listener: double {} at {}{}", kRecentNames[recentKey(key)],
                    time, taken ? "" : ", passed over (not its key now, stale, or a shortcut being recorded)"));
            if (taken)
                self->deliver({(wParam & 0x100) ? "UIAccess listener: partly simulated" : "", key, time});
        }
        return 0;
    case WM_INPUT:
        if (self)
            self->onInput(reinterpret_cast<HRAWINPUT>(lParam));
        return ::DefWindowProcW(hwnd, msg, wParam, lParam); // lets Windows free the input
    case WM_TIMER:
        if (wParam == kMouseOffTimer && self) {
            ::KillTimer(hwnd, kMouseOffTimer);
            if (self->keyHeld()) // still down: wait for its release
                ::SetTimer(hwnd, kMouseOffTimer, maxGapMs(), nullptr);
            else
                self->listenToMouse(false);
        }
        return 0;
    case WM_CLOSE: {
        if (self && self->m_callbacks.doubleCtrl) {
            const RAWINPUTDEVICE keyboard {kGenericDesktop, kKeyboard, RIDEV_REMOVE, nullptr};
            ::RegisterRawInputDevices(&keyboard, 1, sizeof keyboard);
        }
        if (self) {
            self->m_callbacks.doubleClick = nullptr; // so the mouse goes too
            self->listenToMouse(false);
            self->m_active = false;
        }
        ::DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void KeyListener::onInput(HRAWINPUT input)
{
    RAWINPUT raw {};
    UINT size = sizeof raw;
    if (::GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return;
    if (raw.header.dwType == RIM_TYPEMOUSE) { // mostly moves, which tell nothing
        if (raw.data.mouse.usButtonFlags & (kAnyButtonDown | kWheel))
            onMouse(raw.data.mouse);
        return;
    }
    if (raw.header.dwType != RIM_TYPEKEYBOARD || !m_callbacks.doubleCtrl)
        return;
    const RAWKEYBOARD& key = raw.data.keyboard;
    const bool up = key.Flags & RI_KEY_BREAK;
    const std::uint32_t vk = sidedKey(key);
    // A program's has no device. Not Win顺's own: the Alt KeyRouter lets go
    // after its mask was pressed by a finger.
    const bool device = raw.header.hDevice != nullptr;
    const bool sent = !device && !isOwnInput(key.ExtraInformation);
    const auto time = static_cast<std::uint32_t>(::GetMessageTime()); // when it was pressed, not when we got to it
    // 0xFF: the extra part of an escaped key sequence, not a key. kOwnMask:
    // KeyRouter's mask just before it lets a Shift or Alt pressed alone go
    // on to the program: no key in between, the Alt was alone.
    if (key.VKey == 0xFF || key.ExtraInformation == kOwnMask) {
        if (m_keyLog)
            log(describeKey(key, vk, up, device, sent, time) + ", passed over");
        return;
    }
    followService();
    m_recent[m_recentNext] = {recentKey(vk), up, sent, time};
    m_recentNext = (m_recentNext + 1) % m_recent.size();
    const Keys keys = m_keys.load();
    const bool tapped
        = std::ranges::any_of(m_detectors, [&](const auto& d) { return takes(keys, d.key()) && d.isKey(vk); });
    POINT pt {};
    if (tapped) {
        ::GetCursorPos(&pt);
        if (!up)
            listenToMouse(true);
        // Until a second tap can no longer follow. Armed on the press too: a
        // release can be lost (Ctrl+Alt+Del to lock the screen), and the timer
        // then finds the key up.
        ::SetTimer(m_window.load(), kMouseOffTimer, maxGapMs() + 100, nullptr);
    }
    std::optional<Tap> tap;
    std::string states; // for the key log
    for (DoubleTapDetector& d : m_detectors) {
        if (!takes(keys, d.key()))
            continue;
        const bool own = d.isKey(vk);
        if (own && !up) {
            d.setMaxGap(maxGapMs());
            d.setSide(m_side.load());
        }
        const bool fire = up ? d.keyUp(vk, time, pt.x, pt.y, sent) : d.keyDown(vk, time, pt.x, pt.y, sent);
        if (own && !up && mouseButtonHeld())
            d.mouseUsed();
        if (fire && !m_usingService)
            tap = Tap {sentTrace(d.key()), vk, time};
        if (m_keyLog)
            states += std::format("; {} {}{}{}", d.key() == Key::Alt ? "Alt" : d.key() == Key::Shift ? "Shift" : "Ctrl", d.clean() ? "clean" : "not clean",
                d.armed() ? ", armed" : "", fire ? ", DOUBLE TAP" : "");
    }
    if (m_keyLog)
        log(std::format("{}; keys {}, side {}; service active {}, tells {}, used {}{}",
            describeKey(key, vk, up, device, sent, time), keysName(keys), sideName(m_side.load()),
            yesNo(serviceActive()), yesNo(serviceTells()), yesNo(m_usingService), states));
    if (tap)
        deliver(std::move(*tap));
}

bool KeyListener::serviceActive() const noexcept
{
    const DWORD pid = m_servicePid.load();
    return pid && pid == m_readyPid.load() && ::GetTickCount64() - m_heartbeat.load() < keyproto::kLeaseMs;
}

// The UIAccess listener tells of the key's taps, and then only it, so that
// none comes twice. One from before double Alt (or Shift) never tells of
// that key's: they are left to this one.
bool KeyListener::serviceTells() const noexcept
{
    if (!serviceActive())
        return false;
    switch (m_keys.load()) {
    case Keys::Alt:
        return (m_serviceReports.load() & keyproto::kReportsAlt) != 0;
    case Keys::Shift:
        return (m_serviceReports.load() & keyproto::kReportsShift) != 0;
    case Keys::Ctrl:
    case Keys::All:
        break;
    }
    return true;
}

// Never joins half a gesture seen here to one from before the service came
// or went (a lost connection, an elevated window that stopped delivering).
void KeyListener::followService()
{
    // Said once: Alt is then this process's own, and over administrator
    // windows its Raw Input brings nothing (UIPI, docs/pitfalls.md).
    if (!m_oldServiceLogged && (m_keys.load() == Keys::Alt || m_keys.load() == Keys::Shift) && serviceActive()
        && !serviceTells()) {
        m_oldServiceLogged = true;
        log("Double Alt or Shift: the service's UIAccess listener is from before that key and does not tell of it. "
            "Over administrator windows it does nothing until the service is updated.");
    }
    const bool external = serviceTells();
    if (external == m_usingService)
        return;
    for (DoubleTapDetector& d : m_detectors)
        d.reset();
    m_usingService = external;
    if (m_keyLog)
        log(external ? "Key log: double taps now from the UIAccess listener"
                     : "Key log: double taps now from this process's Raw Input");
}

// A key listened for is down: its release is still to come, or was lost.
bool KeyListener::keyHeld() const
{
    const Keys keys = m_keys.load();
    return (takes(keys, Key::Ctrl) && held(VK_CONTROL)) || (takes(keys, Key::Alt) && held(VK_MENU))
        || (takes(keys, Key::Shift) && held(VK_SHIFT));
}

// A double tap of the key and side set. The UIAccess listener tells of all.
bool KeyListener::wanted(std::uint32_t key) const noexcept
{
    const bool alt = key == VK_LMENU || key == VK_RMENU;
    const bool shift = key == VK_LSHIFT || key == VK_RSHIFT;
    if ((!alt && !shift && key != VK_LCONTROL && key != VK_RCONTROL)
        || !takes(m_keys.load(), alt ? Key::Alt : shift ? Key::Shift : Key::Ctrl))
        return false;
    switch (m_side.load()) {
    case DoubleTapDetector::Side::Left:
        return key == VK_LCONTROL || key == VK_LMENU || key == VK_LSHIFT;
    case DoubleTapDetector::Side::Right:
        return key == VK_RCONTROL || key == VK_RMENU || key == VK_RSHIFT;
    case DoubleTapDetector::Side::Both:
        break;
    }
    return true;
}

void KeyListener::deliver(Tap tap)
{
    const char* const name = kRecentNames[recentKey(tap.key)];
    if (!wanted(tap.key)) {
        if (m_keyLog)
            log(std::format("Key log: double {} passed over: not the key or side set", name));
        return;
    }
    // Both sources can finish the same gesture during a handover. WM_INPUT
    // uses the same event timestamp in both processes; late duplicates lose.
    if (m_delivered && static_cast<DWORD>(::GetTickCount()) - m_lastDelivered < 1000
        && static_cast<std::int32_t>(tap.time - m_lastDelivered) <= 0) {
        if (m_keyLog)
            log(std::format("Key log: double {} passed over: already told", name));
        return;
    }
    m_delivered = true;
    m_lastDelivered = tap.time;
    if (m_keyLog)
        log(std::format("Key log: double {} at {} told", name, tap.time));
    m_callbacks.doubleCtrl(tap);
}

void KeyListener::log(const std::string& line) const
{
    if (m_callbacks.log)
        m_callbacks.log(line);
}

// The last keys, oldest first, when a program sent any of the last four
// events of the key tapped: "LAlt↓ +0, LCtrl↓ sent +812, …". Empty when the
// taps were all typed.
std::string KeyListener::sentTrace(Key key) const
{
    const int left = key == Key::Alt ? 5 : key == Key::Shift ? 3 : 1; // its left one among the names, the right one next
    bool sent = false;
    int events = 0;
    for (std::size_t i = 0; i < m_recent.size() && events < 4; ++i) {
        const Recent& r = m_recent[(m_recentNext + m_recent.size() - 1 - i) % m_recent.size()];
        if (r.key == left || r.key == left + 1) {
            ++events;
            sent = sent || r.sent;
        }
    }
    if (!sent)
        return {};
    std::string trace;
    std::uint32_t first = 0;
    for (std::size_t i = 0; i < m_recent.size(); ++i) {
        const Recent& r = m_recent[(m_recentNext + i) % m_recent.size()];
        if (r.key == 0)
            continue;
        if (trace.empty())
            first = r.time;
        else
            trace += ", ";
        trace
            += std::format("{}{}{} +{}", kRecentNames[r.key], r.up ? "↑" : "↓", r.sent ? " sent" : "", r.time - first);
    }
    return trace;
}

void KeyListener::onMouse(const RAWMOUSE& mouse)
{
    for (DoubleTapDetector& d : m_detectors)
        d.mouseUsed();
    if (!m_callbacks.doubleClick)
        return;
    // The device's buttons: with them swapped (left-handed), its right one
    // is the one that clicks.
    const USHORT primary = ::GetSystemMetrics(SM_SWAPBUTTON) ? RI_MOUSE_RIGHT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_DOWN;
    if ((mouse.usButtonFlags & kAnyButtonDown) != primary) {
        m_clicks.otherInput(); // another button, the wheel, or more than one at once
        return;
    }
    // Where the pointer was when the press was sent, by its own clock, as
    // Windows judges a double click.
    const DWORD at = ::GetMessagePos();
    const POINT pt {static_cast<short>(LOWORD(at)), static_cast<short>(HIWORD(at))};
    const auto time = static_cast<std::uint32_t>(::GetMessageTime());
    // Safe here: this thread has no window that WindowFromPoint would ask.
    const HWND window = ::WindowFromPoint(pt);
    m_clicks.setConfig(
        {::GetDoubleClickTime(), ::GetSystemMetrics(SM_CXDOUBLECLK) / 2, ::GetSystemMetrics(SM_CYDOUBLECLK) / 2});
    if (!m_clicks.leftDown(time, pt.x, pt.y, reinterpret_cast<std::uintptr_t>(window), m_menuOpen.load()))
        return;
    if (m_clicks.firstClosedMenu() || modifierHeld())
        return;
    const int other = ::GetSystemMetrics(SM_SWAPBUTTON) ? VK_LBUTTON : VK_RBUTTON;
    if (held(other) || held(VK_MBUTTON) || held(VK_XBUTTON1) || held(VK_XBUTTON2))
        return;
    m_callbacks.doubleClick(window, pt);
}

// The mouse only while it can spoil a tap: from a press of the key until a
// second tap can no longer follow. Listened to all the time, every move wakes this
// thread, up to thousands of times a second with a gaming mouse: only when
// double clicks are asked for (cheap still: nothing waits on it, and a move
// is one look at the button flags).
void KeyListener::listenToMouse(bool on)
{
    if (!on && m_callbacks.doubleClick)
        return;
    if (on == m_mouseOn)
        return;
    const RAWINPUTDEVICE mouse {kGenericDesktop, kMouse, static_cast<DWORD>(on ? RIDEV_INPUTSINK : RIDEV_REMOVE),
        on ? m_window.load() : nullptr};
    if (::RegisterRawInputDevices(&mouse, 1, sizeof mouse) || !on)
        m_mouseOn = on;
}

} // namespace ws
