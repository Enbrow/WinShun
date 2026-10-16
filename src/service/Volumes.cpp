// The operations on volumes (Server.h): the MFT streamed as parsed records
// (never the raw bytes: a record holds a small file's contents), the
// change journal read batch by batch as FSCTL_READ_USN_JOURNAL hands it
// over (no file contents in there), the journal's existence, letting go of a
// volume, and the bulk read: the contents of files the client names (by
// their folder's record and their name) and may read by its own token, read
// off the volume by their records (serveOpenBulkRead). A handle on a volume
// exists only while a request needs it.
#include "Log.h"
#include "Ntfs.h"
#include "NtfsBulk.h"
#include "Server.h"
#include "Win32Util.h"

#include <algorithm>
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
    if (!server.bind(conn, request.root, Bind::Mft)) {
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
                block.begin(reader.firstRecord(), conn.minor >= kMinorSizes);
                for (std::size_t i = 0; i < reader.blockRecords(); ++i) {
                    const bool parsed = reader.parse(i, record);
                    inUse += parsed && record.inUse ? 1 : 0;
                    block.add(record, parsed);
                    // A frame of a megabyte at most: the next records go in another.
                    if (block.payloadBytes() >= kBlockBytes && i + 1 < reader.blockRecords()) {
                        ok = conn.channel.write(block.finish(reader.bytesDone()), timeout);
                        if (!ok)
                            break;
                        block.begin(reader.firstRecord() + static_cast<std::uint32_t>(i) + 1, conn.minor >= kMinorSizes);
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
    server.bind(conn, request.root, Bind::Journal);
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

// ---- bulk reads -------------------------------------------------------------------
//
// The content index's first build (VolumeProtocol.h, Ntfs.h BulkSource): the
// client names the files it wants by their folder's record number and their
// name, the volume is scanned once (a fresh handle: what is on the disk
// now), and each file found is marked Data only when the client's own token
// may read it (ntfs::AccessGate: an AccessCheck of its security descriptor
// for FILE_GENERIC_READ, the descriptor taken through an open for
// READ_CONTROL only). The bytes are read off the volume in the batches the
// client asks for, in disk order. The client never names a path, a cluster
// or an offset, and gets nothing it could not have read itself.
void serveOpenBulkRead(Server& server, Connection& conn, std::span<const std::byte> payload)
{
    const DWORD timeout = server.options().writeTimeoutMs;
    OpenBulkRead request;
    if (!decode(payload, request)) {
        replyError(conn, ERROR_INVALID_DATA, L"malformed OpenBulkRead", timeout);
        return;
    }
    if (!server.options().bulkReads) {
        replyError(conn, ERROR_NOT_SUPPORTED, L"this service has no bulk reads", timeout);
        return;
    }
    if (!serialMatches(request.root, request.serial)) {
        log::write(L"pid %lu: %ls has another serial now: refused", conn.pid, request.root.c_str());
        replyError(conn, ERROR_WRONG_DISK, L"the drive letter holds another volume now", timeout);
        return;
    }
    // Who asks: the access checks are made with the client's own token (it
    // wrote the Hello and the request, so the pipe knows). Nobody: not served.
    std::wstring why;
    if (conn.token)
        ::CloseHandle(conn.token); // an earlier bulk read on this connection
    conn.token = ntfs::clientIdentificationToken(conn.channel.handle(), &why);
    if (!conn.token) {
        log::write(L"pid %lu: %ls: bulk read not served: %ls", conn.pid, request.root.c_str(), why.c_str());
        replyError(conn, ERROR_ACCESS_DENIED, (L"bulk reads are not served to this client: " + why).c_str(), timeout);
        return;
    }
    if (!server.bind(conn, request.root, Bind::Bulk)) {
        log::write(L"pid %lu: reads %ls in bulk already: refused", conn.pid, request.root.c_str());
        replyError(conn, ERROR_BUSY, L"this process reads that volume in bulk already", timeout);
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    const wchar_t* end = L"the client went away";
    std::uint64_t bytesSent = 0;
    std::size_t wanted = 0, served = 0, denied = 0, fetched = 0;
    ntfs::BulkReader::ScanStats scan;
    std::size_t checks = 0;
    {
        ntfs::BulkReader reader(request.root, ntfs::BulkReader::Options {.quiet = request.quiet});
        ntfs::AccessGate gate(request.root, conn.token, reader);
        if (!reader.valid() || !gate.valid()) {
            const std::wstring& error = reader.valid() ? gate.error() : reader.error();
            log::write(L"pid %lu: %ls: cannot open for a bulk read: %ls", conn.pid, request.root.c_str(), error.c_str());
            replyError(conn, ERROR_OPEN_FAILED, error.c_str(), timeout);
            server.unbind(conn);
            return;
        }
        bool ok = reply(conn, Type::BulkReady, timeout);

        // The wants, frame by frame, up to BulkScan.
        std::vector<ntfs::BulkWant> wants;
        Type type {};
        std::vector<std::byte> frame;
        while (ok) {
            if (!conn.channel.read(type, frame, server.options().helloTimeoutMs, conn.abort)) {
                ok = false;
                break;
            }
            if (type == Type::BulkScan)
                break;
            if (type != Type::BulkWant || !decode(frame, wants) || wants.size() > kMaxBulkWants) {
                replyError(conn, type == Type::BulkWant ? ERROR_INVALID_DATA : ERROR_INVALID_FUNCTION,
                    L"bad frame while a bulk read is set up", timeout);
                ok = false;
                break;
            }
        }
        wanted = wants.size();

        // The scan, and who may read what.
        std::vector<ntfs::BulkReader::Found> found;
        std::vector<bool> readable; // Data: the client may fetch it
        if (ok && !reader.scan(wants, found, &scan)) {
            log::write(L"pid %lu: %ls: scanning the MFT for a bulk read failed: %ls", conn.pid, request.root.c_str(),
                reader.error().c_str());
            replyError(conn, ERROR_READ_FAULT, reader.error().c_str(), timeout);
            ok = false;
        }
        if (ok && aborted(conn)) {
            reply(conn, Type::VolumeLost, timeout);
            end = L"the volume was let go of";
            ok = false;
        }
        if (ok) {
            std::vector<ntfs::BulkFile> files;
            files.reserve(found.size());
            std::vector<std::uint32_t> data;
            readable.assign(found.size(), false);
            for (std::size_t i = 0; i < found.size(); ++i) {
                const ntfs::BulkReader::Found& f = found[i];
                ntfs::BulkFile file {static_cast<std::uint32_t>(i), ntfs::BulkStatus::Fallback, f.skip, f.info.size,
                    f.info.modified};
                if (f.skip == ntfs::BulkSkip::None) {
                    if (gate.allowed(f)) {
                        readable[i] = true;
                        data.push_back(static_cast<std::uint32_t>(i));
                        continue;
                    }
                    file.status = ntfs::BulkStatus::Denied;
                    file.reason = ntfs::BulkSkip::None;
                    ++denied;
                } else if (f.skip == ntfs::BulkSkip::NotFound) {
                    file.status = ntfs::BulkStatus::NotFound;
                } else if (f.skip == ntfs::BulkSkip::TooLarge) {
                    file.status = ntfs::BulkStatus::TooLarge;
                }
                files.push_back(file);
            }
            checks = gate.checks();
            served = data.size();
            // The readable ones last, as they lie on the disk: the client
            // fetches them in that order.
            std::sort(data.begin(), data.end(), [&](std::uint32_t a, std::uint32_t b) {
                return ntfs::BulkReader::firstLcn(found[a]) < ntfs::BulkReader::firstLcn(found[b]);
            });
            for (const std::uint32_t i : data)
                files.push_back({i, ntfs::BulkStatus::Data, ntfs::BulkSkip::None, found[i].info.size, found[i].info.modified});
            constexpr std::size_t kPerFrame = 40'000; // 22 bytes each: under a megabyte
            Writer w;
            for (std::size_t at = 0; ok && at < files.size(); at += kPerFrame) {
                w.reset();
                encode(w, std::span<const ntfs::BulkFile>(files).subspan(at, std::min(kPerFrame, files.size() - at)));
                ok = conn.channel.write(w.finish(Type::BulkFiles), timeout);
            }
            if (ok) {
                w.reset();
                encode(w, BulkScanned {static_cast<std::uint32_t>(scan.matched), static_cast<std::uint32_t>(checks)});
                ok = conn.channel.write(w.finish(Type::BulkScanned), timeout);
            }
        }

        // The fetches, as the client asks (it may take its time between
        // them: its memory is full, or someone is searching), until it goes.
        std::vector<bool> done(found.size(), false);
        bool broken = false; // a write failed: the reads are given up
        const auto send = [&](std::uint32_t ticket, std::span<const std::byte> bytes) {
            if (broken)
                return;
            Writer w;
            std::size_t at = 0;
            do {
                const std::size_t n = std::min(kBlockBytes, bytes.size() - at);
                w.reset();
                encode(w, BulkDataHeader {ticket, at, bytes.size()}, bytes.subspan(at, n));
                if (!conn.channel.write(w.finish(Type::BulkData), timeout)) {
                    broken = true;
                    ::SetEvent(conn.abort); // stops a readAll under way; the connection is no good anyway
                    return;
                }
                bytesSent += n;
                at += n;
            } while (at < bytes.size());
            ++fetched;
        };
        while (ok) {
            if (!conn.channel.read(type, frame, INFINITE, conn.abort)) {
                if (aborted(conn)) {
                    reply(conn, Type::VolumeLost, timeout);
                    end = L"the volume was let go of";
                }
                break;
            }
            if (type == Type::CloseVolume) {
                serveCloseVolume(server, conn, frame);
                continue;
            }
            std::vector<std::uint32_t> tickets;
            if (type != Type::BulkFetch || !decode(frame, tickets)) {
                replyError(conn, type == Type::BulkFetch ? ERROR_INVALID_DATA : ERROR_INVALID_FUNCTION,
                    L"bad frame during a bulk read", timeout);
                break;
            }
            std::vector<std::uint32_t> only;
            for (const std::uint32_t t : tickets) {
                if (t >= found.size() || !readable[t] || done[t])
                    continue; // never asked for, not theirs, or had already
                done[t] = true;
                const ntfs::BulkReader::Found& f = found[t];
                if (f.info.data == ntfs::FileInfo::Data::Resident || f.want == 0)
                    send(t, std::span<const std::byte>(f.resident.data(), static_cast<std::size_t>(f.want)));
                else
                    only.push_back(t);
            }
            if (!only.empty() && !broken) {
                ntfs::BulkReader::ReadStats stats;
                const bool read = reader.readAll(
                    found, [&](std::size_t index, std::span<const std::byte> bytes) { send(static_cast<std::uint32_t>(index), bytes); },
                    &stats, only, conn.abort);
                if (!read && !broken) {
                    if (aborted(conn)) {
                        reply(conn, Type::VolumeLost, timeout);
                        end = L"the volume was let go of";
                    } else {
                        log::write(L"pid %lu: %ls: reading off the volume failed: %ls", conn.pid, request.root.c_str(),
                            reader.error().c_str());
                        replyError(conn, ERROR_READ_FAULT, reader.error().c_str(), timeout);
                    }
                    break;
                }
            }
            if (broken || !reply(conn, Type::BulkFetched, timeout))
                break;
        }
    } // the volume is closed here, the found records and resident bytes gone
    log::write(L"pid %lu: %ls: bulk read: %zu wanted, %llu matched (MFT %llu records in %.0f ms), %zu readable, %zu denied, "
               L"%zu access checks; %zu files, %.1f MB sent in %.0f ms: %ls",
        conn.pid, request.root.c_str(), wanted, static_cast<unsigned long long>(scan.matched),
        static_cast<unsigned long long>(scan.records), scan.ms, served, denied, checks, fetched,
        static_cast<double>(bytesSent) / 1048576.0, msSince(started), end);
    server.unbind(conn);
}

} // namespace ws::service
