#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QStyleHints>
#include <QTest>
#include <QWheelEvent>

#include <memory>

class ScrollAlignmentTest : public QObject {
    Q_OBJECT

    QQmlEngine engine;
    std::unique_ptr<QQuickWindow> window;
    QQuickItem* view = nullptr;
    QQuickItem* first = nullptr;
    QQuickItem* second = nullptr;
    QObject* wheelScroll = nullptr;
    QElapsedTimer inputClock;

    void scrollTo(qreal y)
    {
        view->setProperty("contentY", y);
        QCoreApplication::processEvents();
    }

    qreal rowY() const { return first->mapToScene({0, 0}).y(); }
    qreal top() const
    {
        return view->property("originY").toReal() - view->property("topMargin").toReal();
    }

    qreal contentY() const { return view->property("contentY").toReal(); }

    void wheel(int delta, bool inverted = false)
    {
        const QPointF point = view->mapToScene({200, 100});
        QWheelEvent event(point, window->mapToGlobal(point), {}, QPoint(0, delta),
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, inverted);
        event.setTimestamp(inputClock.elapsed() + 1);
        QCoreApplication::sendEvent(window.get(), &event);
    }

    qreal destination(qreal from, int delta) const
    {
        const int lines = QGuiApplication::styleHints()->wheelScrollLines();
        const qreal step = -delta / 120.0 * (lines > 0 ? lines * 24 : view->height());
        const qreal dpr = window->devicePixelRatio();
        return top() + qRound((from + step - top()) * dpr) / dpr;
    }

private slots:
    void init()
    {
        inputClock.start();
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            Window {
                width: 760; height: 560; visible: true
                ListModel {
                    id: mutableRows
                    Component.onCompleted: {
                        for (let i = 0; i < 100; ++i)
                            append({value: i})
                    }
                }
                ListView {
                    id: list
                    objectName: "view"
                    y: 65; width: 760; height: 488
                    topMargin: 4; bottomMargin: 4
                    boundsBehavior: Flickable.StopAtBounds
                    model: 100
                    cacheBuffer: 1200
                    property bool mixedRows: false
                    function useMutableRows() { model = mutableRows; cacheBuffer = 0 }
                    function removeHead() { mutableRows.remove(0, 1) }
                    ListWheelScroll { objectName: "wheelScroll"; view: list }
                    delegate: Rectangle {
                        id: row
                        required property int index
                        objectName: "row" + index
                        width: 760
                        height: list.mixedRows && index % 3 === 1 ? 100 : 60
                        color: "black"
                        transform: ScrollPixelAlignment {
                            view: list
                            viewY: list.y
                            itemY: row.y
                        }
                    }
                }
            }
        )", QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR) + "/scroll-test.qml"));
        window.reset(qobject_cast<QQuickWindow*>(component.create()));
        QVERIFY2(window, qPrintable(component.errorString()));
        view = window->findChild<QQuickItem*>("view");
        QVERIFY(view);
        QMetaObject::invokeMethod(view, "forceLayout");
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, first), Q_ARG(int, 0)));
        QVERIFY(QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, second), Q_ARG(int, 1)));
        QVERIFY(first);
        QVERIFY(second);
        wheelScroll = window->findChild<QObject*>("wheelScroll");
        QVERIFY(wheelScroll);
    }

    void cleanup() { window.reset(); }

    void endpointTail_data()
    {
        QTest::addColumn<qreal>("listY");
        QTest::newRow("reported-layout") << qreal(65);
        // An endpoint exactly halfway between device pixels, at any DPI.
        QTest::newRow("half-device-pixel") << 103.5 / QGuiApplication::primaryScreen()->devicePixelRatio() - 4;
    }

    void endpointTail()
    {
        QFETCH(qreal, listY);
        view->setY(listY);
        scrollTo(top());
        const qreal resting = rowY();
        const qreal dpr = window->devicePixelRatio();
        // Recorded values approaching -4 included -3.999999986277995.
        // Once visually at rest, finishing or reversing the tiny tail must
        // not move a row. Compare scene positions, not merely contentY.
        for (qreal pixels : {0.49, 0.2, 0.01, 0.0001, 0.00000002, 0.0, 0.01, 0.2}) {
            const qreal requested = top() + pixels / dpr;
            scrollTo(requested);
            QVERIFY2(qAbs(rowY() - resting) * dpr < 0.001, "late visible step at the top");
            QCOMPARE(view->property("contentY").toReal(), requested);
        }
    }

    void rowsMoveTogetherOnDevicePixels()
    {
        view->setY(65);
        scrollTo(top());
        const qreal dpr = window->devicePixelRatio();
        const qreal gap = second->mapToScene({0, 0}).y() - rowY();
        qreal previous = rowY();
        for (int step = 0; step < 100; ++step) {
            scrollTo(top() + step * 0.2 / dpr);
            const qreal current = rowY();
            QVERIFY(qAbs(current * dpr - qRound(current * dpr)) < 0.001);
            QVERIFY(current <= previous + 0.001);
            QVERIFY(qAbs(second->mapToScene({0, 0}).y() - current - gap) < 0.001);
            previous = current;
        }
    }

    void wheelStops_data()
    {
        QTest::addColumn<qreal>("start");
        QTest::addColumn<int>("delta");
        QTest::newRow("down-from-fraction") << qreal(120.4) << -120;
        QTest::newRow("up-from-fraction") << qreal(240.4) << 120;
        QTest::newRow("small-down") << qreal(120.15) << -15;
        QTest::newRow("small-up") << qreal(240.15) << 15;
    }

    void wheelStops()
    {
        QFETCH(qreal, start);
        QFETCH(int, delta);
        view->setY(65);
        QMetaObject::invokeMethod(view, "cancelFlick");
        scrollTo(start);
        QTest::qWait(30);
        const qreal dpr = window->devicePixelRatio();
        wheel(delta);
        QElapsedTimer clock;
        clock.start();
        qreal previous = rowY() * dpr;
        qint64 lastStep = 0, longestPause = 0;
        int reverseSteps = 0;
        while (clock.elapsed() < 500) {
            QTest::qWait(3);
            const qreal current = rowY() * dpr;
            if (qAbs(current - previous) > 0.1) {
                longestPause = qMax(longestPause, clock.elapsed() - lastStep);
                lastStep = clock.elapsed();
                if ((current - previous) * delta < 0)
                    ++reverseSteps;
                previous = current;
            }
        }
        const qreal displacement = (view->property("contentY").toReal() - top()) * dpr;
        qInfo() << "wheel stop" << start << delta << "final" << view->property("contentY")
                << "longest pause" << longestPause << "reverse steps" << reverseSteps;
        QCOMPARE(reverseSteps, 0);
        QVERIFY(qAbs(contentY() - destination(start, delta)) < 0.001);
        QVERIFY2(qAbs(displacement - qRound(displacement)) < 0.001,
            "wheel endpoint was not planned on the device-pixel grid");
        QVERIFY2(longestPause < 80, "a visible step occurred after the glide appeared to stop");
    }

    void burstAndReversal()
    {
        scrollTo(300.25);
        wheel(-120);
        QTest::qWait(35);
        wheel(-120);
        QTest::qWait(35);
        const qreal turn = contentY();
        wheel(120);
        qreal previous = contentY();
        for (int i = 0; i < 60; ++i) {
            QTest::qWait(5);
            QVERIFY2(contentY() <= previous + 0.001, "continued in the old direction after reversal");
            previous = contentY();
        }
        QVERIFY(qAbs(contentY() - destination(turn, 120)) < 0.001);

        scrollTo(300.25);
        wheel(-120);
        QTest::qWait(35);
        wheel(-120);
        QTest::qWait(300);
        QVERIFY(qAbs(contentY() - destination(300.25, -240)) < 0.001);
    }

    void externalMovementCancelsWheel()
    {
        scrollTo(300.25);
        wheel(-120);
        QTest::qWait(40);
        scrollTo(500); // scroll bar or positionViewAtIndex()
        QTest::qWait(300);
        QCOMPARE(contentY(), qreal(500));
        QVERIFY(!wheelScroll->property("running").toBool());

        wheel(-120);
        QTest::qWait(40);
        view->setProperty("currentIndex", 5); // selection can stay within the viewport
        const qreal selected = contentY();
        QTest::qWait(300);
        QCOMPARE(contentY(), selected);

        wheel(-120);
        window->hide(); // also cancel before the first animation frame
        QTest::qWait(30);
        window->show();
        scrollTo(top());
        QTest::qWait(300);
        QCOMPARE(contentY(), top());
    }

    void boundsAndReducedMotion()
    {
        scrollTo(18.4);
        wheel(1200);
        QTest::qWait(300);
        QCOMPARE(contentY(), top());
        wheel(120);
        QTest::qWait(300);
        QCOMPARE(contentY(), top());

        wheelScroll->setProperty("animate", false);
        wheel(-120);
        QVERIFY(qAbs(contentY() - destination(top(), -120)) < 0.001);
        QVERIFY(!wheelScroll->property("running").toBool());
        wheel(-12000);
        const qreal bottom = view->property("originY").toReal()
            + view->property("contentHeight").toReal() + 4 - view->height();
        QCOMPARE(contentY(), bottom);
        QTest::qWait(300);
        QCOMPARE(contentY(), bottom);
    }

    void synthesizedGestureReachesFlickable()
    {
        scrollTo(300.25);
        const QPointF point = view->mapToScene({200, 100});
        const auto send = [&](Qt::ScrollPhase phase, int pixels) {
            QWheelEvent event(point, window->mapToGlobal(point), QPoint(0, pixels),
                QPoint(0, pixels ? -120 : 0), Qt::NoButton, Qt::NoModifier,
                phase, false, Qt::MouseEventSynthesizedBySystem);
            event.setTimestamp(inputClock.elapsed() + 1);
            QCoreApplication::sendEvent(window.get(), &event);
            // Flickable accepts the event itself; WheelHandler only accepts
            // its event point. Check delivery, not a simulated finger's feel.
            if (phase == Qt::ScrollUpdate)
                QVERIFY(event.isAccepted());
        };
        send(Qt::ScrollBegin, 0);
        QTest::qWait(20);
        send(Qt::ScrollUpdate, -20);
        QTest::qWait(20);
        send(Qt::ScrollUpdate, -20);
        QTest::qWait(20);
        QVERIFY(!wheelScroll->property("running").toBool());
        send(Qt::ScrollEnd, 0);
    }

    void naturalScrolling_data()
    {
        QTest::addColumn<int>("delta");
        QTest::newRow("positive-delivered-delta") << 120;
        QTest::newRow("negative-delivered-delta") << -120;
    }

    void naturalScrolling()
    {
        QFETCH(int, delta);
        scrollTo(300);
        wheel(delta);
        QTest::qWait(260);
        const qreal normal = contentY();
        scrollTo(300);
        // inverted describes the delivered delta; it must not invert it again.
        wheel(delta, true);
        QTest::qWait(260);
        QCOMPARE(contentY(), normal);
        QVERIFY((contentY() - 300) * delta < 0);
    }

    void smallDeltasAccumulate_data()
    {
        QTest::addColumn<bool>("animate");
        QTest::addColumn<bool>("waitForStop");
        QTest::newRow("animated") << true << false;
        QTest::newRow("reduced-motion") << false << false;
        QTest::newRow("separate-animated-events") << true << true;
    }

    void smallDeltasAccumulate()
    {
        QFETCH(bool, animate);
        QFETCH(bool, waitForStop);
        wheelScroll->setProperty("animate", animate);
        scrollTo(300);
        const int count = waitForStop ? 3 : 20;
        for (int i = 0; i < count; ++i) {
            wheel(-1);
            QTest::qWait(waitForStop ? 240 : 4);
        }
        QTest::qWait(260);
        const qreal expected = 300 + QGuiApplication::styleHints()->wheelScrollLines() * 24 * count / 120.0;
        QVERIFY2(qAbs(contentY() - expected) * window->devicePixelRatio() <= 0.501,
            "small delivered deltas lost their fractional distance");
    }

    void zeroScrollLines()
    {
        wheelScroll->setProperty("scrollLines", 0);
        scrollTo(300);
        wheel(-120);
        QTest::qWait(260);
        QCOMPARE(contentY(), qreal(300));
    }

    void modelChangeDuringWheel()
    {
        QVERIFY(QMetaObject::invokeMethod(view, "useMutableRows"));
        QMetaObject::invokeMethod(view, "forceLayout");
        scrollTo(600);
        QTest::qWait(30);
        wheel(-120);
        QTest::qWait(45);
        const qreal oldOrigin = view->property("originY").toReal();
        QVERIFY(QMetaObject::invokeMethod(view, "removeHead"));
        QMetaObject::invokeMethod(view, "forceLayout");
        QTest::qWait(30);
        QVERIFY(view->property("originY").toReal() != oldOrigin);
        QVERIFY(!wheelScroll->property("running").toBool());
        const qreal settled = contentY();
        QTest::qWait(260);
        QCOMPARE(contentY(), settled);
    }

    void mixedRowsReachBothEnds()
    {
        wheelScroll->setProperty("logicalPixelSteps", true);
        view->setProperty("cacheBuffer", 0);
        view->setProperty("mixedRows", true);
        QMetaObject::invokeMethod(view, "forceLayout");
        QTest::qWait(30);
        scrollTo(top());
        wheel(-12000);
        QTRY_VERIFY_WITH_TIMEOUT(view->property("atYEnd").toBool(), 1500);
        QTest::qWait(250);
        QVERIFY(view->property("atYEnd").toBool());
        wheel(12000);
        QTRY_VERIFY_WITH_TIMEOUT(view->property("atYBeginning").toBool(), 1500);
        QTest::qWait(250);
        QCOMPARE(contentY(), top());
    }

    void wholePageStop()
    {
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            Window {
                width: 760; height: 560; visible: true
                Flickable {
                    id: flick
                    objectName: "view"
                    y: 65; width: 760; height: 488
                    contentWidth: width; contentHeight: 3000
                    boundsBehavior: Flickable.StopAtBounds
                    ListWheelScroll { view: flick }
                    Rectangle {
                        id: content
                        objectName: "content"
                        y: 8; width: 760; height: 3000
                        transform: ScrollPixelAlignment {
                            view: flick; viewY: flick.y; itemY: content.y
                            pixelBias: 0.02
                        }
                    }
                }
            }
        )", QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR) + "/page-scroll-test.qml"));
        window.reset(qobject_cast<QQuickWindow*>(component.create()));
        QVERIFY2(window, qPrintable(component.errorString()));
        view = window->findChild<QQuickItem*>("view");
        first = window->findChild<QQuickItem*>("content");
        QVERIFY(view);
        QVERIFY(first);
        scrollTo(120.4);
        wheel(-15);
        qreal previous = rowY();
        for (int i = 0; i < 100; ++i) {
            QTest::qWait(5);
            const qreal current = rowY();
            QVERIFY2(current <= previous + 0.001, "whole-page wheel reversed at its tail");
            const qreal pixels = current * window->devicePixelRatio() - 0.02;
            QVERIFY(qAbs(pixels - qRound(pixels)) < 0.001);
            previous = current;
        }
        QVERIFY(qAbs(contentY() - destination(120.4, -15)) < 0.001);
    }
};

QTEST_MAIN(ScrollAlignmentTest)
#include "tst_scrollalignment.moc"
