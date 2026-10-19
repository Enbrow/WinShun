#include "CaptureSession.h"

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws::capture {

// ---------------------------------------------------------------------------
// CaptureScreen

CaptureScreen::CaptureScreen(int index, QObject* parent)
    : QObject(parent)
    , m_index(index)
{
}

void CaptureScreen::setGeometry(const QRect& rect, qreal dpr, QSize logicalWindow)
{
    const qreal scaleX = 1.0 / bufferScale(logicalWindow.width(), dpr);
    const qreal scaleY = 1.0 / bufferScale(logicalWindow.height(), dpr);
    if (rect == m_rect && dpr == m_dpr && scaleX == m_scaleX && scaleY == m_scaleY)
        return;
    m_rect = rect;
    m_dpr = dpr;
    m_scaleX = scaleX;
    m_scaleY = scaleY;
    emit geometryChanged();
}

void CaptureScreen::setImage(const QImage& image)
{
    m_image = image;
    if (image.isNull()) {
        m_loupe = {};
        m_color.clear();
    }
    emit imageChanged();
}

void CaptureScreen::setState(const QRect& hover, const QRect& selection, bool dimmed, bool adjusting)
{
    if (hover == m_hover && selection == m_selection && dimmed == m_dimmed && adjusting == m_adjusting)
        return;
    m_hover = hover;
    m_selection = selection;
    m_dimmed = dimmed;
    m_adjusting = adjusting;
    emit changed();
}

void CaptureScreen::setCursor(bool here, QPoint at, int shape)
{
    if (here == m_cursorHere && at == m_cursor && shape == m_cursorShape)
        return;
    const bool moved = here != m_cursorHere || at != m_cursor;
    m_cursorHere = here;
    m_cursor = at;
    m_cursorShape = shape;
    if (moved && here && !m_image.isNull()) {
        // The pixels around the cursor, those off the picture black; the
        // magnifier scales them up by a whole number, unsmoothed.
        constexpr int half = kLoupeCells / 2;
        QImage loupe(kLoupeCells, kLoupeCells, QImage::Format_RGB32);
        loupe.fill(Qt::black);
        for (int y = 0; y < kLoupeCells; ++y) {
            const int sy = at.y() - half + y;
            if (sy < 0 || sy >= m_image.height())
                continue;
            const auto* in = reinterpret_cast<const QRgb*>(m_image.constScanLine(sy));
            auto* out = reinterpret_cast<QRgb*>(loupe.scanLine(y));
            for (int x = 0; x < kLoupeCells; ++x) {
                const int sx = at.x() - half + x;
                if (sx >= 0 && sx < m_image.width())
                    out[x] = in[sx] | 0xFF000000u;
            }
        }
        m_loupe = loupe;
        m_color = at.x() >= 0 && at.y() >= 0 && at.x() < m_image.width() && at.y() < m_image.height()
            ? colorHex(m_image.pixel(at))
            : QString();
    }
    emit cursorChanged();
}

void CaptureScreen::setFlash(const QString& text)
{
    if (text == m_flash)
        return;
    m_flash = text;
    emit flashChanged();
}

QSize CaptureScreen::toPhysical(qreal width, qreal height) const
{
    return QSize(static_cast<int>(std::ceil(width / m_scaleX - 1e-6)), static_cast<int>(std::ceil(height / m_scaleY - 1e-6)));
}

QPointF CaptureScreen::toLogical(QPoint p) const
{
    return QPointF(p.x() * m_scaleX, p.y() * m_scaleY);
}

QPointF CaptureScreen::placeToolbar(const QRect& selection, qreal width, qreal height) const
{
    const int gap = static_cast<int>(std::lround(8 * m_dpr));
    return toLogical(capture::placeToolbar(selection, toPhysical(width, height), bounds(), gap).topLeft);
}

QPointF CaptureScreen::placeSizeLabel(const QRect& selection, qreal width, qreal height) const
{
    const int gap = static_cast<int>(std::lround(6 * m_dpr));
    return toLogical(capture::placeSizeLabel(selection, toPhysical(width, height), bounds(), gap));
}

QPointF CaptureScreen::placeMagnifier(const QPoint& cursor, qreal width, qreal height) const
{
    const int offset = static_cast<int>(std::lround(20 * m_dpr));
    return toLogical(capture::placeMagnifier(cursor, toPhysical(width, height), bounds(), offset));
}

// ---------------------------------------------------------------------------
// CaptureSession

CaptureSession::CaptureSession(QObject* parent)
    : QObject(parent)
{
}

void CaptureSession::begin(std::vector<CaptureScreen*> screens, Windows windows)
{
    m_screens = std::move(screens);
    m_windows = std::move(windows);
    m_flyout = {};
    m_phase = Phase::Hover;
    m_selection = {};
    m_previous = {};
    m_handle = Handle::None;
    m_cursorScreen = -1;
    m_hoverScreen = -1;
    m_chain.clear();
    m_level = 0;
    for (CaptureScreen* s : m_screens)
        s->setFlash({});
    const bool was = m_active;
    m_active = true;
    publish();
    if (!was)
        emit activeChanged();
    emit selectionChanged();
}

void CaptureSession::setFlyout(const QRect& rect)
{
    m_flyout = rect;
    if (m_cursorScreen >= 0 && m_phase == Phase::Hover) {
        m_hoverScreen = -1; // picked again with it
        updateHover(m_cursorScreen, m_cursorAt);
        publish();
    }
}

void CaptureSession::setOcrAvailable(bool on)
{
    if (std::exchange(m_ocrAvailable, on) != on)
        emit ocrAvailableChanged();
}

void CaptureSession::end()
{
    for (CaptureScreen* s : m_screens) {
        s->setState({}, {}, false, false);
        s->setCursor(false, {-1, -1}, Qt::CrossCursor);
        s->setFlash({});
    }
    m_screens.clear();
    m_windows = {};
    m_flyout = {};
    m_chain.clear();
    m_selection = {};
    m_previous = {};
    m_phase = Phase::Hover;
    if (std::exchange(m_active, false))
        emit activeChanged();
    emit selectionChanged();
}

CaptureScreen* CaptureSession::screenAt(int index) const
{
    const auto found = std::ranges::find(m_screens, index, &CaptureScreen::index);
    return found != m_screens.end() ? *found : nullptr;
}

QRect CaptureSession::hoverRect() const
{
    return m_level < m_chain.size() ? m_chain[m_level] : QRect();
}

QPoint CaptureSession::clampTo(int screen, QPoint p) const
{
    const CaptureScreen* s = screenAt(screen);
    if (!s)
        return p;
    return QPoint(std::clamp(p.x(), 0, std::max(0, s->pixels().width() - 1)),
        std::clamp(p.y(), 0, std::max(0, s->pixels().height() - 1)));
}

int CaptureSession::reach(int screen) const
{
    const CaptureScreen* s = screenAt(screen);
    return static_cast<int>(std::lround(6 * (s ? s->dpr() : 1.0)));
}

int CaptureSession::dragStart(int screen) const
{
    const CaptureScreen* s = screenAt(screen);
    return std::max(2, static_cast<int>(std::lround(3 * (s ? s->dpr() : 1.0))));
}

void CaptureSession::updateHover(int screen, QPoint p)
{
    const CaptureScreen* s = screenAt(screen);
    if (!s)
        return;
    const QPoint origin = s->rect().topLeft();
    // The flyout, when there is one, first: it is over everything listed.
    const std::size_t above = m_flyout.isEmpty() ? 0 : 1;
    std::vector<QRect> tops;
    tops.reserve(m_windows.tops.size() + above);
    if (above)
        tops.push_back(m_flyout.translated(-origin));
    for (const QRect& r : m_windows.tops)
        tops.push_back(r.translated(-origin));
    const auto children = [this, origin, above](std::size_t i) {
        std::vector<QRect> list;
        if (m_windows.children && i >= above) {
            for (const QRect& r : m_windows.children(i - above))
                list.push_back(r.translated(-origin));
        }
        return list;
    };
    std::vector<QRect> chain = hitChain(tops, children, p, s->bounds());
    // Stepped out with the wheel: stays out while that pick is still there.
    std::size_t level = 0;
    if (screen == m_hoverScreen && m_level > 0 && m_level < m_chain.size()) {
        const auto same = std::ranges::find(chain, m_chain[m_level]);
        if (same != chain.end())
            level = static_cast<std::size_t>(same - chain.begin());
    }
    m_hoverScreen = screen;
    m_chain = std::move(chain);
    m_level = level;
}

void CaptureSession::updateCursor(int screen, QPoint p)
{
    m_cursorScreen = screen;
    m_cursorAt = clampTo(screen, p);
    for (CaptureScreen* s : m_screens) {
        const bool here = s->index() == screen;
        int shape = Qt::CrossCursor;
        if (here && m_phase == Phase::Selected && m_selection.screen == screen)
            shape = cursorFor(handleAt(m_selection.rect, m_cursorAt, reach(screen)));
        else if (here && m_phase == Phase::Adjusting)
            shape = cursorFor(m_handle);
        s->setCursor(here, here ? m_cursorAt : QPoint(-1, -1), shape);
    }
}

void CaptureSession::publish()
{
    const bool pressedOverPrevious = m_phase == Phase::Pressed && m_previous.screen >= 0;
    const Pick shown = pressedOverPrevious ? m_previous : m_selection;
    const bool hovering = (m_phase == Phase::Hover || m_phase == Phase::Pressed) && !pressedOverPrevious;
    const bool adjusting = m_phase == Phase::Dragging || m_phase == Phase::Adjusting;
    for (CaptureScreen* s : m_screens) {
        const QRect hover = hovering && s->index() == m_hoverScreen ? hoverRect() : QRect();
        const QRect selection = s->index() == shown.screen ? shown.rect : QRect();
        s->setState(hover, selection, shown.screen >= 0, adjusting);
    }
}

CaptureSession::Pick CaptureSession::hovered() const
{
    if (m_hoverScreen >= 0 && m_level < m_chain.size())
        return {m_hoverScreen, m_chain[m_level]};
    if (const CaptureScreen* s = m_cursorScreen >= 0 ? screenAt(m_cursorScreen) : nullptr)
        return {s->index(), s->bounds()};
    if (!m_screens.empty())
        return {m_screens.front()->index(), m_screens.front()->bounds()};
    return {};
}

void CaptureSession::pointerMove(int screen, QPoint p)
{
    if (!m_active || !screenAt(screen))
        return;
    switch (m_phase) {
    case Phase::Hover:
        updateHover(screen, clampTo(screen, p));
        break;
    case Phase::Pressed: {
        // Positions while the button is held come from the window pressed
        // in (Qt grabs the mouse there), beyond its edges too.
        const QPoint here = p + screenAt(screen)->rect().topLeft() - screenAt(m_pressScreen)->rect().topLeft();
        if ((here - m_pressAt).manhattanLength() < dragStart(m_pressScreen)) {
            if (m_previous.screen < 0)
                updateHover(screen, clampTo(screen, p));
            break;
        }
        m_phase = Phase::Dragging;
        m_previous = {};
        m_selection = {m_pressScreen, spanRect(m_pressAt, clampTo(m_pressScreen, here))};
        emit selectionChanged();
        break;
    }
    case Phase::Dragging: {
        const QPoint here = p + screenAt(screen)->rect().topLeft() - screenAt(m_pressScreen)->rect().topLeft();
        m_selection.rect = spanRect(m_pressAt, clampTo(m_pressScreen, here));
        break;
    }
    case Phase::Adjusting: {
        const QPoint here = p + screenAt(screen)->rect().topLeft() - screenAt(m_pressScreen)->rect().topLeft();
        m_selection.rect = dragHandle(m_startRect, m_handle, here - m_pressAt, screenAt(m_pressScreen)->bounds());
        break;
    }
    case Phase::Selected:
        break;
    }
    updateCursor(screen, p);
    publish();
}

void CaptureSession::pointerPress(int screen, QPoint p, Qt::MouseButton button)
{
    if (!m_active || !screenAt(screen))
        return;
    if (button == Qt::RightButton) {
        back();
        return;
    }
    if (button != Qt::LeftButton)
        return;
    p = clampTo(screen, p);
    m_pressScreen = screen;
    m_pressAt = p;
    if (m_phase == Phase::Selected) {
        const Handle handle = m_selection.screen == screen ? handleAt(m_selection.rect, p, reach(screen)) : Handle::None;
        if (handle != Handle::None) {
            m_phase = Phase::Adjusting;
            m_handle = handle;
            m_startRect = m_selection.rect;
        } else {
            // Outside: a drag draws a new selection, a click keeps this one.
            m_phase = Phase::Pressed;
            m_previous = m_selection;
        }
    } else {
        m_phase = Phase::Pressed;
        m_previous = {};
        updateHover(screen, p);
    }
    updateCursor(screen, p);
    publish();
}

void CaptureSession::pointerRelease(int screen, QPoint p, Qt::MouseButton button)
{
    if (!m_active || button != Qt::LeftButton)
        return;
    switch (m_phase) {
    case Phase::Pressed:
        if (m_previous.screen >= 0) {
            m_selection = std::exchange(m_previous, {});
            m_phase = Phase::Selected;
            break;
        }
        m_selection = hovered(); // a click picks the window or control under it
        m_phase = Phase::Selected;
        emit selectionChanged();
        if (m_copyOnSelect) {
            finish(Action::Copy);
            return;
        }
        break;
    case Phase::Dragging:
        m_phase = Phase::Selected;
        if (m_copyOnSelect) {
            finish(Action::Copy);
            return;
        }
        break;
    case Phase::Adjusting:
        m_phase = Phase::Selected;
        m_handle = Handle::None;
        break;
    case Phase::Hover:
    case Phase::Selected:
        break;
    }
    if (screenAt(screen))
        updateCursor(screen, p);
    publish();
}

void CaptureSession::doubleClick(int screen, QPoint p)
{
    if (!m_active)
        return;
    // The first click picked (or kept) the selection: the second copies it.
    if (m_selection.screen == screen && m_selection.rect.contains(clampTo(screen, p))) {
        if (m_phase == Phase::Adjusting) {
            m_selection.rect = m_startRect; // the press began a move that did not happen
            m_phase = Phase::Selected;
        }
        finish(Action::Copy);
    }
}

void CaptureSession::wheel(int screen, QPoint p, int steps)
{
    if (!m_active || m_phase != Phase::Hover || steps == 0)
        return;
    if (screen != m_hoverScreen)
        updateHover(screen, clampTo(screen, p));
    if (m_chain.empty())
        return;
    const auto last = static_cast<long long>(m_chain.size()) - 1;
    m_level = static_cast<std::size_t>(std::clamp(static_cast<long long>(m_level) + steps, 0LL, last));
    publish();
}

bool CaptureSession::key(int key, Qt::KeyboardModifiers modifiers)
{
    if (!m_active)
        return false;
    const bool ctrl = modifiers & Qt::ControlModifier;
    const bool shift = modifiers & Qt::ShiftModifier;
    const bool busy = m_phase == Phase::Pressed || m_phase == Phase::Dragging || m_phase == Phase::Adjusting;
    switch (key) {
    case Qt::Key_Escape:
        back();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!busy)
            finish(Action::Copy);
        return true;
    case Qt::Key_S:
        if (ctrl && !busy) {
            finish(Action::Save);
            return true;
        }
        return false;
    case Qt::Key_F3:
        if (!busy)
            finish(Action::Pin);
        return true;
    case Qt::Key_C:
        if (busy)
            return true;
        if (ctrl) {
            finish(Action::Copy);
        } else if (const CaptureScreen* s = screenAt(m_cursorScreen); s && !s->color().isEmpty()) {
            emit colorCopied(s->color());
            if (CaptureScreen* target = screenAt(m_cursorScreen))
                target->setFlash(tr("已复制 %1").arg(s->color()));
        }
        return true;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down: {
        if (busy)
            return true;
        const int dx = key == Qt::Key_Left ? -1 : key == Qt::Key_Right ? 1 : 0;
        const int dy = key == Qt::Key_Up ? -1 : key == Qt::Key_Down ? 1 : 0;
        if (m_selection.screen >= 0) {
            const QRect moved = nudge(m_selection.rect, dx, dy, shift, screenAt(m_selection.screen)->bounds());
            if (moved != m_selection.rect) {
                m_selection.rect = moved;
                publish();
            }
        } else if (const CaptureScreen* s = screenAt(m_cursorScreen)) {
            // Nothing selected: the cursor itself, for the magnifier.
            emit cursorMoveRequested(s->rect().topLeft() + clampTo(m_cursorScreen, m_cursorAt + QPoint(dx, dy)));
        }
        return true;
    }
    default:
        return false;
    }
}

