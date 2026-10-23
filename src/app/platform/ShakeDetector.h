#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ws {

// Recognises the mouse shaken left and right while the primary button is
// held: a file being dragged, shaken to bring up the favorites' ring
// (DropPie). Pure logic (no Win32), so it is unit-tested; KeyListener feeds
// it the button and the pointer from Raw Input.
//
// It counts only once the pointer has left the drag rectangle around the
// press (SM_CXDRAG × SM_CYDRAG: a drag has begun). A swing is a run of the
// pointer one way; it turns when the pointer comes back a few pixels from
// the farthest point. It fires on `swings` swings in a row (→←→ or ←→←),
// each at least `minSwing` logical pixels long and mostly sideways, the
// turns at most `maxTurnGapMs` apart and the whole at most `maxTotalMs`. A
// first swing that began slowly (the file carried along before the shake)
// counts from where its last `minSwing` pixels began. It fires once per run
// of swings; swings that began within `rearmMs` of a shake do not count
// towards another, so a fresh shake is needed for the next.
class ShakeDetector {
public:
    struct Config {
        double minSwing = 16; // logical pixels
        double reversal = 6; // logical pixels back from the farthest point: a turn
        double sidewaysRatio = 1.5; // |dx| ≥ ratio × |dy| per swing
        std::uint32_t maxTurnGapMs = 350;
        std::uint32_t maxTotalMs = 900;
        std::uint32_t rearmMs = 600;
        int swings = 3;
    };

    ShakeDetector() = default;
    explicit ShakeDetector(Config config)
        : m_config(config)
    {
    }

    // The primary button went down at (x, y), physical pixels. `dragX`,
    // `dragY`: the drag rectangle's size (SM_CXDRAG, SM_CYDRAG at that
    // monitor's DPI); `scale`: that monitor's device pixels per logical pixel.
    void press(int x, int y, std::uint32_t timeMs, int dragX, int dragY, double scale) noexcept;
    // The pointer is at (x, y): true when this completes a shake.
    bool move(int x, int y, std::uint32_t timeMs) noexcept;
    void release() noexcept { m_pressed = false; }

    bool pressed() const noexcept { return m_pressed; }
    bool dragging() const noexcept { return m_dragging; } // past the drag rectangle

private:
    struct Sample {
        double x = 0;
        double y = 0;
        std::uint32_t time = 0;
    };

    // Whether the swing from m_start to `end` counts: the run's length and
    // start with it.
    struct Verdict {
        bool good = false;
        int count = 0;
        std::uint32_t runStart = 0;
    };
    Verdict judge(const Sample& end) const noexcept;
    void startSwing(const Sample& at) noexcept;
    bool sideways(const Sample& from, const Sample& to) const noexcept;
    std::uint32_t lastStretchStart(double extremeX) const noexcept;
    void remember(const Sample& s) noexcept;
    void resetRun() noexcept { m_count = 0; }

    Config m_config;
    double m_scale = 1;
    bool m_pressed = false;
    bool m_dragging = false;
    int m_downX = 0;
    int m_downY = 0;
    int m_dragX = 2;
    int m_dragY = 2;

    // The swing under way: where it began (the last turn), its direction
    // (+1 right, -1 left, 0 not yet known) and its farthest point.
    Sample m_start;
    Sample m_extreme;
    int m_direction = 0;

    // The run of good swings: how many, when the run began (the first
    // swing's last stretch), when the last turn was.
    int m_count = 0;
    std::uint32_t m_runStart = 0;
    std::uint32_t m_lastTurn = 0;
    bool m_fired = false;
    std::uint32_t m_firedAt = 0;

    // Recent points of this swing, a few logical pixels apart, to find where
    // its last minSwing pixels began.
    std::array<Sample, 64> m_trail {};
    std::size_t m_trailNext = 0;
    std::size_t m_trailSize = 0;
};

} // namespace ws
