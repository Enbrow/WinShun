// The drop pie's geometry (PieGeometry): which ring and sector a point is
// on, the move / copy latch, where the ring goes near a screen's edges and
// the window around it.
#include "PieGeometry.h"

#include <QtTest>

#include <cmath>

using namespace ws::pie;

class TestPieGeometry : public QObject {
    Q_OBJECT

private slots:
    void angles()
    {
        QCOMPARE(angleOf({0, -10}), 0.0);
        QCOMPARE(angleOf({10, 0}), 90.0);
        QCOMPARE(angleOf({0, 10}), 180.0);
        QCOMPARE(angleOf({-10, 0}), 270.0);
        const QPointF p = polar(100, 90);
        QVERIFY(std::abs(p.x() - 100) < 1e-9 && std::abs(p.y()) < 1e-9);
    }

    void sectors()
    {
        // Eight: sector 0 centred at the top, then clockwise.
        QCOMPARE(sectorAt(0, 8), 0);
        QCOMPARE(sectorAt(22, 8), 0);
        QCOMPARE(sectorAt(23, 8), 1);
        QCOMPARE(sectorAt(359, 8), 0);
        QCOMPARE(sectorAt(337, 8), 7);
        QCOMPARE(sectorAt(180, 8), 4);
        QCOMPARE(sectorCentre(2, 8), 90.0);
        // One fills the ring; three split it in thirds.
        QCOMPARE(sectorAt(200, 1), 0);
        QCOMPARE(sectorAt(59, 3), 0);
        QCOMPARE(sectorAt(61, 3), 1);
        QCOMPARE(sectorAt(181, 3), 2);
    }

    void zones()
    {
        const Radii r;
        QCOMPARE(hitTest({0, 0}, 8, r).zone, Zone::Dead);
        QCOMPARE(hitTest({0, -53}, 8, r).zone, Zone::Dead); // the gap's inner half
        const Hit left = hitTest({-80, 0}, 8, r);
        QCOMPARE(left.zone, Zone::Inner);
        QCOMPARE(left.half, Mode::Move);
        QCOMPARE(hitTest({80, 5}, 8, r).half, Mode::Copy);
        QCOMPARE(hitTest({0, -115}, 8, r).zone, Zone::Inner); // the gap's inner half
        const Hit top = hitTest({0, -170}, 8, r);
        QCOMPARE(top.zone, Zone::Outer);
        QCOMPARE(top.sector, 0);
        QCOMPARE(hitTest({170, 0}, 8, r).sector, 2);
        QCOMPARE(hitTest({0, -236}, 8, r).zone, Zone::Outer); // within the leave margin
        QCOMPARE(hitTest({0, -240}, 8, r).zone, Zone::Outside);
        QCOMPARE(hitTest({0, -170}, 0, r).zone, Zone::Outside); // no sectors: no outer ring
        // Scaled for 宽松 (14 px labels).
        const Radii big = r.scaled(scaleForStep(2));
        QCOMPARE(hitTest({0, -240}, 8, big).zone, Zone::Outer);
    }

    void latch()
    {
        Latch l;
        QCOMPARE(l.effective(false, false, true, true), Mode::None);
        QCOMPARE(l.effective(false, false, true, true, true), Mode::Move); // a drop falls back to what is allowed
        l.update({Zone::Inner, Mode::Copy}, true, true);
        QCOMPARE(l.latched(), Mode::Copy);
        l.update({Zone::Outer, Mode::None, 3}, true, true); // along the outer ring: kept
        l.update({Zone::Dead}, true, true);
        QCOMPARE(l.latched(), Mode::Copy);
        l.update({Zone::Inner, Mode::Move}, true, true);
        QCOMPARE(l.latched(), Mode::Move);
        // Keys override while held; both held is neither.
        QCOMPARE(l.effective(true, false, true, true), Mode::Copy);
        QCOMPARE(l.effective(false, true, true, true), Mode::Move);
        QCOMPARE(l.effective(true, true, true, true), Mode::Move);
        // A half the source does not allow is passed over.
        Latch copyOnly;
        copyOnly.update({Zone::Inner, Mode::Move}, false, true);
        QCOMPARE(copyOnly.latched(), Mode::None);
        QCOMPARE(copyOnly.effective(false, false, false, true, true), Mode::Copy);
        QCOMPARE(copyOnly.effective(false, true, false, true, true), Mode::Copy); // Shift cannot move
    }

    void placement()
    {
        const QRectF area(0, 0, 1920, 1040);
        const double ring = 228;
        // Room all round: on the pointer.
        QCOMPARE(placeCentre({900, 500}, area, ring, 106), QPointF(900, 500));
        // Near the right edge: moved in just enough.
        QCOMPARE(placeCentre({1750, 500}, area, ring, 106), QPointF(1920 - ring, 500));
        // In a corner: as far as allowed, never beyond the inner ring.
        const QPointF corner = placeCentre({10, 10}, area, ring, 106);
        QVERIFY(std::hypot(corner.x() - 10, corner.y() - 10) <= 106 + 1e-9);
        QVERIFY(corner.x() > 10 && corner.y() > 10);
        // A work area that does not hold the ring: its middle, if near enough.
        QCOMPARE(placeCentre({200, 200}, QRectF(0, 0, 400, 400), ring, 106), QPointF(200, 200));
    }

    void window()
    {
        const QRect monitor(0, 0, 1920, 1080);
        const QRect rect = windowRect({900.5, 500.5}, 228, monitor);
        QVERIFY(rect.contains(QPoint(900 - 228, 500 - 228)));
        QVERIFY(rect.contains(QPoint(900 + 228, 500 + 228)));
        // On a second monitor to the right, near its left edge: never onto the first.
        const QRect second(1920, 0, 1280, 1024);
        const QRect onSecond = windowRect({1920 + 240, 400}, 228, second);
        QVERIFY(second.contains(onSecond));
        QCOMPARE(onSecond.left(), 1920);
    }

    void labelWidths()
    {
        const Radii r;
        QCOMPARE(labelWidth(8, r, 96), 96.0);
        QCOMPARE(labelWidth(1, r, 96), 96.0);
        QVERIFY(labelWidth(16, r, 96) < 96);
    }
};

QTEST_APPLESS_MAIN(TestPieGeometry)
#include "tst_piegeometry.moc"
