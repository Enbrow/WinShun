#include "PieLabels.h"

#include <QHash>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::pielabels {

namespace {

// QML lays text out with hinted advances that can differ a little from what
// is measured here: a pixel and a half to spare, so a line said to fit never
// gets cut by the Text.
constexpr double kSlack = 1.5;

const QString kEllipsis = u"…"_s;

// A place between two characters, not inside a surrogate pair.
bool boundary(const QString& text, qsizetype at)
{
    return at > 0 && at < text.size() && !text.at(at).isLowSurrogate();
}

// The longest start of `text` within `width`.
QString headFit(const QString& text, const QFontMetricsF& metrics, double width)
{
    qsizetype end = 0;
    for (qsizetype k = 1; k <= text.size(); ++k) {
        if (k < text.size() && !boundary(text, k))
            continue;
        if (metrics.horizontalAdvance(text.left(k)) > width)
            break;
        end = k;
    }
    return text.left(end);
}

// The longest end of `text` within `width`.
QString tailFit(const QString& text, const QFontMetricsF& metrics, double width)
{
    qsizetype start = text.size();
    for (qsizetype k = text.size() - 1; k >= 0; --k) {
        if (k > 0 && !boundary(text, k))
            continue;
        if (metrics.horizontalAdvance(text.mid(k)) > width)
            break;
        start = k;
    }
    return text.mid(start);
}

} // namespace

QString elideMiddle(const QString& text, const QFontMetricsF& metrics, double maxWidth)
{
    const double max = maxWidth - kSlack;
    if (metrics.horizontalAdvance(text) <= max)
        return text;
    const QString head = headFit(text, metrics, max * 0.4);
    const double left = max - metrics.horizontalAdvance(head) - metrics.horizontalAdvance(kEllipsis);
    return head + kEllipsis + tailFit(text.mid(head.size()), metrics, std::max(0.0, left));
}

QString elidePath(const QString& path, const QFontMetricsF& metrics, double maxWidth)
{
    const double max = maxWidth - kSlack;
    if (metrics.horizontalAdvance(path) <= max)
        return path;
    // Its root: "F:\", or "\\server\share\".
    qsizetype rootEnd = 0;
    if (path.startsWith(u"\\\\"_s)) {
        const qsizetype server = path.indexOf(u'\\', 2);
        const qsizetype share = server < 0 ? -1 : path.indexOf(u'\\', server + 1);
        rootEnd = share < 0 ? path.size() : share + 1;
    } else {
        const qsizetype slash = path.indexOf(u'\\');
        rootEnd = slash < 0 ? 0 : slash + 1;
    }
    const QString root = path.left(rootEnd);
    const QStringList parts = path.mid(rootEnd).split(u'\\', Qt::SkipEmptyParts);
    if (root.isEmpty() || parts.size() < 2 || metrics.horizontalAdvance(root + kEllipsis) > max)
        return elideMiddle(path, metrics, maxWidth);
    // As many of the last folders as fit after "root…\".
    const QString lead = root + kEllipsis + u'\\';
    for (qsizetype from = 1; from < parts.size(); ++from) {
        const QString candidate = lead + parts.mid(from).join(u'\\');
        if (metrics.horizontalAdvance(candidate) <= max)
            return candidate;
    }
    return lead + elideMiddle(parts.last(), metrics, maxWidth - metrics.horizontalAdvance(lead));
}

QStringList parentHints(const QStringList& names, const QStringList& paths)
{
    QStringList hints(names.size());
    QHash<QString, QList<qsizetype>> byName;
    for (qsizetype i = 0; i < names.size(); ++i)
        byName[names[i].toCaseFolded()].append(i);
    for (const QList<qsizetype>& same : std::as_const(byName)) {
        if (same.size() < 2)
            continue;
        QStringList parents;
        for (const qsizetype i : same) {
            const QString path = i < paths.size() ? paths[i] : QString();
            const qsizetype slash = path.lastIndexOf(u'\\', path.endsWith(u'\\') ? -2 : -1);
            parents.append(slash <= 0 ? path : path.left(slash == 2 && path.at(1) == u':' ? 3 : slash));
        }
        for (qsizetype j = 0; j < same.size(); ++j) {
            const QString parentName = lastName(parents[j]);
            const bool shared = std::count_if(parents.cbegin(), parents.cend(), [&](const QString& p) {
                return lastName(p).compare(parentName, Qt::CaseInsensitive) == 0;
            }) > 1;
            hints[same[j]] = shared ? parents[j] : parentName;
        }
    }
    return hints;
}

QString lastName(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(u'\\');
    return slash < 0 || slash == path.size() - 1 ? path : path.mid(slash + 1);
}

} // namespace ws::pielabels
