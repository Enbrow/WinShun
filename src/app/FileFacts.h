#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QtQml/qqmlregistration.h>

namespace ws {

// What the index does not keep about a file or folder, asked of the disk off
// the GUI thread for the tip over a row whose name is cut (NameTip.qml): when
// it was created. The index has its size and when it was last written.
// Network paths are not asked (a server that does not answer would hold a
// worker for half a minute); answers are kept for the session.
class FileFacts : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit FileFacts(QObject* parent = nullptr);
    ~FileFacts() override;

    // created() for `path`: at once when known, else once read (not at all
    // when it cannot be).
    Q_INVOKABLE void ask(const QString& path);

signals:
    // "2026-10-15 14:32", local time.
    void created(const QString& path, const QString& when);

private:
    void answer(const QString& path, const QString& when);

    QThreadPool m_pool;
    QHash<QString, QString> m_known;
    QSet<QString> m_asking;
};

} // namespace ws
