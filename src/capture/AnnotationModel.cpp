#include "AnnotationModel.h"

#include "Geometry.h"

#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QRandomGenerator>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

using annot::Kind;

// What [ and ] and a notch of the wheel step through, logical px (3.5): the
// next of these above or below the value now, whatever the panel's presets.
constexpr std::array kWidthSteps {1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40};
constexpr std::array kSizeSteps {10, 12, 14, 16, 18, 20, 24, 28, 32, 40, 48, 64};
constexpr std::array kCellSteps {4, 6, 8, 10, 12, 16, 18, 24, 32};
constexpr int kMaxLogical = 500; // as StyleMemory keeps them
constexpr int kMaxNumber = 99999;
// A touchpad sends the wheel in small pieces: a notch is 120 of them, and
// one notch in 200 ms at most, or a flick runs through every size (3.5).
constexpr int kNotch = 120;
constexpr qint64 kNotchMs = 200;

bool usesWidth(Kind kind)
{
    return kind == Kind::Rect || kind == Kind::Ellipse || kind == Kind::Arrow || kind == Kind::Line || kind == Kind::Pen
        || kind == Kind::Marker;
}

bool usesFill(Kind kind)
{
    return kind == Kind::Rect || kind == Kind::Ellipse;
}

bool usesSize(Kind kind)
{
    return kind == Kind::Text || kind == Kind::Number;
}

template <std::size_t N>
int stepped(const std::array<int, N>& steps, int value, int direction)
{
    if (direction > 0) {
        for (int step : steps) {
            if (step > value)
                return step;
        }
        return value;
    }
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        if (*it < value)
            return *it;
    }
    return value;
}

QVariantList variantList(const QList<int>& values)
{
    QVariantList list;
    for (int v : values)
        list.append(v);
    return list;
}

// TextEdit's plain text, as layoutText() splits it: '\n' between lines.
QString plainText(QString text)
{
    text.replace(QChar::ParagraphSeparator, u'\n');
    text.replace(QChar::LineSeparator, u'\n');
    text.remove(u'\r');
    return text;
}

QPoint clampInto(QPoint p, const QRect& room)
{
    if (room.isEmpty())
        return p;
    return QPoint(std::clamp(p.x(), room.left(), room.right()), std::clamp(p.y(), room.top(), room.bottom()));
}

int logicalValue(const QVariant& value)
{
    const double v = value.toDouble();
    if (!std::isfinite(v))
        return 1;
    return static_cast<int>(std::lround(std::clamp(v, 1.0, double(kMaxLogical))));
}

// Applies the panel's `change` (logical px) to `style`, whose sizes are px
// at `scale`: the document's scale for an object, 1 for a tool's memory,
// which StyleMemory keeps in logical px. Only the fields `kind` uses are
// taken, so an arrow never gets a fill it does not draw. True when any was.
bool applyPanel(const QVariantMap& change, Kind kind, annot::Style& style, qreal scale)
{
    bool any = false;
    for (auto it = change.cbegin(); it != change.cend(); ++it) {
        const QString& key = it.key();
        const QVariant& value = it.value();
        if (key == u"color") {
            if (const std::optional<QRgb> color = annot::parseColor(value.toString())) {
                style.color = *color;
                any = true;
            }
        } else if (key == u"width" && usesWidth(kind)) {
            style.width = annot::toImagePx(logicalValue(value), scale);
            any = true;
        } else if (key == u"fill" && usesFill(kind)) {
            if (const std::optional<annot::Fill> fill = annot::fillFromName(value.toString())) {
                style.fill = *fill;
                any = true;
            }
        } else if (key == u"size" && usesSize(kind)) {
            style.fontPx = annot::toImagePx(logicalValue(value), scale);
            any = true;
        } else if (key == u"bold" && kind == Kind::Text) {
            style.bold = value.toBool();
            any = true;
        } else if (key == u"boxed" && kind == Kind::Text) {
            style.boxed = value.toBool();
            any = true;
        } else if (key == u"mode" && kind == Kind::Redact) {
            if (const std::optional<annot::RedactMode> mode = annot::redactModeFromName(value.toString())) {
                style.redact = *mode;
                any = true;
            }
        } else if (key == u"cell" && kind == Kind::Redact) {
            style.cell = annot::toImagePx(logicalValue(value), scale);
            any = true;
        }
    }
    return any;
}

// The resize cursor of a box handle (BoxHandle's order), or of a line's end
// by the line's direction.
int handleCursor(const annot::Item& item, int handle)
{
    if (annot::isLine(item.kind) && item.points.size() >= 2) {
        const QPointF v = item.points[1] - item.points[0];
        const qreal angle = std::fmod(std::atan2(v.y(), v.x()) * 180 / std::numbers::pi + 180, 180);
        if (angle < 22.5 || angle >= 157.5)
            return Qt::SizeHorCursor;
        if (angle < 67.5)
            return Qt::SizeFDiagCursor; // y grows downwards: down and right is "\"
        if (angle < 112.5)
            return Qt::SizeVerCursor;
        return Qt::SizeBDiagCursor;
    }
    switch (annot::BoxHandle(handle)) {
    case annot::BoxHandle::TopLeft:
    case annot::BoxHandle::BottomRight:
        return Qt::SizeFDiagCursor;
    case annot::BoxHandle::TopRight:
    case annot::BoxHandle::BottomLeft:
        return Qt::SizeBDiagCursor;
    case annot::BoxHandle::Top:
    case annot::BoxHandle::Bottom:
        return Qt::SizeVerCursor;
    case annot::BoxHandle::Left:
    case annot::BoxHandle::Right:
        return Qt::SizeHorCursor;
    }
    return Qt::SizeAllCursor;
}

// The pixel a handle stands for: the box's pixel at that corner or edge, a
// line's end. Reshaping takes it plus the mouse's travel, so grabbing a
// handle a pixel off its edge does not make the box jump by that pixel.
QPoint handlePixel(const annot::Item& item, int handle)
{
    if (annot::isLine(item.kind))
        return annot::pixelAt(item.points.value(handle));
    const QRect box = annot::itemBox(item);
    const auto h = annot::BoxHandle(handle);
    const bool left = h == annot::BoxHandle::TopLeft || h == annot::BoxHandle::Left || h == annot::BoxHandle::BottomLeft;
    const bool top = h == annot::BoxHandle::TopLeft || h == annot::BoxHandle::Top || h == annot::BoxHandle::TopRight;
    return QPoint(left ? box.x() : box.x() + std::max(0, box.width() - 1),
        top ? box.y() : box.y() + std::max(0, box.height() - 1));
}

// Shift's square from `anchor` (the press) towards `to`, as large as fits
// in `room`: the side is cut where the room ends, rather than letting the
// square leave the selection.
QPoint fitSquare(QPoint anchor, QPoint to, const QRect& room)
{
    const int dx = to.x() - anchor.x();
    const int dy = to.y() - anchor.y();
    const int sx = dx >= 0 ? 1 : -1; // as reshaped() turns a square that has not moved
    const int sy = dy >= 0 ? 1 : -1;
    int side = std::max(std::abs(dx), std::abs(dy));
    if (!room.isEmpty()) {
        side = std::min(side, sx > 0 ? room.right() - anchor.x() : anchor.x() - room.left());
        side = std::min(side, sy > 0 ? room.bottom() - anchor.y() : anchor.y() - room.top());
    }
    side = std::max(0, side);
    return anchor + QPoint(sx * side, sy * side);
}

