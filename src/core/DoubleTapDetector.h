#pragma once

#include <cstdint>
#include <cstdlib>

namespace ws {

// Recognises "tap Ctrl twice" from raw key events. Pure logic (no Win32), so
// it is unit-tested; KeyListener feeds it events.
//
// A tap is a Ctrl press and release with no other key, mouse button or wheel
// in between, shorter than maxPressMs, and with no Shift, Alt or Win held at
// the press. Two taps of the same Ctrl within maxGapMs trigger, unless the
// mouse moved in between. The exceptions are Ctrl used with the mouse, not a
// double tap: Ctrl+click multi-selection (the same spot twice, or one row
// and the next), Ctrl+wheel zooming, and a touchpad pinch, which Windows
// sends as Ctrl+wheel.
//
// Keys come as the left or right one (VK_LCONTROL, VK_RSHIFT…); VK_CONTROL,
// VK_SHIFT and VK_MENU count as the left one.
class DoubleTapDetector {
public:
    enum class Side : std::uint8_t {
        Both, // either Ctrl, both taps on the same one
        Left,
        Right,
    };

    struct Config {
        std::uint32_t maxPressMs = 300;
        std::uint32_t maxGapMs = 400;
        int maxMovePx = 12;
        // Longer than a keyboard waits before it repeats a held key (1 s at
        // most) or between repeats: a press after this much quiet is a new
        // one, and the release of the last was lost.
        std::uint32_t lostReleaseMs = 1500;
        Side side = Side::Both;
        // Whether a key is down now (GetAsyncKeyState), for a Shift, Alt or
        // Win whose release may have been lost; none: as the events say.
        bool (*isDown)(std::uint32_t vk) = nullptr;
    };

    DoubleTapDetector() = default;
    explicit DoubleTapDetector(Config config)
        : m_config(config)
    {
    }

    static constexpr bool isCtrl(std::uint32_t vk) noexcept
    {
        return vk == kCtrl || vk == kLeftCtrl || vk == kRightCtrl;
    }

    // Both return true when the event completes a double tap.
    bool keyDown(std::uint32_t vk, std::uint32_t timeMs, int x, int y) noexcept
    {
        if (const int m = modifierOf(vk); m >= 0) {
            m_modifierDown[m] = true;
            m_modifierTime[m] = timeMs; // repeats too
        }
        if (!counts(vk)) {
            m_clean = false;
            m_armed = false;
            return false;
        }
        vk = leftOrRight(vk);
        if (m_down && vk != m_downKey) { // both Ctrl keys at once
            m_clean = false;
            m_armed = false;
            return false;
        }
        // Auto-repeat while held. Unless the release was never seen: it went
        // to the secure desktop (Ctrl+Alt+Del), or a program's hook ate it.
        const bool repeat = m_down && timeMs - m_lastDownTime <= m_config.lostReleaseMs;
        m_lastDownTime = timeMs;
        if (repeat)
            return false;
        m_down = true;
        m_downKey = vk;
        // Ctrl with Alt held is part of a chord, or another program's doing:
        // a hotkey tool taps Ctrl so that letting go of Alt will not open a
        // window's menu bar (AutoHotkey's #MenuMaskKey), once per hotkey.
        m_clean = !modifierHeld(timeMs);
        m_downTime = timeMs;
        m_downX = x;
        m_downY = y;
        if (m_armed && (timeMs - m_firstUpTime > m_config.maxGapMs || !m_clean))
            m_armed = false;
        return false;
    }

    bool keyUp(std::uint32_t vk, std::uint32_t timeMs, int x, int y) noexcept
    {
        if (const int m = modifierOf(vk); m >= 0)
            m_modifierDown[m] = false;
        if (!counts(vk) || !m_down || leftOrRight(vk) != m_downKey)
            return false;
        m_down = false;
        const bool tap = m_clean && timeMs - m_downTime <= m_config.maxPressMs && closeTo(x, y, m_downX, m_downY);
        if (!tap) {
            m_armed = false;
            return false;
        }
        // A tap of the other Ctrl starts over from this one.
        if (m_armed && m_firstKey == m_downKey && closeTo(m_downX, m_downY, m_firstX, m_firstY)) {
            m_armed = false;
            return true;
        }
        m_armed = true;
        m_firstKey = m_downKey;
        m_firstUpTime = timeMs;
        m_firstX = m_downX;
        m_firstY = m_downY;
        return false;
    }

