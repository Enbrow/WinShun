#include "TypeToSearch.h"

#include "FolderViewHit.h"
#include "Integrity.h"
#include "KeyRouter.h" // kOwnInput
#include "SearchTyping.h"

#include <cwchar>
#include <latch>
#include <utility>

namespace ws {

namespace {

// Low-level hooks and out-of-context events are called on the thread that set them.
thread_local TypeToSearch* t_typing = nullptr;

constexpr UINT kDeliverMessage = WM_APP + 1; // to the hook's thread (deliver)
constexpr UINT kGiveBackMessage = WM_APP + 2; // to the hook's thread (giveBack)
constexpr UINT kPassedMessage = WM_APP + 3; // to the hook's thread: the keys typed again have gone by
constexpr UINT kHoldTimeoutMs = 3000; // the launcher comes in well under a second; keys held longer are dropped
// The keys typed again go by within milliseconds; should some never (a hook
// before this one took them), the hold ends after this all the same.
constexpr UINT kPassTimeoutMs = 1000;

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

// The Start menu, or Windows' search, which takes it over when typed in.
// Windows 11 has them in SearchHost.exe (the Start menu in front, on 25H2)
// and StartMenuExperienceHost.exe, Windows 10 in SearchApp.exe or SearchUI.exe.
bool isStartMenu(HWND window)
{
    DWORD pid = 0;
    if (!window || !::GetWindowThreadProcessId(window, &pid) || pid == 0)
        return false;
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    wchar_t path[MAX_PATH] {};
    DWORD size = MAX_PATH;
    const bool named = ::QueryFullProcessImageNameW(process, 0, path, &size);
    ::CloseHandle(process);
    if (!named)
        return false;
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    const wchar_t* file = slash ? slash + 1 : path;
    for (const wchar_t* host : {L"SearchHost.exe", L"StartMenuExperienceHost.exe", L"SearchApp.exe", L"SearchUI.exe"}) {
        if (_wcsicmp(file, host) == 0)
            return true;
    }
    return false;
}

INPUT keyInput(WORD vk, WORD scanCode, DWORD flags)
{
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = scanCode;
    input.ki.dwFlags = flags;
    input.ki.dwExtraInfo = kOwnInput;
    return input;
}

} // namespace

TypeToSearch::TypeToSearch(Surfaces surfaces, Callbacks callbacks)
    : m_surfaces(surfaces)
    , m_callbacks(std::move(callbacks))
{
    std::latch ready(1);
    m_thread = std::jthread([this, &ready] {
        t_typing = this;
        MSG msg;
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's message queue, for WM_QUIT
        m_threadId = ::GetCurrentThreadId();
        // While the hook is there, every key press in the system waits for
        // it: at once, and not slowed down when the system saves power
        // (Windows drops a hook that keeps it waiting too long).
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        THREAD_POWER_THROTTLING_STATE throttling {THREAD_POWER_THROTTLING_CURRENT_VERSION,
            THREAD_POWER_THROTTLING_EXECUTION_SPEED, 0};
        ::SetThreadInformation(::GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof throttling);
        const HWINEVENTHOOK foreground = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
            &TypeToSearch::foregroundProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        ready.count_down();
        foregroundChanged(::GetForegroundWindow(), nullptr); // open already
        // The hook and the events are called from inside GetMessage.
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.hwnd == nullptr && msg.message == kDeliverMessage)
                typeHeld();
            else if (msg.hwnd == nullptr && msg.message == kGiveBackMessage)
                returnHeld();
            else if (msg.hwnd == nullptr && msg.message == kPassedMessage)
                heldPassed();
            else if (msg.hwnd == nullptr && msg.message == WM_TIMER && msg.wParam == m_timer)
                dropHeld();
            else
                ::DispatchMessageW(&msg);
        }
        setHooked(false);
        if (foreground)
            ::UnhookWinEvent(foreground);
    });
    ready.wait();
}

TypeToSearch::~TypeToSearch()
{
    ::PostThreadMessageW(m_threadId.load(), WM_QUIT, 0, 0); // ends the message loop
    if (m_thread.joinable())
        m_thread.join();
}

void TypeToSearch::deliver()
{
    ::PostThreadMessageW(m_threadId.load(), kDeliverMessage, 0, 0);
}

void TypeToSearch::giveBack()
{
    ::PostThreadMessageW(m_threadId.load(), kGiveBackMessage, 0, 0);
}

LRESULT CALLBACK TypeToSearch::keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code != HC_ACTION || !t_typing)
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (t_typing->handleKey(wParam, event))
        return 1; // seen by nobody else
    const LRESULT result = ::CallNextHookEx(nullptr, code, wParam, lParam);
    // Through the rest of the chain: on its way to the window, ahead of any
    // key that comes here after it.
    if (isOwnInput(event.dwExtraInfo))
        t_typing->ownKeyPassed(wParam, event);
    return result;
}

void CALLBACK TypeToSearch::foregroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD)
{
    // The window really in front now, not the one the event names: that
    // can be a part of it, or already gone. That one counts too when it is
    // an Explorer window left behind (foregroundChanged).
    if (t_typing)
        t_typing->foregroundChanged(::GetForegroundWindow(), hwnd);
}