// Shift's line turned to a 15 degree step around its first point may end
// outside `room`, where clamping the end would bend it: it is shortened
// along its direction instead, until it ends on a pixel inside. `start` is
// the item reshaped() made `made` from.
annot::Item fitLine(const annot::Item& start, annot::Item made, const QRect& room)
{
    if (room.isEmpty() || made.points.size() < 2 || room.contains(annot::pixelAt(made.points.last())))
        return made;
    const QPointF a = made.points.first();
    const QPointF v = made.points.last() - a;
    const qreal length = std::hypot(v.x(), v.y());
    if (length <= 0)
        return made;
    // The largest share of the line whose end is a pixel centre inside.
    const qreal left = room.left() + 0.5;
    const qreal right = room.right() + 0.5;
    const qreal top = room.top() + 0.5;
    const qreal bottom = room.bottom() + 0.5;
    qreal t = 1;
    if (v.x() > 0)
        t = std::min(t, (right - a.x()) / v.x());
    else if (v.x() < 0)
        t = std::min(t, (left - a.x()) / v.x());
    if (v.y() > 0)
        t = std::min(t, (bottom - a.y()) / v.y());
    else if (v.y() < 0)
        t = std::min(t, (top - a.y()) / v.y());
    t = std::max<qreal>(0, t);
    // Turning it to the step again rounds to whole pixels, which can reach
    // a pixel past the edge: a pixel shorter each time.
    for (int i = 0; i < 8; ++i) {
        const annot::Item fitted = annot::reshaped(start, int(annot::LineHandle::End), a + v * t, true);
        if (fitted.points.size() >= 2 && room.contains(annot::pixelAt(fitted.points.last())))
            return fitted;
        t = std::max<qreal>(0, t - 1 / length);
    }
    made.points.last() = annot::pixelCentre(QPointF(clampInto(annot::pixelAt(made.points.last()), room)));
    return made;
}

// What a stroke growing by one point changes: the centre line is the same
// up to the midpoint before its last point (strokeCentreline), so only the
// last two points, the new one and the width around them are drawn again,
// not the whole stroke at every move. None when more may have changed: a
// stroke still level or upright is drawn half a pixel off its points until
// it bends (alignedPoints), and Shift makes it a line again.
std::optional<QRectF> strokeGrowth(const annot::Item& before, const annot::Item& after)
{
    const qsizetype n = before.points.size();
    if (n < 3 || after.points.size() != n + 1 || after.style != before.style)
        return std::nullopt;
    if (!std::equal(before.points.cbegin(), before.points.cend(), after.points.cbegin()))
        return std::nullopt;
    bool level = true;
    bool upright = true;
    for (const QPointF& p : before.points) {
        level = level && p.y() == before.points[0].y();
        upright = upright && p.x() == before.points[0].x();
    }
    if (level || upright)
        return std::nullopt;
    const qreal x0 = std::min({before.points[n - 2].x(), before.points[n - 1].x(), after.points[n].x()});
    const qreal x1 = std::max({before.points[n - 2].x(), before.points[n - 1].x(), after.points[n].x()});
    const qreal y0 = std::min({before.points[n - 2].y(), before.points[n - 1].y(), after.points[n].y()});
    const qreal y1 = std::max({before.points[n - 2].y(), before.points[n - 1].y(), after.points[n].y()});
    // Round caps and joins reach half the width past the points (bounds()),
    // and a pixel more for the antialiased edge.
    const qreal m = std::max(1, before.style.width) / 2.0 + 2;
    return QRectF(QPointF(x0, y0), QPointF(x1, y1)).adjusted(-m, -m, m, m);
}

} // namespace

struct AnnotationModel::Private {
    QElapsedTimer clock;

    // The gesture under way (3.7): its command, made at the press, reverted,
    // changed and applied again at every move; in the document only while
    // `applied`.
    std::unique_ptr<annot::Command> command;
    bool applied = false;
    bool dragging = false; // moved past the drag start since the press
    QPoint pressAt;
    QPoint last;
    Qt::KeyboardModifiers modifiers;
    annot::Item before; // Draw: the item as the press made it; Reshape: the item before
    int handle = -1; // Reshape
    QPoint own; // Reshape: the pixel the handle stands for (handlePixel)

    int wheelSum = 0;
    bool notched = false;
    qint64 lastNotch = 0;

    // What the change signals were last sent for: sync() sends each one when
    // its value is no longer this.
    struct Shown {
        int tool = -1;
        bool canUndo = false;
        bool canRedo = false;
        QString undoLabel;
        QString redoLabel;
        int itemCount = 0;
        bool panelVisible = false;
        QVariantMap panel;
        QVariantList selection;
        QVariantMap editing;
        QVariantMap numberEdit;
        bool gestureActive = false;
        QRectF gestureBounds;
    } shown;
};

AnnotationModel::AnnotationModel(QObject* parent)
    : QObject(parent)
    , m_compositor(std::make_unique<annot::Compositor>())
    , d(std::make_unique<Private>())
{
    d->clock.start();
    d->shown.panel = panel();
}

AnnotationModel::~AnnotationModel() = default;

// ---- the capture's side ----------------------------------------------------

void AnnotationModel::begin(const QImage& base, qreal dpr, const annot::StyleMemory& styles)
{
    reset();
    m_base = base;
    m_document.canvas = base.size();
    m_document.dpr = dpr > 0 ? dpr : 1.0;
    m_styles = styles;
    m_stylesChanged = false;
    emit changed(QRectF()); // a new picture: all of it
    sync();
}

void AnnotationModel::end()
{
    reset();
    m_base = QImage();
    emit changed(QRectF()); // the canvas lets go of its tiles
    sync();
}

void AnnotationModel::reset()
{
    d->command.reset();
    d->applied = false;
    d->dragging = false;
    d->wheelSum = 0;
    d->notched = false;
    m_gesture = Gesture::None;
    m_document = annot::Document {};
    m_stack.clear();
    m_compositor->clear();
    m_tool = -1;
    m_selected.reset();
    m_editing.reset();
    m_numberEdit.reset();
}

void AnnotationModel::setSelection(const QRect& selection)
{
    if (selection == m_document.crop)
        return;
    m_document.crop = selection;
    // Redactions take their colours from inside the selection (3.10), so
    // every one is made again; nothing else is drawn from it.
    annot::Change change;
    change.redactions = true;
    for (const QRect& box : annot::redactionBoxes(m_document))
        change.dirty |= QRectF(box);
    touched(change);
    sync();
}

QRect AnnotationModel::sampleBounds() const
{
    return m_document.crop.isEmpty() ? QRect(QPoint(0, 0), m_document.canvas) : m_document.crop;
}

std::optional<quint32> AnnotationModel::skipped() const
{
    if (m_editing && m_editing->id != 0)
        return m_editing->id;
    return std::nullopt;
}

QImage AnnotationModel::composed()
{
    commitPending();
    if (m_base.isNull() || m_document.items.isEmpty())
        return {};
    return m_compositor->exportImage(m_document, m_base, sampleBounds());
}

annot::Project AnnotationModel::project() const
{
    const annot::Document doc = committed();
    if (m_base.isNull() || doc.items.isEmpty())
        return {};
    return annot::projectFromOverlay(doc, m_base, doc.crop);
}

