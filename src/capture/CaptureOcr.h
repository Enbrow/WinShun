#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace ws::capture {

// The words in a capture or a pin ("识别文字"), for the OCR window
// (OcrPanel.qml): read by PaddleOcr (PP-OCRv6 tiny, the clipboard's reader)
// on a thread of its own below normal priority, the window up at once
// saying so meanwhile. The reader stays loaded for a minute after the last
// picture (loading it takes ~0.1 s, keeping it ~40 MB), then goes.
//
// Text captured at 100% or 125% is made twice as large first (ocrUpscale);
// PaddleOcr itself pads thin strips and shrinks very wide pictures.
//
// One window, one picture at a time: a new one replaces what is shown and
// a result for an older one is dropped.
class CaptureOcr : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")

    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(QString text READ text NOTIFY changed FINAL) // the lines read, one per line
    Q_PROPERTY(QString message READ message NOTIFY changed FINAL) // none found, failed: why
    Q_PROPERTY(int round READ round NOTIFY changed FINAL) // counts up with each picture: the field takes the new text

public:
    explicit CaptureOcr(QObject* parent = nullptr);
    ~CaptureOcr() override;

    // The models are next to the program (cheap; nothing is loaded).
    static bool available();

    // Reads `image`, taken on a monitor at `dpr` (its text's size).
    void read(const QImage& image, double dpr);

    bool busy() const { return m_busy; }
    QString text() const { return m_text; }
    QString message() const { return m_message; }
    int round() const { return m_round; }

    Q_INVOKABLE void copyAll(const QString& text) { emit copyRequested(text); }
    // The text (the selection, else all of it) as one line, for the search box.
    Q_INVOKABLE void search(const QString& text);
    // The same one line, for the default web search.
    Q_INVOKABLE void webSearch(const QString& text);
    // Lines as words, trimmed, cut at what a search box takes; empty when no words.
    static QString oneLine(const QString& text);
    Q_INVOKABLE void close() { emit closeRequested(); }

signals:
    void changed();
    void copyRequested(const QString& text);
    void searchRequested(const QString& query);
    void webSearchRequested(const QString& query);
    void closeRequested();

private:
    struct Job {
        int round = 0;
        QImage image;
        double scale = 1.0;
    };
    void work(const std::stop_token& stop);
    void done(int round, const QString& text, const QString& error);

    bool m_busy = false;
    QString m_text;
    QString m_message;
    int m_round = 0;

    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::optional<Job> m_job;
    std::jthread m_thread; // last: started once the rest is there
};

} // namespace ws::capture
