#pragma once

#include <QJsonObject>
#include <QString>

class QFont;
class QQuickWindow;

// What the diagnostics record (DiagnosticLog) and the exported summary hold
// about how Win顺 draws: for display problems that come and go (GitHub #70),
// where a restart takes the evidence away. Nothing of the user's: no text,
// no paths, no monitor names.
namespace ws::diagnostics {

// How the UI font came to be (main.cpp: applyUiFont), with the font engine
// asked for: recorded once and kept for the summary.
void setUiFont(const QJsonObject& loaded);

// Screens added, removed, rescaled or resized: each new arrangement is
// recorded. And each time Qt empties its font caches (fontDatabaseChanged:
// an application font added or removed), which Windows' WM_FONTCHANGE does not.
void watchDisplay();

// A font a result row highlights in (ResultModel::elidedHtml: the GUI thread,
// every row). Every 10 minutes the fonts seen go through textcheck::check;
// the first result of a run and every change are recorded, a failure also
// in WinShun.log.
void watchHighlightFont(const QFont& font);

// Windows' messages that change how windows draw (MessageWindow): sleep and
// resume, fonts added or removed.
void noteSystemMessage(unsigned message, quintptr wParam);

// A window's scene graph: which graphics API when it first starts, how often
// it starts again, its errors.
void watchWindow(QQuickWindow* window, const QString& name);

// For an export's summary, on the GUI thread (Qt's fonts are per thread):
// the renderer, the UI font, locale, ClearType, screens, adapters, uptime,
// sleep and fonts since the start, and the text check run now.
QJsonObject display(const QString& renderer, bool backdrop);

} // namespace ws::diagnostics
