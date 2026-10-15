#include "SvgIcon.h"

#include <QPainter>
#include <QRegularExpression>
#include <QString>
#include <QSvgRenderer>

using namespace Qt::StringLiterals;

namespace ws::svgicon {

namespace {

QByteArrayView withoutBom(QByteArrayView bytes)
{
    return bytes.startsWith("\xEF\xBB\xBF") ? bytes.sliced(3) : bytes;
}

// Every reference must stay within the SVG: "#id", or a picture in a data:
// address. QtSvg reads a local file for <image href="C:/…"> (or file:),
// and anything else named outside would be a request of some kind.
bool referencesOutside(const QString& text)
{
    static const QRegularExpression href(u"href\\s*=\\s*([\"'])(.*?)\\1"_s,
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression url(u"url\\s*\\(\\s*([\"']?)([^)]*?)\\1\\s*\\)"_s, QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression dataPicture(u"^data:image/(png|jpeg|jpg|gif)[;,]"_s, QRegularExpression::CaseInsensitiveOption);
    for (auto it = href.globalMatch(text); it.hasNext();) {
        const QString target = it.next().captured(2).trimmed();
        if (!target.startsWith(u'#') && !dataPicture.match(target).hasMatch())
            return true;
    }
    for (auto it = url.globalMatch(text); it.hasNext();) {
        if (!it.next().captured(2).trimmed().startsWith(u'#'))
            return true;
    }
    return text.contains(u"@import"_s, Qt::CaseInsensitive) || text.contains(u"<!ENTITY"_s, Qt::CaseInsensitive);
}

// A <!DOCTYPE svg PUBLIC … "….dtd"> as iconfont's files have it: nothing
// QtSvg needs (and one with entities in it was turned away above).
QString withoutDoctype(QString text)
{
    static const QRegularExpression doctype(u"<!DOCTYPE[^>\\[]*>"_s, QRegularExpression::CaseInsensitiveOption);
    return text.remove(doctype);
}

} // namespace

bool looksLikeSvg(QByteArrayView bytes)
{
    const QByteArrayView start = withoutBom(bytes).trimmed();
    const QByteArray lower = start.first(std::min<qsizetype>(start.size(), 16)).toByteArray().toLower();
    return lower.startsWith("<svg") || lower.startsWith("<?xml") || lower.startsWith("<!--")
        || lower.startsWith("<!doctype svg");
}

QImage render(QByteArrayView bytes, int side, Problem* problem)
{
    const auto fail = [problem](Problem p) {
        if (problem)
            *problem = p;
        return QImage();
    };
    if (problem)
        *problem = Problem::None;
    if (bytes.size() > kMaxBytes)
        return fail(Problem::TooLarge);
    const QString text = QString::fromUtf8(withoutBom(bytes));
    if (!looksLikeSvg(bytes) || !text.contains(u"<svg"_s, Qt::CaseInsensitive))
        return fail(Problem::NotSvg);
    if (referencesOutside(text))
        return fail(Problem::External);

    QSvgRenderer renderer;
    // Not AssumeTrustedSource: QtSvg's own limits for what it does not know stay on.
    renderer.setOptions(QtSvg::DisableAnimations);
    if (!renderer.load(withoutDoctype(text).toUtf8()) || !renderer.isValid())
        return fail(Problem::NotSvg);
    QRectF box = renderer.viewBoxF();
    if (box.isEmpty())
        box = QRectF(QPointF(), renderer.defaultSize());
    if (box.isEmpty() || side <= 0)
        return fail(Problem::Empty);
    renderer.setAspectRatioMode(Qt::KeepAspectRatio);

    QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const qreal scale = side / std::max(box.width(), box.height());
    const QSizeF drawn = box.size() * scale;
    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    renderer.render(&painter, QRectF(QPointF((side - drawn.width()) / 2, (side - drawn.height()) / 2), drawn));
    painter.end();

    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) != 0)
                return image;
        }
    }
    return fail(Problem::Empty);
}

} // namespace ws::svgicon
