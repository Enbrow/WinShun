#pragma once

#include "Annotation.h"
#include "AnnotationCommands.h"
#include "AnnotationFormat.h"
#include "AnnotationRender.h"
#include "AnnotationStyles.h"

#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace ws::capture {

// The annotations of one capture, as the overlay shows and edits them
// (docs/design-capture-roadmap.md, 3.2, 3.5-3.9): the document, its undo
// stack, the tool, the selected object, the text being typed, the style
// panel. CaptureSession owns it and routes the mouse and keys to it (3.6);
// the canvas (AnnotationCanvas) draws it; AnnotateLayer.qml and the toolbar
// show its state and call back into the Q_INVOKABLE members.
//
// Every position here is in image pixels: the pixels of the frozen screen,
// which are the physical pixels of that monitor. The overlay's own unit
// (logical) never comes in. Logical sizes (a width of "4") appear only in
// the panel map, as the user thinks of them; the document keeps image
// pixels (3.4).
class AnnotationModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the capture")

    // The tool's index in annot::tools() (the toolbar's order); -1: none.
    Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged FINAL)
    // [{kind, name (translated), letter, digit, glyph}] in the toolbar's order.
    Q_PROPERTY(QVariantList tools READ toolList CONSTANT FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged FINAL)
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY historyChanged FINAL)
    Q_PROPERTY(QString redoLabel READ redoLabel NOTIFY historyChanged FINAL)
    Q_PROPERTY(bool hasItems READ hasItems NOTIFY itemsChanged FINAL)
    Q_PROPERTY(int itemCount READ itemCount NOTIFY itemsChanged FINAL)
    // The style panel is shown: a tool is on or an object is selected.
    Q_PROPERTY(bool panelVisible READ panelVisible NOTIFY panelChanged FINAL)
    // What the panel shows, logical units (see panel()).
    Q_PROPERTY(QVariantMap panel READ panel NOTIFY panelChanged FINAL)
    // The selected object (none or one): its outline and handles, to draw
    // over the canvas (see selection()).
    Q_PROPERTY(QVariantList selection READ selection NOTIFY selectionChanged FINAL)
    // The text being typed; empty map when none (see editing()).
    Q_PROPERTY(QVariantMap editing READ editing NOTIFY editingChanged FINAL)
    // A Number being renumbered by double click: {id, x, y (its centre,
    // image px), value (-1: counted), shown (what it shows now)}; empty when none.
    Q_PROPERTY(QVariantMap numberEdit READ numberEdit NOTIFY numberEditChanged FINAL)
    // A drawing, moving or reshaping gesture is under way.
    // The family the compositor draws text in (annotationFont); the text box uses the same.
    Q_PROPERTY(QString fontFamily READ fontFamily CONSTANT FINAL)
    Q_PROPERTY(bool gestureActive READ gestureActive NOTIFY gestureChanged FINAL)
    // What the gesture is changing (image px, set 16 logical px wider): the
    // toolbar steps aside when it reaches it (3.5). Null when no gesture.
    Q_PROPERTY(QRectF gestureBounds READ gestureBounds NOTIFY gestureChanged FINAL)

