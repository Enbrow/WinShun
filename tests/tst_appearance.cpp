#include "Settings.h"
#include "SystemTheme.h"
#include "platform/WindowEffects.h"

#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>

#include <dwmapi.h>

using namespace ws;
using namespace Qt::StringLiterals;

class AppearanceTest : public QObject {
    Q_OBJECT

private slots:
    void settingsRoundTrip()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        Settings::useProfile(profile.path());
        const QString expected = QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11
            ? u"round"_s : u"square"_s;
        QCOMPARE(Settings::defaults().corners, expected);
        Settings settings;
        settings.load();
        QCOMPARE(settings.corners, expected);
        for (const QString& value : {u"square"_s, u"round"_s}) {
            settings.corners = value;
            settings.save();
            Settings loaded;
            loaded.load();
            QCOMPARE(loaded.corners, value);
        }
        for (const QString& value : {u"invalid"_s, u""_s, u" SQUARE "_s}) {
            QSettings ini(Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Appearance/Corners"_s, value);
            ini.sync();
            settings.load();
            QCOMPARE(settings.corners, value == u" SQUARE " ? u"square"_s : expected);
        }
    }

    void liveWindows()
    {
        if (QOperatingSystemVersion::current() < QOperatingSystemVersion::Windows11)
            QSKIP("DWM corner attributes require Windows 11");
        QWindow normal;
        normal.setFlags(Qt::Tool | Qt::FramelessWindowHint);
        win::styleFramelessWindow(&normal);
        const HWND page = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
            0, 0, 80, 60, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(page);
        win::setWindowCorners(page, true);
        const auto preference = [](HWND hwnd) {
            int value = -1;
            return SUCCEEDED(::DwmGetWindowAttribute(hwnd, 33, &value, sizeof value)) ? value : -1;
        };
        for (bool round : {false, true, false, true}) {
            SystemTheme::setRoundedCorners(round);
            QCOMPARE(preference(reinterpret_cast<HWND>(normal.winId())), round ? 2 : 1);
            QCOMPARE(preference(page), round ? 3 : 1);
            QWindow createdAfter;
            createdAfter.setFlags(Qt::Tool | Qt::FramelessWindowHint);
            win::styleFramelessWindow(&createdAfter);
            QCOMPARE(preference(reinterpret_cast<HWND>(createdAfter.winId())), round ? 2 : 1);
        }
        ::DestroyWindow(page);
    }

    void liveTokensAndSquarePixels()
    {
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
        qmlRegisterSingletonType<SystemTheme>("WinShun", 1, 0, "SystemTheme",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new SystemTheme; });
        const QUrl folder = QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR) + u'/');
        qmlRegisterSingletonType(folder.resolved(QUrl(u"Theme.qml"_s)), "WinShun", 1, 0, "Theme");
        QQmlEngine engine;
        QQmlComponent probe(&engine);
        probe.setData(R"(
            import QtQuick
            import WinShun 1.0
            QtObject {
                readonly property real radius: Theme.controlRadius
                readonly property real pill: Theme.pillRadius(26)
                readonly property color edge: Theme.windowEdge
            }
        )", folder);
        std::unique_ptr<QObject> values(probe.create());
        QVERIFY2(values, qPrintable(probe.errorString()));
        QSignalSpy changed(values.get(), SIGNAL(radiusChanged()));
        for (bool round : {true, false, true, false}) {
            SystemTheme::setRoundedCorners(round);
            const bool effective = round && QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11;
            QCOMPARE(values->property("radius").toDouble(), effective ? 4.0 : 0.0);
            QCOMPARE(values->property("pill").toDouble(), effective ? 13.0 : 0.0);
        }
        if (QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11)
            QVERIFY(changed.count() >= 3);

        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        window.resize(160, 90);
        window.setColor(QColor("#F9F9F9"));
        QQmlComponent edge(&engine, folder.resolved(QUrl(u"WindowEdge.qml"_s)));
        std::unique_ptr<QObject> object(edge.create());
        QVERIFY2(object, qPrintable(edge.errorString()));
        auto* item = qobject_cast<QQuickItem*>(object.get());
        QVERIFY(item);
        item->setParentItem(window.contentItem());
        win::styleFramelessWindow(&window);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const QImage pixels = window.grabWindow();
        QVERIFY(!pixels.isNull());
        const QColor border = values->property("edge").value<QColor>();
        for (const QPoint& p : {QPoint(0, 0), QPoint(pixels.width() - 1, 0),
                 QPoint(0, pixels.height() - 1), QPoint(pixels.width() - 1, pixels.height() - 1),
                 QPoint(1, 0), QPoint(0, 1)})
            QCOMPARE(pixels.pixelColor(p), border);
        QCOMPARE(item->property("antialiasing").toBool(), false);
    }
};

QTEST_MAIN(AppearanceTest)
#include "tst_appearance.moc"
