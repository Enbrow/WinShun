#pragma once

#include <QObject>
#include <QString>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace ws {

// The words in pictures, read by Windows' own text recognition
// (Windows.Media.Ocr, part of a language's optional features: Chinese,
// English...), for searching the clipboard history by them (GitHub #9). On a
// thread of its own below normal priority, one picture after another; the
// recognizer is let go when nothing is left to read.
//
// It reads in the first East Asian language of the user's list that it has
// a recognizer for (those read Latin letters too, not the other way round),
// else in the user's languages as Windows picks. A Chinese line comes back
// as one word per character; the words are put together again without the
// spaces between them.
class TextRecognizer : public QObject {
    Q_OBJECT

public:
    explicit TextRecognizer(QObject* parent = nullptr);
    ~TextRecognizer() override; // what is queued is dropped

    // `done` later; `next`: before what is queued (just copied, the older
    // ones after).
    void read(qint64 id, const QString& imageFile, bool next = false);

signals:
    // From the thread: queued to the receiver. `ok` false: no recognizer
    // (no language with one installed) or recognition failed, to be tried
    // again another time. A file that cannot be loaded has no words.
    void done(qint64 id, const QString& words, bool ok);

private:
    struct Job {
        qint64 id = 0;
        QString file;
    };
    void work(const std::stop_token& stop);

    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<Job> m_jobs;
    std::jthread m_thread; // last: started once the rest is there
};

} // namespace ws
