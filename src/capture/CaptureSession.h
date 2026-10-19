#pragma once

#include "Geometry.h"

#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <vector>

namespace ws::capture {

// One monitor of a capture, as its overlay window (CaptureOverlay.qml)
// shows it: the frozen picture and what is drawn over it, all in the
// monitor's own physical pixels (0,0 is its top left). The window turns
// them into its logical units with scaleX and scaleY, Qt's real buffer
// scale (bufferScale), so every edge lands on a whole screen pixel.
class CaptureScreen : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the capture")

    Q_PROPERTY(int index READ index CONSTANT FINAL)
    Q_PROPERTY(QImage image READ image NOTIFY imageChanged FINAL)
    // QML cannot look into a QImage (image.width is undefined there): whether there is one.
    Q_PROPERTY(bool hasImage READ hasImage NOTIFY imageChanged FINAL)
    Q_PROPERTY(QSize pixels READ pixels NOTIFY geometryChanged FINAL)
    Q_PROPERTY(qreal scaleX READ scaleX NOTIFY geometryChanged FINAL) // logical units per physical pixel
    Q_PROPERTY(qreal scaleY READ scaleY NOTIFY geometryChanged FINAL)
    Q_PROPERTY(qreal dpr READ dpr NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QPoint origin READ origin NOTIFY geometryChanged FINAL) // its top left, physical, virtual desktop
    Q_PROPERTY(QRect hover READ hover NOTIFY changed FINAL) // what a click would pick; null when none here
    Q_PROPERTY(QRect selection READ selection NOTIFY changed FINAL) // null when none here
    Q_PROPERTY(bool dimmed READ dimmed NOTIFY changed FINAL) // a selection exists, here or on another monitor
    Q_PROPERTY(bool adjusting READ adjusting NOTIFY changed FINAL) // being drawn, moved or resized: no toolbar
    Q_PROPERTY(bool cursorHere READ cursorHere NOTIFY cursorChanged FINAL)
    Q_PROPERTY(QPoint cursor READ cursor NOTIFY cursorChanged FINAL)
    Q_PROPERTY(QImage loupe READ loupe NOTIFY cursorChanged FINAL) // the pixels around the cursor, kLoupeCells square
    Q_PROPERTY(QString color READ color NOTIFY cursorChanged FINAL) // "#RRGGBB" under the cursor
    Q_PROPERTY(int cursorShape READ cursorShape NOTIFY cursorChanged FINAL)
    Q_PROPERTY(QString flash READ flash NOTIFY flashChanged FINAL) // a word by the magnifier ("已复制 #…")

public:
    static constexpr int kLoupeCells = 15; // odd: the cursor's pixel in the middle

    CaptureScreen(int index, QObject* parent = nullptr);

    int index() const { return m_index; }
    QImage image() const { return m_image; }
    bool hasImage() const { return !m_image.isNull(); }
    QSize pixels() const { return m_rect.size(); }
    QRect rect() const { return m_rect; } // physical, virtual desktop
    QPoint origin() const { return m_rect.topLeft(); }
    QRect bounds() const { return QRect(QPoint(0, 0), m_rect.size()); } // physical, its own
    qreal dpr() const { return m_dpr; }
    qreal scaleX() const { return m_scaleX; }
    qreal scaleY() const { return m_scaleY; }
    QRect hover() const { return m_hover; }
    QRect selection() const { return m_selection; }
    bool dimmed() const { return m_dimmed; }
    bool adjusting() const { return m_adjusting; }
    bool cursorHere() const { return m_cursorHere; }
    QPoint cursor() const { return m_cursor; }
    QImage loupe() const { return m_loupe; }
    QString color() const { return m_color; }
    int cursorShape() const { return m_cursorShape; }
    QString flash() const { return m_flash; }

    // Where the monitor is (physical, virtual desktop), its DPR and its
    // window's logical size, which the scale follows.
    void setGeometry(const QRect& rect, qreal dpr, QSize logicalWindow);
    void setImage(const QImage& image);

    // Kept for QML's sake: the selection's toolbar, size label and the
    // magnifier, placed by Geometry.h from sizes QML measured (logical),
    // returned in logical units.
    Q_INVOKABLE QPointF placeToolbar(const QRect& selection, qreal width, qreal height) const;
    Q_INVOKABLE QPointF placeSizeLabel(const QRect& selection, qreal width, qreal height) const;
    Q_INVOKABLE QPointF placeMagnifier(const QPoint& cursor, qreal width, qreal height) const;

signals:
    void imageChanged();
    void geometryChanged();
    void changed();
    void cursorChanged();
    void flashChanged();

private:
    friend class CaptureSession;
    void setState(const QRect& hover, const QRect& selection, bool dimmed, bool adjusting);
    void setCursor(bool here, QPoint at, int shape);
    void setFlash(const QString& text);
    QSize toPhysical(qreal width, qreal height) const;
    QPointF toLogical(QPoint p) const;

    int m_index = 0;
    QImage m_image;
    QRect m_rect;
    qreal m_dpr = 1;
    qreal m_scaleX = 1;
    qreal m_scaleY = 1;
    QRect m_hover;
    QRect m_selection;
    bool m_dimmed = false;
    bool m_adjusting = false;
    bool m_cursorHere = false;
    QPoint m_cursor {-1, -1};
    QImage m_loupe;
    QString m_color;
    int m_cursorShape = Qt::CrossCursor;
    QString m_flash;
};

