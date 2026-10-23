#pragma once

#include <QFontMetricsF>
#include <QString>
#include <QStringList>

namespace ws::pielabels {

// A folder's name as the drop pie's sector shows it: on one line if it fits,
// else on two, else shortened in its middle (the end kept: 终版, a date, a
// version). Measured with the font the label is drawn in, not counted in
// characters (tst_pielabels).
struct Fitted {
    QString line1;
    QString line2; // empty: one line
    bool elided = false; // something left out ("…")

    bool twoLines() const noexcept { return !line2.isEmpty(); }
    friend bool operator==(const Fitted&, const Fitted&) = default;
};

// Two lines broken after _ - space or ·, or before an opening bracket （ (
// 【 [, the halves as even as can be; any other place if none of those
// fits; else the start that fits on the first line and "…" with the end on
// the second.
Fitted fitTwoLines(const QString& text, const QFontMetricsF& metrics, double maxWidth);

// `text` shortened in its middle to `maxWidth`: about two fifths of the
// width for its start, the rest for its end.
QString elideMiddle(const QString& text, const QFontMetricsF& metrics, double maxWidth);

// A folder's path shortened in its middle, its drive (or \\server\share)
// and its last folder kept: "F:\工作\…\季度报告\终版"; the last folder itself
// shortened only when even that is too wide.
QString elidePath(const QString& path, const QFontMetricsF& metrics, double maxWidth);

// For names shown more than once (two folders called 资料): each one's
// parent folder's name, for a second line "在 工作"; when the parents share
// a name too, the parent's whole path. Empty for a name shown once.
QStringList parentHints(const QStringList& names, const QStringList& paths);

// "D:\Work\资料" -> "资料"; "D:\" -> "D:\".
QString lastName(const QString& path);

} // namespace ws::pielabels
