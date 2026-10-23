#include "DropPieText.h"

#include "PieGeometry.h"
#include "PieLabels.h"
#include "SystemTheme.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QList>

#include <algorithm>

namespace ws {

namespace {

// GUI thread only.
int g_folderCount = 0;
QList<DropPieText*> g_instances;

} // namespace

DropPieText::DropPieText(QObject* parent)
    : QObject(parent)
{
    g_instances.append(this);
}

DropPieText::~DropPieText()
{
    g_instances.removeOne(this);
}

int DropPieText::folderCount() const
{
    return g_folderCount;
}

void DropPieText::setFolderCount(int count)
{
    if (g_folderCount == count)
        return;
    g_folderCount = count;
    for (DropPieText* text : std::as_const(g_instances))
        emit text->folderCountChanged();
}

int DropPieText::uiStep()
{
    const QString size = SystemTheme::uiSize();
    return size == u"compact" ? 0 : size == u"roomy" ? 2 : 1;
}

QFont DropPieText::labelFont()
{
    QFont font = QGuiApplication::font(); // what a QML Text uses unless told otherwise
    font.setPixelSize(pie::labelPixelSize(uiStep()));
    return font;
}

double DropPieText::labelWidth(int sectors)
{
    const double scale = pie::scaleForStep(uiStep());
    return pie::labelWidth(sectors, pie::Radii {}.scaled(scale), 96 * scale);
}

QStringList DropPieText::fit(const QString& name) const
{
    const pielabels::Fitted fitted
        = pielabels::fitTwoLines(name, QFontMetricsF(labelFont()), labelWidth(std::clamp(g_folderCount, 1, kMaxFolders)));
    return {fitted.line1, fitted.line2};
}

} // namespace ws
