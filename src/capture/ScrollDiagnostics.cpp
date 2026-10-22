#include "ScrollDiagnostics.h"

using namespace Qt::StringLiterals;

namespace ws::capture {

ScrollDiagnostics::ScrollDiagnostics(int capacity)
    : m_capacity(std::max(1, capacity))
{
}

void ScrollDiagnostics::begin(const Facts& facts)
{
    m_facts = facts;
    m_events.clear();
    m_total = 0;
    m_frames = 0;
    m_states.clear();
    m_predicted = 0;
    m_scrolls.clear();
    m_end.clear();
    m_height = m_top = m_bottom = 0;
    m_partial = false;
    m_ms = 0;
}

void ScrollDiagnostics::event(qint64 ms, const QString& line)
{
    m_events.push_back(u"%1 ms  %2"_s.arg(ms, 6).arg(line));
    while (int(m_events.size()) > m_capacity)
        m_events.pop_front();
    ++m_total;
}

void ScrollDiagnostics::scrolled(qint64 ms, const ScrollResult& result, double amount)
{
    ++m_scrolls[methodName(result.method) + u' ' + codeName(result.code)].first;
    QString line = u"scroll: %1 by %2: %3"_s.arg(methodName(result.method)).arg(amount, 0, 'f', 2).arg(codeName(result.code));
    if (result.detail)
        line += u" (%1)"_s.arg(result.detail);
    event(ms, line);
}

void ScrollDiagnostics::moved(qint64 ms, ScrollMethod method, int rows)
{
    if (rows != 0)
        ++m_scrolls[methodName(method) + u' ' + codeName(ScrollOk)].second;
    event(ms, rows ? u"moved %1 px"_s.arg(rows) : u"did not move"_s);
}

void ScrollDiagnostics::stitched(qint64 ms, const ScrollStitcher::Step& step, int took)
{
    ++m_frames;
    ++m_states[step.state];
    m_predicted += step.predicted;
    QString line = u"frame %1: %2"_s.arg(m_frames).arg(stateName(step.state));
    if (step.state == ScrollStitcher::State::Appended || step.state == ScrollStitcher::State::Behind || step.predicted)
        line += u", dy %1, score %2, votes %3"_s.arg(step.dy).arg(step.score, 0, 'f', 3).arg(step.votes);
    if (step.predicted)
        line += u", guessed"_s;
    line += u", %1 px high, bands %2/%3, %4 ms"_s.arg(step.height).arg(step.top).arg(step.bottom).arg(took);
    event(ms, line);
    // The result so far, for a copy taken while it runs (ended() sets the last).
    m_height = step.height;
    m_top = step.top;
    m_bottom = step.bottom;
    m_partial = m_partial || step.predicted;
}

void ScrollDiagnostics::ended(qint64 ms, const QString& how, int height, int top, int bottom, bool partial)
{
    m_end = how;
    m_height = height;
    m_top = top;
    m_bottom = bottom;
    m_partial = partial;
    m_ms = ms;
    event(ms, u"end: %1, %2 px high%3"_s.arg(how).arg(height).arg(partial ? u", partly guessed"_s : QString()));
}

QStringList ScrollDiagnostics::lines() const
{
    return QStringList(m_events.begin(), m_events.end());
}

QString ScrollDiagnostics::text(int lastEvents) const
{
    QStringList out;
    out << u"WinShun long screenshot diagnostics"_s;
    out << u"WinShun %1, Windows %2"_s.arg(m_facts.version, m_facts.windows);
    out << u"program: %1%2"_s.arg(m_facts.program.isEmpty() ? u"(none found)"_s : m_facts.program,
        m_facts.elevated ? u" (runs as administrator)"_s : QString());
    out << u"region: %1 x %2 px at %3 %, %4; right %5 px left out of the comparing; auto-scroll %6"_s
               .arg(m_facts.region.width())
               .arg(m_facts.region.height())
               .arg(qRound(m_facts.scale * 100))
               .arg(m_facts.hdr ? u"HDR"_s : u"SDR"_s)
               .arg(m_facts.ignoreRight)
               .arg(m_facts.automatic ? u"on"_s : u"off"_s);
    if (!m_end.isEmpty())
        out << u"ended: %1 after %2 s"_s.arg(m_end).arg(m_ms / 1000.0, 0, 'f', 1);
    QStringList states;
    for (const auto& [state, n] : m_states)
        states << u"%1 %2"_s.arg(stateName(state)).arg(n);
    out << u"frames %1: %2%3"_s.arg(m_frames)
               .arg(states.isEmpty() ? u"none"_s : states.join(u", "_s))
               .arg(m_predicted ? u"; %1 guessed"_s.arg(m_predicted) : QString());
    out << u"result%1: %2 px high, fixed top %3 px, bottom %4 px%5"_s.arg(m_end.isEmpty() ? u" so far"_s : QString())
               .arg(m_height).arg(m_top).arg(m_bottom)
               .arg(m_partial ? u", partly guessed"_s : QString());
    QStringList scrolls;
    for (const auto& [what, n] : m_scrolls)
        scrolls << u"%1 x%2 (moved %3)"_s.arg(what).arg(n.first).arg(n.second);
    out << u"scrolling: %1"_s.arg(scrolls.isEmpty() ? u"none (manual)"_s : scrolls.join(u", "_s));
    const int shown = std::min<int>(std::max(0, lastEvents), int(m_events.size()));
    out << u"events (the last %1 of %2):"_s.arg(shown).arg(m_total);
    for (auto it = m_events.end() - shown; it != m_events.end(); ++it)
        out << u"  "_s + *it;
    return out.join(u'\n') + u'\n';
}

} // namespace ws::capture
