#include "ClipImportJob.h"

#include "ClipStore.h"

#include <QMetaObject>

#include <chrono>
#include <future>
#include <memory>

using namespace std::chrono_literals;

namespace ws {

ClipImportJob::ClipImportJob(ClipStore* store, QObject* parent)
    : QObject(parent)
    , m_store(store)
{
}

ClipImportJob::~ClipImportJob() = default; // the thread is told to stop, and joined

void ClipImportJob::start(const QString& path, clipimport::Split split)
{
    if (m_thread.joinable())
        return;
    m_group = m_store->uniqueGroupName(clipimport::groupName(path));
    m_thread = std::jthread([this, path, split](const std::stop_token& stop) {
        clipimport::Options options;
        options.split = split;
        clipimport::Hooks hooks;
        // The store is the GUI thread's: each batch goes there, and the
        // reading goes on when it is in.
        hooks.sink = [this, &stop](std::vector<ClipImportItem>&& items) {
            auto done = std::make_shared<std::promise<ClipImportResult>>();
            std::future<ClipImportResult> result = done->get_future();
            QMetaObject::invokeMethod(
                this,
                [this, done, items = std::move(items)]() mutable { done->set_value(m_store->importClips(std::move(items), m_group)); },
                Qt::QueuedConnection);
            while (result.wait_for(50ms) != std::future_status::ready) {
                if (stop.stop_requested())
                    return ClipImportResult {}; // going away: the batch is dropped with the object
            }
            return result.get();
        };
        hooks.cancelled = [this, &stop] { return m_cancelled.load() || stop.stop_requested(); };
        hooks.progress = [this](int done, int total) {
            QMetaObject::invokeMethod(this, [this, done, total] { emit progress(done, total); }, Qt::QueuedConnection);
        };
        const clipimport::Report report = clipimport::importFile(path, options, hooks);
        if (!stop.stop_requested()) {
            QMetaObject::invokeMethod(this, [this, report] { emit finished(report, m_group); }, Qt::QueuedConnection);
        }
    });
}

} // namespace ws
