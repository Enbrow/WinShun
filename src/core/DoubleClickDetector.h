#pragma once

#include <cstdint>
#include <cstdlib>

namespace ws {

// Recognises a double click of the left button from raw button events, by
// Windows' own rules, so that it agrees with the window clicked on: the
// second press in the same window, within the double-click time of the
// first and within the double-click box around it (SM_CXDOUBLECLK ×
// SM_CYDOUBLECLK, centred on the first). A third press starts over. Another
// button or the wheel in between spoils the pair. Pure logic (no Win32), so
// it is unit-tested; KeyListener feeds it events.
class DoubleClickDetector {
public:
    struct Config {
        std::uint32_t maxGapMs = 500; // GetDoubleClickTime()
        int halfBoxX = 2; // half of SM_CXDOUBLECLK
        int halfBoxY = 2;
    };

    DoubleClickDetector() = default;
    explicit DoubleClickDetector(Config config)
        : m_config(config)
    {
    }

    // Returns true when the press completes a double click. `window`: the
    // one under the pointer (an HWND). `menuWasOpen`: Win顺's folder menu was
    // up when the press came (see firstClosedMenu()).
    bool leftDown(std::uint32_t timeMs, int x, int y, std::uintptr_t window, bool menuWasOpen) noexcept
    {
        const bool second = m_armed && window == m_window && timeMs - m_time <= m_config.maxGapMs
            && std::abs(x - m_x) <= m_config.halfBoxX && std::abs(y - m_y) <= m_config.halfBoxY;
        if (second) {
            m_armed = false;
            m_firstClosedMenu = m_menuWasOpen;
            return true;
        }
        m_armed = true;
        m_time = timeMs;
        m_x = x;
        m_y = y;
        m_window = window;
        m_menuWasOpen = menuWasOpen;
        return false;
    }

    // Another button went down, or the wheel turned.
    void otherInput() noexcept { m_armed = false; }

    // The first press of the double click just recognised came while the
    // menu was up: that press closed it (a click outside), and the pair
    // means "close", not "open again".
    bool firstClosedMenu() const noexcept { return m_firstClosedMenu; }

    void setConfig(Config config) noexcept { m_config = config; }

private:
    Config m_config;
    bool m_armed = false;
    bool m_menuWasOpen = false;
    bool m_firstClosedMenu = false;
    std::uint32_t m_time = 0;
    int m_x = 0;
    int m_y = 0;
    std::uintptr_t m_window = 0;
};

} // namespace ws
