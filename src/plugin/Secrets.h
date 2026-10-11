#pragma once

// A plugin's password settings (the plugin design, §4.7): kept by Windows'
// Credential Manager, never in WinShun.ini, as generic credentials named
// "WinShun/plugin/<id>/<key>". Read when the plugin is told its settings,
// removed with the plugin.

#include "Manifest.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

namespace ws::plugin::secrets {

QString target(const QString& pluginId, const QString& key); // "WinShun/plugin/<id>/<key>"
bool write(const QString& pluginId, const QString& key, const QString& value); // an empty value removes it
std::optional<QString> read(const QString& pluginId, const QString& key); // nothing when there is none
bool remove(const QString& pluginId, const QString& key);
// Every secret of the plugin's, removed; how many there were.
int removeAll(const QString& pluginId);
QStringList keysOf(const QString& pluginId); // the keys with a secret kept

// A declared setting's value, kept where it belongs: a password in the
// Credential Manager, anything else among `iniValues` (the [Plugin.<id>]
// Setting.<key> lines, as text). True when `iniValues` changed.
bool store(const QString& pluginId, const Manifest& manifest, const QString& key, const QString& value,
    QList<std::pair<QString, QString>>& iniValues);
// The plugin's settings as it reads them: the INI's text by the manifest's
// types (bool, number, text), the passwords from the Credential Manager.
QJsonObject settingsOf(const QString& pluginId, const Manifest& manifest, const QList<std::pair<QString, QString>>& iniValues);

} // namespace ws::plugin::secrets
