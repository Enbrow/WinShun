#include "SiteIcons.h"

#include "Http.h"
#include "SiteIconImage.h"
#include "SvgIcon.h"
#include "WebShortcut.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <deque>
#include <thread>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws::siteicons {

namespace {

constexpr int kTimeoutMs = 10000;
constexpr qsizetype kMaxPage = 2 << 20; // a home page's first 2 MB: the icons are in its <head>
constexpr qsizetype kMaxIcon = 1 << 20;
constexpr int kMaxTries = 5; // browser icon addresses asked per site
constexpr int kMaxTouchTries = 2;
constexpr int kSharp = 64; // pixels an icon needs to look sharp in a row at 200 %
constexpr int kEnough = 32; // a browser icon this large is taken over a phones' icon
constexpr int kMinSide = 16;
constexpr int kKeptDays = 30;

// Never destroyed: the fetching thread may still be at it when Win顺 ends.
struct State {
    QMutex mutex;
    QString folder;
    QSet<QString> kept; // keys with a file
    QSet<QString> queued;
    QSet<QString> done; // fetched, or tried and failed, since Win顺 started
    std::deque<QString> homes;
    bool running = false;
    bool ended = false;
};

State& state()
{
    static auto* s = new State;
    return *s;
}

QString fileOf(const QString& folder, const QString& key)
{
    QString name = key;
    name.replace(u':', u'_'); // an IPv6 address
    return folder + u'\\' + name + u".png"_s;
}

QString outcome(const http::Response& r)
{
    return r.status != 0 ? u"HTTP %1, %2 bytes"_s.arg(r.status).arg(r.body.size()) : u"error %1"_s.arg(r.error);
}

// A picture as kept (before siteicon::normalized): PNG and ICO as a site's
// are read, SVG drawn at the size kept for a picture of the user's.
constexpr int kKeptSide = 256;
QImage decodeAny(QByteArrayView bytes, const QString& contentType = {})
{
    if (contentType.contains(u"svg"_s, Qt::CaseInsensitive) || svgicon::looksLikeSvg(bytes))
        return svgicon::render(bytes, kKeptSide);
    return siteicon::decode(bytes);
}

void work()
{
    State& s = state();
    for (;;) {
        QString home;
        QString folder;
        {
            QMutexLocker lock(&s.mutex);
            if (s.homes.empty() || s.ended) {
                s.running = false;
                return;
            }
            home = s.homes.front();
            s.homes.pop_front();
            folder = s.folder;
        }
        const QString key = siteIconKey(home);
        const Fetched f = fetchNow(home);
        bool saved = false;
        if (!f.image.isNull() && QDir().mkpath(folder)) {
            // Beside it first, then in its place: the icon provider may be reading it.
            const QString file = fileOf(folder, key);
            const QString part = file + u".part"_s;
            saved = f.image.save(part, "PNG") && (!QFile::exists(file) || QFile::remove(file)) && QFile::rename(part, file);
            if (!saved)
                QFile::remove(part);
        }
        QMutexLocker lock(&s.mutex);
        s.queued.remove(key);
        s.done.insert(key);
        if (saved)
            s.kept.insert(key);
        if (saved && !s.ended)
            QMetaObject::invokeMethod(notifier(), [key] { emit notifier()->fetched(key); }, Qt::QueuedConnection);
    }
}

} // namespace

void setFolder(const QString& folder)
{
    State& s = state();
    QMutexLocker lock(&s.mutex);
    s.folder = folder;
    s.kept.clear();
    for (const QFileInfo& f : QDir(folder).entryInfoList({u"*.png"_s}, QDir::Files))
        s.kept.insert(f.completeBaseName()); // "www.baidu.com"
}

void shutdown()
{
    State& s = state();
    QMutexLocker lock(&s.mutex);
    s.ended = true;
}

bool has(const QString& key)
{
    State& s = state();
    QMutexLocker lock(&s.mutex);
    return s.kept.contains(key);
}

