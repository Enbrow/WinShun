#pragma once

#include <string>

// The service's log: %ProgramData%\WinShun\service.log (service.old.log
// once it passes 1 MB), or the console in --console mode. One line per
// event that matters (starts, stops, each MFT read, journals followed and
// lost, volumes let go of, refusals); never one per journal batch.
namespace ws::service::log {

void toConsole(); // --console: lines go to stderr instead of the file
// Make %ProgramData%\WinShun writable only by administrators / SYSTEM, once,
// where there are the rights to (at --install and at service start): an older
// version let it inherit ProgramData's user-writable ACL. Harmless otherwise.
void secureFolder();
void write(const wchar_t* format, ...);
// Start and stop failures go to the Windows event log too (source "WinShun").
void toEventLog(const std::wstring& message);

} // namespace ws::service::log
