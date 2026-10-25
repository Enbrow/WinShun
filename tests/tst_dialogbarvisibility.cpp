#include "DialogBarVisibility.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QSignalBlocker>
#include <QTest>

#include <dwmapi.h>

#include <thread>

namespace {

HWND handle(QQuickWindow& window)
{
    return reinterpret_cast<HWND>(window.winId());
}

DWORD cloaked(QQuickWindow& window)
{
    DWORD value = 0;
    ::DwmGetWindowAttribute(handle(window), DWMWA_CLOAKED, &value, sizeof value);
    return value;
}

void prepare(QQuickWindow& window)
{
    window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    window.setColor(Qt::darkGray);
    window.setGeometry(80, 80, 480, 44);
    window.create();
}

// Deliver a frame from a render thread while the GUI is busy placing the
// bar. Its queued reveal must not yet let place() show the separate logo.
void swapFrame(QQuickWindow& window)
{
    std::thread render([&window] {
        emit window.afterSynchronizing();
        emit window.frameSwapped();
    });
    render.join();
}

} // namespace

class DialogBarVisibilityTest : public QObject {
    Q_OBJECT

private slots:
    void frameWaitsForGuiReveal()
    {
        QQuickWindow window;
        prepare(window);
        bool logo = false;
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&window, [&](bool shown) { logo = shown; });
        visibility.show();
        QVERIFY(window.isVisible());
        QVERIFY(cloaked(window) != 0);
        swapFrame(window);
        QVERIFY(!visibility.isRevealed());
        QVERIFY(!logo);
        QTRY_VERIFY(visibility.isRevealed());
        QCOMPARE(cloaked(window), DWORD(0));
        QVERIFY(logo);
    }

    void previousFrameCannotRevealNewOpening()
    {
        QQuickWindow window;
        prepare(window);
        bool logo = false;
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&window, [&](bool shown) { logo = shown; });
        visibility.show();
        swapFrame(window);
        visibility.hide();
        visibility.show();
        // Deliver only the old posted frame, without drawing a new one.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QVERIFY(!visibility.isRevealed());
        QVERIFY(!logo);
        QVERIFY(cloaked(window) != 0);
        QTRY_VERIFY(visibility.isRevealed());
        QVERIFY(logo);
        QCOMPARE(cloaked(window), DWORD(0));
    }

    void frameInFlightCannotRevealNewOpening()
    {
        QQuickWindow window;
        prepare(window);
        bool logo = false;
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&window, [&](bool shown) { logo = shown; });
        visibility.show();
        std::thread synchronize([&window] { emit window.afterSynchronizing(); });
        synchronize.join();
        visibility.hide();
        visibility.show();
        std::thread present([&window] { emit window.frameSwapped(); });
        present.join();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QVERIFY(!visibility.isRevealed());
        QVERIFY(!logo);
        QVERIFY(cloaked(window) != 0);
        QTRY_VERIFY(visibility.isRevealed());
        QVERIFY(logo);
    }

    void externalHideHidesLogo_data()
    {
        QTest::addColumn<bool>("close");
        QTest::newRow("Qt hide") << false;
        QTest::newRow("Qt close") << true;
    }

    void externalHideHidesLogo()
    {
        QFETCH(bool, close);
        QQuickWindow window;
        prepare(window);
        bool logo = false;
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&window, [&](bool shown) { logo = shown; });
        visibility.show();
        QTRY_VERIFY(logo);
        const HWND beforeClose = handle(window);
        if (close)
            window.close(); // close/Alt+F4, outside DialogBar::hideWindow
        else
            window.hide();
        QVERIFY(!window.isVisible());
        QVERIFY(!logo);
        visibility.hide(); // cleanup must also work after Qt already hid it
        QVERIFY(!logo);
        QVERIFY(::IsWindow(beforeClose));
        visibility.show();
        QTRY_VERIFY(logo);
        QCOMPARE(handle(window), beforeClose); // DialogJump's companion HWND stays valid
    }

    void nativeHiddenWindowRecoversWithoutActivation()
    {
        QQuickWindow window;
        prepare(window);
        bool logo = false;
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&window, [&](bool shown) { logo = shown; });
        visibility.show();
        QTRY_VERIFY(logo);
        ::ShowWindow(handle(window), SW_HIDE);
        QVERIFY(window.isVisible());
        QVERIFY(!::IsWindowVisible(handle(window)));
        visibility.show();
        QTRY_VERIFY_WITH_TIMEOUT(::IsWindowVisible(handle(window)), 1500);
        QTRY_VERIFY(visibility.isRevealed());
        QVERIFY(::GetForegroundWindow() != handle(window));
        QVERIFY(logo);
    }

    void timeoutShowsWindowWithoutFrameSignal()
    {
        QQuickWindow window;
        prepare(window);
        bool logo = false;
        ws::DialogBarVisibility visibility;
        visibility.setWindow(&window, [&](bool shown) { logo = shown; });
        const QSignalBlocker noFrameSignal(&window);
        visibility.show();
        QTRY_VERIFY_WITH_TIMEOUT(logo, 1500);
        QVERIFY(visibility.isRevealed());
        QVERIFY(::IsWindowVisible(handle(window)));
        QCOMPARE(cloaked(window), DWORD(0));
        visibility.hide();
        QVERIFY(!logo);
        QVERIFY(!window.isVisible());
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    DialogBarVisibilityTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_dialogbarvisibility.moc"