// An Explorer folder window typing can count in: told by its classes alone;
// whether it is elevated is asked here, once, not at every key.
bool TypeToSearch::typingExplorer(HWND window) const
{
    return m_surfaces.explorer && window && folderview::classifyWindow(window).surface == folderview::Surface::Explorer
        && !integrity::higherThanOurs(window);
}

// Still the active window of its thread while not in front: brought forward
// now, it comes with no foreground event. No message sent.
bool TypeToSearch::activeBehind(HWND window)
{
    GUITHREADINFO info {sizeof info};
    const DWORD thread = ::GetWindowThreadProcessId(window, nullptr);
    return thread && ::GetGUIThreadInfo(thread, &info) && info.hwndActive == window;
}

void TypeToSearch::foregroundChanged(HWND window, HWND named)
{
    m_startInFront = m_surfaces.startMenu && isStartMenu(window);
    if (!m_startInFront)
        m_windowsSearch = false; // Left Alt counts for one opening
    // Explorer raises this event when it goes to another folder too
    // (docs/pitfalls.md), so a window that was something else (Home) and now
    // shows a folder is seen as often as needed; which folder is asked only
    // when typed in. A window that raises it while another stays in front
    // (opened by a program that may not take the front) is kept apart: it
    // comes to the front later, when clicked, with no event (measured), so
    // the hook stays and the key looks at what is in front then
    // (searchtyping::typingWindow). It stays kept through later events
    // (the launcher up and away, another program) while it is still the
    // active window of its own thread: that is what makes its coming to
    // the front silent, and it ends when it really comes forward or closes.
    const HWND behind = m_explorerBehind;
    m_explorer = nullptr;
    m_explorerBehind = nullptr;
    if (!m_startInFront) {
        if (typingExplorer(window))
            m_explorer = window;
        if (named != window && typingExplorer(named))
            m_explorerBehind = named; // behind another window, Explorer's own too
        else if (behind && behind != window && activeBehind(behind))
            m_explorerBehind = behind;
    }
    setHooked(m_startInFront || m_explorer || m_explorerBehind || m_holding);
}

void TypeToSearch::setHooked(bool hooked)
{
    if (hooked == (m_hook != nullptr))
        return;
    if (hooked) {
        m_hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &TypeToSearch::keyboardProc, ::GetModuleHandleW(nullptr), 0);
    } else {
        ::UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
}

// Whether the files and folders shown in m_explorer have the keyboard focus
// (not a name being renamed, the address bar, the search box, the navigation
// pane), with no menu of the window's open over them (GitHub #37), and which
// tab they are. GetGUIThreadInfo reads the other thread's state without
// sending it anything: safe inside the hook, microseconds.
bool TypeToSearch::explorerFocused(HWND& tab) const
{
    GUITHREADINFO info {sizeof info};
    if (!::GetGUIThreadInfo(::GetWindowThreadProcessId(m_explorer, nullptr), &info) || !info.hwndFocus)
        return false;
    const folderview::Hit hit = folderview::classify(info.hwndFocus, {});
    const bool itemsView = hit.surface == folderview::Surface::Explorer && hit.root == m_explorer;
    if (searchtyping::focusOf(static_cast<unsigned>(info.flags), itemsView) != searchtyping::Focus::ItemsView)
        return false;
    tab = hit.tab;
    return true;
}

bool TypeToSearch::handleKey(WPARAM message, const KBDLLHOOKSTRUCT& event)
{
    // Ours: being typed again, given back, the Esc that closes the Start
    // menu for the launcher (win::bringToFront). Other programs' are typing
    // all the same (keys remapped by AutoHotkey or PowerToys, the on-screen
    // keyboard).
    if (isOwnInput(event.dwExtraInfo))
        return false;
    const UINT vk = event.vkCode;
    const bool press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (searchtyping::isModifier(vk)) {
        if (press && vk == VK_LMENU && !m_holding && m_startInFront)
            m_windowsSearch = true;
        return false; // seen as they are; the keys held back carry Shift with them
    }
    const bool combination = held(VK_CONTROL) || held(VK_MENU) || held(VK_LWIN) || held(VK_RWIN);
    if (!m_holding) {
        if (!press || combination)
            return false;
        HWND tab = nullptr;
        if (m_startInFront) {
            if (m_windowsSearch || !searchtyping::startsTyping(vk))
                return false;
        } else if (m_explorer || m_explorerBehind) {
            // The key alone first; what is in front and what has the focus
            // are asked only for one that would start a search.
            if (!searchtyping::explorerStarts(vk, press, combination, searchtyping::Focus::ItemsView, false))
                return false;
            const HWND front = searchtyping::typingWindow(::GetForegroundWindow(), m_explorer, m_explorerBehind);
            if (!front)
                return false;
            if (front != m_explorer)
                std::swap(m_explorer, m_explorerBehind); // the one left behind is in front now, the other may come back
            if (!explorerFocused(tab))
                return false;
        } else {
            return false;
        }
        m_holding = true;
        m_holdingForExplorer = m_explorer != nullptr;
        m_held.clear();
        m_timer = ::SetTimer(nullptr, 0, kHoldTimeoutMs, nullptr);
        if (m_holdingForExplorer) {
            if (m_callbacks.typedInExplorer)
                m_callbacks.typedInExplorer(m_explorer, tab, vk);
        } else if (m_callbacks.typed) {
            m_callbacks.typed();
        }
    } else if (combination) {
        return false;
    }
    m_held.push_back({static_cast<WORD>(vk), static_cast<WORD>(event.scanCode), (event.flags & LLKHF_EXTENDED) != 0,
        !press, press && held(VK_SHIFT)});
    return true;
}

