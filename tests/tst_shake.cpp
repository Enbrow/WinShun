// The shake that brings up the drop pie (ShakeDetector): a quick left-right
// shake during a drag fires, once; slow wiggles, vertical zigzags, straight
// drags, jitter and anything before the drag has begun do not.
#include "ShakeDetector.h"

#include <QtTest>

#include <cmath>

using namespace ws;

namespace {

// A pointer moved by hand: straight lines sampled every 8 ms (a 125 Hz mouse).
struct Hand {
    ShakeDetector detector;
    double x = 500;
    double y = 500;
    std::uint32_t t = 1000;
    int fired = 0;
    std::uint32_t lastFired = 0;

    void press(int dragX = 4, int dragY = 4, double scale = 1)
    {
        detector.press(static_cast<int>(x), static_cast<int>(y), t, dragX, dragY, scale);
    }
    void to(double toX, double toY, std::uint32_t ms)
    {
        const double fromX = x;
        const double fromY = y;
        const std::uint32_t start = t;
        for (std::uint32_t step = 8; step <= ms; step += 8) {
            const double f = static_cast<double>(step) / ms;
            x = fromX + (toX - fromX) * f;
            y = fromY + (toY - fromY) * f;
            t = start + step;
            if (detector.move(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), t)) {
                ++fired;
                lastFired = t;
            }
        }
        x = toX;
        y = toY;
        t = start + ms;
    }
    void dragOff() { to(x + 20, y, 40); } // past the drag rectangle
};

} // namespace

class TestShake : public QObject {
    Q_OBJECT

private slots:
    void realShakeFires()
    {
        Hand h;
        h.press();
        h.dragOff();
        h.to(h.x + 40, h.y, 120);
        h.to(h.x - 40, h.y + 4, 120);
        QCOMPARE(h.fired, 0);
        h.to(h.x + 40, h.y - 2, 120);
        QCOMPARE(h.fired, 1);
    }

    void leftFirstFires()
    {
        Hand h;
        h.press();
        h.dragOff();
        h.to(h.x - 30, h.y, 100);
        h.to(h.x + 30, h.y, 100);
        h.to(h.x - 30, h.y, 100);
        QCOMPARE(h.fired, 1);
    }

    // The file carried a long way first, then shaken: the carry's last
    // stretch is quick enough to be the first swing.
    void carryThenShake()
    {
        Hand h;
        h.press();
        h.dragOff();
        h.to(h.x + 300, h.y + 60, 1500);
        h.to(h.x - 30, h.y, 110);
        h.to(h.x + 30, h.y, 110);
        QCOMPARE(h.fired, 1);
    }

    void slowWiggleDoesNot()
    {
        Hand h;
        h.press();
        h.dragOff();
        for (int i = 0; i < 6; ++i)
            h.to(h.x + (i % 2 ? -40 : 40), h.y, 500);
        QCOMPARE(h.fired, 0);
    }

    void verticalZigzagDoesNot()
    {
        Hand h;
        h.press();
        h.dragOff();
        for (int i = 0; i < 6; ++i)
            h.to(h.x + (i % 2 ? -12 : 12), h.y + (i % 2 ? -40 : 40), 100);
        QCOMPARE(h.fired, 0);
    }

    void straightDragDoesNot()
    {
        Hand h;
        h.press();
        h.dragOff();
        h.to(h.x + 800, h.y + 200, 1000);
        h.to(h.x, h.y + 300, 500);
        QCOMPARE(h.fired, 0);
    }

    void tinyJitterDoesNot()
    {
        Hand h;
        h.press();
        h.dragOff();
        for (int i = 0; i < 20; ++i)
            h.to(h.x + (i % 2 ? -5 : 5), h.y, 40);
        QCOMPARE(h.fired, 0);
    }

    // Swings shorter than 16 logical pixels: at twice the scale, 20 device
    // pixels are 10 logical ones.
    void smallSwingsAtHighScaleDoNot()
    {
        Hand h;
        h.press(8, 8, 2);
        h.to(h.x + 30, h.y, 40);
        for (int i = 0; i < 6; ++i)
            h.to(h.x + (i % 2 ? -20 : 20), h.y, 80);
        QCOMPARE(h.fired, 0);
    }

    // Shaken within the drag rectangle: a click, no drag yet.
    void beforeTheDragDoesNot()
    {
        Hand h;
        h.press(200, 200);
        for (int i = 0; i < 6; ++i)
            h.to(h.x + (i % 2 ? -40 : 40), h.y, 100);
        QCOMPARE(h.fired, 0);
        QVERIFY(!h.detector.dragging());
    }

    void notWithoutAPress()
    {
        Hand h;
        h.press();
        h.dragOff();
        h.detector.release();
        for (int i = 0; i < 4; ++i)
            h.to(h.x + (i % 2 ? -40 : 40), h.y, 100);
        QCOMPARE(h.fired, 0);
    }

    // Shaken on and on: once, then again only from swings begun 600 ms
    // after it.
    void onceThenRearmed()
    {
        Hand h;
        h.press();
        h.dragOff();
        h.to(h.x + 40, h.y, 100);
        h.to(h.x - 40, h.y, 100);
        h.to(h.x + 40, h.y, 100);
        QCOMPARE(h.fired, 1);
        const std::uint32_t first = h.lastFired;
        for (int i = 0; i < 4; ++i) // 400 ms more of it: the same shake
            h.to(h.x + (i % 2 ? 40 : -40), h.y, 100);
        QCOMPARE(h.fired, 1);
        for (int i = 0; i < 6; ++i)
            h.to(h.x + (i % 2 ? 40 : -40), h.y, 100);
        QCOMPARE(h.fired, 2);
        QVERIFY(h.lastFired - first >= 600);
    }

    // A new press is a new gesture: it may fire at once.
    void newPressFiresAgain()
    {
        Hand h;
        h.press();
        h.dragOff();
        h.to(h.x + 40, h.y, 100);
        h.to(h.x - 40, h.y, 100);
        h.to(h.x + 40, h.y, 100);
        h.detector.release();
        h.t += 50;
        h.press();
        h.dragOff();
        h.to(h.x - 40, h.y, 100);
        h.to(h.x + 40, h.y, 100);
        h.to(h.x - 40, h.y, 100);
        QCOMPARE(h.fired, 2);
    }

    // A pause between swings breaks the run.
    void pauseBreaksTheRun()
    {
        Hand h;
        h.press();
        h.dragOff();
        h.to(h.x + 40, h.y, 100);
        h.to(h.x - 40, h.y, 100);
        h.to(h.x, h.y, 500); // held still
        h.to(h.x + 40, h.y, 100);
        QCOMPARE(h.fired, 0);
    }

    void clockWraps()
    {
        Hand h;
        h.t = 0xFFFFFF00u;
        h.press();
        h.dragOff();
        h.to(h.x + 40, h.y, 100);
        h.to(h.x - 40, h.y, 100);
        h.to(h.x + 40, h.y, 100);
        QCOMPARE(h.fired, 1);
    }
};

QTEST_APPLESS_MAIN(TestShake)
#include "tst_shake.moc"
