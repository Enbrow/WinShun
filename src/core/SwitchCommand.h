#pragma once

#include "SwitchIntent.h"
#include "SystemCatalog.h"

#include <QStringView>

#include <cstddef>
#include <optional>

namespace ws {

// What a query asks of one system switch ("关蓝牙", "音量 30", "保持唤醒 到 18:00",
// "蓝牙设置"): which place of the list, and what to do to it.
struct SwitchCommand {
    std::size_t place = 0; // into the PlaceList: a place with a switchId
    SwitchIntent intent;
    // A verb, a number, a time or "设置" was typed: the row goes on top. A
    // state word alone ("深色", "静音") is an ordinary row, ranked as any
    // keyword, that only knows what Enter then does.
    bool onTop = true;
    friend bool operator==(const SwitchCommand&, const SwitchCommand&) = default;
};

// Whether `text` (as typed) asks something of exactly one switch. Nothing
// else is eaten: after the verb, the number, the time or "设置" is taken off,
// what is left must be one switch's name, keyword or state word, whole. The
// switch alone ("蓝牙") is not a command (an ordinary row, Enter toggles it),
// and neither are "关于", "关机", "关闭防火墙" or "关蓝牙和wifi". Pure, and
// made for every keystroke: no regular expressions built, no system asked.
[[nodiscard]] std::optional<SwitchCommand> parseSwitchCommand(QStringView text, const PlaceList& places);

} // namespace ws