void TypeToSearch::typeHeld()
{
    if (!m_holding || m_retyped.pending())
        return; // nothing held, or on its way already (what is typed meanwhile follows it)
    sendHeld();
}

// The keys held, as input to send: in order, with Shift around the keys
// pressed with it (unless it is held now).
std::vector<INPUT> TypeToSearch::takeHeld()
{
    const bool shiftNow = held(VK_SHIFT);
    std::vector<INPUT> inputs;
    for (const Held& key : m_held) {
        const DWORD flags = (key.extended ? KEYEVENTF_EXTENDEDKEY : 0) | (key.release ? KEYEVENTF_KEYUP : 0);
        const bool shifted = key.shift && !key.release && !shiftNow;
        if (shifted)
            inputs.push_back(keyInput(VK_LSHIFT, static_cast<WORD>(::MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC)), 0));
        inputs.push_back(keyInput(key.vk, key.scanCode, flags));
        if (shifted)
            inputs.push_back(keyInput(VK_LSHIFT, static_cast<WORD>(::MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC)),
                KEYEVENTF_KEYUP));
    }
    m_held.clear();
    return inputs;
}

// The keys held, typed again. The hold goes on until the last of them has
// gone down the hook chain (ownKeyPassed): a key typed or let go meanwhile
// is held too and follows them. Let through at once, it went in among them
// (q down, z down, z up, q up) and "qz" came out "zq" (GitHub #39).
void TypeToSearch::sendHeld()
{
    std::vector<INPUT> inputs = takeHeld();
    if (inputs.empty()) {
        endHolding();
        return;
    }
    std::vector<searchtyping::Retyped::Key> keys;
    keys.reserve(inputs.size());
    for (const INPUT& input : inputs)
        keys.push_back({input.ki.wVk, (input.ki.dwFlags & KEYEVENTF_KEYUP) != 0});
    m_retyped.sent(std::move(keys));
    restartTimer(kPassTimeoutMs);
    if (::SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) != inputs.size()) {
        m_retyped.clear(); // not all went in (blocked): none to wait for
        heldPassed();
    }
}

// One of Win顺's own keys went down the chain (keyboardProc): the last of
// those typed again lets the keys typed meanwhile follow, once out of the
// hook (a posted message comes after the hooks' calls already waiting).
void TypeToSearch::ownKeyPassed(WPARAM message, const KBDLLHOOKSTRUCT& event)
{
    if (m_retyped.passed(event.vkCode, message == WM_KEYUP || message == WM_SYSKEYUP))
        ::PostThreadMessageW(m_threadId.load(), kPassedMessage, 0, 0);
}

void TypeToSearch::heldPassed()
{
    if (!m_holding || m_retyped.pending())
        return; // over already (it waited too long), or more on their way
    if (m_held.empty())
        endHolding();
    else
        sendHeld(); // typed while those were on their way: after them
}

// The same keys, to the window they were typed in: Explorer is still in
// front (the launcher never showed), and jumps by them as it would have.
void TypeToSearch::returnHeld()
{
    if (!m_holding)
        return;
    typeHeld();
}

void TypeToSearch::dropHeld()
{
    if (!m_holding)
        return;
    // Typed again, and some never went by (a hook before this one took
    // them): the keys typed since go on after them, and the hold ends.
    if (m_retyped.pending()) {
        m_retyped.clear();
        std::vector<INPUT> inputs = takeHeld();
        endHolding();
        if (!inputs.empty())
            ::SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
        return;
    }
    // Typed in Explorer, the keys are the user's: back to it rather than
    // lost. In the Start menu they would go into Windows' search, which was
    // not wanted: dropped.
    if (m_holdingForExplorer) {
        returnHeld();
        return;
    }
    endHolding();
    m_held.clear();
}

void TypeToSearch::restartTimer(UINT ms)
{
    ::KillTimer(nullptr, m_timer);
    m_timer = ::SetTimer(nullptr, 0, ms, nullptr);
}

void TypeToSearch::endHolding()
{
    m_holding = false;
    m_holdingForExplorer = false;
    m_retyped.clear();
    ::KillTimer(nullptr, m_timer);
    m_timer = 0;
    setHooked(m_startInFront || m_explorer || m_explorerBehind); // the launcher is in front: gone
}

} // namespace ws
