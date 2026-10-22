#pragma once

#include "AnnotationFormat.h"
#include "CaptureSession.h"
#include "Grabber.h"
#include "PinManager.h"
#include "WindowSnapshot.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

class QQmlEngine;
class QQuickWindow;
class QScreen;

namespace ws::capture {

// A picture written as PNG off the GUI thread (`thread` joined first: one at
// a time), into `path`, or with none into the Screenshots folder named for
// now (WinShun_yyyyMMdd_HHmmss.png); `done(path, error)` back on `context`'s
// thread. The capture's Ctrl+S and the pins' 保存 / 另存为… alike.
void saveImage(std::jthread& thread, QObject* context, const QImage& image, const QString& path,
    std::function<void(const QString& path, const QString& error)> done);

// The screen capture (docs/design-capture.md; docs/architecture.md, 截图):
// exists while it is turned on in the settings, and holds everything it
// needs warm meanwhile, so a hotkey shows the frozen screens in a few
// frames: the grabber's D3D device and capture items, and one overlay
// window per monitor (CaptureOverlay.qml), made and drawn once hidden. A
// capture freezes every monitor, lists the windows, shows the overlays and
// hands the picture taken to the app; then the pictures, textures and the
// window list are let go of at once (the windows stay, hidden). Destroying
// it gives back the rest.
class Capture : public QObject {
    Q_OBJECT

public:
    struct Callbacks {
        // Enter, a double click, the toolbar. `project`: the annotated
        // picture's editable form (empty: it has no annotations).
        std::function<void(const QImage&, const QByteArray& project)> copy;
        // Ctrl+S and F3 also put the picture in the screenshot history,
        // leaving the system clipboard alone.
        std::function<void(const QImage&, const QByteArray& project)> record;
        // What the annotation tools remember ([Capture] AnnotateStyles, JSON):
        // read when a capture starts, written when one ends with a change.
        std::function<QString()> annotateStyles;
        std::function<void(const QString&)> annotateStylesChanged;
        std::function<void(const QString&)> copyColor; // C: "#RRGGBB"
        std::function<void(const QString& path, const QString& error)> saved; // Ctrl+S
        std::function<void()> ended; // the overlays are gone and the memory let go of
        // 识别文字: the picture, the scale of the monitor it was on, where it
        // is on the screen (physical; the OCR window goes beside it).
        std::function<void(const QImage&, double dpr, const QRect& where)> ocr;
        // 识别文字并复制 / 搜索 / 网页搜索 (the mouse buttons' choices): read, then go
        // on at once with `then` (OcrCopy, OcrSearch or OcrWeb) without the window.
        std::function<void(const QImage&, double dpr, const QRect& where, CaptureSession::Action then)> ocrThen;
        // How the annotation reads the selection's text and what it hides
        // (SmartRedact.h): 智能打码 and its pre-scan.
        SmartHooks smart;
        // The pins (the app's: they outlive the capture's pictures, and a
        // file can be pinned with the capture off). Made when first asked for.
        std::function<PinManager*()> pins;
    };

    Capture(QQmlEngine* engine, Callbacks callbacks, QObject* parent = nullptr);
    ~Capture() override;

    void setCopyOnSelect(bool on) { m_session.setCopyOnSelect(on); }
    void setMouseActions(const CaptureSession::MouseActions& actions) { m_session.setMouseActions(actions); }
    void setHiddenButtons(const QStringList& ids) { m_session.setHiddenButtons(ids); }
    // The hotkey, PrtScn, the tray menu: a capture, or the end of the one
    // under way (what a key opened, the same key closes).
    void trigger();
    bool busy() const { return m_state != State::Idle; }

    // 贴上最近一次截图 (the tray menu).
    bool hasLast() const { return !m_last.isNull(); }
    void pinLast(); // the last capture taken, where it was
    // 找回刚放弃的标注: a capture ended with annotations on it (hotkey, Esc)
    // keeps them for kDraftMs or until the next capture; this pins their
    // picture where the capture was, with the project.
    bool hasDraft() const { return m_draft.has_value(); }
    void restoreDraft();
    static constexpr int kDraftMs = 10 * 60 * 1000;
    // Ctrl+S, 保存 on a pin: into the Screenshots folder (or `path`), off the
    // GUI thread; saved() tells how it went.
    void save(const QImage& image, const QString& path = {});

private:
    enum class State { Idle, Waiting, Showing, Closing };
    struct Overlay {
        QPointer<QScreen> screen;
        HMONITOR monitor = nullptr;
        CaptureScreen* model = nullptr;
        QPointer<QQuickWindow> window;
        bool cloaked = false; // shown cloaked until its first frame is drawn
        bool warming = false; // the first, hidden show that sets up its graphics
        int closingFrames = 0; // frames to draw without the picture before hiding
    };

    void buildOverlays();
    void destroyOverlays();
    void screensChanged(); // monitors came, went or changed: everything made again
    void start();
    void show();
    void frameDrawn(QQuickWindow* window);
    void finish(CaptureSession::Action action, int screen, const QRect& rect);
    void hideAll(); // once the frames without the pictures are drawn
    void findFlyout(); // the Start menu closed: where it was in the picture

    QQmlEngine* m_engine;
    Callbacks m_callbacks;
    Grabber m_grabber;
    CaptureSession m_session;
    std::vector<Overlay> m_overlays;
    std::vector<Grabber::Shot> m_shots;
    WindowSnapshot m_snapshot;
    State m_state = State::Idle;
    QElapsedTimer m_clock; // since the hotkey
    qint64 m_grabMs = 0;
    qint64 m_listMs = 0;
    qint64 m_flyoutMs = -1;
    QString m_grabReport;
    bool m_reported = false;
    HWND m_flyout = nullptr; // the Start menu or a shell flyout in front at the hotkey, closing
    HMONITOR m_flyoutMonitor = nullptr;
    QImage m_last; // the last capture taken (copied, saved, pinned, read), for 贴上最近一次截图
    QPoint m_lastOrigin; // where it was, physical
    QByteArray m_lastProject;
    struct Draft {
        annot::Project project; // the selection's pixels and the annotations over them
        QPoint origin; // physical, virtual desktop
    };
    std::optional<Draft> m_draft;
    QTimer m_draftTimer;
    // The time between frames drawn while an annotation gesture is under way,
    // logged at the end of the capture: how smooth drawing was (nanoseconds).
    QElapsedTimer m_frameClock;
    std::vector<qint64> m_gestureFrames;
    std::vector<qint64> m_allGestureFrames;
    bool m_gestureFrameOpen = false;
    QTimer m_flyoutWait;
    QTimer m_hideTimeout;
    QTimer m_rebuild;
    std::jthread m_saver; // last: joined first
};

} // namespace ws::capture
