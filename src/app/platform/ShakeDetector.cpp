#include "ShakeDetector.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ws {

namespace {

// Points of a swing closer together than this are not all kept: a gaming
// mouse reports a thousand a second, and the trail only has to tell where
// the last few pixels of a swing began.
constexpr double kTrailStep = 2; // logical pixels

// Time from `from` to `to` on the input's clock, which wraps every 49 days.
std::uint32_t since(std::uint32_t from, std::uint32_t to) noexcept
{
    return to - from;
}

} // namespace

void ShakeDetector::press(int x, int y, std::uint32_t timeMs, int dragX, int dragY, double scale) noexcept
{
    m_pressed = true;
    m_dragging = false;
    m_downX = x;
    m_downY = y;
    m_dragX = std::max(dragX, 1);
    m_dragY = std::max(dragY, 1);
    m_scale = scale > 0 ? scale : 1;
    m_direction = 0;
    m_count = 0;
    m_fired = false; // once per gesture: a new press is a new one
    m_trailSize = 0;
    m_start = m_extreme = {x / m_scale, y / m_scale, timeMs};
}

bool ShakeDetector::move(int x, int y, std::uint32_t timeMs) noexcept
{
    if (!m_pressed)
        return false;
    const Sample s {x / m_scale, y / m_scale, timeMs};
    if (!m_dragging) {
        // Windows starts a drag once the pointer leaves the rectangle centred
        // on the press (DragDetect): before that it is a click, not a shake.
        if (std::abs(x - m_downX) * 2 <= m_dragX && std::abs(y - m_downY) * 2 <= m_dragY)
            return false;
        m_dragging = true;
        startSwing(s);
        return false;
    }
    if (m_direction == 0) {
        if (std::abs(s.x - m_start.x) < m_config.reversal)
            return false;
        m_direction = s.x > m_start.x ? 1 : -1;
    }
    const double ahead = (s.x - m_extreme.x) * m_direction;
    if (ahead > 0) {
        m_extreme = s; // farther along the swing
        remember(s);
    } else if (-ahead >= m_config.reversal) {
        // Turned at the farthest point: that swing is done.
        const Verdict v = judge(m_extreme);
        if (v.good) {
            m_count = v.count;
            m_runStart = v.runStart;
            m_lastTurn = m_extreme.time;
        } else {
            resetRun();
        }
        const Sample turn = m_extreme;
        startSwing(turn);
        m_direction = s.x > turn.x ? 1 : -1;
        m_extreme = s;
        remember(s);
    } else {
        return false; // a little back from the farthest point: not a turn yet
    }
    // The swing under way may already complete the shake: no need to wait
    // for its turn.
    const Verdict v = judge(m_extreme);
    if (!v.good || v.count < m_config.swings)
        return false;
    m_fired = true;
    m_firedAt = timeMs;
    resetRun();
    return true;
}

ShakeDetector::Verdict ShakeDetector::judge(const Sample& end) const noexcept
{
    if (std::abs(end.x - m_start.x) < m_config.minSwing || !sideways(m_start, end))
        return {};
    int count = m_count;
    std::uint32_t runStart = m_runStart;
    // The run broke: too long since the last turn, or since it began. This
    // swing may begin a new one.
    if (count > 0
        && (since(m_lastTurn, end.time) > m_config.maxTurnGapMs || since(runStart, end.time) > m_config.maxTotalMs))
        count = 0;
    if (count == 0) {
        runStart = lastStretchStart(end.x);
        if (since(runStart, end.time) > m_config.maxTurnGapMs)
            return {};
    }
    // Swings that began soon after a shake are its tail, not a new one.
    if (m_fired && static_cast<std::int32_t>(since(m_firedAt, runStart)) < static_cast<std::int32_t>(m_config.rearmMs))
        return {};
    return {true, count + 1, runStart};
}

void ShakeDetector::startSwing(const Sample& at) noexcept
{
    m_start = m_extreme = at;
    m_direction = 0;
    m_trailSize = 0;
    m_trailNext = 0;
    remember(at);
}

bool ShakeDetector::sideways(const Sample& from, const Sample& to) const noexcept
{
    return std::abs(to.x - from.x) >= m_config.sidewaysRatio * std::abs(to.y - from.y);
}

// When the pointer was last minSwing short of `extremeX`, along this swing:
// a swing that went slowly at first (the file carried somewhere) counts
// from there.
std::uint32_t ShakeDetector::lastStretchStart(double extremeX) const noexcept
{
    std::uint32_t oldest = m_start.time;
    for (std::size_t i = 0; i < m_trailSize; ++i) {
        const Sample& p = m_trail[(m_trailNext + m_trail.size() - 1 - i) % m_trail.size()];
        if (std::abs(extremeX - p.x) >= m_config.minSwing)
            return p.time;
        oldest = p.time;
    }
    return oldest;
}

void ShakeDetector::remember(const Sample& s) noexcept
{
    if (m_trailSize > 0) {
        const Sample& last = m_trail[(m_trailNext + m_trail.size() - 1) % m_trail.size()];
        if (std::abs(s.x - last.x) < kTrailStep)
            return;
    }
    m_trail[m_trailNext] = s;
    m_trailNext = (m_trailNext + 1) % m_trail.size();
    m_trailSize = std::min(m_trailSize + 1, m_trail.size());
}

} // namespace ws
