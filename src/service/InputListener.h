#pragma once

#include <windows.h>
#include <cstddef>
#include <span>

namespace ws::service {
class Server;
struct Connection;
bool canStartInputListener();
void serveInputListener(Server& server, Connection& conn, std::span<const std::byte> payload);
// The same small, statically linked executable, in a user's session. No
// service dispatcher, volume access or pipe server is started in this mode.
int runInputListener(DWORD clientPid, HWND target);
} // namespace ws::service
