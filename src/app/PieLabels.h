#pragma once

#include <QFontMetricsF>
#include <QString>
#include <QStringList>

namespace ws::pielabels {

// A folder's name and path as the drop pie shows them, measured with the
// font they are drawn in, not counted in characters (tst_pielabels).

// `text` shortened in its middle to `maxWidth`: about two fifths of the
// width for its start, the rest for its end (终版, a date, a version).
QString elideMiddle(const QString& text, const QFontMetricsF& metrics, double maxWidth);

// A folder's path shortened in its middle, its drive (or \\server\share)
// and its last folder kept: "F:\工作\…\季度报告\终版"; the last folder itself
// shortened only when even that is too wide.
QString elidePath(const QString& path, const QFontMetricsF& metrics, double maxWidth);

// For names shown more than once (two folders called 资料): each one's
// parent folder's name, shown after it ("资料 · 工作"); when the parents share
// a name too, the parent's whole path. Empty for a name shown once.
QStringList parentHints(const QStringList& names, const QStringList& paths);

// "D:\Work\资料" -> "资料"; "D:\" -> "D:\".
QString lastName(const QString& path);

} // namespace ws::pielabels
