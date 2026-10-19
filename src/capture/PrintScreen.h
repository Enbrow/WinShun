#pragma once

#include <QString>

#include <windows.h>

#include <optional>

// Taking the PrtScn key over from the Snipping Tool (docs/design-capture.md,
// 4.5). Windows 11 gives PrtScn to the Snipping Tool while
// HKCU\Control Panel\Keyboard\PrintScreenKeyForSnippingEnabled is not 0;
// at 0 a RegisterHotKey on it gets WM_HOTKEY. The value is read when the
// keyboard settings change, so every write is followed by WM_SETTINGCHANGE
// for "Control Panel\Keyboard": without it (stage 0, capture-probe prtscn)
// the hotkey is not delivered and PrtScn copies the whole screen to the
// clipboard instead, as before Windows 11.
namespace ws::printscreen {

// The value as it is: none when absent.
std::optional<DWORD> read();
// Writes the value (none: deletes it), then tells everyone.
bool write(std::optional<DWORD> value);
void broadcast();

// The value as kept in WinShun.ini to be put back: "absent" or the number.
QString encode(std::optional<DWORD> value);
// Back from that; false when the text is neither.
bool decode(const QString& text, std::optional<DWORD>* value);

} // namespace ws::printscreen