void CaptureSession::back()
{
    switch (m_phase) {
    case Phase::Pressed:
    case Phase::Dragging:
    case Phase::Adjusting:
        // A drag let go of: back to how it was before the press.
        if (m_phase == Phase::Adjusting)
            m_selection.rect = m_startRect;
        else if (m_previous.screen >= 0)
            m_selection = std::exchange(m_previous, {});
        else
            m_selection = {};
        m_phase = m_selection.screen >= 0 ? Phase::Selected : Phase::Hover;
        break;
    case Phase::Selected:
        m_selection = {};
        m_phase = Phase::Hover;
        if (m_cursorScreen >= 0)
            updateHover(m_cursorScreen, m_cursorAt);
        break;
    case Phase::Hover:
        finish(Action::Cancel);
        return;
    }
    emit selectionChanged();
    if (m_cursorScreen >= 0)
        updateCursor(m_cursorScreen, m_cursorAt);
    publish();
}

void CaptureSession::finish(Action action)
{
    Pick pick;
    if (action != Action::Cancel)
        pick = m_selection.screen >= 0 ? m_selection : hovered();
    if (action != Action::Cancel && (pick.screen < 0 || pick.rect.isEmpty()))
        action = Action::Cancel;
    emit finished(action, pick.screen, pick.rect);
}

