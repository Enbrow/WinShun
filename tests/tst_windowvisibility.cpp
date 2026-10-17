#include "platform/WindowEffects.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QProcess>
#include <QTest>
#include <QWindow>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace {

// A separate process is needed: Windows applies the launcher's show flag
// to its first normal window. Match the settings window's native frame.
int probe(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QString mode = app.arguments().value(2);
    QWindow window;
    window.setFlags(Qt::Window | Qt::FramelessWindowHint);
    window.resize(480, 320);
    window.setPosition(12000, 12000);
    window.create();
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    window.show();
    if (mode == u"hidden-start") {
        if (!window.isVisible() || ::IsWindowVisible(hwnd)) {
            qCritical("The hidden-start precondition was not reproduced");
            return 1;
        }
        window.show(); // Qt already considers it visible: this cannot recover it.
        if (::IsWindowVisible(hwnd)) {
            qCritical("Repeated Qt show unexpectedly recovered the hidden window");
            return 2;
        }
    } else if (mode == u"native-hidden") {
        ::ShowWindow(hwnd, SW_HIDE);
        if (!window.isVisible() || ::IsWindowVisible(hwnd)) {
            qCritical("Native and Qt visibility did not diverge");
            return 3;
        }
    } else if (mode == u"minimized") {
        window.showMinimized();
        if (!::IsIconic(hwnd)) {
            qCritical("The window was not minimized");
            return 4;
        }
    } else if (mode == u"maximized") {
        window.showMaximized();
        if (!::IsZoomed(hwnd)) {
            qCritical("The window was not maximized");
            return 5;
        }
    }

    ws::win::bringToFront(&window);
    if (!::IsWindowVisible(hwnd) || ::IsIconic(hwnd)) {
        qCritical("An explicit open left the native window hidden or minimized");
        return 6;
    }
    if (mode == u"maximized" && !::IsZoomed(hwnd)) {
        qCritical("An already maximized window lost its state");
        return 7;
    }
    window.hide();
    return 0;
}

} // namespace

class WindowVisibilityTest : public QObject {
    Q_OBJECT

private slots:
    void explicitOpen_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("hidden-start") << u"hidden-start"_s;
        QTest::newRow("native-hidden") << u"native-hidden"_s;
        QTest::newRow("minimized") << u"minimized"_s;
        QTest::newRow("maximized") << u"maximized"_s;
    }

    void explicitOpen()
    {
        QFETCH(QString, mode);
        QProcess child;
        child.setCreateProcessArgumentsModifier([mode](QProcess::CreateProcessArguments* args) {
            args->flags |= CREATE_NO_WINDOW;
            args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            args->startupInfo->wShowWindow = mode == u"hidden-start" ? SW_HIDE : SW_SHOWNORMAL;
        });
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {u"--visibility-probe"_s, mode});
        QVERIFY2(child.waitForStarted(), qPrintable(child.errorString()));
        QVERIFY2(child.waitForFinished(15000), qPrintable(child.errorString()));
        const QByteArray output = child.readAll();
        QVERIFY2(child.exitStatus() == QProcess::NormalExit, output.constData());
        QVERIFY2(child.exitCode() == 0, output.constData());
    }
};

int main(int argc, char** argv)
{
    if (argc > 1 && qstrcmp(argv[1], "--visibility-probe") == 0)
        return probe(argc, argv);
    QCoreApplication app(argc, argv);
    WindowVisibilityTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_windowvisibility.moc"
