#include "SystemTheme.h"

#include "UiSize.h"
#include "platform/WindowEffects.h"

#include <QEvent>
#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QPalette>
#include <QStyleHints>

#include <windows.h>

#include <cmath>

using namespace Qt::StringLiterals;

namespace ws {

SystemTheme::SystemTheme(QObject* parent)
    : QObject(parent)
    , m_iconFont(iconFontName())
{
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &SystemTheme::changed);
    qApp->installEventFilter(this); // accent colour changes arrive as a palette change
    qApp->installNativeEventFilter(this); // transparency effects, high contrast: WM_SETTINGCHANGE
    s_instance = this;
}

QString SystemTheme::iconFontName()
{
    // Windows 11 ships "Segoe Fluent Icons"; Windows 10 has "Segoe MDL2 Assets".
    // Decided by OS version: enumerating all installed fonts is expensive.
    return QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11 ? u"Segoe Fluent Icons"_s
                                                                                    : u"Segoe MDL2 Assets"_s;
}

bool SystemTheme::backdropSystem() const
{
    return win::backdropSupported();
}

bool SystemTheme::roundedCorners() const
{
    return win::roundedCorners();
}

void SystemTheme::setBackdropOn(bool on)
{
    if (s_backdropOn == on)
        return;
    s_backdropOn = on;
    if (s_instance)
        emit s_instance->changed();
}

int SystemTheme::headerHeight()
{
    return uisize::metrics(s_uiSize).header;
}

void SystemTheme::setUiSize(const QString& size)
{
    if (s_uiSize == size)
        return;
    s_uiSize = size;
    if (s_instance)
        emit s_instance->uiSizeChanged();
}

bool SystemTheme::dark() const
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

QColor SystemTheme::accent() const
{
    return accentColor();
}

namespace {

// WCAG relative luminance.
double luminance(const QColor& c)
{
    const auto channel = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

} // namespace

QColor SystemTheme::accentFill() const
{
    // Under white words or marks (a highlighted button, a switch that is
    // on, the current sub-tab): the accent, a little lighter on dark, then
    // darker until white on it reads (WCAG AA, 4.5:1), whatever the accent
    // (a yellow one too). Theme.accent, much lighter on dark, is for text
    // and lines, where black on it would be the only legible word colour.
    QColor fill = accentColor();
    if (dark())
        fill = fill.lighter(115);
    for (int i = 0; i < 30 && 1.05 / (luminance(fill) + 0.05) < 4.5; ++i)
        fill = fill.darker(108);
    return fill;
}

QColor SystemTheme::accentColor()
{
    const QColor accent = QGuiApplication::palette().color(QPalette::Accent);
    // A grey accent (a popular Windows choice) would make highlighted matches
    // indistinguishable from plain text; use the standard Windows blue then.
    if (!accent.isValid() || accent.hsvSaturationF() < 0.25)
        return QColor(0x00, 0x67, 0xC0);
    return accent;
}

bool SystemTheme::animations() const
{
    BOOL on = TRUE;
    return !::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on;
}

bool SystemTheme::materials() const
{
    return win::materialsEnabled();
}

bool SystemTheme::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange)
        emit changed();
    return QObject::eventFilter(watched, event);
}

bool SystemTheme::nativeEventFilter(const QByteArray& eventType, void* message, qintptr*)
{
    // Sent to every top-level window when the colour settings change.
    const auto* msg = static_cast<const MSG*>(message);
    if (eventType != "windows_generic_MSG" || !msg || msg->message != WM_SETTINGCHANGE)
        return false;
    if (msg->wParam == SPI_SETHIGHCONTRAST
        || (msg->lParam && ::lstrcmpiW(reinterpret_cast<LPCWSTR>(msg->lParam), L"ImmersiveColorSet") == 0))
        emit changed();
    return false;
}

} // namespace ws
