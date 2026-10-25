#pragma once

#include "Crawler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// Reading network shares: folders, whether a server answers at all, and
// what went wrong in words a user understands.
namespace ws::share {

// What stands between us and a share. The order is how bad it is to the
// user, from not at all to a fault on the server.
enum class Problem : std::uint8_t {
    None,
    Offline, // the server does not answer (off, another network, VPN down)
    Dropped, // the connection broke off while reading
    NotFound, // the server answers, the share is not there
    NeedsSignIn, // Windows has no user name and password for it (or wrong ones)
    GuestBlocked, // the server allows guests only, which Windows 11 refuses
    Signing, // the server cannot sign, which Windows 11 requires
    Smb1Only, // an old device that speaks only SMB 1, which Windows no longer does
    Denied, // a folder this user may not read: skipped, not a problem with the share
    Other,
    WslStopped,
    WslNotRegistered,
    WslUnavailable,
};

// `atRoot`: the share itself (or the folder added) could not be opened;
// "access denied" there means signing in, inside it a folder kept from us.
Problem classify(unsigned long error, bool atRoot) noexcept;
// Problems with the server or share as a whole: a pass stops at them.
bool affectsShare(Problem problem) noexcept;

// What answers on the SMB port (445) of a server.
enum class Answer : std::uint8_t {
    None, // nothing (off, another network), or not SMB
    Smb2, // a server Windows can use
    Smb1Only, // an old one: Windows 11 no longer connects to it (unless SMB 1 was installed again)
};

// Asks `host` within `timeout`, so an offline server costs this much rather
// than the 20-45 seconds Windows spends on it. Remembered for a while
// (shorter when nothing answered); `fresh` asks again.
Answer probe(std::wstring_view host, std::chrono::milliseconds timeout = std::chrono::milliseconds(1500),
    bool fresh = false);
// Whether an SMB server answers on `host` (probe()).
inline bool reachable(std::wstring_view host, std::chrono::milliseconds timeout = std::chrono::milliseconds(1500),
    bool fresh = false)
{
    return probe(host, timeout, fresh) != Answer::None;
}

// Background only: WSL checks its running state without touching its filesystem;
// SMB checks its server. Unlike reachable(), this takes the complete path.
bool reachablePath(std::wstring_view path, bool fresh = false);

// The folders `host` shares (NetShareEnum): disk shares only, without the
// hidden ones ("C$"). Slow when the server is: call after reachable().
// `error` is why the server refused, where NetShareEnum does not say (it
// calls a guest refused "not found").
std::vector<std::wstring> listShares(std::wstring_view host, unsigned long* error = nullptr);

// Reads folders on a share for Crawler::sync, on any number of threads.
// File names are filtered by `excludedFiles` ("._*", ".DS_Store": what Macs
// leave on a NAS); folders by the crawler's rules. Three problems with the
// share in a row (or one at the folder added) stop the pass: the rest would
// each wait for Windows to time out.
class Lister {
public:
    struct Options {
        std::vector<std::wstring> excludedFiles; // a name, or "prefix*"
        bool shortNames = false; // FileIdBothDirectoryInfo instead of FileFullDirectoryInfo (for wsbench)
        std::size_t bufferBytes = 64 * 1024;
        std::atomic<std::size_t>* progress = nullptr; // counts the items read, when set
    };
    Lister(const Crawler& crawler, std::wstring root, Options options, std::stop_source stopPass);

    bool operator()(const Crawler::Root& dir, DirListing& out);

    // What happened, once the pass is over (or while it runs, roughly).
    unsigned long firstError() const noexcept { return m_firstError.load(); } // of the share as a whole; 0: none
    Problem problem() const noexcept;
    bool stoppedPass() const noexcept { return m_stopped.load(); }
    std::size_t items() const noexcept { return m_items.load(std::memory_order_relaxed); }
    std::size_t deniedFolders() const noexcept { return m_denied.load(std::memory_order_relaxed); }

private:
    bool listWslDirectory(const Crawler::Root& dir, DirListing& out);
    bool fail(unsigned long error, bool atRoot);
    bool excludedFile(std::wstring_view name) const noexcept;

    const Crawler& m_crawler;
    std::wstring m_root;
    Options m_options;
    std::stop_source m_stopPass;
    std::atomic<unsigned long> m_firstError {0};
    std::atomic<bool> m_rootFailed {false};
    std::atomic<bool> m_stopped {false};
    std::atomic<int> m_inARow {0};
    std::atomic<std::size_t> m_items {0};
    std::atomic<std::size_t> m_denied {0};
};

} // namespace ws::share
