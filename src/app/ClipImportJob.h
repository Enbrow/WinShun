#pragma once

#include "ClipImport.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <thread>

namespace ws {

class ClipStore;

// One import of a file into the clipboard history: the file is read on a
// thread of its own (Ditto's database can hold tens of thousands of entries),
// and each batch goes into the store on the GUI thread, where the store lives.
// Going away stops it.
class ClipImportJob : public QObject {
    Q_OBJECT

public:
    explicit ClipImportJob(ClipStore* store, QObject* parent = nullptr);
    ~ClipImportJob() override;

    // Not while one is running. The entries go in a group named after the file.
    void start(const QString& path, clipimport::Split split);
    void cancel() { m_cancelled = true; }
    QString group() const { return m_group; }

signals:
    void progress(int done, int total);
    void finished(const ws::clipimport::Report& report, const QString& group);

private:
    ClipStore* m_store;
    QString m_group;
    std::atomic<bool> m_cancelled {false};
    std::jthread m_thread;
};

} // namespace ws