// The document as committing the text under way would leave it, without
// committing it (project() is const).
annot::Document AnnotationModel::committed() const
{
    annot::Document doc = m_document;
    if (m_editing) {
        if (const std::unique_ptr<annot::Command> command = textCommand(doc, m_editing->text))
            command->apply(doc);
    }
    return doc;
}

// ---- state for QML ---------------------------------------------------------

void AnnotationModel::setTool(int tool)
{
    if (tool < -1 || tool >= int(annot::tools().size()) || tool == m_tool)
        return;
    if (m_gesture != Gesture::None)
        cancelGesture();
    commitPending();
    m_numberEdit.reset();
    // An object of another kind is let go of: the panel then shows the new
    // tool's style, which is what the next drawing gets.
    if (tool >= 0 && m_selected) {
        const annot::Item* item = find(m_selected);
        if (!item || item->kind != annot::tools()[tool].kind)
            m_selected.reset();
    }
    m_tool = tool;
    d->wheelSum = 0;
    sync();
}

QVariantList AnnotationModel::toolList() const
{
    QVariantList list;
    for (const annot::ToolInfo& t : annot::tools()) {
        list.append(QVariantMap {{u"kind"_s, int(t.kind)}, {u"name"_s, annot::toolName(t.kind)},
            {u"letter"_s, QString(t.letter)}, {u"digit"_s, t.digit}, {u"glyph"_s, t.glyph}});
    }
    return list;
}

QString AnnotationModel::fontFamily() const
{
    return annot::annotationFont(18, false).family();
}

bool AnnotationModel::canUndo() const
{
    return m_stack.canUndo();
}

bool AnnotationModel::canRedo() const
{
    return m_stack.canRedo();
}

QString AnnotationModel::undoLabel() const
{
    return m_stack.undoLabel();
}

QString AnnotationModel::redoLabel() const
{
    return m_stack.redoLabel();
}

bool AnnotationModel::panelVisible() const
{
    return active() && (m_tool >= 0 || m_selected.has_value() || m_editing.has_value());
}

QVariantMap AnnotationModel::panel() const
{
    QVariantMap m;
    QVariantList colors;
    for (QRgb c : annot::palette())
        colors.append(colorHex(c));
    m[u"colors"_s] = colors;
    const std::optional<Target> target = styleTarget();
    if (!target) {
        m[u"kind"_s] = -1;
        m[u"selected"_s] = false;
        m[u"canRaise"_s] = false;
        m[u"canLower"_s] = false;
        return m;
    }
    const Kind kind = target->kind;
    // An object's sizes through the document's scale; a tool's as it
    // remembers them, which are logical already.
    annot::StyleMemory::Tool t = m_styles.tool(kind);
    if (target->id || target->typing) {
        const annot::Style s = targetStyle(*target);
        const qreal dpr = this->dpr();
        t.color = s.color;
        t.width = annot::toLogicalPx(s.width, dpr);
        t.fill = s.fill;
        t.size = annot::toLogicalPx(s.fontPx, dpr);
        t.bold = s.bold;
        t.boxed = s.boxed;
        t.mode = s.redact;
        t.cell = annot::toLogicalPx(s.cell, dpr);
    }
    m[u"kind"_s] = int(kind);
    m[u"name"_s] = annot::toolName(kind);
    m[u"color"_s] = colorHex(t.color);
    if (usesWidth(kind)) {
        m[u"width"_s] = t.width;
        m[u"widths"_s] = variantList(annot::sizePresets(kind));
    }
    if (usesFill(kind))
        m[u"fill"_s] = annot::fillName(t.fill);
    if (usesSize(kind)) {
        m[u"size"_s] = t.size;
        m[u"sizes"_s] = variantList(annot::sizePresets(kind));
    }
    if (kind == Kind::Text) {
        m[u"bold"_s] = t.bold;
        m[u"boxed"_s] = t.boxed;
    }
    if (kind == Kind::Redact) {
        m[u"mode"_s] = annot::redactModeName(t.mode);
        m[u"cell"_s] = t.cell;
        m[u"cells"_s] = variantList(annot::sizePresets(kind));
    }
    if (kind == Kind::Number) {
        int start = 1;
        for (const annot::Series& s : m_document.series) {
            if (s.id == m_document.currentSeries)
                start = s.start;
        }
        m[u"start"_s] = start;
    }
    m[u"selected"_s] = target->id.has_value();
    const bool layered = target->id && !m_editing;
    m[u"canRaise"_s] = layered && canReorder(1);
    m[u"canLower"_s] = layered && canReorder(-1);
    return m;
}

QVariantList AnnotationModel::selection() const
{
    if (m_editing)
        return {}; // the text box stands for it
    const annot::Item* item = find(m_selected);
    if (!item)
        return {};
    const QRectF outline = annot::bounds(m_document, *item);
    const QList<QPointF> points = annot::handles(*item);
    QVariantList handles;
    for (qsizetype i = 0; i < points.size(); ++i)
        handles.append(QVariantMap {{u"x"_s, points[i].x()}, {u"y"_s, points[i].y()}, {u"index"_s, int(i)}});
    return {QVariantMap {{u"id"_s, int(item->id)}, {u"kind"_s, int(item->kind)}, {u"x"_s, outline.x()},
        {u"y"_s, outline.y()}, {u"width"_s, outline.width()}, {u"height"_s, outline.height()},
        {u"box"_s, points.size() == 8}, {u"handles"_s, handles}}};
}

QVariantMap AnnotationModel::editing() const
{
    if (!m_editing)
        return {};
    const annot::Style style = editingStyle();
    const int f = std::max(1, style.fontPx);
    // Ascent and pitch are the font's, whatever the words.
    const annot::TextMetrics metrics = annot::textMetrics(QString(), f, style.bold);
    const int pad = style.boxed ? qRound(0.3 * f) : 0; // as textGeometry() pads the box
    const qreal radius = style.boxed ? 0.25 * f : 0;
    return QVariantMap {{u"id"_s, int(m_editing->id)}, {u"x"_s, m_editing->at.x()}, {u"y"_s, m_editing->at.y()},
        {u"fontPx"_s, f}, {u"bold"_s, style.bold}, {u"boxed"_s, style.boxed},
        {u"color"_s, colorHex(style.boxed ? annot::inkOn(style.color) : style.color)},
        {u"fill"_s, colorHex(style.color)}, {u"text"_s, m_editing->text}, {u"pad"_s, pad}, {u"radius"_s, radius},
        {u"pitch"_s, metrics.pitch}, {u"ascent"_s, metrics.ascent}, {u"clickX"_s, m_editing->click.x()},
        {u"clickY"_s, m_editing->click.y()}};
}

QVariantMap AnnotationModel::numberEdit() const
{
    const annot::Item* item = find(m_numberEdit);
    if (!item || item->kind != Kind::Number || item->points.isEmpty())
        return {};
    return QVariantMap {{u"id"_s, int(item->id)}, {u"x"_s, item->points[0].x()}, {u"y"_s, item->points[0].y()},
        {u"value"_s, item->pinned ? *item->pinned : -1},
        {u"shown"_s, annot::numbers(m_document).value(item->id, 1)}};
}