QImage load(const QString& key)
{
    QString folder;
    {
        State& s = state();
        QMutexLocker lock(&s.mutex);
        folder = s.folder;
    }
    QImage image(fileOf(folder, key), "PNG");
    return image.isNull() ? image : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

void fetch(const QStringList& homeUrls)
{
    State& s = state();
    QMutexLocker lock(&s.mutex);
    if (s.folder.isEmpty() || s.ended)
        return;
    for (const QString& home : homeUrls) {
        const QString key = siteIconKey(home);
        if (key.isEmpty() || s.queued.contains(key) || s.done.contains(key))
            continue;
        if (s.kept.contains(key)) { // again after a month (still shown meanwhile)
            const QFileInfo file(fileOf(s.folder, key));
            if (file.lastModified().daysTo(QDateTime::currentDateTime()) < kKeptDays)
                continue;
        }
        s.queued.insert(key);
        s.homes.push_back(home);
    }
    if (!s.homes.empty() && !s.running) {
        s.running = true;
        std::thread(work).detach();
    }
}

Notifier* notifier()
{
    static auto* n = new Notifier; // made on the GUI thread first (App), never destroyed
    return n;
}

Fetched fetchNow(const QString& homeUrl)
{
    Fetched f;
    const http::Response page = http::get(homeUrl, {{u"Accept"_s, u"text/html,*/*;q=0.8"_s}}, kTimeoutMs, kMaxPage);
    f.tried.append(u"page %1: %2"_s.arg(homeUrl, outcome(page)));
    SiteIconCandidates candidates;
    if (page.status >= 200 && page.status < 400)
        candidates = siteIconCandidates(page.body, page.finalUrl);
    // Redirected elsewhere (a login, a visitor check): the site's own too.
    for (const QString& c : siteIconCandidates({}, homeUrl).icons) {
        if (!candidates.icons.contains(c))
            candidates.icons.append(c);
    }
    QImage best;
    int tries = 0;
    const auto tryOne = [&](const QString& address) {
        const http::Response r = http::get(address, {{u"Accept"_s, u"image/*"_s}}, kTimeoutMs, kMaxIcon);
        const QImage image = r.status == 200 ? decodeAny(r.body, r.contentType) : QImage();
        f.tried.append(u"%1: %2%3"_s.arg(address, outcome(r),
            image.isNull() ? u", not an icon read"_s : u", %1x%2"_s.arg(image.width()).arg(image.height())));
        if (image.isNull() || std::min(image.width(), image.height()) < kMinSide)
            return;
        if (best.isNull() || image.width() > best.width()) {
            best = image;
            f.from = address;
        }
    };
    // The browser's icon, as large as the site has it.
    for (const QString& address : std::as_const(candidates.icons)) {
        if (tries++ >= kMaxTries || (!best.isNull() && best.width() >= kSharp))
            break;
        tryOne(address);
    }
    // Only 16 px or none: the phones' icon is the better picture then.
    tries = 0;
    for (const QString& address : std::as_const(candidates.touchIcons)) {
        if (tries++ >= kMaxTouchTries || (!best.isNull() && best.width() >= kEnough))
            break;
        tryOne(address);
    }
    f.original = best.size();
    f.image = siteicon::normalized(best);
    return f;
}

namespace {

QString customFolder()
{
    State& s = state();
    QMutexLocker lock(&s.mutex);
    return s.folder.isEmpty() ? QString() : s.folder + u"\\custom"_s;
}

QString svgFile(const QString& folder, const QString& name) // "3f2a….png" -> "…\3f2a….svg"
{
    return folder + u'\\' + name.chopped(4) + u".svg"_s;
}

} // namespace

QString keepCustom(QByteArrayView bytes)
{
    if (bytes.size() > kMaxIcon)
        return {};
    return keepCustomImage(decodeAny(bytes));
}

QString keepCustomImage(const QImage& image)
{
    const QString folder = customFolder();
    if (folder.isEmpty() || image.isNull() || image.width() < kMinSide || image.height() < kMinSide)
        return {};
    // Sharp in the settings' rows and the launcher's at 200 % and more; an
    // SVG drawn at this size and then made smaller is drawn as sharp as at
    // the row's size (each pixel the average of the many it covers).
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !siteicon::normalized(image, kKeptSide).save(&buffer, "PNG"))
        return {};
    const QString name = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha1).toHex().left(16)) + u".png"_s;
    const QString file = folder + u'\\' + name;
    if (QFile::exists(file))
        return name; // that picture already
    QFile out(file + u".part"_s);
    if (!QDir().mkpath(folder) || !out.open(QIODevice::WriteOnly) || out.write(png) != png.size()) {
        out.remove();
        return {};
    }
    out.close();
    if (!out.rename(file)) {
        out.remove();
        return {};
    }
    return name;
}

QImage loadCustom(const QString& name)
{
    if (!isValidCustomIconName(name))
        return {};
    QImage image(customFolder() + u'\\' + name, "PNG");
    return image.isNull() ? image : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QByteArray customPng(const QString& name)
{
    if (!isValidCustomIconName(name))
        return {};
    QFile file(customFolder() + u'\\' + name);
    return file.open(QIODevice::ReadOnly) ? file.read(kMaxIcon) : QByteArray();
}

void keepCustomSvg(const QString& name, QByteArrayView svg)
{
    const QString folder = customFolder();
    if (folder.isEmpty() || !isValidCustomIconName(name) || svg.isEmpty() || svg.size() > svgicon::kMaxBytes)
        return;
    QSaveFile out(svgFile(folder, name));
    if (out.open(QIODevice::WriteOnly) && out.write(svg.data(), svg.size()) == svg.size())
        out.commit();
}

QString customSvg(const QString& name)
{
    const QString folder = customFolder();
    if (folder.isEmpty() || !isValidCustomIconName(name))
        return {};
    QFile file(svgFile(folder, name));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.read(svgicon::kMaxBytes)) : QString();
}

void pruneCustom(const QStringList& inUse)
{
    const QString folder = customFolder();
    if (folder.isEmpty())
        return;
    for (const QFileInfo& f : QDir(folder).entryInfoList({u"*.png"_s, u"*.svg"_s, u"*.part"_s}, QDir::Files)) {
        // SVG code goes with the picture drawn from it.
        const QString picture = f.suffix() == u"svg"_s ? f.completeBaseName() + u".png"_s : f.fileName();
        if (!inUse.contains(picture))
            QFile::remove(f.filePath());
    }
}

} // namespace ws::siteicons
