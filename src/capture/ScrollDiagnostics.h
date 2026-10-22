#pragma once

#include "ScrollInput.h"
#include "ScrollStitcher.h"

#include <QSize>
#include <QString>
#include <QStringList>

#include <deque>
#include <map>

// What the long screenshot keeps to say what happened (docs/design-capture-
// roadmap.md, 4.2, 诊断): the last 2000 events in memory, counts, and the
// text "复制诊断信息" puts on the clipboard and the diagnostics log gets.
// Only sizes, counts and the program's exe name: never pixels, never a
// window's title.
namespace ws::capture {

class ScrollDiagnostics {
public:
    static constexpr int kCapacity = 2000;

    struct Facts {
        QString windows; // "10.0.26200"
        QString version; // WinShun's
        QString program; // the exe name under the region
        bool elevated = false; // that program runs with higher rights than ours
        QSize region; // physical px
        double scale = 1; // the monitor's
        bool hdr = false;
        int ignoreRight = 0; // px left out of the comparing
        bool automatic = true; // auto-scroll on at the start
    };

    explicit ScrollDiagnostics(int capacity = kCapacity);

    void begin(const Facts& facts);
    // One event: `ms` since the start, a short line (no titles, no text from the screen).
    void event(qint64 ms, const QString& line);
    // A step's attempt; `amount`: wheel notches, or the part of the view for UI Automation.
    void scrolled(qint64 ms, const ScrollResult& result, double amount);
    // What the frames said the step before did: rows the content moved (0: nothing).
    void moved(qint64 ms, ScrollMethod method, int rows);
    // A frame the stitcher took and what it said; `took`: ms it took.
    void stitched(qint64 ms, const ScrollStitcher::Step& step, int took);
    void ended(qint64 ms, const QString& how, int height, int top, int bottom, bool partial);

    int frames() const { return m_frames; }
    qint64 events() const { return m_total; }
    QStringList lines() const;
    // Everything above as one text; at most `lastEvents` of the events.
    QString text(int lastEvents = 400) const;

private:
    int m_capacity;
    Facts m_facts;
    std::deque<QString> m_events;
    qint64 m_total = 0;
    int m_frames = 0;
    std::map<ScrollStitcher::State, int> m_states;
    int m_predicted = 0;
    // "post wheel ok": [sent, moved]
    std::map<QString, std::pair<int, int>> m_scrolls;
    QString m_end;
    int m_height = 0;
    int m_top = 0;
    int m_bottom = 0;
    bool m_partial = false;
    qint64 m_ms = 0;
};

} // namespace ws::capture