QRectF AnnotationModel::gestureBounds() const
{
    if (m_gesture == Gesture::None || !d->command)
        return {};
    QRectF r;
    if (m_gesture == Gesture::Draw) {
        // Not in the document until it moved: measured on the command's copy.
        const auto* add = static_cast<const annot::AddItems*>(d->command.get());
        if (!add->items.isEmpty()) {
            const annot::Item& item = add->items[0];
            r = d->applied ? annot::bounds(m_document, item) : annot::bounds(item, dpr());
        }
    } else if (const annot::Item* item = find(d->before.id)) {
        r = annot::bounds(m_document, *item);
    }
    if (r.isEmpty())
        r = QRectF(QPointF(d->last), QSizeF(1, 1));
    const qreal m = 16 * dpr();
    return r.adjusted(-m, -m, m, m);
}

// ---- helpers ---------------------------------------------------------------

qint64 AnnotationModel::now() const
{
    return d->clock.elapsed();
}

qreal AnnotationModel::tolerance() const
{
    return 4 * dpr(); // 3.2: lines count within half their width and 4 logical px
}

int AnnotationModel::reach() const
{
    return static_cast<int>(std::lround(6 * dpr())); // as CaptureSession reaches the selection's handles
}

int AnnotationModel::dragStart() const
{
    return std::max(2, static_cast<int>(std::lround(3 * dpr()))); // as CaptureSession starts a drag
}

QPoint AnnotationModel::clampToSelection(QPoint p) const
{
    return clampInto(p, m_document.crop);
}

std::optional<Kind> AnnotationModel::toolKind() const
{
    if (m_tool < 0 || m_tool >= int(annot::tools().size()))
        return std::nullopt;
    return annot::tools()[m_tool].kind;
}

const annot::Item* AnnotationModel::find(std::optional<quint32> id) const
{
    return id ? annot::findItem(m_document, *id) : nullptr;
}

std::optional<quint32> AnnotationModel::itemAt(QPoint p) const
{
    return annot::hitTest(m_document, QPointF(p), tolerance());
}

// The selected object under `p`, even where others lie over it (3.6, 4.1):
// tested alone, a Number keeping the number it shows (its disc's width).
bool AnnotationModel::onSelected(QPoint p) const
{
    const annot::Item* item = find(m_selected);
    if (!item || m_editing)
        return false;
    annot::Document probe;
    probe.canvas = m_document.canvas;
    probe.dpr = m_document.dpr;
    probe.items = {*item};
    if (item->kind == Kind::Number)
        probe.items[0].pinned = annot::numbers(m_document).value(item->id, 1);
    return annot::hitTest(probe, QPointF(p), tolerance()) == item->id;
}

int AnnotationModel::handleAt(QPoint p) const
{
    const annot::Item* item = find(m_selected);
    if (!item || m_editing)
        return -1;
    const QList<QPointF> points = annot::handles(*item);
    const QPointF at = annot::pixelCentre(QPointF(p));
    const qreal reach = this->reach();
    int best = -1;
    qreal nearest = 0;
    for (qsizetype i = 0; i < points.size(); ++i) {
        const qreal distance = std::hypot(points[i].x() - at.x(), points[i].y() - at.y());
        if (distance <= reach && (best < 0 || distance < nearest)) {
            best = int(i);
            nearest = distance;
        }
    }
    return best;
}

bool AnnotationModel::onHandle(QPoint p) const
{
    return active() && m_gesture == Gesture::None && handleAt(p) >= 0;
}

std::optional<AnnotationModel::Target> AnnotationModel::styleTarget() const
{
    if (!active())
        return std::nullopt;
    if (m_editing) {
        if (m_editing->id != 0 && find(m_editing->id))
            return Target {Kind::Text, m_editing->id, false};
        return Target {Kind::Text, std::nullopt, true};
    }
    if (const annot::Item* item = find(m_selected))
        return Target {item->kind, item->id, false};
    if (const std::optional<Kind> kind = toolKind())
        return Target {*kind, std::nullopt, false};
    return std::nullopt;
}

annot::Style AnnotationModel::targetStyle(const Target& target) const
{
    if (const annot::Item* item = find(target.id))
        return item->style;
    if (target.typing && m_editing)
        return m_editing->style;
    return m_styles.forTool(target.kind, dpr());
}

annot::Style AnnotationModel::editingStyle() const
{
    if (!m_editing)
        return {};
    if (const annot::Item* item = find(m_editing->id ? std::optional<quint32>(m_editing->id) : std::nullopt))
        return item->style;
    return m_editing->style;
}

// The box of the text being typed, its padding included.
QRect AnnotationModel::editingBox() const
{
    if (!m_editing)
        return {};
    annot::Item probe;
    probe.kind = Kind::Text;
    probe.points = {QPointF(m_editing->at)};
    probe.style = editingStyle();
    probe.text = m_editing->text;
    QRect box = annot::textGeometry(probe).box;
    // A new text has no width yet; the caret's place still counts as inside
    // (a double click's second press lands there).
    box.setWidth(std::max(box.width(), std::max(1, probe.style.fontPx / 2)));
    box.setHeight(std::max(box.height(), 1));
    return box;
}

// The step that committing `text` is (3.8): a new text with words is added,
// a new one without is nothing; an old one without words is removed, a
// changed one retyped, an unchanged one nothing.
std::unique_ptr<annot::Command> AnnotationModel::textCommand(const annot::Document& doc, const QString& text) const
{
    if (!m_editing)
        return nullptr;
    const bool words = !text.trimmed().isEmpty();
    if (m_editing->id == 0) {
        if (!words)
            return nullptr;
        annot::Item item;
        item.id = doc.nextId;
        item.seq = doc.nextSeq;
        item.kind = Kind::Text;
        item.points = {QPointF(m_editing->at)};
        item.style = m_editing->style;
        item.text = text;
        return std::make_unique<annot::AddItems>(QList<annot::Item> {item});
    }
    const annot::Item* item = annot::findItem(doc, m_editing->id);
    if (!item)
        return nullptr;
    if (!words)
        return std::make_unique<annot::RemoveItems>(QList<quint32> {item->id});
    if (text == item->text)
        return nullptr;
    return std::make_unique<annot::Retext>(doc, item->id, text);
}

void AnnotationModel::push(std::unique_ptr<annot::Command> command)
{
    touched(m_stack.push(std::move(command), m_document, now()));
}

// Every change the document went through: the compositor drops what it
// kept of the items, the canvas draws the tiles again. An empty region is
// nothing to draw (the canvas reads an empty one as all of it).
void AnnotationModel::touched(const annot::Change& change)
{
    m_compositor->invalidate(change);
    for (const QRectF& area : change.areas()) {
        if (!area.isEmpty())
            emit changed(area);
    }
}

// After undo and redo: what was selected or renumbered may be gone.
void AnnotationModel::keepValid()
{
    if (m_selected && !find(m_selected))
        m_selected.reset();
    if (m_numberEdit && !find(m_numberEdit))
        m_numberEdit.reset();
}

