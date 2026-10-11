#pragma once

#include <QByteArrayView>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>

// The icons of the sites web shortcuts open, as a browser shows them left of
// the address. Fetched from each site the first time it is shown (its home
// page names them; /favicon.ico otherwise), kept as PNG in the data folder
// (icons\web\<site>.png), fetched again after a month. Nothing of other sites
// ships with Win顺. Until a site's arrives, its rows show the browser's icon.
namespace ws::siteicons {

// Where they are kept; once, at start, before anything asks.
void setFolder(const QString& folder);
// No more telling of icons that arrive: Win顺 is ending.
void shutdown();

// Whether the icon of a site (siteIconKey) is kept. Quick.
bool has(const QString& key);
// As kept (siteicon::normalized); null when not.
QImage load(const QString& key);

// Fetches the icons of these home pages that are not kept, or were kept a
// month ago: one at a time, on a thread of their own. A site whose icon
// could not be had is not asked again before Win顺 starts again.
void fetch(const QStringList& homeUrls);

class Notifier : public QObject {
    Q_OBJECT
signals:
    void fetched(const QString& key); // a site's icon is kept now (GUI thread)
};
Notifier* notifier();

struct Fetched {
    QImage image; // as it would be kept
    QString from; // the address it came from
    QSize original; // its size as sent
    QStringList tried; // each address asked, and what came of it
};
// One site's icon read now, not kept; blocking (the thread above, wsbench).
Fetched fetchNow(const QString& homeUrl);

// ---- pictures users chose ------------------------------------------------
// A web shortcut's own icon (WebShortcut::icon), kept as PNG in custom\ of
// the folder, named after its content: the same picture twice is one file.

// A picture file's bytes (PNG or ICO, read as a site's are: siteicon::decode)
// kept as an icon; its name, or empty for anything else.
QString keepCustom(QByteArrayView bytes);
// As kept; null when not (or for a name that is no custom icon's).
QImage loadCustom(const QString& name);
QByteArray customPng(const QString& name); // its file as kept, to export
// Deletes those no shortcut uses any more.
void pruneCustom(const QStringList& inUse);

} // namespace ws::siteicons
