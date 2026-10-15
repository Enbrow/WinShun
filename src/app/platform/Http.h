#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <utility>

namespace ws::http {

struct Response {
    int status = 0; // HTTP status; 0 when no answer came
    QByteArray body;
    unsigned long error = 0; // Win32 / WinHTTP error code when status is 0
    QString finalUrl; // where the redirects led
    QString contentType; // as sent ("text/javascript; charset=gbk"); empty when none
};

// Own: a WinHTTP session per call, which pays for its connection and TLS
// handshake each time (an icon, an update check: now and then). Shared: one
// session kept for the life of Win顺, whose connections stay open between
// calls: search suggestions ask the same host at every pause in typing, and
// a reused connection answers in tens of milliseconds instead of hundreds.
enum class Session { Own, Shared };

// A blocking GET through WinHTTP: the system's TLS and proxy settings (a
// proxy many users need to reach GitHub), and no TLS plugin for Qt Network
// to ship. Redirects are followed. Run it off the GUI thread. The body stops
// at `maxBytes` (0: no limit). `timeoutMs` is each step's (resolving,
// connecting, sending, waiting for the answer), not the whole call's.
Response get(const QString& url, const QList<std::pair<QString, QString>>& headers, int timeoutMs = 20000,
    qsizetype maxBytes = 0, Session session = Session::Own);

} // namespace ws::http