    // A mouse button went down or is held, or the wheel turned: the Ctrl held
    // now, or just tapped, was for a click, a drag or zooming.
    void mouseUsed() noexcept
    {
        m_clean = false;
        m_armed = false;
    }

    // The second tap may come as late as this after the first.
    void setMaxGap(std::uint32_t ms) noexcept { m_config.maxGapMs = ms; }
    void setSide(Side side) noexcept { m_config.side = side; }

    void reset() noexcept { *this = DoubleTapDetector(m_config); }

private:
    static constexpr std::uint32_t kCtrl = 0x11, kLeftCtrl = 0xA2, kRightCtrl = 0xA3;

    static constexpr std::uint32_t leftOrRight(std::uint32_t ctrl) noexcept
    {
        return ctrl == kCtrl ? kLeftCtrl : ctrl;
    }

    // 0 Shift, 1 Alt, 2 Win; -1 any other key.
    static constexpr int modifierOf(std::uint32_t vk) noexcept
    {
        switch (vk) {
        case 0x10: // VK_SHIFT
        case 0xA0: // VK_LSHIFT
        case 0xA1: // VK_RSHIFT
            return 0;
        case 0x12: // VK_MENU
        case 0xA4: // VK_LMENU
        case 0xA5: // VK_RMENU
            return 1;
        case 0x5B: // VK_LWIN
        case 0x5C: // VK_RWIN
            return 2;
        default:
            return -1;
        }
    }

    // The Ctrl keys this listens to; any other is "another key".
    bool counts(std::uint32_t vk) const noexcept
    {
        switch (m_config.side) {
        case Side::Left:
            return vk == kCtrl || vk == kLeftCtrl;
        case Side::Right:
            return vk == kRightCtrl;
        case Side::Both:
            break;
        }
        return isCtrl(vk);
    }

    // As the events say, in their order: by the time an event is looked at,
    // a program that sent a whole sequence may have let go of Alt already.
    // A release seen by no one (Win+L, Ctrl+Alt+Del: it went to the secure
    // desktop) is noticed once the key has been quiet a while and is up.
    bool modifierHeld(std::uint32_t timeMs) noexcept
    {
        static constexpr std::uint32_t kQueryKey[] {0x10, 0x12, 0x5B};
        bool held = false;
        for (int m = 0; m < 3; ++m) {
            if (!m_modifierDown[m])
                continue;
            if (timeMs - m_modifierTime[m] <= m_config.lostReleaseMs || !m_config.isDown
                || m_config.isDown(kQueryKey[m]) || (m == 2 && m_config.isDown(0x5C)))
                held = true;
            else
                m_modifierDown[m] = false;
        }
        return held;
    }

    bool closeTo(int x1, int y1, int x2, int y2) const noexcept
    {
        return std::abs(x1 - x2) <= m_config.maxMovePx && std::abs(y1 - y2) <= m_config.maxMovePx;
    }

    Config m_config;
    bool m_down = false;
    bool m_clean = false;
    bool m_armed = false;
    bool m_modifierDown[3] {};
    std::uint32_t m_modifierTime[3] {}; // the last press or repeat
    std::uint32_t m_downKey = 0; // VK_LCONTROL or VK_RCONTROL
    std::uint32_t m_firstKey = 0;
    std::uint32_t m_downTime = 0;
    std::uint32_t m_lastDownTime = 0; // repeats included
    std::uint32_t m_firstUpTime = 0;
    int m_downX = 0;
    int m_downY = 0;
    int m_firstX = 0;
    int m_firstY = 0;
};

} // namespace ws