void CaptureSession::mouseMoved(int screen, qreal x, qreal y)
{
    if (const CaptureScreen* s = screenAt(screen))
        pointerMove(screen, QPoint(toPhysical(x, s->dpr()), toPhysical(y, s->dpr())));
}

void CaptureSession::mousePressed(int screen, qreal x, qreal y, int button)
{
    if (const CaptureScreen* s = screenAt(screen))
        pointerPress(screen, QPoint(toPhysical(x, s->dpr()), toPhysical(y, s->dpr())), static_cast<Qt::MouseButton>(button));
}

void CaptureSession::mouseReleased(int screen, qreal x, qreal y, int button)
{
    if (const CaptureScreen* s = screenAt(screen))
        pointerRelease(screen, QPoint(toPhysical(x, s->dpr()), toPhysical(y, s->dpr())), static_cast<Qt::MouseButton>(button));
}

void CaptureSession::mouseDoubleClicked(int screen, qreal x, qreal y)
{
    if (const CaptureScreen* s = screenAt(screen))
        doubleClick(screen, QPoint(toPhysical(x, s->dpr()), toPhysical(y, s->dpr())));
}

void CaptureSession::wheeled(int screen, qreal x, qreal y, int angleDeltaY)
{
    const CaptureScreen* s = screenAt(screen);
    if (!s || angleDeltaY == 0)
        return;
    wheel(screen, QPoint(toPhysical(x, s->dpr()), toPhysical(y, s->dpr())), angleDeltaY > 0 ? 1 : -1);
}

bool CaptureSession::keyPressed(int key, int modifiers)
{
    return this->key(key, Qt::KeyboardModifiers(modifiers));
}

void CaptureSession::act(int action)
{
    if (m_active && action >= static_cast<int>(Action::Copy) && action <= static_cast<int>(Action::Ocr))
        finish(static_cast<Action>(action));
}

} // namespace ws::capture
