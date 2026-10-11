#include "ShortcutCapture.h"

#include <QtLogging>

#include <iterator>
#include <latch>
#include <optional>

namespace ws {

namespace {

thread_local ShortcutCapture* t_capture = nullptr; // a low-level hook is called on the thread that set it

constexpr USHORT kGenericDesktop = 0x01;
constexpr USHORT kKeyboard = 0x06;

// The process's Raw Input registration for keyboards (KeyListener's), taken
// away: returned to put it back.
std::optional<RAWINPUTDEVICE> setKeyboardRawInputAside()
{
    RAWINPUTDEVICE devices[8];
    UINT count = std::size(devices);
    const UINT got = ::GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
    if (got == static_cast<UINT>(-1))
        return std::nullopt;
    for (UINT i = 0; i < got; ++i) {
        if (devices[i].usUsagePage != kGenericDesktop || devices[i].usUsage != kKeyboard)
            continue;
        const RAWINPUTDEVICE remove {kGenericDesktop, kKeyboard, RIDEV_REMOVE, nullptr};
        if (!::RegisterRawInputDevices(&remove, 1, sizeof remove))
            return std::nullopt;
        return devices[i];
    }
    return std::nullopt;
}

// The key as the settings window's recorder knows it (see HotkeyRecorder.qml).
int qtKey(UINT vk, Qt::KeyboardModifiers modifiers)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return static_cast<int>(vk); // Qt::Key_A… and Qt::Key_0… have the same codes
    if (vk >= VK_F1 && vk <= VK_F24)
        return Qt::Key_F1 + static_cast<int>(vk - VK_F1);
    switch (vk) {
    case VK_CONTROL: // sent by programs; the keyboard says which one
    case VK_LCONTROL:
    case VK_RCONTROL:
        return Qt::Key_Control;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return Qt::Key_Alt;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return Qt::Key_Shift;
    case VK_LWIN:
    case VK_RWIN:
        return Qt::Key_Meta;
    case VK_SPACE:
        return Qt::Key_Space;
    case VK_OEM_3:
        return Qt::Key_QuoteLeft;
    case VK_TAB:
        return modifiers.testFlag(Qt::ShiftModifier) ? Qt::Key_Backtab : Qt::Key_Tab; // as Qt spells Shift+Tab
    case VK_ESCAPE:
        return Qt::Key_Escape;
    case VK_BACK:
        return Qt::Key_Backspace;
    case VK_DELETE:
        return Qt::Key_Delete;
    case VK_RETURN:
        return Qt::Key_Return;
    default:
        return Qt::Key_unknown; // the recorder says it cannot be used
    }
}

} // namespace

ShortcutCapture::ShortcutCapture(HWND window, std::function<void(const Key&)> onKey)
    : m_window(window)
    , m_onKey(std::move(onKey))
{
    std::latch ready(1);
    m_thread = std::jthread([this, &ready] {
        t_capture = this;
        MSG msg;
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's message queue, for WM_QUIT
        m_threadId = ::GetCurrentThreadId();
        // Every key press in the system waits for the hook's answer.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const std::optional<RAWINPUTDEVICE> rawInput = setKeyboardRawInputAside(); // see the header
        const HHOOK hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &ShortcutCapture::hookProc, ::GetModuleHandleW(nullptr), 0);
        m_active = hook != nullptr;
        if (!hook)
            qWarning("ShortcutCapture: no keyboard hook (error %lu)", ::GetLastError());
        ready.count_down();
        if (hook) {
            // The hook is called from inside GetMessage; there is nothing to dispatch.
            while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            }
            ::UnhookWindowsHookEx(hook);
        }
        if (rawInput && !::RegisterRawInputDevices(&*rawInput, 1, sizeof(RAWINPUTDEVICE)))
            qWarning("ShortcutCapture: keyboard Raw Input not restored (error %lu)", ::GetLastError());
    });
    ready.wait();
}

ShortcutCapture::~ShortcutCapture()
{
    ::PostThreadMessageW(m_threadId.load(), WM_QUIT, 0, 0); // ends the message loop
    if (m_thread.joinable())
        m_thread.join();
}

LRESULT CALLBACK ShortcutCapture::hookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && t_capture
        && t_capture->handle(wParam, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)))
        return 1; // seen by nobody else: not Windows, not the window, not other programs' hotkeys
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

bool ShortcutCapture::handle(WPARAM message, const KBDLLHOOKSTRUCT& event)
{
    const UINT vk = event.vkCode;
    if (vk >= m_taken.size())
        return false;
    const bool press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    bool autoRepeat = false;
    if (press) {
        autoRepeat = m_taken.test(vk); // a taken key stays taken, wherever the focus went
        if (!autoRepeat) {
            if (::GetForegroundWindow() != m_window)
                return false;
            if (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2)
                return false; // volume, media and browser keys keep working
            // Windows saw this key go down before the capture began (this is
            // its auto-repeat): it must see all of it, up to the release.
            if (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000)
                return false;
        }
    } else if (!m_taken.test(vk)) {
        return false; // likewise
    }
    m_taken.set(vk, !press); // a press is not counted yet, a release still is (see Key::modifiers)
    const Qt::KeyboardModifiers held = modifiers();
    m_taken.set(vk, press);
    m_onKey({qtKey(vk, held), held, press, autoRepeat});
    return true;
}

bool ShortcutCapture::isDown(UINT vk) const
{
    return m_taken.test(vk) || (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000); // or held from before
}

Qt::KeyboardModifiers ShortcutCapture::modifiers() const
{
    Qt::KeyboardModifiers held;
    if (isDown(VK_CONTROL) || isDown(VK_LCONTROL) || isDown(VK_RCONTROL))
        held |= Qt::ControlModifier;
    if (isDown(VK_MENU) || isDown(VK_LMENU) || isDown(VK_RMENU))
        held |= Qt::AltModifier;
    if (isDown(VK_SHIFT) || isDown(VK_LSHIFT) || isDown(VK_RSHIFT))
        held |= Qt::ShiftModifier;
    if (isDown(VK_LWIN) || isDown(VK_RWIN))
        held |= Qt::MetaModifier;
    return held;
}

} // namespace ws
