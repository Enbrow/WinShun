#pragma once

#include "Ntfs.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace ws::ntfs {

// The volumes through the index service (WinShunService.exe, src/service),
// over its pipe: for a Win顺 that runs with the user's normal rights. Each
// MFT read, journal query, journal followed and CloseVolume is a connection
// of its own (VolumeProtocol.h), so the sources here are independent of one
// another and of this object, which only keeps the pipe's name.
//
// A journal source whose connection breaks (the service restarted) connects
// again on each of its next reads, for up to reconnectWindow(), and goes on
// from the same USN: only the service saying that the journal is lost (or
// the window running out) makes finish() return false.
class ServiceVolumeAccess final : public VolumeAccess {
public:
    explicit ServiceVolumeAccess(std::wstring pipeName = defaultPipeName());
    // WinShun.Volumes, or the WINSHUN_PIPE environment variable (a server
    // started with --console --pipe NAME, for tests).
    static std::wstring defaultPipeName();
    // Whether the service is registered on this computer (by the installer,
    // or `WinShunService.exe --install`), running or not: for the settings.
    static bool installed();

    // Connects and shakes hands once: whether the service is there and
    // speaks our protocol. serviceVersion() and lastError() say more.
    bool probe();
    std::wstring serviceVersion() const; // "0.7.3" once probe() succeeded
    std::wstring lastError() const; // of the last probe or connection that failed
    const std::wstring& pipeName() const noexcept { return m_pipeName; }

    Kind kind() const noexcept override { return Kind::Service; }
    std::unique_ptr<RecordSource> openMft(std::wstring_view root, std::uint32_t serial, bool lowPriority) override;
    std::optional<JournalInfo> queryJournal(std::wstring_view root, std::uint32_t serial) override;
    std::unique_ptr<JournalSource> openJournal(std::wstring_view root, std::uint32_t serial,
        std::uint64_t journalId) override;
    // Asks the service to let go of everything it opened on this process's
    // behalf on the volume, and waits (2 s at most) for it to say so.
    void closeVolume(std::wstring_view root) override;
    // Null when the service is an older one without bulk reads.
    std::unique_ptr<BulkSource> openBulkRead(std::wstring_view root, std::uint32_t serial, bool quiet) override;

    // How long a journal source keeps trying to reach the service again
    // before it gives the journal up as lost (30 s; tests shorten it).
    static std::chrono::milliseconds reconnectWindow() noexcept;
    static void setReconnectWindow(std::chrono::milliseconds window) noexcept;

private:
    void noteError(std::wstring error);

    std::wstring m_pipeName;
    mutable std::mutex m_mutex;
    std::wstring m_serviceVersion;
    std::wstring m_lastError;
};

} // namespace ws::ntfs
