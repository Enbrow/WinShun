#include "TypeToSearch.h"

#include "FolderViewHit.h"
#include "Integrity.h"
#include "KeyRouter.h" // kOwnInput
#include "PasteOffer.h"
#include "SearchTyping.h"

#include <cwchar>
#include <latch>
#include <utility>

namespace ws {

namespace {

// Low-level hooks and out-of-context events are called on the thread that set them.
thread_local TypeToSearch* t_typing = nullptr;
std::atomic<TypeToSearch::RequestId> g_nextFolderRequest = 0;

constexpr UINT kDeliverMessage = WM_APP + 1; // to the hook's thread (deliver)
constexpr UINT kGiveBackMessage = WM_APP + 2; // to the hook's thread (giveBack)
constexpr UINT kPassedMessage = WM_APP + 3; // to the hook's thread: the keys typed again have gone by
constexpr UINT kHoldTimeoutMs = 3000; // the launcher comes in well under a second; keys held longer are dropped
// The keys typed again go by within milliseconds; should some never (a hook
// before this one took them), the hold ends after this all the same.
constexpr UINT kPassTimeoutMs = 1000;
// Keys for the launcher wait while Win顺 is not in front (GitHub #66): how often
// to look again, besides at each foreground event.
constexpr UINT kFrontPollMs = 20;

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
            else if (msg.hwnd == nullptr && msg.message == kGiveBackMessage) {
                if (!msg.wParam || pending(static_cast<RequestId>(msg.wParam)))
                    returnHeld();
            } else if (msg.hwnd == nullptr && msg.message == kPassedMessage)
                heldPassed();
            else if (msg.hwnd == nullptr && msg.message == WM_TIMER && m_frontTimer && msg.wParam == m_frontTimer)
                heldPassed(); // looks again whether Win顺 is in front
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

void TypeToSearch::giveBack(RequestId request)
{
    ::PostThreadMessageW(m_threadId.load(), kGiveBackMessage, static_cast<WPARAM>(request), 0);
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

// An Explorer folder window typing or pasting can count in: told by its
// classes alone; whether it is elevated is asked here, once, not at every key.
bool TypeToSearch::watchedExplorer(HWND window) const
{
    return (m_surfaces.explorer || m_surfaces.pasteFile) && window
        && folderview::classifyWindow(window).surface == folderview::Surface::Explorer
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
    m_opus = nullptr;
    m_desktop = nullptr;
    m_pasteTaken = false; // V's release goes where the front went
    if (!m_startInFront) {
        if (m_surfaces.opus && folderview::isOpusLister(window) && !integrity::higherThanOurs(window))
            m_opus = window;
        if (watchedExplorer(window))
            m_explorer = window;
        if (named != window && watchedExplorer(named))
            m_explorerBehind = named; // behind another window, Explorer's own too
        else if (behind && behind != window && activeBehind(behind))
            m_explorerBehind = behind;
        if (m_surfaces.pasteFile && folderview::classifyWindow(window).surface == folderview::Surface::Desktop
            && !integrity::higherThanOurs(window))
            m_desktop = window;
    }
    setHooked(wantsHook());
    if (m_frontTimer)
        heldPassed(); // keys waiting for Win顺 to come to the front: maybe now
}

bool TypeToSearch::wantsHook() const
{
    return m_startInFront || m_explorer || m_explorerBehind || m_opus || m_desktop || m_holding;
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

// Only Opus's file display itself, not its child editors or an open menu.
bool TypeToSearch::opusFocused(HWND& tab) const
{
    GUITHREADINFO info {sizeof info};
    if (!m_opus || ::GetForegroundWindow() != m_opus
        || !::GetGUIThreadInfo(::GetWindowThreadProcessId(m_opus, nullptr), &info))
        return false;
    const bool items = folderview::isOpusFileDisplay(m_opus, info.hwndFocus);
    if (searchtyping::focusOf(static_cast<unsigned>(info.flags), items) != searchtyping::Focus::ItemsView)
        return false;
    tab = info.hwndFocus;
    return true;
}

bool TypeToSearch::sourceFocused() const
{
    GUITHREADINFO info {sizeof info};
    return m_sourceWindow && ::GetForegroundWindow() == m_sourceWindow
        && ::GetGUIThreadInfo(::GetWindowThreadProcessId(m_sourceWindow, nullptr), &info)
        && info.hwndFocus == m_sourceFocus
        && searchtyping::focusOf(static_cast<unsigned>(info.flags), true) == searchtyping::Focus::ItemsView;
}

bool TypeToSearch::desktopFocused() const
{
    if (!m_desktop || ::GetForegroundWindow() != m_desktop)
        return false;
    GUITHREADINFO info {sizeof info};
    if (!::GetGUIThreadInfo(::GetWindowThreadProcessId(m_desktop, nullptr), &info) || !info.hwndFocus)
        return false;
    const folderview::Hit hit = folderview::classify(info.hwndFocus, {});
    const bool icons = hit.surface == folderview::Surface::Desktop && hit.root == m_desktop;
    return searchtyping::focusOf(static_cast<unsigned>(info.flags), icons) == searchtyping::Focus::ItemsView;
}

// Whichever Explorer window seen is really in front, made m_explorer (the
// one left behind may have come forward with no event).
bool TypeToSearch::explorerInFront()
{
    if (!m_explorer && !m_explorerBehind)
        return false;
    const HWND front = searchtyping::typingWindow(::GetForegroundWindow(), m_explorer, m_explorerBehind);
    if (!front)
        return false;
    if (front != m_explorer)
        std::swap(m_explorer, m_explorerBehind); // the one left behind is in front now, the other may come back
    return true;
}

// Ctrl+V on the files of an Explorer folder or the desktop's icons, with words
// or a picture on the clipboard and no files: made a file there by the stub.
// The clipboard is asked by its formats alone (no program is asked), only once
// the focus is right.
bool TypeToSearch::handlePaste(bool press)
{
    if (!press) {
        const bool taken = std::exchange(m_pasteTaken, false);
        return taken; // V's release, after its press was taken
    }
    if (m_pasteTaken)
        return true; // held down: one file for one press
    HWND view = nullptr;
    HWND tab = nullptr;
    if (explorerInFront() && explorerFocused(tab))
        view = tab;
    else if (desktopFocused())
        view = m_desktop;
    if (!view || !pastefile::takesCtrlV(pastefile::clipboardOffer()))
        return false;
    m_pasteTaken = true;
    if (m_callbacks.pastedInto)
        m_callbacks.pastedInto(view);
    return true;
}

bool TypeToSearch::handleKey(WPARAM message, const KBDLLHOOKSTRUCT& event)
{
    const UINT vk = event.vkCode;
    const bool press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    // Ours too: the clipboard panel's paste, an entry picked with a folder in
    // front, is a paste into it all the same.
    if (vk == 'V' && m_surfaces.pasteFile && !m_holding && !m_startInFront
        && (m_pasteTaken
            || (press && held(VK_CONTROL) && !held(VK_SHIFT) && !held(VK_MENU) && !held(VK_LWIN) && !held(VK_RWIN)))
        && handlePaste(press))
        return true;
    // Ours: being typed again, given back, the Esc that closes the Start
    // menu for the launcher (win::bringToFront). Other programs' are typing
    // all the same (keys remapped by AutoHotkey or PowerToys, the on-screen
    // keyboard).
    if (isOwnInput(event.dwExtraInfo))
        return false;
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
        HWND source = nullptr;
        bool opus = false;
        if (m_startInFront) {
            if (m_windowsSearch || !searchtyping::startsTyping(vk))
                return false;
        } else if (m_opus && ::GetForegroundWindow() == m_opus) {
            if (!searchtyping::opusStarts(vk, press, combination, held(VK_SHIFT), searchtyping::Focus::ItemsView, false)
                || !opusFocused(tab))
                return false;
            source = m_opus;
            opus = true;
        } else if (m_surfaces.explorer && (m_explorer || m_explorerBehind)) {
            // The key alone first; what is in front and what has the focus
            // are asked only for one that would start a search.
            if (!searchtyping::explorerStarts(vk, press, combination, searchtyping::Focus::ItemsView, false))
                return false;
            if (!explorerInFront() || !explorerFocused(tab))
                return false;
            source = m_explorer;
        } else {
            return false;
        }
        m_holding = true;
        m_holdingForFolder = source != nullptr;
        m_sourceWindow = source;
        GUITHREADINFO focus {sizeof focus};
        m_sourceFocus = source && ::GetGUIThreadInfo(::GetWindowThreadProcessId(source, nullptr), &focus)
            ? focus.hwndFocus
            : nullptr;
        m_held.clear();
        m_toLauncher = false;
        m_askedAt = ::GetTickCount();
        m_asksAgain = 0;
        m_typedAlready = false;
        m_timer = ::SetTimer(nullptr, 0, kHoldTimeoutMs, nullptr);
        if (m_holdingForFolder) {
            const RequestId request = ++g_nextFolderRequest;
            m_pendingRequest = request;
            const auto& callback = opus ? m_callbacks.typedInOpus : m_callbacks.typedInExplorer;
            if (callback)
                callback(source, tab, vk, request);
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
        return; // nothing held
    m_toLauncher = true;
    if (!m_retyped.pending()) // on its way already: what is typed meanwhile follows it
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
    if (!m_toLauncher && m_holdingForFolder && !sourceFocused()) {
        m_held.clear(); // the user moved on: never type into another window or edit field
        endHolding();
        return;
    }
    if (m_toLauncher && !m_held.empty() && waitForFront())
        return; // held on: not into the window in front, which is not Win顺's
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
    m_typedAlready = true;
    restartTimer(kPassTimeoutMs);
    if (::SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) != inputs.size()) {
        m_retyped.clear(); // not all went in (blocked): none to wait for
        heldPassed();
    }
}

// The keys are for the launcher, which said it has the keyboard: whether a
// window of Win顺's really is in front. Qt can call the launcher active while
// the Start menu still is in front (its Esc came while it was still opening),
// and the Start menu can take the front back a few milliseconds after the
// launcher got it (Windows slow to start it): typed then, the keys went into
// Windows' search (GitHub #66). So they wait, looked at again at each foreground
// event and every kFrontPollMs, and with the Start menu in front again the
// launcher is asked for once more (searchtyping::delivery), until the hold runs
// out (dropHeld). True: wait.
bool TypeToSearch::waitForFront()
{
    const HWND front = ::GetForegroundWindow();
    DWORD pid = 0;
    ::GetWindowThreadProcessId(front, &pid);
    const DWORD now = ::GetTickCount();
    switch (searchtyping::delivery(front && pid == ::GetCurrentProcessId(), isStartMenu(front), !m_holdingForFolder,
        m_typedAlready, m_asksAgain, now - m_askedAt)) {
    case searchtyping::Delivery::Type:
        ::KillTimer(nullptr, m_frontTimer);
        m_frontTimer = 0;
        return false;
    case searchtyping::Delivery::AskAgain:
        ++m_asksAgain;
        m_askedAt = now;
        if (m_callbacks.typed)
            m_callbacks.typed(); // over the taskbar again; its Esc closes the Start menu
        break;
    case searchtyping::Delivery::Wait:
        break;
    }
    if (!m_frontTimer)
        m_frontTimer = ::SetTimer(nullptr, 0, kFrontPollMs, nullptr);
    return true;
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

// The same keys, to the file display they were typed in, if it is still in
// front (the launcher never showed).
void TypeToSearch::returnHeld()
{
    if (!m_holding)
        return;
    m_toLauncher = false; // to the original file display
    if (!m_retyped.pending())
        sendHeld();
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
    // Typed in a file display, the keys are the user's: back to it rather than
    // lost. In the Start menu they would go into Windows' search, which was
    // not wanted: dropped.
    if (m_holdingForFolder) {
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
    m_holdingForFolder = false;
    m_pendingRequest = 0;
    m_sourceWindow = nullptr;
    m_sourceFocus = nullptr;
    m_retyped.clear();
    m_toLauncher = false;
    ::KillTimer(nullptr, m_timer);
    m_timer = 0;
    ::KillTimer(nullptr, m_frontTimer);
    m_frontTimer = 0;
    setHooked(wantsHook()); // the launcher is in front: gone
}

} // namespace ws