void AnnotationModel::sync()
{
    Private::Shown& s = d->shown;
    if (m_tool != s.tool) {
        s.tool = m_tool;
        emit toolChanged();
    }
    const bool undo = canUndo();
    const bool redo = canRedo();
    const QString undoText = undoLabel();
    const QString redoText = redoLabel();
    if (undo != s.canUndo || redo != s.canRedo || undoText != s.undoLabel || redoText != s.redoLabel) {
        s.canUndo = undo;
        s.canRedo = redo;
        s.undoLabel = undoText;
        s.redoLabel = redoText;
        emit historyChanged();
    }
    if (itemCount() != s.itemCount) {
        s.itemCount = itemCount();
        emit itemsChanged();
    }
    const bool visible = panelVisible();
    QVariantMap panelNow = panel();
    if (visible != s.panelVisible || panelNow != s.panel) {
        s.panelVisible = visible;
        s.panel = std::move(panelNow);
        emit panelChanged();
    }
    QVariantList selectionNow = selection();
    if (selectionNow != s.selection) {
        s.selection = std::move(selectionNow);
        emit selectionChanged();
    }
    QVariantMap editingNow = editing();
    if (editingNow != s.editing) {
        s.editing = std::move(editingNow);
        emit editingChanged();
    }
    QVariantMap numberNow = numberEdit();
    if (numberNow != s.numberEdit) {
        s.numberEdit = std::move(numberNow);
        emit numberEditChanged();
    }
    const bool gesture = gestureActive();
    const QRectF gestureRect = gestureBounds();
    if (gesture != s.gestureActive || gestureRect != s.gestureBounds) {
        s.gestureActive = gesture;
        s.gestureBounds = gestureRect;
        emit gestureChanged();
    }
}

// ---- the mouse -------------------------------------------------------------

bool AnnotationModel::pressFirst(QPoint p, Qt::KeyboardModifiers modifiers)
{
    if (!active())
        return false;
    if (m_gesture != Gesture::None)
        finishGesture(); // a release that never came
    m_numberEdit.reset();
    if (m_editing) {
        // The text box takes the presses inside it; one that reaches here
        // only commits the text, and does nothing else (3.6, step 1). One on
        // the box's padding counts as inside.
        if (!editingBox().contains(p))
            commitPending();
        sync();
        return true;
    }
    const int handle = handleAt(p);
    if (handle >= 0) {
        startReshape(handle, p, modifiers);
        sync();
        return true;
    }
    sync();
    return false;
}

bool AnnotationModel::pressSecond(QPoint p, Qt::KeyboardModifiers modifiers)
{
    if (!active())
        return false;
    if (m_gesture != Gesture::None)
        finishGesture();
    m_numberEdit.reset();
    if (m_editing) { // pressFirst() takes these; for a caller that skipped it
        commitPending();
        sync();
        return true;
    }
    if (const std::optional<Kind> kind = toolKind()) {
        // 3.6, step 4: the selected object moves, a text is edited, a number
        // picked up; anything else inside the selection is drawn on.
        if (onSelected(p)) {
            startMove(*m_selected, p, modifiers);
            sync();
            return true;
        }
        // A text under the text tool, a number under the number tool: picked
        // up, so a drag moves it; a click on the text opens it for typing
        // (at the release, finishGesture()).
        const annot::Item* under = find(itemAt(p));
        if (under && under->kind == *kind && (*kind == Kind::Text || *kind == Kind::Number)) {
            m_selected = under->id;
            startMove(under->id, p, modifiers);
            sync();
            return true;
        }
        m_selected.reset();
        if (!m_document.crop.contains(p)) {
            emit notice(tr("只能在选区里画"));
            sync();
            return true;
        }
        if (*kind == Kind::Text)
            beginNewText(p);
        else
            startDraw(*kind, p, modifiers);
        sync();
        return true;
    }
    // 3.6, step 5: without a tool an object is picked up; a press elsewhere
    // lets the selected one go and leaves the press to the session.
    if (const std::optional<quint32> hit = itemAt(p)) {
        m_selected = *hit;
        startMove(*hit, p, modifiers);
        sync();
        return true;
    }
    if (m_selected) {
        m_selected.reset();
        sync();
    }
    return false;
}

void AnnotationModel::startDraw(Kind kind, QPoint p, Qt::KeyboardModifiers modifiers)
{
    const QPoint at = clampToSelection(p);
    annot::Item item;
    item.id = m_document.nextId;
    item.seq = m_document.nextSeq;
    item.kind = kind;
    item.style = m_styles.forTool(kind, dpr());
    switch (kind) {
    case Kind::Rect:
    case Kind::Ellipse:
    case Kind::Redact:
        item.points = annot::boxPoints(annot::pixelBox(at, at));
        if (kind == Kind::Redact)
            item.seed = QRandomGenerator::global()->generate64();
        break;
    case Kind::Arrow:
    case Kind::Line:
        item.points = {annot::pixelCentre(QPointF(at)), annot::pixelCentre(QPointF(at))};
        break;
    case Kind::Pen:
    case Kind::Marker:
        item.points = {annot::pixelCentre(QPointF(at))};
        break;
    case Kind::Number:
        item.points = {annot::pixelCentre(QPointF(at))};
        item.series = m_document.currentSeries;
        break;
    case Kind::Text:
        return; // beginNewText()
    }
    d->before = item;
    d->command = std::make_unique<annot::AddItems>(QList<annot::Item> {item});
    d->applied = false;
    d->dragging = false;
    d->pressAt = at;
    d->last = at;
    d->modifiers = modifiers;
    m_gesture = Gesture::Draw;
    if (kind == Kind::Number) {
        // One click puts a number down (3.9); a drag then carries it.
        touched(d->command->apply(m_document));
        d->applied = true;
    }
}

void AnnotationModel::startMove(quint32 id, QPoint p, Qt::KeyboardModifiers modifiers)
{
    const annot::Item* item = find(id);
    if (!item)
        return;
    auto move = std::make_unique<annot::MoveItems>(QList<quint32> {id}, QPoint());
    move->merge = false; // a drag is a step of its own
    d->before = *item;
    d->command = std::move(move);
    d->applied = true; // moved by nothing: the document is as it would be
    d->dragging = false;
    d->pressAt = p;
    d->last = p;
    d->modifiers = modifiers;
    m_gesture = Gesture::Move;
}

void AnnotationModel::startReshape(int handle, QPoint p, Qt::KeyboardModifiers modifiers)
{
    const annot::Item* item = find(m_selected);
    if (!item)
        return;
    auto reshape = std::make_unique<annot::Reshape>(item->id, *item, *item);
    reshape->merge = false;
    d->before = *item;
    d->handle = handle;
    d->own = handlePixel(*item, handle);
    d->command = std::move(reshape);
    d->applied = true; // reshaped to itself
    d->dragging = false;
    d->pressAt = p;
    d->last = p;
    d->modifiers = modifiers;
    m_gesture = Gesture::Reshape;
}

annot::Item AnnotationModel::shapeTo(QPoint p, bool constrain) const
{
    const QRect room = m_document.crop;
    const QPoint to = clampToSelection(p);
    const annot::Item& start = d->before;
    if (annot::isBox(start.kind)) {
        // The press's pixel and the one under the mouse, both inside (3.4):
        // the box as reshaped() spans it from its 1 x 1 start.
        const QPoint target = constrain ? fitSquare(d->pressAt, to, room) : to;
        return annot::reshaped(start, int(annot::BoxHandle::BottomRight), QPointF(target), constrain);
    }
    if (annot::isLine(start.kind)) {
        const annot::Item made = annot::reshaped(start, int(annot::LineHandle::End), QPointF(to), constrain);
        return constrain ? fitLine(start, made, room) : made;
    }
    annot::Item made = start; // a Number goes where the mouse is
    made.points = {annot::pixelCentre(QPointF(to))};
    return made;
}

annot::Item AnnotationModel::strokeTo(const annot::Item& stroke, QPoint p, bool constrain) const
{
    const QPoint to = clampToSelection(p);
    const annot::Item made = annot::reshaped(stroke, int(annot::LineHandle::End), QPointF(to), constrain);
    return constrain ? fitLine(stroke, made, m_document.crop) : made;
}

