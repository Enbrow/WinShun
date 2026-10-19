#pragma once

#include <QtGlobal>

namespace ws {

// What the user wants done to a system switch (a place with a switch id,
// places.txt "switch ="): a plain Enter on its row toggles it, a typed
// "关蓝牙" or "音量 30" says more. The search only says which; the app reads
// and writes the system.
struct SwitchIntent {
    enum class Kind : quint8 {
        Toggle, // the other state than the one shown
        On, // on / dark / shown / audible (the volume's switch is "sound on")
        Off,
        SetLevel, // volume: `value` 0–100 as typed (not clamped: the caller says "最大 100"); unmutes
        StepLevel, // volume: `value` more or less (±); a step up unmutes
        AwakeFor, // keep awake: `value` minutes
        AwakeUntil, // keep awake: `value` minutes after midnight (a time already past means tomorrow's)
        OpenSettings, // "蓝牙设置": the page, not the switch
        // Volume: to `value` (0–100) with the mute left as it is. Never typed:
        // a step down as the app works it out from the level shown
        // (Switches::resolve, Switches::stepped); a step up is a SetLevel.
        LevelOnly,
    };
    Kind kind = Kind::Toggle;
    int value = 0;

    friend bool operator==(const SwitchIntent&, const SwitchIntent&) = default;
};

} // namespace ws
