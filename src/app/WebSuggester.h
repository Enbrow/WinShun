#pragma once

#include "WebSuggest.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace ws {

// Asks a web shortcut's suggestion address (WebShortcut::suggest) for the
// words typed after its keyword, for the launcher's suggestion rows.
//
// Never on the GUI thread, never in the way of typing: an ask waits for a
// pause (kPauseMs; a word in the cache is answered at once), then goes out
// on a thread of its own (a slow site never holds up the next ask) through
// one WinHTTP session kept open (http::Session::Shared). Each ask replaces
// the one before: only the latest's answer is told; one in flight is left
// to finish and its answer dropped (a blocking WinHTTP call cannot be
// stopped cheaply). An answer later than kDeadlineMs counts as none.
// Failures say nothing: no rows, no message.
class WebSuggester : public QObject {
    Q_OBJECT

public:
    static constexpr int kPauseMs = 150;
    static constexpr int kDeadlineMs = 1500;

    explicit WebSuggester(QObject* parent = nullptr);

    // `endpoint` with %s (WebShortcut::suggest), `words` as typed after the keyword.
    void ask(const QString& endpoint, const QString& words);
    void cancel(); // nothing is asked any more; an answer on its way is dropped
    // The shortcuts changed (an address edited, added, removed): nothing
    // kept is used again, and an answer on its way is dropped.
    void clearCache();

signals:
    // The latest ask's answer: the suggestions, none when the site has none
    // for the words, could not be reached, or took too long.
    void answered(const QString& endpoint, const QString& words, const QStringList& suggestions);

private:
    void send();
    void receive(quint64 generation, const QString& endpoint, const QString& words, bool ok,
        const QStringList& suggestions, qint64 elapsedMs);

    websuggest::Cache m_cache;
    QElapsedTimer m_clock; // the cache's time
    QTimer m_pause;
    QTimer m_deadline;
    quint64 m_generation = 0; // of the latest ask: answers to earlier ones are dropped
    quint64 m_clearedAt = 0; // m_generation when the cache was last cleared: earlier answers are not kept
    QString m_endpoint; // the latest ask
    QString m_words;
    bool m_waiting = false; // for the latest ask's answer (paused or in flight)
};

} // namespace ws
