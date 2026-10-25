#include "Placement.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <windows.h>

using namespace Qt::StringLiterals;

class DisplayGeometryTest : public QObject {
    Q_OBJECT
private slots:
    void userDropAfterDpiChangeKeepsItsNewPosition()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QWindow window;
        window.resize(760, 400);
        window.create();
        const QString statePath = temporary.filePath(u"state.ini"_s);
        ws::Placement placement(statePath, u"Launcher"_s);
        placement.setContentSize({760, 400});
        placement.rememberWidth(760, 520);
        placement.setWindow(&window);
        QCoreApplication::processEvents();
        const QPoint dropped = window.position() + QPoint(40, 40);
        const auto handle = reinterpret_cast<HWND>(window.winId());
        ::SendMessageW(handle, WM_ENTERSIZEMOVE, 0, 0);
        window.setPosition(dropped);
        QEvent changed(QEvent::DevicePixelRatioChange);
        QCoreApplication::sendEvent(&window, &changed);
        QCoreApplication::processEvents(); // the display change waits for the user's drag
        ::SendMessageW(handle, WM_EXITSIZEMOVE, 0, 0);
        QTest::qWait(260); // queued position saving and any glide must have finished
        QCOMPARE(window.position(), dropped);
        QCOMPARE(window.size(), QSize(760, 400));
        QVERIFY(QSettings(statePath, QSettings::IniFormat).contains(u"Launcher/CenterX"_s));
    }

    void nativeResizeNeverBecomesPreferredSize()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString statePath = temporary.filePath(u"state.ini"_s);
        QWindow window;
        window.resize(760, 400);
        window.create(); // hidden native window; the user's desktop stays as it was
        ws::Placement placement(statePath, u"Launcher"_s);
        placement.setContentSize({760, 400});
        placement.rememberWidth(760, 520);
        placement.setWindow(&window);
        QCoreApplication::processEvents();
        const auto reconnect = [&] {
            const qreal dpr = window.devicePixelRatio();
            ::SetWindowPos(reinterpret_cast<HWND>(window.winId()), nullptr, 0, 0,
                qRound(480 * dpr), qRound(280 * dpr), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            QEvent event(QEvent::DevicePixelRatioChange);
            QCoreApplication::sendEvent(&window, &event);
        };
        for (int i = 0; i < 8; ++i) {
            reconnect();
            QTRY_COMPARE(window.size(), QSize(760, 400));
            QCOMPARE(placement.boxWidth(), 760);
        }
        QVERIFY(!QSettings(statePath, QSettings::IniFormat).contains(u"Launcher/Width"_s));
        placement.enterPanel(window.screen());
        const QSize panelSize = window.size();
        for (int i = 0; i < 8; ++i) {
            reconnect();
            QTRY_COMPARE(window.size(), panelSize);
        }
        placement.leavePanel();
        placement.enterPanel(window.screen());
        QCOMPARE(window.size(), panelSize);
        QVERIFY(!QSettings(statePath, QSettings::IniFormat).contains(u"Panel/Width"_s));
    }
};

QTEST_MAIN(DisplayGeometryTest)
#include "tst_displaygeometry.moc"
