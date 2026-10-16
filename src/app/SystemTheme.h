#pragma once

#include <QAbstractNativeEventFilter>
#include <QColor>
#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace ws {

// System appearance for QML: light/dark mode, accent colour, icon font.
// Colours derived from these live in Theme.qml.
class SystemTheme : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool dark READ dark NOTIFY changed FINAL)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed FINAL)
    // A fill that carries white words or marks (see accentFill()).
    Q_PROPERTY(QColor accentFill READ accentFill NOTIFY changed FINAL)
    Q_PROPERTY(QString iconFont READ iconFont CONSTANT FINAL)
    // Settings > Accessibility > Visual effects > Animation effects.
    Q_PROPERTY(bool animations READ animations NOTIFY changed FINAL)
    // Whether our windows can stand on Mica (see WindowEffects): Windows 11
    // 22H2 or later and the GPU renderer. Decided once at startup.
    Q_PROPERTY(bool backdropAvailable READ backdropAvailable CONSTANT FINAL)
    // Windows has Mica (11 22H2 or later), whatever the renderer.
    Q_PROPERTY(bool backdropSystem READ backdropSystem CONSTANT FINAL)
    // They do (the 透明效果 setting): their surfaces are translucent then.
    Q_PROPERTY(bool backdrop READ backdrop NOTIFY changed FINAL)
    // Transparency effects on and no high contrast: otherwise Windows draws
    // no Mica.
    Q_PROPERTY(bool materials READ materials NOTIFY changed FINAL)
    // Windows rounds the corners of our windows (11 or later).
    Q_PROPERTY(bool roundedCorners READ roundedCorners CONSTANT FINAL)
    // How large the launcher and the clipboard are drawn (界面大小: compact,
    // standard, roomy; see uisize::), and its header's height, as C++ places by.
    Q_PROPERTY(QString uiSize READ uiSize NOTIFY uiSizeChanged FINAL)
    Q_PROPERTY(int headerHeight READ headerHeight NOTIFY uiSizeChanged FINAL)
    // How long the mouse rests before a tip shows (SPI_GETMOUSEHOVERTIME, ms).
    Q_PROPERTY(int hoverTime READ hoverTime NOTIFY changed FINAL)

public:
    explicit SystemTheme(QObject* parent = nullptr);

    bool dark() const;
    QColor accent() const;
    QColor accentFill() const;
    static QColor accentColor(); // accent(), for C++
    QString iconFont() const { return m_iconFont; }
    static QString iconFontName(); // iconFont(), for C++ (FileIconProvider draws glyph icons)
    bool animations() const;
    static int hoverTime();
    bool materials() const;
    bool backdropSystem() const;
    bool roundedCorners() const;
    static bool backdropAvailable() { return s_backdropAvailable; }
    static void setBackdropAvailable(bool on) { s_backdropAvailable = on; } // before the first window
    static bool backdrop() { return s_backdropAvailable && s_backdropOn; }
    static void setBackdropOn(bool on);
    static QString uiSize() { return s_uiSize; }
    static int headerHeight();
    static void setUiSize(const QString& size);

signals:
    void changed();
    void uiSizeChanged(); // not changed(): the colours stay

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    static inline bool s_backdropAvailable = false;
    static inline bool s_backdropOn = true;
    static inline QString s_uiSize = QStringLiteral("standard");
    static inline SystemTheme* s_instance = nullptr; // the QML singleton
    QString m_iconFont;
};

} // namespace ws