void AnnotationModel::move(QPoint p, Qt::KeyboardModifiers modifiers)
{
    if (m_gesture == Gesture::None || !d->command)
        return;
    d->last = p;
    d->modifiers = modifiers;
    if (!d->dragging && (p - d->pressAt).manhattanLength() >= dragStart())
        d->dragging = true;
    updateGesture();
    sync();
}

// The gesture's command brought to where the mouse is: reverted, changed
// and applied again, the old place and the new one drawn again (3.7).
void AnnotationModel::updateGesture()
{
    if (!d->command)
        return;
    const bool constrain = d->modifiers & Qt::ShiftModifier;
    switch (m_gesture) {
    case Gesture::Draw: {
        auto* add = static_cast<annot::AddItems*>(d->command.get());
        if (add->items.isEmpty())
            return;
        const annot::Item& current = add->items[0];
        // A stroke keeps every point from the press on; a shape is made
        // again from the press, once the mouse went past the drag start.
        annot::Item next = current;
        if (annot::isStroke(current.kind))
            next = strokeTo(current, d->last, constrain);
        else if (d->dragging)
            next = shapeTo(d->last, constrain);
        const bool show = d->dragging || current.kind == Kind::Number;
        if (next == current && show == d->applied)
            return;
        std::optional<QRectF> grown;
        if (d->applied && show && annot::isStroke(current.kind))
            grown = strokeGrowth(current, next);
        annot::Change change;
        if (d->applied)
            change = add->revert(m_document);
        add->items[0] = std::move(next);
        d->applied = show;
        if (show)
            change.merge(add->apply(m_document));
        if (grown) {
            change.dirty = *grown; // the stroke's recipe still goes (change.ids)
            change.parts.clear();
        }
        touched(change);
        break;
    }
    case Gesture::Move: {
        if (!d->dragging)
            return;
        auto* move = static_cast<annot::MoveItems*>(d->command.get());
        const QPoint delta = d->last - d->pressAt;
        if (delta == move->delta)
            return;
        annot::Change change = move->revert(m_document);
        move->delta = delta;
        change.merge(move->apply(m_document));
        touched(change);
        break;
    }
    case Gesture::Reshape: {
        if (!d->dragging)
            return;
        auto* reshape = static_cast<annot::Reshape*>(d->command.get());
        const QRect canvas(QPoint(0, 0), m_document.canvas);
        const QPoint to = clampInto(d->own + (d->last - d->pressAt), canvas);
        annot::Item after = annot::reshaped(d->before, d->handle, QPointF(to), constrain);
        if (after == reshape->after)
            return;
        annot::Change change = reshape->revert(m_document);
        reshape->after = std::move(after);
        change.merge(reshape->apply(m_document));
        touched(change);
        break;
    }
    case Gesture::None:
        break;
    }
}

void AnnotationModel::release(QPoint p)
{
    if (m_gesture == Gesture::None)
        return;
    if (p != d->last) {
        d->last = p;
        if (!d->dragging && (p - d->pressAt).manhattanLength() >= dragStart())
            d->dragging = true;
        updateGesture();
    }
    finishGesture();
    sync();
}

void AnnotationModel::finishGesture()
{
    std::unique_ptr<annot::Command> command = std::move(d->command);
    const Gesture gesture = std::exchange(m_gesture, Gesture::None);
    const bool applied = std::exchange(d->applied, false);
    // A shape spans from the press to the release, both inside the
    // selection: a drag along its edge, out of it, made nothing wider.
    const QPoint end = gesture == Gesture::Draw ? clampToSelection(d->last) : d->last;
    const bool shortDrag = (end - d->pressAt).manhattanLength() < dragStart();
    d->dragging = false;
    if (!command || !applied)
        return; // nothing was drawn: a click
    switch (gesture) {
    case Gesture::Draw: {
        auto* add = static_cast<annot::AddItems*>(command.get());
        if (add->items.isEmpty())
            return;
        annot::Item& item = add->items[0];
        // A shape is its press and its release: those closer than a drag
        // are a click, not a shape (a stroke shows its path instead).
        bool nothing = item.kind != Kind::Number && !annot::isStroke(item.kind) && shortDrag;
        if (annot::isLine(item.kind))
            nothing = nothing || item.points.value(0) == item.points.value(1);
        if (nothing) {
            touched(add->revert(m_document));
            return;
        }
        if (annot::isStroke(item.kind)) {
            const QList<QPointF> simple = annot::simplifyStroke(item.points);
            if (simple != item.points) {
                annot::Change change = add->revert(m_document);
                item.points = simple;
                change.merge(add->apply(m_document));
                touched(change);
            }
        }
        const quint32 id = item.id;
        m_stack.pushApplied(std::move(command), now());
        m_selected = id; // the tool stays; what it drew can be adjusted at once (3.6)
        break;
    }
    case Gesture::Move: {
        if (!static_cast<annot::MoveItems*>(command.get())->delta.isNull()) {
            m_stack.pushApplied(std::move(command), now());
            break;
        }
        // A click, not a drag: with the text tool on a text, it is typed in
        // (3.8), the caret where the press was.
        if (toolKind() == Kind::Text) {
            if (const annot::Item* item = find(d->before.id); item && item->kind == Kind::Text)
                beginEdit(*item, d->pressAt);
        }
        break;
    }
    case Gesture::Reshape: {
        const auto* reshape = static_cast<annot::Reshape*>(command.get());
        if (reshape->after == reshape->before)
            return;
        m_stack.pushApplied(std::move(command), now());
        break;
    }
    case Gesture::None:
        break;
    }
}

void AnnotationModel::cancelGesture()
{
    if (m_gesture == Gesture::None)
        return;
    std::unique_ptr<annot::Command> command = std::move(d->command);
    m_gesture = Gesture::None;
    if (command && d->applied)
        touched(command->revert(m_document));
    d->applied = false;
    d->dragging = false;
    keepValid();
    sync();
}

int AnnotationModel::cursorAt(QPoint p) const
{
    if (!active())
        return -1;
    switch (m_gesture) {
    case Gesture::Move:
        return Qt::SizeAllCursor;
    case Gesture::Reshape:
        return handleCursor(d->before, d->handle);
    case Gesture::Draw:
        return Qt::CrossCursor;
    case Gesture::None:
        break;
    }
    if (const int handle = handleAt(p); handle >= 0)
        return handleCursor(*find(m_selected), handle);
    if (const std::optional<Kind> kind = toolKind()) {
        if (onSelected(p))
            return Qt::SizeAllCursor;
        if (const annot::Item* under = find(itemAt(p))) {
            if (*kind == Kind::Text && under->kind == Kind::Text)
                return Qt::IBeamCursor;
            if (*kind == Kind::Number && under->kind == Kind::Number)
                return Qt::SizeAllCursor;
        }
        if (m_document.crop.contains(p))
            return *kind == Kind::Text ? Qt::IBeamCursor : Qt::CrossCursor;
        return -1;
    }
    return itemAt(p) ? Qt::SizeAllCursor : -1;
}

