#include "KeyListener.h"
#include "KeyListenerProtocol.h"
#include "OwnInput.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <format>
#include <latch>
#include <optional>
#include <string_view>
#include <utility>
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
// The key under Esc, left of 1, by where it is: ` and ~ on US and Chinese
// layouts (· in a Chinese input method), ^ on German ones, ² on French ones.
constexpr USHORT kDragKeyScan = 0x29;
// Its hotkeys while a drag is under way: alone, with Shift, Ctrl, both.
constexpr int kDragKeyHotkey = 0x5750;
constexpr std::array<UINT, 4> kDragKeyMods {0, MOD_SHIFT, MOD_CONTROL, MOD_CONTROL | MOD_SHIFT};
constexpr std::uint32_t kDragLookMs = 30;
// The key pressed on a file before it was dragged: the drag started for it,
// looked for every 20 ms, for up to 300.
constexpr UINT_PTR kStartDragTimer = 2;
constexpr UINT kStartDragTickMs = 20;
constexpr int kStartDragTicks = 15;

// The windows that list files, where a press may become a drag of them:
// File Explorer's view and the file dialogs (DirectUIHWND), the desktop and
// older lists (SysListView32), folder trees, Total Commander's panels (64
// and 32 bit). Pressed elsewhere, the key is left alone until a drag begins.
bool listsFiles(HWND window)
{
    wchar_t name[32] {};
    if (!window || !::GetClassNameW(window, name, 32))
        return false;
    const std::wstring_view cls(name);
    return cls == L"DirectUIHWND" || cls == L"SysListView32" || cls == L"SysTreeView32" || cls == L"LCLListBox"
        || cls == L"TMyListBox";
}

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
        if (hwnd && keyboardWanted()) {
            const RAWINPUTDEVICE keyboard {kGenericDesktop, kKeyboard, RIDEV_INPUTSINK, hwnd};
            active = ::RegisterRawInputDevices(&keyboard, 1, sizeof keyboard);
        }
        if (hwnd && mouseWanted()) {
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
        if (wParam == kStartDragTimer && self)
            self->onStartDragTimer();
        if (wParam == kMouseOffTimer && self) {
            ::KillTimer(hwnd, kMouseOffTimer);
            if (self->keyHeld()) // still down: wait for its release
                ::SetTimer(hwnd, kMouseOffTimer, maxGapMs(), nullptr);
            else
                self->listenToMouse(false);
        }
        return 0;
    case WM_HOTKEY: // the drop pie's key during a drag; once a press (MOD_NOREPEAT)
        if (self && wParam >= WPARAM {kDragKeyHotkey} && wParam < kDragKeyHotkey + kDragKeyMods.size())
            self->onDragKeyPressed();
        return 0;
    case WM_CLOSE: {
        if (self)
            self->guardDragKey(false);
        if (self && self->keyboardWanted()) {
            const RAWINPUTDEVICE keyboard {kGenericDesktop, kKeyboard, RIDEV_REMOVE, nullptr};
            ::RegisterRawInputDevices(&keyboard, 1, sizeof keyboard);
        }
        if (self) {
            self->m_callbacks.doubleClick = nullptr; // so the mouse goes too
            self->m_callbacks.dragMenu = nullptr;
            self->m_callbacks.leftButtonUp = nullptr;
            self->m_callbacks.mousePress = nullptr;
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
    if (raw.header.dwType == RIM_TYPEMOUSE) { // mostly moves, which tell only whether a drag began
        if (m_callbacks.dragMenu)
            onDrag(raw.data.mouse);
        if (raw.data.mouse.usButtonFlags & (kAnyButtonDown | kWheel))
            onMouse(raw.data.mouse);
        return;
    }
    if (raw.header.dwType != RIM_TYPEKEYBOARD)
        return;
    const RAWKEYBOARD& key = raw.data.keyboard;
    if (m_callbacks.dragMenu)
        onDragKey(key);
    if (!m_callbacks.doubleCtrl)
        return;
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
    if (m_callbacks.mousePress && (mouse.usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN))) {
        POINT point {};
        if (::GetPhysicalCursorPos(&point)) {
            const bool swapped = ::GetSystemMetrics(SM_SWAPBUTTON) != 0;
            const auto inputTime = static_cast<std::uint32_t>(::GetMessageTime());
            if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) m_callbacks.mousePress(point, swapped, inputTime);
            if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) m_callbacks.mousePress(point, !swapped, inputTime);
        }
    }
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

