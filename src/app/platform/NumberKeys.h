#pragma once

#include <vector>

namespace ws::numberkeys {

// Ctrl+1…9 (or Alt+1…9) on Win顺's own search boxes reach them as the
// window's own keys, except the digits another program holds as global
// hotkeys (PixPin takes Ctrl+1 and up; Windows hands a global hotkey to its
// program before the window in front sees the key): those stay its, and
// their rows show the chord struck through.
//
// No low-level keyboard hook for them: in a process with keyboard Raw Input
// (KeyListener's), a hook set while one of its windows is in front is never
// called, and no hook set before it is either, AutoHotkey's included
// (GitHub #10, docs/pitfalls.md).

enum class Modifier { Ctrl, Alt };

// The digits 1…9 whose chord with `modifier` is a global hotkey already
// (RegisterHotKey refuses it: another program's, or one of Win顺's own),
// for the settings to name. Hooks other programs keep (AutoHotkey's) do
// not show. GUI thread.
std::vector<int> heldAsHotkeys(Modifier modifier);
// The same as bits: 1 << n for each digit n held.
unsigned heldMask(Modifier modifier);

} // namespace ws::numberkeys