public:
    explicit AnnotationModel(QObject* parent = nullptr);
    ~AnnotationModel() override;

    // A new capture on one monitor's frozen picture: an empty document the
    // size of `base`, no tool, nothing selected, an empty undo stack; every
    // tool's style as `styles` remembers it. `dpr` is the monitor's scale.
    void begin(const QImage& base, qreal dpr, const annot::StyleMemory& styles);
    // Everything let go of (the picture, the caches, the stack).
    void end();
    bool active() const { return !m_base.isNull(); }
    // The selection rectangle (image px): the document's crop and the range
    // redactions take their colours from. Moving it redraws the redactions.
    void setSelection(const QRect& selection);
    QRect selectionRect() const { return m_document.crop; }
    // What redactions take their colours from (3.10, S): the selection in
    // the overlay (all of the picture before there is one). The canvas and
    // composed() render with it.
    QRect sampleBounds() const;

    // ---- what the canvas and the capture read ----
    const annot::Document& document() const { return m_document; }
    const QImage& base() const { return m_base; }
    qreal dpr() const { return m_document.dpr; }
    annot::Compositor& compositor() { return *m_compositor; }
    // The text being edited is not drawn by the canvas (the TextEdit shows it).
    std::optional<quint32> skipped() const;
    // The styles as they are now (the tools' remembered ones, including this
    // capture's changes); `stylesChanged()` tells whether any changed.
    const annot::StyleMemory& styles() const { return m_styles; }
    bool stylesChanged() const { return m_stylesChanged; }
    // The composed picture of the selection (annot::Compositor::exportImage)
    // and the project that makes it editable again; both commit a text under
    // way first. Null / empty when there are no items.
    QImage composed();
    annot::Project project() const;

    int tool() const { return m_tool; }
    void setTool(int tool); // index into annot::tools(); -1: none; ends a text under way
    QVariantList toolList() const;
    QString fontFamily() const;
    bool canUndo() const;
    bool canRedo() const;
    QString undoLabel() const;
    QString redoLabel() const;
    bool hasItems() const { return !m_document.items.isEmpty(); }
    int itemCount() const { return static_cast<int>(m_document.items.size()); }
    bool panelVisible() const;
    // Keys, all in logical units except `colors` (RGB strings):
    //   kind        annot::Kind of the tool or selected object, -1 for none
    //   name        its translated name
    //   color       "#RRGGBB" (for a redaction: the solid colour)
    //   colors      the palette
    //   width, widths   line width and its presets (Rect, Ellipse, Arrow, Line, Pen, Marker)
    //   fill        "none" | "soft" | "solid" (Rect, Ellipse)
    //   size, sizes     font size and presets (Text, Number)
    //   bold, boxed     Text
    //   mode        "mosaic" | "blur" | "solid" (Redact)
    //   cell, cells     mosaic cell and presets (Redact)
    //   start       the current series' first number (Number)
    //   selected    an object is selected (the panel shows its style, and
    //               a change goes to it as well as to the tool's memory)
    //   canRaise, canLower   the selected object can move up / down a layer
    QVariantMap panel() const;
    // [] or [{id, kind, x, y, width, height (the outline: its bounds, image
    // px), box (true: drawn with eight handles; false: a line's two end
    // handles), handles: [{x, y, index}] (image px; index is the value
    // reshaped() takes)}].
    QVariantList selection() const;
    // {id (0: a new text), x, y (the text's top left, image px), fontPx,
    // bold, boxed, color ("#RRGGBB" of the text), fill ("#RRGGBB" of the box
    // when boxed), text, pad and radius (the box's padding and corner radius
    // round the text, image px; 0 when not boxed), pitch (baseline to
    // baseline) and ascent (top to the first baseline), image px, as the
    // compositor lays the text out, clickX, clickY (where the press landed
    // inside it, relative to x, y; -1 when it began with a new text)}.
    QVariantMap editing() const;
    QVariantMap numberEdit() const;
    bool gestureActive() const { return m_gesture != Gesture::None; }
    QRectF gestureBounds() const;

    // ---- the mouse, from CaptureSession (3.6); positions are image px ----
    // The first questions of a press, before the selection's own handles:
    // commit a text when the press is outside it; a handle of the selected
    // object. Returns true when the press is taken.
    bool pressFirst(QPoint p, Qt::KeyboardModifiers modifiers);
    // The rest, after the selection's handles: with a tool, the selected
    // object's body (move), a text or number under it (edit / select), or
    // a new object inside the selection (outside it: taken, nothing done,
    // notice()); without one, an object under the press (select, drag to
    // move). A text under the text tool is picked up like a number: a drag
    // moves it, a click (released without a drag) opens it for typing, the
    // caret where the press was. Returns true when taken; a press that does
    // nothing (outside the selection with a tool) is taken too, but
    // gestureActive() stays false.
    bool pressSecond(QPoint p, Qt::KeyboardModifiers modifiers);
    void move(QPoint p, Qt::KeyboardModifiers modifiers);
    // Ends the gesture: the object is kept (one undo step) or, when nothing
    // was drawn (a click that did not move), dropped.
    void release(QPoint p);
    // The cursor over `p` when the model has a say (Qt::CursorShape): the
    // selected object's handles, a movable object, the tool's cursor inside
    // the selection. -1 when the session's own cursors apply.
    int cursorAt(QPoint p) const;
    // `p` is on a handle of the selected object: pressFirst() takes it, so
    // its cursor comes before the selection's own handles (3.6, step 2
    // before 3); everything else cursorAt() says comes after them.
    bool onHandle(QPoint p) const;
    // A double click: a Text under it opens for editing, a Number for its
    // number. True as well on any other object and whenever a tool is on
    // (3.6: those never copy); false only on the bare picture without a
    // tool, where the session copies.
    bool doubleClick(QPoint p);
    // Wheel with a tool or a selected object: size steps; `angleDelta` is
    // the wheel's, summed until 120 makes a notch, one notch per 200 ms at
    // most (touchpads). True when the wheel is the model's (tool or selection).
    bool wheel(int angleDelta, qint64 nowMs);
    // The tool keys, undo and redo, Delete, arrows on a selected object,
    // [ and ], Ctrl+[ and Ctrl+]. True when taken.
    bool key(int key, Qt::KeyboardModifiers modifiers);
    // Esc, steps 2 and 3 of 3.6: commit the text under way, or cancel the
    // gesture under way; before either, close a number's edit box. True
    // when it did one of them.
    bool escape();
    bool editingText() const { return m_editing.has_value(); }
    bool editingNumber() const { return m_numberEdit.has_value(); }
    void cancelGesture();
    // Commits a text under way (for Ctrl+S / F3 / Enter, and the hotkey).
    void commitPending();

    // ---- from QML ----
    // The text box's end: the text typed. A new text with words becomes an
    // object, a new empty one is dropped, an old one with no words is
    // deleted, a changed one is retyped; the result is selected.
    Q_INVOKABLE void commitText(const QString& text);
    // The text box's words as they are now (TextEdit.onTextChanged): what
    // commitPending(), escape() and a press outside the box commit, since
    // those come from the session, not from the box. No signal: the box
    // has the words already (editing.text follows them).
    Q_INVOKABLE void setEditingText(const QString& text);
    // A change on the panel: any of color ("#RRGGBB"), width, fill, size,
    // bold, boxed, mode, cell (logical units). Goes to the selected object
    // when there is one, and is remembered as the tool's style either way.
    Q_INVOKABLE void setPanel(const QVariantMap& change);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void raise(bool toTop);
    Q_INVOKABLE void lower(bool toBottom);
    // "另起一组": the next numbers start a new group counting from 1.
    Q_INVOKABLE void newSeries();
    // "起始值": where the current group's numbers start.
    Q_INVOKABLE void setStart(int start);
    // 双击序号: a number set by hand (an empty value: counted again).
    Q_INVOKABLE void setNumber(int id, const QVariant& value);
    Q_INVOKABLE void closeNumberEdit();