// The primary button and the pointer while it is held: whether a drag has
// begun, for the drop pie's key. Kept apart from onMouse: a move or a release there
// would spoil a double tap (mouseUsed) or the double click's pair.
void KeyListener::onDrag(const RAWMOUSE& mouse)
{
    const USHORT flags = mouse.usButtonFlags;
    const auto time = static_cast<std::uint32_t>(::GetMessageTime());
    if (flags != 0) {
        // The device's buttons: swapped (left-handed), its right one drags.
        const bool swapped = ::GetSystemMetrics(SM_SWAPBUTTON) != 0;
        const USHORT down = swapped ? RI_MOUSE_RIGHT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_DOWN;
        const USHORT up = swapped ? RI_MOUSE_RIGHT_BUTTON_UP : RI_MOUSE_LEFT_BUTTON_UP;
        if (flags & down) {
            POINT pt {};
            ::GetPhysicalCursorPos(&pt);
            // The drag rectangle and the scale of the monitor pressed on.
            UINT dpiX = 96;
            UINT dpiY = 96;
            if (FAILED(::GetDpiForMonitor(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
                dpiX = dpiY = 96;
            m_dragRect = {::GetSystemMetricsForDpi(SM_CXDRAG, dpiX), ::GetSystemMetricsForDpi(SM_CYDRAG, dpiY)};
            m_pressed = true;
            m_dragging = false;
            m_pressedAt = pt;
            m_dragFound = false;
            m_dragLooked = time;
            // Held on a list of files, the key is the pie's from now on: it
            // may come before the hand moves (startDrag).
            if (listsFiles(::WindowFromPoint(pt)))
                guardDragKey(true);
        } else if (flags & (kAnyButtonDown & ~down)) {
            m_pressed = false; // another button too: not a plain drag
            guardDragKey(false);
            m_starting = false;
        }
        if (flags & up) {
            const bool dragged = m_pressed && (m_dragging || m_dragFound);
            m_pressed = false;
            guardDragKey(false);
            m_starting = false;
            if (dragged && m_callbacks.leftButtonUp)
                m_callbacks.leftButtonUp();
        }
        return;
    }
    if (!m_pressed)
        return;
    POINT pt {};
    ::GetPhysicalCursorPos(&pt);
    // Windows starts a drag once the pointer leaves the rectangle centred on
    // the press (DragDetect): before that it is a click.
    if (!m_dragging)
        m_dragging = std::abs(pt.x - m_pressedAt.x) * 2 > m_dragRect.cx || std::abs(pt.y - m_pressedAt.y) * 2 > m_dragRect.cy;
    // Once a drag has begun, the pie's key is held back from it.
    if (!m_dragFound && m_dragging && time - m_dragLooked >= kDragLookMs) {
        m_dragLooked = time;
        if (oleDrag().capture) {
            m_dragFound = true;
            guardDragKey(true);
        }
    }
}

// The drop pie's key pressed: during an OLE drag it brings up the pie (or
// takes it away again); typed at any other time it is left alone. Once a
// drag is under way it is a hotkey (guardDragKey), which raw input does not
// see: WM_HOTKEY tells of it then.
void KeyListener::onDragKey(const RAWKEYBOARD& key)
{
    if (key.MakeCode != kDragKeyScan || (key.Flags & (RI_KEY_E0 | RI_KEY_E1)) || isOwnInput(key.ExtraInformation))
        return; // replayDragKey's own too
    if (key.Flags & RI_KEY_BREAK) {
        m_dragKeyDown = false;
        return;
    }
    if (!m_dragKeyGuarded && !std::exchange(m_dragKeyDown, true)) // held down: its repeats
        onDragKeyPressed();
}

void KeyListener::onDragKeyPressed()
{
    if (m_starting)
        return;
    // Held on a file, not moved yet: no drag to bring the pie into.
    if (m_dragKeyGuarded && m_pressed && !m_dragging && !m_dragFound) {
        startDrag();
        return;
    }
    const DragSource source = oleDrag();
    if (!source.capture) {
        if (m_dragKeyGuarded)
            replayDragKey(); // not a drag after all: typed, as it would have been
        if (m_keyLog)
            log("Key log: the drop pie's key, but no OLE drag is under way");
        return;
    }
    POINT pt {};
    ::GetPhysicalCursorPos(&pt);
    m_callbacks.dragMenu(pt, source.capture, source.pid);
}

// A drag of files is an OLE drag: while DoDragDrop runs, the source's thread
// has captured the mouse with OLE's own window. Its thread is the one of the
// window pressed on; else the one in front. A window dragged by its title
// bar (Aero Shake), a selection rectangle, a slider: no. (Safe here: this
// thread has no window that WindowFromPoint would ask.) Total Commander
// drags files inside its own window by itself, its main window holding the
// capture, and starts an OLE drag only once the pointer is over another
// program's window: such as the pie, which is shown (unseen until
// DragEnter) all the same. Its title bar dragged (Windows' own move, Aero
// Shake) or a menu open is not that.
KeyListener::DragSource KeyListener::oleDrag() const
{
    const auto captureOf = [](DWORD thread, bool totalCommander) -> HWND {
        GUITHREADINFO info {sizeof info};
        if (!::GetGUIThreadInfo(thread, &info) || !info.hwndCapture
            || (info.flags & (GUI_INMOVESIZE | GUI_INMENUMODE | GUI_POPUPMENUMODE)))
            return nullptr;
        wchar_t name[32] {};
        ::GetClassNameW(info.hwndCapture, name, 32);
        return std::wstring_view(name) == (totalCommander ? L"TTOTAL_CMD" : L"CLIPBRDWNDCLASS") ? info.hwndCapture : nullptr;
    };
    const HWND pressed = m_pressed ? ::WindowFromPoint(m_pressedAt) : nullptr;
    const DWORD pressedThread = pressed ? ::GetWindowThreadProcessId(pressed, nullptr) : 0;
    HWND capture = pressed ? captureOf(pressedThread, false) : nullptr;
    if (!capture)
        capture = captureOf(0, false); // the foreground thread
    if (!capture && pressed)
        capture = captureOf(pressedThread, true);
    DWORD pid = 0;
    if (capture)
        ::GetWindowThreadProcessId(capture, &pid);
    return {capture, pid};
}

// The drop pie's key as a hotkey of this window while a file list is pressed
// on or a drag is under way:
// Windows takes a hotkey before the input method and the program in front,
// so neither types it (OLE's drag loop leaves keys alone, Total Commander's
// own drag does not: "·" went into its command line). Raw input does not see
// it then either. By the layout of the program in front, where the key is
// under Esc. One another program holds already stays its own.
void KeyListener::guardDragKey(bool on)
{
    const HWND window = m_window.load();
    if (on == m_dragKeyGuarded || !window)
        return;
    if (!on) {
        for (int i = 0; i < static_cast<int>(kDragKeyMods.size()); ++i)
            ::UnregisterHotKey(window, kDragKeyHotkey + i);
        m_dragKeyGuarded = false;
        return;
    }
    const HKL layout = ::GetKeyboardLayout(::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr));
    const UINT vk = ::MapVirtualKeyExW(kDragKeyScan, MAPVK_VSC_TO_VK, layout);
    std::string held;
    for (int i = 0; vk && i < static_cast<int>(kDragKeyMods.size()); ++i) {
        const UINT mods = kDragKeyMods[static_cast<std::size_t>(i)];
        if (!::RegisterHotKey(window, kDragKeyHotkey + i, mods | MOD_NOREPEAT, vk))
            continue;
        m_dragKeyGuarded = true;
        held += std::format(" {}{}key", (mods & MOD_CONTROL) ? "Ctrl+" : "", (mods & MOD_SHIFT) ? "Shift+" : "");
    }
    if (m_keyLog)
        log(std::format("Key log: a file list pressed or a drag begun: the drop pie's key (vk 0x{:02X}) held back as:{}", vk,
            held.empty() ? " nothing" : held));
}

// The key pressed while a file is held but not moved: the program has not
// begun a drag (it waits for the pointer to leave the drag rectangle), so the
// pointer is moved just past it, and put back (SetCursorPos, which OLE
// follows) once the drag has begun.
void KeyListener::startDrag()
{
    ::GetPhysicalCursorPos(&m_startFrom);
    // Six drag rectangles: Total Commander begins its own past about four.
    const LONG slop = std::max(m_dragRect.cx, m_dragRect.cy);
    POINT to {m_startFrom.x + 6 * slop, m_startFrom.y};
    if (!::MonitorFromPoint(to, MONITOR_DEFAULTTONULL))
        to.x = m_startFrom.x - 6 * slop; // by the screen's right edge
    m_starting = true;
    m_startTicks = 0;
    // As input, in steps: Total Commander takes no drag from SetCursorPos.
    const int left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = std::max(2, ::GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int height = std::max(2, ::GetSystemMetrics(SM_CYVIRTUALSCREEN));
    std::array<INPUT, 4> moves {};
    for (std::size_t i = 0; i < moves.size(); ++i) {
        const LONG x = m_startFrom.x + (to.x - m_startFrom.x) * static_cast<LONG>(i + 1) / static_cast<LONG>(moves.size());
        moves[i].type = INPUT_MOUSE;
        moves[i].mi.dx = MulDiv(x - left, 65535, width - 1);
        moves[i].mi.dy = MulDiv(to.y - top, 65535, height - 1);
        moves[i].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        moves[i].mi.dwExtraInfo = kOwnInput;
    }
    ::SendInput(static_cast<UINT>(moves.size()), moves.data(), sizeof(INPUT));
    ::SetTimer(m_window.load(), kStartDragTimer, kStartDragTickMs, nullptr);
}

void KeyListener::onStartDragTimer()
{
    const HWND window = m_window.load();
    if (!m_starting || !m_pressed) {
        ::KillTimer(window, kStartDragTimer);
        m_starting = false;
        return;
    }
    ++m_startTicks;
    // Total Commander holds its capture from the press on: its own drag
    // begins at the move, given a moment.
    const DragSource source = m_startTicks >= 2 ? oleDrag() : DragSource {};
    if (!source.capture && m_startTicks < kStartDragTicks)
        return;
    ::KillTimer(window, kStartDragTimer);
    m_starting = false;
    ::SetCursorPos(m_startFrom.x, m_startFrom.y);
    if (!source.capture) {
        if (m_keyLog)
            log("Key log: the drop pie's key on a file held, but no drag began");
        replayDragKey();
        return;
    }
    m_dragFound = true;
    if (m_keyLog)
        log(std::format("Key log: the drop pie's key on a file held: a drag begun for it in {} ms",
            m_startTicks * kStartDragTickMs));
    m_callbacks.dragMenu(m_startFrom, source.capture, source.pid);
}

// The key was held back, but no drag came of it: typed after all, where it
// would have gone (marked as Win顺's, so not taken for the pie's again).
void KeyListener::replayDragKey()
{
    guardDragKey(false);
    std::array<INPUT, 2> keys {};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        keys[i].type = INPUT_KEYBOARD;
        keys[i].ki.wScan = kDragKeyScan;
        keys[i].ki.dwFlags = KEYEVENTF_SCANCODE | (i == 1 ? KEYEVENTF_KEYUP : 0);
        keys[i].ki.dwExtraInfo = kOwnInput;
    }
    ::SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
}

// The mouse only while it can spoil a tap: from a press of the key until a
// second tap can no longer follow. Listened to all the time, every move wakes this
// thread, up to thousands of times a second with a gaming mouse: only when
// double clicks are asked for (cheap still: nothing waits on it, and a move
// is one look at the button flags).
void KeyListener::listenToMouse(bool on)
{
    if (!on && mouseWanted())
        return;
    if (on == m_mouseOn)
        return;
    const RAWINPUTDEVICE mouse {kGenericDesktop, kMouse, static_cast<DWORD>(on ? RIDEV_INPUTSINK : RIDEV_REMOVE),
        on ? m_window.load() : nullptr};
    if (::RegisterRawInputDevices(&mouse, 1, sizeof mouse) || !on)
        m_mouseOn = on;
}

} // namespace ws