bool AnnotationModel::doubleClick(QPoint p)
{
    if (!active())
        return false;
    if (m_editing)
        return true; // the text box's own (a word selected)
    const annot::Item* item = find(itemAt(p));
    if (item && (item->kind == Kind::Text || item->kind == Kind::Number)) {
        // The second press began a gesture that the double click replaces.
        if (m_gesture != Gesture::None)
            cancelGesture();
        item = find(itemAt(p));
        if (!item)
            return true;
        if (item->kind == Kind::Text) {
            beginEdit(*item, p);
        } else {
            m_selected = item->id;
            m_numberEdit = item->id;
        }
        sync();
        return true;
    }
    // A double click copies only on the bare picture without a tool (3.6):
    // on an object, or with a tool on, it is the model's and does nothing.
    return item != nullptr || m_tool >= 0;
}

bool AnnotationModel::wheel(int angleDelta, qint64 nowMs)
{
    if (!styleTarget())
        return false;
    if (angleDelta == 0 || m_gesture != Gesture::None)
        return true;
    if (d->wheelSum != 0 && (angleDelta > 0) != (d->wheelSum > 0))
        d->wheelSum = 0; // turned the other way: start again
    d->wheelSum += angleDelta;
    if (std::abs(d->wheelSum) < kNotch)
        return true;
    const int direction = d->wheelSum > 0 ? 1 : -1;
    d->wheelSum = 0;
    if (d->notched && nowMs - d->lastNotch < kNotchMs)
        return true;
    d->notched = true;
    d->lastNotch = nowMs;
    stepSize(direction);
    return true;
}

// ---- keys ------------------------------------------------------------------

bool AnnotationModel::key(int key, Qt::KeyboardModifiers modifiers)
{
    if (!active() || m_editing)
        return false; // the text box has the keys
    const Qt::KeyboardModifiers mods = modifiers & ~Qt::KeypadModifier;
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const bool other = mods & (Qt::AltModifier | Qt::MetaModifier);
    if (m_gesture != Gesture::None && key == Qt::Key_Shift) {
        // Shift pressed while the mouse holds still: the square or the 15
        // degree step shows at once, not at the next move.
        d->modifiers |= Qt::ShiftModifier;
        updateGesture();
        sync();
        return false;
    }

    enum class Action { None, Tool, Undo, Redo, Delete, Nudge, Smaller, Bigger, Lower, Raise };
    Action action = Action::None;
    int toolIndex = -1;
    QPoint delta;
    bool toEnd = false;
    if (!ctrl && !other) {
        // A tool's letter or digit, alone or with Shift (3.5); never with
        // Ctrl (Ctrl + digit is PixPin's).
        for (qsizetype i = 0; i < annot::tools().size() && action == Action::None; ++i) {
            const annot::ToolInfo& t = annot::tools()[i];
            const bool digit = t.digit >= 1 && t.digit <= 9 && key == Qt::Key_0 + t.digit;
            if (key == int(t.letter.toUpper().unicode()) || digit) {
                action = Action::Tool;
                toolIndex = int(i);
            }
        }
        if (action == Action::None) {
            const int step = shift ? 10 : 1;
            switch (key) {
            case Qt::Key_Delete:
            case Qt::Key_Backspace:
                action = Action::Delete;
                break;
            case Qt::Key_Left:
                action = Action::Nudge;
                delta = QPoint(-step, 0);
                break;
            case Qt::Key_Right:
                action = Action::Nudge;
                delta = QPoint(step, 0);
                break;
            case Qt::Key_Up:
                action = Action::Nudge;
                delta = QPoint(0, -step);
                break;
            case Qt::Key_Down:
                action = Action::Nudge;
                delta = QPoint(0, step);
                break;
            case Qt::Key_BracketLeft:
                action = shift ? Action::None : Action::Smaller;
                break;
            case Qt::Key_BracketRight:
                action = shift ? Action::None : Action::Bigger;
                break;
            default:
                break;
            }
        }
    } else if (ctrl && !other) {
        switch (key) {
        case Qt::Key_Z:
            action = shift ? Action::Redo : Action::Undo;
            break;
        case Qt::Key_Y:
            action = shift ? Action::None : Action::Redo;
            break;
        // Ctrl+Shift+[ comes as { on most layouts.
        case Qt::Key_BracketLeft:
        case Qt::Key_BraceLeft:
            action = Action::Lower;
            toEnd = shift || key == Qt::Key_BraceLeft;
            break;
        case Qt::Key_BracketRight:
        case Qt::Key_BraceRight:
            action = Action::Raise;
            toEnd = shift || key == Qt::Key_BraceRight;
            break;
        default:
            break;
        }
    }
    if (action == Action::None)
        return false;
    if (m_gesture != Gesture::None)
        return true; // while the button is held these wait; they would tear the gesture

    switch (action) {
    case Action::Tool:
        setTool(m_tool == toolIndex ? -1 : toolIndex); // the key that opened a tool closes it
        return true;
    case Action::Undo:
        undo();
        return true;
    case Action::Redo:
        redo();
        return true;
    case Action::Delete:
        if (!m_selected)
            return false;
        deleteSelection();
        return true;
    case Action::Nudge:
        if (!m_selected)
            return false; // the session nudges the selection instead
        nudge(delta);
        return true;
    case Action::Smaller:
    case Action::Bigger:
        if (!styleTarget())
            return false;
        stepSize(action == Action::Bigger ? 1 : -1);
        return true;
    case Action::Lower:
    case Action::Raise:
        if (!m_selected)
            return false;
        reorder(action == Action::Raise ? 1 : -1, toEnd);
        return true;
    case Action::None:
        break;
    }
    return false;
}

void AnnotationModel::nudge(QPoint delta)
{
    if (!m_selected)
        return;
    auto move = std::make_unique<annot::MoveItems>(QList<quint32> {*m_selected}, delta);
    move->merge = true; // a run of presses is one step (3.7)
    push(std::move(move));
    sync();
}

bool AnnotationModel::escape()
{
    if (!active())
        return false;
    if (m_numberEdit) {
        m_numberEdit.reset();
        sync();
        return true;
    }
    if (m_editing) {
        commitPending();
        return true;
    }
    if (m_gesture != Gesture::None) {
        cancelGesture();
        return true;
    }
    return false;
}

void AnnotationModel::commitPending()
{
    if (m_editing)
        commitText(m_editing->text);
}

// ---- from QML --------------------------------------------------------------

void AnnotationModel::beginNewText(QPoint p)
{
    Editing editing;
    editing.at = clampToSelection(p);
    editing.style = m_styles.forTool(Kind::Text, dpr());
    m_editing = editing;
    m_selected.reset();
    m_numberEdit.reset();
}

void AnnotationModel::beginEdit(const annot::Item& item, QPoint p)
{
    Editing editing;
    editing.id = item.id;
    editing.at = annot::pixelAt(item.points.value(0));
    editing.text = item.text;
    editing.click = p - editing.at;
    editing.style = item.style;
    m_editing = editing;
    m_selected.reset();
    m_numberEdit.reset();
    emit changed(annot::bounds(m_document, item)); // the canvas leaves it out now
}

void AnnotationModel::setEditingText(const QString& text)
{
    if (!m_editing)
        return;
    m_editing->text = plainText(text);
    // The box shows these words already: editing.text follows them without
    // a signal, so the box is not set again under the caret.
    d->shown.editing[u"text"_s] = m_editing->text;
}