signals:
    // Image px: what the canvas redraws. An empty rectangle asks for all.
    void changed(const QRectF& dirty);
    void toolChanged();
    void historyChanged();
    void itemsChanged();
    void panelChanged();
    void selectionChanged();
    void editingChanged();
    void numberEditChanged();
    void gestureChanged();
    // A word for the user (the overlay shows it by the selection).
    void notice(const QString& text);

private:
    enum class Gesture { None, Draw, Move, Reshape };
    // What a style change goes to (3.5): the text being typed, else the
    // selected object, else the tool.
    struct Target {
        annot::Kind kind = annot::Kind::Rect;
        std::optional<quint32> id; // an object in the document
        bool typing = false; // a new text, not in the document yet
    };
    struct Editing {
        quint32 id = 0; // 0: a new text, in the document only once committed
        QPoint at; // the text's top left
        QString text; // the words as typed so far
        QPoint click {-1, -1};
        annot::Style style; // a new text's (an old one keeps its own in the document)
    };
    // The gesture's command and where it began, the wheel's sum, the clock
    // the undo merges go by, what the change signals were last sent for.
    struct Private;

    qint64 now() const;
    qreal tolerance() const;
    int reach() const;
    int dragStart() const;
    QPoint clampToSelection(QPoint p) const;
    std::optional<annot::Kind> toolKind() const;
    const annot::Item* find(std::optional<quint32> id) const;
    std::optional<quint32> itemAt(QPoint p) const;
    bool onSelected(QPoint p) const;
    int handleAt(QPoint p) const;
    std::optional<Target> styleTarget() const;
    annot::Style targetStyle(const Target& target) const;
    annot::Style editingStyle() const;
    QRect editingBox() const;
    annot::Document committed() const;
    std::unique_ptr<annot::Command> textCommand(const annot::Document& doc, const QString& text) const;
    void startDraw(annot::Kind kind, QPoint p, Qt::KeyboardModifiers modifiers);
    void startMove(quint32 id, QPoint p, Qt::KeyboardModifiers modifiers);
    void startReshape(int handle, QPoint p, Qt::KeyboardModifiers modifiers);
    void updateGesture();
    void finishGesture();
    annot::Item shapeTo(QPoint p, bool constrain) const;
    annot::Item strokeTo(const annot::Item& stroke, QPoint p, bool constrain) const;
    void beginNewText(QPoint p);
    void beginEdit(const annot::Item& item, QPoint p);
    void stepSize(int direction);
    void nudge(QPoint delta);
    void reorder(int direction, bool toEnd);
    bool canReorder(int direction) const;
    void push(std::unique_ptr<annot::Command> command);
    void touched(const annot::Change& change);
    void keepValid();
    void reset();
    void sync();

    annot::Document m_document;
    QImage m_base;
    annot::StyleMemory m_styles;
    bool m_stylesChanged = false;
    annot::CommandStack m_stack;
    std::unique_ptr<annot::Compositor> m_compositor;
    int m_tool = -1;
    Gesture m_gesture = Gesture::None;
    std::optional<quint32> m_selected;
    std::optional<Editing> m_editing;
    std::optional<quint32> m_numberEdit;
    std::unique_ptr<Private> d;
};

} // namespace ws::capture
