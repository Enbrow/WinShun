#include "platform/KeyRouter.h"

#include <QCoreApplication>
#include <QProcess>
#include <QScopeGuard>
#include <QTest>

#include <imm.h>

#include <array>
#include <atomic>
#include <cstdio>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

LRESULT CALLBACK receiverProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_KEYDOWN || message == WM_KEYUP) {
        std::printf("%s:%u\n", message == WM_KEYDOWN ? "down" : "up", static_cast<unsigned>(wParam));
        std::fflush(stdout);
    } else if (message == WM_ACTIVATEAPP && !wParam) {
        std::puts("deactivated");
        std::fflush(stdout);
    } else if (message == WM_DESTROY) {
        ::PostQuitMessage(0);
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

int receiver()
{
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSW type {};
    type.lpfnWndProc = receiverProc;
    type.hInstance = instance;
    type.lpszClassName = L"WinShun.KeyRouterTest";
    if (!::RegisterClassW(&type))
        return 1;
    const HWND window = ::CreateWindowW(type.lpszClassName, L"WinShun keyboard routing test (temporary)",
        WS_OVERLAPPEDWINDOW, 100, 100, 520, 220, nullptr, nullptr, instance, nullptr);
    if (!window)
        return 2;
    ::ImmAssociateContextEx(window, nullptr, 0); // the fixture records raw letters, independent of the user's IME mode
    ::ShowWindow(window, SW_SHOWNORMAL);
    ::SetForegroundWindow(window);
    std::printf("window:%llu\n", static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(window)));
    std::fflush(stdout);
    MSG message;
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return 0;
}

struct PassiveWindow {
    HWND hwnd = ::CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"Passive clipboard test",
        WS_POPUP, 120, 340, 350, 80, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    ~PassiveWindow() { if (hwnd) ::DestroyWindow(hwnd); }
};

bool send(HWND target, WORD vk, bool up)
{
    // Never inject into a user window, or while a physical modifier is held.
    if (::GetForegroundWindow() != target)
        return false;
    for (const int modifier : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN}) {
        if (::GetAsyncKeyState(modifier) & 0x8000)
            return false;
    }
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (vk == VK_DOWN)
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    return ::SendInput(1, &input, sizeof input) == 1;
}

} // namespace

class KeyRouterTest : public QObject {
    Q_OBJECT

private slots:
    void refreshDoesNotActivateOrResumeDrainedRouting()
    {
        PassiveWindow passive;
        QVERIFY(passive.hwnd);
        const HWND foreground = ::GetForegroundWindow();
        std::atomic<int> keys {0};
        std::atomic<int> drained {0};
        KeyRouter router(nullptr, passive.hwnd, {.key = [&](const auto&) { ++keys; }, .drained = [&] { ++drained; }});
        QVERIFY(router.isActive());
        router.refresh();
        router.drain();
        router.refresh(); // a queued click cannot resume a retired session
        QTRY_COMPARE_WITH_TIMEOUT(drained.load(), 1, 2000);
        QCOMPARE(::GetForegroundWindow(), foreground);
        QVERIFY(!::IsWindowVisible(passive.hwnd));
        QCOMPARE(keys.load(), 0);
    }

    void liveRoutingRefreshAndDrain()
    {
        if (qEnvironmentVariableIntValue("WINSHUN_KEYROUTER_LIVE") != 1)
            QSKIP("Opt-in desktop test: uses guarded keys in its own temporary receiver only.");
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {u"--key-receiver"_s});
        QVERIFY(child.waitForStarted(5000));
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output += child.readAll()).contains("window:"), 5000);
        const QByteArray handle = output.mid(output.indexOf("window:") + 7).split('\n').front().trimmed();
        const HWND target = reinterpret_cast<HWND>(handle.toULongLong());
        QVERIFY(target && ::IsWindow(target));
        const auto close = qScopeGuard([&] {
            ::PostMessageW(target, WM_CLOSE, 0, 0);
            child.waitForFinished(3000);
        });
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), target, 3000);
        PassiveWindow passive;
        QVERIFY(passive.hwnd);
        ::ShowWindow(passive.hwnd, SW_SHOWNOACTIVATE);

        std::array<std::atomic<int>, 256> presses {};
        std::array<std::atomic<int>, 256> releases {};
        KeyRouter router(target, passive.hwnd, {.key = [&](const KeyRouter::Key& key) {
            if (key.vk < presses.size())
                ++(key.press ? presses[key.vk] : releases[key.vk]);
        }});
        QVERIFY(router.isActive());
        const auto tap = [&](WORD vk) { return send(target, vk, false) && send(target, vk, true); };
        QVERIFY(tap('A'));
        QTRY_COMPARE_WITH_TIMEOUT(releases['A'].load(), 1, 2000);
        QVERIFY(tap(VK_DOWN));
        QTRY_COMPARE_WITH_TIMEOUT(releases[VK_DOWN].load(), 1, 2000);

        // Refresh with a key held: its release still belongs to this session.
        QVERIFY(send(target, 'B', false));
        QTRY_COMPARE_WITH_TIMEOUT(presses['B'].load(), 1, 2000);
        router.refresh();
        QTest::qWait(50);
        QVERIFY(send(target, 'B', true));
        QTRY_COMPARE_WITH_TIMEOUT(releases['B'].load(), 1, 2000);
        QCOMPARE(::GetForegroundWindow(), target);
        output += child.readAll();
        QVERIFY2(!output.contains("down:"), output.constData());
        QVERIFY2(!output.contains("up:"), output.constData());
        QVERIFY2(!output.contains("deactivated"), output.constData());

        // Explicit recovery still routes subsequent typing and leaves the
        // receiver's input field in front. No IME or clipboard is involved.
        QVERIFY(tap('C'));
        QTRY_VERIFY2_WITH_TIMEOUT(releases['C'].load() == 1,
            qPrintable(u"C presses=%1 releases=%2; receiver=%3"_s.arg(presses['C'].load()).arg(releases['C'].load())
                .arg(QString::fromUtf8(output + child.readAll()))), 2000);
        router.refresh();
        QTest::qWait(50);
        QVERIFY(tap('D'));
        QTRY_COMPARE_WITH_TIMEOUT(releases['D'].load(), 1, 2000);
        QCOMPARE(::GetForegroundWindow(), target);
        output += child.readAll();
        QVERIFY2(!output.contains("down:68"), output.constData());
        QVERIFY2(!output.contains("up:68"), output.constData());
        QVERIFY2(!output.contains("deactivated"), output.constData());

        router.drain();
        QVERIFY(tap('E'));
        QTRY_VERIFY_WITH_TIMEOUT((output += child.readAll()).contains("down:69"), 2000);
        QCOMPARE(presses['E'].load(), 0);
    }
};

int main(int argc, char** argv)
{
    if (argc > 1 && qstrcmp(argv[1], "--key-receiver") == 0)
        return receiver();
    QCoreApplication app(argc, argv);
    KeyRouterTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_keyrouter.moc"
