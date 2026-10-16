#include "TypeToSearch.h"

#include "FolderViewHit.h"
#include "Integrity.h"
#include "KeyRouter.h" // kOwnInput
#include "SearchTyping.h"

#include <cwchar>
#include <latch>

namespace ws {

namespace {

// Low-level hooks and out-of-context events are called on the thread that set them.
thread_local TypeToSearch* t_typing = nullptr;

constexpr UINT kDeliverMessage = WM_APP + 1; // to the hook's thread (deliver)
constexpr UINT kGiveBackMessage = WM_APP + 2; // to the hook's thread (giveBack)
constexpr UINT kHoldTimeoutMs = 3000; // the launcher comes in well under a second; keys held longer are dropped

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
        foregroundChanged(::GetForegroundWindow()); // open already
        // The hook and the events are called from inside GetMessage.
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.hwnd == nullptr && msg.message == kDeliverMessage)
                typeHeld();
            else if (msg.hwnd == nullptr && msg.message == kGiveBackMessage)
                returnHeld();
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
    if (code == HC_ACTION && t_typing && t_typing->handleKey(wParam, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)))
        return 1; // seen by nobody else
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

void CALLBACK TypeToSearch::foregroundProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
{
    // The window really in front now, not the one the event names: that
    // can be a part of it, or already gone.
    if (t_typing)
        t_typing->foregroundChanged(::GetForegroundWindow());
}

void TypeToSearch::foregroundChanged(HWND window)
{
    m_startInFront = m_surfaces.startMenu && isStartMenu(window);
    if (!m_startInFront)
        m_windowsSearch = false; // Left Alt counts for one opening
    // An Explorer folder window: told by its classes alone; whether it is
    // elevated is asked here, once, not at every key. Explorer raises this
    // event when it goes to another folder too (docs/pitfalls.md), so a
    // window that was something else (Home) and now shows a folder is seen
    // as often as needed; which folder is asked only when typed in.
    m_explorer = nullptr;
    if (m_surfaces.explorer && !m_startInFront
        && folderview::classifyWindow(window).surface == folderview::Surface::Explorer
        && !integrity::higherThanOurs(window))
        m_explorer = window;
    setHooked(m_startInFront || m_explorer || m_holding);
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
// pane), and which tab they are. GetGUIThreadInfo reads the other thread's
// state without sending it anything: safe inside the hook, microseconds.
bool TypeToSearch::explorerFocused(HWND& tab) const
{
    GUITHREADINFO info {sizeof info};
    if (!::GetGUIThreadInfo(::GetWindowThreadProcessId(m_explorer, nullptr), &info) || !info.hwndFocus)
        return false;
    const folderview::Hit hit = folderview::classify(info.hwndFocus, {});
    if (hit.surface != folderview::Surface::Explorer || hit.root != m_explorer)
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
    if (event.dwExtraInfo == kOwnInput)
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
        } else if (m_explorer) {
            // The key alone first; what has the focus is asked only for one
            // that would start a search.
            if (!searchtyping::explorerStarts(vk, press, combination, searchtyping::Focus::ItemsView, false)
                || !explorerFocused(tab))
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
    if (!m_holding)
        return;
    endHolding();
    // In order, with Shift around the keys pressed with it (unless it is
    // held now).
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
    if (!inputs.empty())
        ::SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
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

void TypeToSearch::endHolding()
{
    m_holding = false;
    m_holdingForExplorer = false;
    ::KillTimer(nullptr, m_timer);
    m_timer = 0;
    setHooked(m_startInFront || m_explorer); // the launcher is in front: gone
}

} // namespace ws