void AnnotationModel::commitText(const QString& typed)
{
    if (!m_editing)
        return;
    const QString text = plainText(typed);
    const quint32 old = m_editing->id;
    QRectF area;
    if (const annot::Item* item = find(old ? std::optional<quint32>(old) : std::nullopt))
        area = annot::bounds(m_document, *item); // drawn again: it was left out
    std::unique_ptr<annot::Command> command = textCommand(m_document, text);
    quint32 id = old;
    if (command && old == 0)
        id = static_cast<annot::AddItems*>(command.get())->items[0].id;
    m_editing.reset();
    if (command) {
        annot::Change change = m_stack.push(std::move(command), m_document, now());
        change.dirty |= area;
        touched(change);
    } else if (!area.isEmpty()) {
        emit changed(area);
    }
    m_selected.reset();
    if (id != 0 && find(id))
        m_selected = id;
    sync();
}

void AnnotationModel::setPanel(const QVariantMap& change)
{
    const std::optional<Target> target = styleTarget();
    if (!target || change.isEmpty())
        return;
    // The object (or the text being typed), in image px.
    if (const annot::Item* item = find(target->id)) {
        annot::Style style = item->style;
        if (applyPanel(change, item->kind, style, dpr()) && style != item->style)
            push(std::make_unique<annot::Restyle>(m_document, QList<quint32> {item->id}, QList<annot::Style> {style}));
    } else if (target->typing && m_editing) {
        applyPanel(change, Kind::Text, m_editing->style, dpr());
    }
    // A change made on the panel is a preference: the tool remembers it
    // (3.5), the fields changed only, in logical px (StyleMemory at scale
    // 1 keeps them exactly).
    annot::Style memory = m_styles.forTool(target->kind, 1.0);
    if (applyPanel(change, target->kind, memory, 1.0)) {
        const annot::StyleMemory before = m_styles;
        m_styles.remember(target->kind, memory, 1.0);
        if (!(m_styles == before))
            m_stylesChanged = true;
    }
    sync();
}

// [ and ], a notch of the wheel: the target's width, size or cell one step
// on, as a panel change, so an object and the tool's memory both take it.
void AnnotationModel::stepSize(int direction)
{
    const std::optional<Target> target = styleTarget();
    if (!target)
        return;
    const Kind kind = target->kind;
    const bool remembered = !target->id && !target->typing;
    const annot::StyleMemory::Tool& memory = m_styles.tool(kind);
    const annot::Style style = targetStyle(*target);
    const qreal dpr = this->dpr();
    QString field;
    int value = 0;
    int next = 0;
    if (usesWidth(kind)) {
        field = u"width"_s;
        value = remembered ? memory.width : annot::toLogicalPx(style.width, dpr);
        next = stepped(kWidthSteps, value, direction);
    } else if (usesSize(kind)) {
        field = u"size"_s;
        value = remembered ? memory.size : annot::toLogicalPx(style.fontPx, dpr);
        next = stepped(kSizeSteps, value, direction);
    } else if (kind == Kind::Redact) {
        field = u"cell"_s;
        value = remembered ? memory.cell : annot::toLogicalPx(style.cell, dpr);
        next = stepped(kCellSteps, value, direction);
    }
    if (field.isEmpty() || next == value)
        return;
    setPanel(QVariantMap {{field, next}});
}

void AnnotationModel::undo()
{
    if (!active())
        return;
    if (m_gesture != Gesture::None) {
        cancelGesture(); // mid-drag, a step back is that drag
        return;
    }
    commitPending();
    if (m_stack.canUndo()) {
        touched(m_stack.undo(m_document));
        keepValid();
    }
    sync();
}

void AnnotationModel::redo()
{
    if (!active())
        return;
    if (m_gesture != Gesture::None) {
        cancelGesture();
        return;
    }
    commitPending();
    if (m_stack.canRedo()) {
        touched(m_stack.redo(m_document));
        keepValid();
    }
    sync();
}

void AnnotationModel::deleteSelection()
{
    if (!active() || m_gesture != Gesture::None)
        return;
    commitPending();
    if (!m_selected) {
        sync();
        return;
    }
    const quint32 id = *std::exchange(m_selected, std::nullopt);
    if (m_numberEdit == id)
        m_numberEdit.reset();
    push(std::make_unique<annot::RemoveItems>(QList<quint32> {id}));
    sync();
}

void AnnotationModel::raise(bool toTop)
{
    reorder(1, toTop);
}

void AnnotationModel::lower(bool toBottom)
{
    reorder(-1, toBottom);
}

// One layer up or down, or to the end, inside the object's own segment
// (redactions stay below everything else, 3.2).
bool AnnotationModel::canReorder(int direction) const
{
    const annot::Item* item = find(m_selected);
    if (!item)
        return false;
    const int i = annot::indexOf(m_document, item->id);
    const int end = annot::redactionEnd(m_document);
    const int low = item->kind == Kind::Redact ? 0 : end;
    const int high = item->kind == Kind::Redact ? end : int(m_document.items.size());
    return direction > 0 ? i < high - 1 : i > low;
}

void AnnotationModel::reorder(int direction, bool toEnd)
{
    if (!active() || m_gesture != Gesture::None)
        return;
    commitPending();
    if (!canReorder(direction)) {
        sync();
        return;
    }
    const annot::Item* item = find(m_selected);
    const int i = annot::indexOf(m_document, item->id);
    // Reorder's index is in the list without the item: one past the next
    // one up, the one below, or the segment's end (insertionIndex keeps it
    // inside the segment).
    const int low = item->kind == Kind::Redact ? 0 : annot::redactionEnd(m_document);
    const int to = direction > 0 ? (toEnd ? -1 : i + 1) : (toEnd ? low : i - 1);
    push(std::make_unique<annot::Reorder>(item->id, to));
    sync();
}

void AnnotationModel::newSeries()
{
    if (!active())
        return;
    commitPending();
    push(annot::SetSeries::newSeries(m_document));
    sync();
}

void AnnotationModel::setStart(int start)
{
    if (!active())
        return;
    start = std::clamp(start, 0, kMaxNumber);
    for (const annot::Series& s : m_document.series) {
        if (s.id == m_document.currentSeries && s.start == start)
            return;
    }
    push(annot::SetSeries::setStart(m_document, m_document.currentSeries, start));
    sync();
}

void AnnotationModel::setNumber(int id, const QVariant& value)
{
    const annot::Item* item = find(id > 0 ? std::optional<quint32>(quint32(id)) : std::nullopt);
    if (!item || item->kind != Kind::Number)
        return;
    // Empty (or a negative number, as numberEdit's -1): counted again.
    std::optional<int> pinned;
    if (value.isValid() && !value.isNull()) {
        if (value.metaType().id() == QMetaType::QString) {
            const QString text = value.toString().trimmed();
            if (!text.isEmpty()) {
                bool ok = false;
                const int n = text.toInt(&ok);
                if (!ok)
                    return; // not a number: nothing changes
                pinned = n;
            }
        } else {
            bool ok = false;
            const double n = value.toDouble(&ok);
            if (!ok || !std::isfinite(n))
                return;
            pinned = static_cast<int>(std::lround(std::clamp(n, -1.0, double(kMaxNumber))));
        }
    }
    if (pinned && *pinned < 0)
        pinned.reset();
    if (pinned)
        pinned = std::min(*pinned, kMaxNumber);
    m_numberEdit.reset();
    if (pinned != item->pinned)
        push(std::make_unique<annot::Renumber>(m_document, item->id, pinned));
    sync();
}

void AnnotationModel::closeNumberEdit()
{
    m_numberEdit.reset();
    sync();
}

} // namespace ws::capture
