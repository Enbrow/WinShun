#include "WebSuggester.h"

#include "platform/Http.h"

#include <QDebug>
#include <QPointer>
#include <QUrl>

#include <thread>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr qsizetype kMaxReply = 256 * 1024; // a list of a few words; more is no suggestion reply

// Where answers come back to: made on the GUI thread (the first
// WebSuggester), never destroyed, so a thread finishing while Win顺 ends
// has something to post to.
QObject* relay()
{
    static auto* r = new QObject;
    return r;
}

// For the log (WINSHUN_LOG_INFO only): the site and path, the words left out.
QString siteOf(const QString& endpoint)
{
    const QUrl url(QString(endpoint).replace(u"%s"_s, u"x"_s));
    return url.host() + url.path();
}

} // namespace

WebSuggester::WebSuggester(QObject* parent)
    : QObject(parent)
{
    relay();
    m_clock.start();
    m_pause.setSingleShot(true);
    m_pause.setInterval(kPauseMs);
    connect(&m_pause, &QTimer::timeout, this, &WebSuggester::send);
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(kDeadlineMs);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (!m_waiting)
            return;
        qInfo().noquote() << u"Web suggestions: %1 took over %2 ms, none shown"_s.arg(siteOf(m_endpoint)).arg(kDeadlineMs);
        m_waiting = false;
        emit answered(m_endpoint, m_words, {});
    });
}

void WebSuggester::ask(const QString& endpoint, const QString& words)
{
    if (endpoint == m_endpoint && words == m_words && m_waiting)
        return; // asked already (the same text searched again: a refresh)
    ++m_generation;
    m_endpoint = endpoint;
    m_words = words;
    m_deadline.stop();
    if (const std::optional<QStringList> kept = m_cache.find(endpoint, words, m_clock.elapsed())) {
        m_pause.stop();
        m_waiting = false;
        emit answered(endpoint, words, *kept);
        return;
    }
    m_waiting = true;
    m_pause.start(); // again from now: typing goes on
}

void WebSuggester::cancel()
{
    ++m_generation;
    m_pause.stop();
    m_deadline.stop();
    m_waiting = false;
    m_endpoint.clear();
    m_words.clear();
}

void WebSuggester::clearCache()
{
    m_cache.clear();
    cancel();
    m_clearedAt = m_generation;
}

void WebSuggester::send()
{
    const quint64 generation = m_generation;
    const QString endpoint = m_endpoint;
    const QString words = m_words;
    const QString url = QString(endpoint).replace(u"%s"_s, QString::fromLatin1(QUrl::toPercentEncoding(words)));
    m_deadline.start();
    QPointer<WebSuggester> self(this);
    std::thread([self, generation, endpoint, words, url] {
        QElapsedTimer clock;
        clock.start();
        const http::Response r = http::get(url, {{u"Accept"_s, u"application/json, text/javascript, */*;q=0.5"_s}},
            kDeadlineMs, kMaxReply, http::Session::Shared);
        const bool ok = r.status == 200;
        const QStringList suggestions = ok ? websuggest::parse(websuggest::decodeReply(r.body, r.contentType), words)
                                           : QStringList();
        if (!ok) {
            qInfo().noquote() << u"Web suggestions: %1 failed (%2)"_s.arg(siteOf(endpoint),
                r.status != 0 ? u"HTTP %1"_s.arg(r.status) : u"error %1"_s.arg(r.error));
        }
        const qint64 elapsed = clock.elapsed();
        QMetaObject::invokeMethod(relay(), [self, generation, endpoint, words, ok, suggestions, elapsed] {
            if (self)
                self->receive(generation, endpoint, words, ok, suggestions, elapsed);
        }, Qt::QueuedConnection);
    }).detach();
}

void WebSuggester::receive(quint64 generation, const QString& endpoint, const QString& words, bool ok,
    const QStringList& suggestions, qint64 elapsedMs)
{
    // Kept even when late or superseded: typing back to these words finds them.
    // Not one asked before the cache was cleared: the shortcuts were changed since.
    if (ok && generation > m_clearedAt)
        m_cache.insert(endpoint, words, suggestions, m_clock.elapsed());
    if (generation != m_generation || !m_waiting)
        return; // another ask since, or this one already given up
    m_deadline.stop();
    m_waiting = false;
    qInfo().noquote() << u"Web suggestions: %1 \"%2\": %3 in %4 ms"_s.arg(siteOf(endpoint), words)
                             .arg(suggestions.size()).arg(elapsedMs);
    emit answered(endpoint, words, suggestions);
}

} // namespace ws
