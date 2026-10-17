#include "KeyListener.h"
#include "KeyListenerProtocol.h"

#include <algorithm>
#include <format>
#include <latch>
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

// Ctrl pressed while dragging switches between moving and copying.
bool mouseButtonHeld()
{
    for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) {
        if (::GetAsyncKeyState(button) < 0)
            return true;
    }
    return false;
}

// As long as a double click may take (Control Panel → Mouse), so a slow
// setting there is honoured; within limits, so two separate Ctrl presses do
// not make one.
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

} // namespace

KeyListener::KeyListener(Callbacks callbacks)
    : m_callbacks(std::move(callbacks))
    , m_detector(DoubleTapDetector::Config {.isDown = &isDown})
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
        // only around a Ctrl press (listenToMouse), or all the time for
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
            self->m_readyPid = static_cast<DWORD>(wParam);
            self->m_heartbeat = ::GetTickCount64();
            if (!self->m_usingService) {
                self->m_detector.reset();
                self->m_usingService = true;
            }
        }
        return 0;
    case keyproto::kTap:
        if (self && self->serviceActive() && static_cast<DWORD>(wParam >> 32) == self->m_servicePid.load()
            && keyboardRegistered(hwnd)) {
            const auto key = static_cast<std::uint32_t>(wParam & 0xFF);
            const auto time = static_cast<std::uint32_t>(lParam);
            const auto side = self->m_side.load();
            if ((key == VK_LCONTROL || key == VK_RCONTROL)
                && (side == DoubleTapDetector::Side::Both
                    || (side == DoubleTapDetector::Side::Left && key == VK_LCONTROL)
                    || (side == DoubleTapDetector::Side::Right && key == VK_RCONTROL))
                && static_cast<DWORD>(::GetTickCount()) - time < 1000)
                self->deliver({(wParam & 0x100) ? "UIAccess listener: simulated Ctrl" : "", key, time});
        }
        return 0;
    case WM_INPUT:
        if (self)
            self->onInput(reinterpret_cast<HRAWINPUT>(lParam));
        return ::DefWindowProcW(hwnd, msg, wParam, lParam); // lets Windows free the input
    case WM_TIMER:
        if (wParam == kMouseOffTimer && self) {
            ::KillTimer(hwnd, kMouseOffTimer);
            if (::GetAsyncKeyState(VK_CONTROL) < 0) // still down: wait for its release
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
    if (key.VKey == 0xFF)
        return; // the extra part of an escaped key sequence, not a key
    const bool up = key.Flags & RI_KEY_BREAK;
    const bool external = serviceActive();
    if (external != m_usingService) {
        // Never join half a local gesture to one from before a lost service
        // connection (or from before an elevated window stopped delivering).
        m_detector = DoubleTapDetector(DoubleTapDetector::Config {.isDown = &isDown});
        m_usingService = external;
    }
    const std::uint32_t vk = sidedKey(key);
    const bool ctrl = DoubleTapDetector::isCtrl(vk);
    const auto time = static_cast<std::uint32_t>(::GetMessageTime()); // when it was pressed, not when we got to it
    m_recent[m_recentNext] = {recentKey(vk), up, raw.header.hDevice == nullptr, time};
    m_recentNext = (m_recentNext + 1) % m_recent.size();
    POINT pt {};
    if (ctrl) {
        ::GetCursorPos(&pt);
        if (!up) {
            m_detector.setMaxGap(maxGapMs());
            m_detector.setSide(m_side.load());
            listenToMouse(true);
        }
        // Until a second tap can no longer follow. Armed on the press too: a
        // release can be lost (Ctrl+Alt+Del to lock the screen), and the timer
        // then finds Ctrl up.
        ::SetTimer(m_window.load(), kMouseOffTimer, maxGapMs() + 100, nullptr);
    }
    const bool fire = up ? m_detector.keyUp(vk, time, pt.x, pt.y) : m_detector.keyDown(vk, time, pt.x, pt.y);
    if (ctrl && !up && mouseButtonHeld())
        m_detector.mouseUsed();
    if (fire && !external)
        deliver({sentTrace(), vk, time});
}

bool KeyListener::serviceActive() const noexcept
{
    const DWORD pid = m_servicePid.load();
    return pid && pid == m_readyPid.load() && ::GetTickCount64() - m_heartbeat.load() < keyproto::kLeaseMs;
}

void KeyListener::deliver(Tap tap)
{
    // Both sources can finish the same gesture during a handover. WM_INPUT
    // uses the same event timestamp in both processes; late duplicates lose.
    if (m_delivered && static_cast<DWORD>(::GetTickCount()) - m_lastDelivered < 1000
        && static_cast<std::int32_t>(tap.time - m_lastDelivered) <= 0)
        return;
    m_delivered = true;
    m_lastDelivered = tap.time;
    m_callbacks.doubleCtrl(tap);
}

// The last keys, oldest first, when a program sent any of the last four Ctrl
// events: "LAlt↓ +0, LCtrl↓ sent +812, …". Empty when the taps were all
// typed.
std::string KeyListener::sentTrace() const
{
    bool sent = false;
    int ctrlEvents = 0;
    for (std::size_t i = 0; i < m_recent.size() && ctrlEvents < 4; ++i) {
        const Recent& r = m_recent[(m_recentNext + m_recent.size() - 1 - i) % m_recent.size()];
        if (r.key == 1 || r.key == 2) {
            ++ctrlEvents;
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
    m_detector.mouseUsed();
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

// The mouse only while it can spoil a tap: from a Ctrl press until a second
// tap can no longer follow. Listened to all the time, every move wakes this
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