// A capture from the frozen screens to the picture taken (or none): what
// the mouse and keys do. Moving over the screens highlights the window or
// control a click picks (the wheel steps out to the window and the whole
// screen and back); a drag draws a selection; with one, its eight handles
// resize it and a drag inside moves it, the arrow keys move it a pixel
// (Shift: resize). Enter or a double click copies, Ctrl+S saves, F3 pins it
// to the screen; Esc or a right click drops the selection, or with none ends
// the capture; C copies the colour under the cursor. A selection stays on
// one monitor.
//
// Input comes in physical pixels of the monitor it is on (the overlays
// convert; the tests call these directly). No window is needed here.
class CaptureSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the capture")

    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged FINAL)
    // The OCR models are there: 识别文字 can read; else it says why not.
    Q_PROPERTY(bool ocrAvailable READ ocrAvailable NOTIFY ocrAvailableChanged FINAL)

public:
    // Pin: left on the screen where it was (PinWindow); Ocr: its words read (CaptureOcr).
    enum class Action { Copy, Save, Cancel, Pin, Ocr };
    Q_ENUM(Action)

    // The windows at the freeze: top-level ones from the top of the Z order
    // down, physical, virtual desktop; children(i) gives window i's controls.
    struct Windows {
        std::vector<QRect> tops;
        std::function<std::vector<QRect>(std::size_t)> children;
    };


    explicit CaptureSession(QObject* parent = nullptr);

    void begin(std::vector<CaptureScreen*> screens, Windows windows);
    // The Start menu (or a shell flyout) in the picture: over every window
    // listed, without controls (physical, virtual desktop). EnumWindows
    // does not list it (another band of the Z order); found from the
    // picture once it closed (changedRegion).
    void setFlyout(const QRect& rect);
    void end();
    bool ocrAvailable() const { return m_ocrAvailable; }
    void setOcrAvailable(bool on);
    bool active() const { return m_active; }
    bool hasSelection() const { return m_selection.screen >= 0; }
    int selectionScreen() const { return m_selection.screen; }
    QRect selectionRect() const { return m_selection.rect; } // physical, that monitor's own
    QRect hoverRect() const;
    void setCopyOnSelect(bool on) { m_copyOnSelect = on; } // a drag or a click on a window copies at once

    void pointerMove(int screen, QPoint p);
    void pointerPress(int screen, QPoint p, Qt::MouseButton button);
    void pointerRelease(int screen, QPoint p, Qt::MouseButton button);
    void doubleClick(int screen, QPoint p);
    void wheel(int screen, QPoint p, int steps); // steps > 0: out (bigger)
    bool key(int key, Qt::KeyboardModifiers modifiers);

    // From QML, in the window's logical units (Qt's mouse positions).
    Q_INVOKABLE void mouseMoved(int screen, qreal x, qreal y);
    Q_INVOKABLE void mousePressed(int screen, qreal x, qreal y, int button);
    Q_INVOKABLE void mouseReleased(int screen, qreal x, qreal y, int button);
    Q_INVOKABLE void mouseDoubleClicked(int screen, qreal x, qreal y);
    Q_INVOKABLE void wheeled(int screen, qreal x, qreal y, int angleDeltaY);
    Q_INVOKABLE bool keyPressed(int key, int modifiers);
    Q_INVOKABLE void act(int action); // the toolbar's buttons: an Action

signals:
    // What to take: that monitor's pixels in `rect` (its own physical
    // coordinates); nothing for Cancel.
    void finished(ws::capture::CaptureSession::Action action, int screen, const QRect& rect);
    void colorCopied(const QString& color);
    void cursorMoveRequested(const QPoint& global); // the arrow keys with no selection: a pixel at a time
    void selectionChanged();
    void activeChanged();
    void ocrAvailableChanged();

private:
    enum class Phase { Hover, Pressed, Dragging, Selected, Adjusting };
    struct Pick {
        int screen = -1;
        QRect rect;
    };

    CaptureScreen* screenAt(int index) const;
    QPoint clampTo(int screen, QPoint p) const;
    void updateHover(int screen, QPoint p);
    void updateCursor(int screen, QPoint p);
    void publish(); // the state onto the screens
    void finish(Action action);
    void back(); // Esc, right click
    Pick hovered() const; // what a click would pick now: the hover, else the screen under the cursor
    int reach(int screen) const; // how near a handle a press takes it, physical
    int dragStart(int screen) const; // how far a press moves before it is a drag

    std::vector<CaptureScreen*> m_screens;
    Windows m_windows;
    QRect m_flyout; // over all of m_windows.tops
    bool m_active = false;
    bool m_ocrAvailable = false;
    bool m_copyOnSelect = false;
    Phase m_phase = Phase::Hover;
    Pick m_selection;
    Pick m_previous; // kept while a press outside it may still be a click
    int m_pressScreen = -1;
    QPoint m_pressAt;
    Handle m_handle = Handle::None;
    QRect m_startRect;
    int m_cursorScreen = -1;
    QPoint m_cursorAt;
    // The hover: its monitor, the picks under the cursor smallest first
    // (hitChain), and how far out the wheel went.
    int m_hoverScreen = -1;
    std::vector<QRect> m_chain;
    std::size_t m_level = 0;
};

} // namespace ws::capture
