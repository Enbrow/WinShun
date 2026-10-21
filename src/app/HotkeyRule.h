#pragma once

#include "Settings.h"

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace ws {

// Settings::hotkeyAllowed for the settings window's shortcut recorder, so
// that what it takes and what AI may set are one rule.
class HotkeyRule : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit HotkeyRule(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    Q_INVOKABLE bool allowed(const QString& shortcut) const { return Settings::hotkeyAllowed(shortcut); }
};

} // namespace ws
