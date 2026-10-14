// The operations on volumes (Server.h): the MFT streamed as parsed records
// (never the raw bytes: a record holds a small file's contents), the
// change journal read batch by batch as FSCTL_READ_USN_JOURNAL hands it
// over (no file contents in there), the journal's existence, and letting go
// of a volume. A handle on a volume exists only while a request needs it.
#include "Log.h"
#include "Ntfs.h"
#include "Server.h"
#include "Win32Util.h"

#include <chrono>

namespace ws::service {

using namespace volproto;

namespace {

bool serialMatches(const std::wstring& root, std::uint32_t serial)
{
    if (serial == 0)
        return true; // not checked
    const std::wstring path = root + L"\\";
    DWORD actual = 0;
    return ::GetVolumeInformationW(path.c_str(), nullptr, 0, &actual, nullptr, nullptr, nullptr, 0) && actual == serial;
}

bool aborted(const Connection& conn)
{
    return ::WaitForSingleObject(conn.abort, 0) == WAIT_OBJECT_0;
}

double msSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

void followJournal(Server& server, Connection& conn, const FollowJournal& request);

} // namespace

bool reply(Connection& conn, Type type, DWORD timeoutMs)
{
    Writer w;
    return conn.channel.write(w.finish(type), timeoutMs);
}

bool replyError(Connection& conn, DWORD code, const wchar_t* message, DWORD timeoutMs)
{
    Writer w;
    encode(w, Error {code, message});
    return conn.channel.write(w.finish(Type::Error), timeoutMs);
}

void serveOpenMft(Server& server, Connection& conn, std::span<const std::byte> payload)
{
    const DWORD timeout = server.options().writeTimeoutMs;
    OpenMft request;
    if (!decode(payload, request)) {
        replyError(conn, ERROR_INVALID_DATA, L"malformed OpenMft", timeout);
        return;
    }
    if (!serialMatches(request.root, request.serial)) {
        log::write(L"pid %lu: %ls has another serial now: refused", conn.pid, request.root.c_str());
        replyError(conn, ERROR_WRONG_DISK, L"the drive letter holds another volume now", timeout);
        return;
    }
    if (!server.bind(conn, request.root, true)) {
        log::write(L"pid %lu: reads the MFT of %ls already: refused", conn.pid, request.root.c_str());
        replyError(conn, ERROR_BUSY, L"this process reads that MFT already", timeout);
        return;
    }
    // As the client's own thread would be: the read yields to everything else.
    if (request.lowPriority)
        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    const auto started = std::chrono::steady_clock::now();
    {
        ntfs::MftReader reader(request.root);
        if (!reader.valid()) {
            log::write(L"pid %lu: %ls: cannot open the MFT: %ls", conn.pid, request.root.c_str(), reader.error().c_str());
            Writer w;
            encode(w, MftDone {static_cast<std::uint32_t>(MftStatus::Failed), reader.error()});
            conn.channel.write(w.finish(Type::MftDone), timeout);
        } else {
            Writer w;
            encode(w, MftInfo {reader.geometry(), reader.recordCount(), reader.bytes()});
            bool ok = conn.channel.write(w.finish(Type::MftInfo), timeout);
            MftStatus status = MftStatus::Done;
            BlockWriter block;
            ntfs::FileRecord record;
            std::uint64_t inUse = 0;
            while (ok && reader.next()) {
                if (aborted(conn)) {
                    status = MftStatus::VolumeLost;
                    break;
                }
                block.begin(reader.firstRecord());
                for (std::size_t i = 0; i < reader.blockRecords(); ++i) {
                    const bool parsed = reader.parse(i, record);
                    inUse += parsed && record.inUse ? 1 : 0;
                    block.add(record, parsed);
                    // A frame of a megabyte at most: the next records go in another.
                    if (block.payloadBytes() >= kBlockBytes && i + 1 < reader.blockRecords()) {
                        ok = conn.channel.write(block.finish(reader.bytesDone()), timeout);
                        if (!ok)
                            break;
                        block.begin(reader.firstRecord() + static_cast<std::uint32_t>(i) + 1);
                    }
                }
                if (ok && block.count() > 0)
                    ok = conn.channel.write(block.finish(reader.bytesDone()), timeout);
            }
            if (ok) {
                if (status == MftStatus::Done && !reader.valid())
                    status = MftStatus::Failed;
                w.reset(); // the MftInfo is still in it
                encode(w, MftDone {static_cast<std::uint32_t>(status), status == MftStatus::Failed ? reader.error() : L""});
                conn.channel.write(w.finish(Type::MftDone), timeout);
            }
            log::write(L"pid %lu: %ls: MFT %llu records, %llu in use, %.0f ms: %ls", conn.pid, request.root.c_str(),
                static_cast<unsigned long long>(reader.recordCount()), static_cast<unsigned long long>(inUse),
                msSince(started),
                !ok                              ? L"the client went away"
                    : status == MftStatus::Done  ? L"done"
                    : status == MftStatus::Failed ? reader.error().c_str()
                                                  : L"the volume was let go of");
        }
    } // the volume is closed here
    if (request.lowPriority)
        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    server.unbind(conn);
}

void serveQueryJournal(Server& server, Connection& conn, std::span<const std::byte> payload)
{
    const DWORD timeout = server.options().writeTimeoutMs;
    QueryJournal request;
    if (!decode(payload, request)) {
        replyError(conn, ERROR_INVALID_DATA, L"malformed QueryJournal", timeout);
        return;
    }
    if (!serialMatches(request.root, request.serial)) {
        replyError(conn, ERROR_WRONG_DISK, L"the drive letter holds another volume now", timeout);
        return;
    }
    const std::optional<ntfs::JournalInfo> info = ntfs::queryJournal(request.root);
    if (!info) {
        const DWORD error = ::GetLastError();
        log::write(L"pid %lu: %ls: no change journal (error %lu)", conn.pid, request.root.c_str(), error);
        replyError(conn, error, L"the volume has no change journal that could be read or made", timeout);
        return;
    }
    Writer w;
    encode(w, *info);
    conn.channel.write(w.finish(Type::JournalInfo), timeout);
}

void serveFollowJournal(Server& server, Connection& conn, std::span<const std::byte> payload)
{
    const DWORD timeout = server.options().writeTimeoutMs;
    FollowJournal request;
    if (!decode(payload, request)) {
        replyError(conn, ERROR_INVALID_DATA, L"malformed FollowJournal", timeout);
        return;
    }
    if (!serialMatches(request.root, request.serial)) {
        replyError(conn, ERROR_WRONG_DISK, L"the drive letter holds another volume now", timeout);
        return;
    }
    server.bind(conn, request.root, false);
    followJournal(server, conn, request);
    server.unbind(conn); // the reader is closed by now
}

namespace {

void followJournal(Server& server, Connection& conn, const FollowJournal& request)
{
    const DWORD timeout = server.options().writeTimeoutMs;
    ntfs::JournalReader reader(request.root, request.journalId);
    if (!reader.valid()) {
        const DWORD error = ::GetLastError();
        log::write(L"pid %lu: %ls: cannot open the volume for its journal (error %lu)", conn.pid, request.root.c_str(), error);
        replyError(conn, error, L"cannot open the volume", timeout);
        return;
    }
    if (!reply(conn, Type::Following, timeout))
        return;
    log::write(L"pid %lu: %ls: following journal %llx", conn.pid, request.root.c_str(),
        static_cast<unsigned long long>(request.journalId));

    // Reads as the client asks, each waited for alongside the next request
    // (a Cancel, a CloseVolume) and the volume being let go of.
    bool reading = false;
    const wchar_t* end = L"the client went away";
    for (;;) {
        if (!conn.channel.readPending() && !conn.channel.beginRead())
            break;
        HANDLE handles[3] {conn.abort, conn.channel.event(), reader.event()};
        const DWORD r = ::WaitForMultipleObjects(reading ? 3 : 2, handles, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) {
            reader.cancel();
            reply(conn, Type::VolumeLost, timeout);
            end = L"the volume was let go of";
            break;
        }
        if (r == WAIT_OBJECT_0 + 2) {
            reading = false;
            std::span<const std::byte> data;
            Writer w;
            if (reader.finishRaw(data)) {
                w.bytes(data);
                if (!conn.channel.write(w.finish(Type::UsnBatch), timeout))
                    break;
            } else {
                const DWORD error = ::GetLastError();
                encode(w, JournalLost {error});
                conn.channel.write(w.finish(Type::JournalLost), timeout);
                log::write(L"pid %lu: %ls: journal lost (error %lu)", conn.pid, request.root.c_str(), error);
                end = L"the journal is lost";
                break;
            }
            continue;
        }
        if (r != WAIT_OBJECT_0 + 1)
            break;
        Type type {};
        std::vector<std::byte> frame;
        if (!conn.channel.finishRead(type, frame, timeout))
            break;
        if (type == Type::Read) {
            Read read;
            if (!decode(frame, read) || reading) {
                replyError(conn, reading ? ERROR_BUSY : ERROR_INVALID_DATA, L"bad Read", timeout);
                break;
            }
            reader.start(read.usn, read.wait);
            reading = true;
        } else if (type == Type::Cancel) {
            if (reading)
                reader.cancel();
            reading = false;
            if (!reply(conn, Type::Cancelled, timeout))
                break;
        } else if (type == Type::CloseVolume) {
            serveCloseVolume(server, conn, frame);
        } else {
            replyError(conn, ERROR_INVALID_FUNCTION, L"not while following", timeout);
            break;
        }
    }
    reader.cancel();
    log::write(L"pid %lu: %ls: journal no longer followed: %ls", conn.pid, request.root.c_str(), end);
}

} // namespace

void serveCloseVolume(Server& server, Connection& conn, std::span<const std::byte> payload)
{
    const DWORD timeout = server.options().writeTimeoutMs;
    CloseVolume request;
    if (!decode(payload, request)) {
        replyError(conn, ERROR_INVALID_DATA, L"malformed CloseVolume", timeout);
        return;
    }
    // This process's own handles only: another process's are its own business.
    const unsigned closed = server.closeVolume(request.root, conn.pid, 2000);
    log::write(L"pid %lu: %ls: let go of (%u connections)", conn.pid, request.root.c_str(), closed);
    Writer w;
    encode(w, Closed {closed});
    conn.channel.write(w.finish(Type::Closed), timeout);
}

} // namespace ws::service
