#pragma once

#include "Ntfs.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What goes over the pipe between Win顺 and its index service
// (WinShunService.exe, src/service; the client is ServiceVolumeAccess): the
// frames, their payloads, and a channel that reads and writes them on a pipe
// handle. No Qt: the service has none. Little-endian, both ends on the same
// x64 machine. Neither end trusts the other's lengths: every one is checked
// before it is used.
//
// Each operation is a connection of its own. The first frame is Hello /
// HelloOk; then one of:
//   OpenMft       -> MftInfo, MftBlock..., MftDone
//   QueryJournal  -> JournalInfo | Error
//   FollowJournal -> Following | Error; then Read -> UsnBatch | JournalLost,
//                    Cancel -> Cancelled, any number of times
//   CloseVolume   -> Closed
// VolumeLost comes from the service on an MFT or journal connection whose
// volume it had to let go of (an eject, a lock, or the client's own
// CloseVolume on another connection).
namespace ws::volproto {

constexpr std::uint16_t kMajor = 1; // different on the two ends: they cannot talk
constexpr std::uint16_t kMinor = 0; // only ever adds fields at the end of a payload
constexpr wchar_t kPipeName[] = L"WinShun.Volumes"; // one service for the whole machine
constexpr wchar_t kPipeEnvironment[] = L"WINSHUN_PIPE"; // another pipe's name, for tests (--console --pipe)
constexpr wchar_t kServiceName[] = L"WinShun";
constexpr std::size_t kMaxPayloadBytes = 4u << 20; // a larger frame is refused unread
constexpr std::size_t kBlockBytes = 1u << 20; // the service ends an MftBlock past this
constexpr std::size_t kMaxNames = 255; // per record in an MftBlock (a 4 KB record holds some 60)

enum class Type : std::uint16_t {
    Hello = 1,
    HelloOk = 2,
    HelloRefused = 3,
    OpenMft = 10,
    MftInfo = 11,
    MftBlock = 12,
    MftDone = 13,
    QueryJournal = 20,
    JournalInfo = 21,
    FollowJournal = 30,
    Following = 31,
    Read = 32,
    UsnBatch = 33,
    JournalLost = 34,
    Cancel = 35,
    Cancelled = 36,
    CloseVolume = 40,
    Closed = 41,
    VolumeLost = 50,
    Error = 60,
};

std::wstring fullPipePath(std::wstring_view name); // "\\.\pipe\" + name

// ---- payloads ----------------------------------------------------------------

struct Hello {
    std::uint16_t major = kMajor;
    std::uint16_t minor = kMinor;
    std::uint16_t version[3] {}; // of the program
    std::uint32_t pid = 0; // for the log only: the service asks the pipe who the client is
};

struct HelloOk {
    std::uint16_t major = kMajor;
    std::uint16_t minor = kMinor;
    std::uint16_t version[3] {};
    std::uint32_t capabilities = 0; // none yet
};

enum class Refusal : std::uint32_t { Protocol = 1, TooManyConnections = 2 };

struct HelloRefused {
    std::uint32_t reason = 0; // Refusal
};

struct OpenMft {
    std::wstring root; // "C:"
    std::uint32_t serial = 0; // 0: not checked
    bool lowPriority = false;
};

struct MftInfo {
    ntfs::Geometry geometry;
    std::uint64_t recordCount = 0;
    std::uint64_t validBytes = 0;
};

enum class MftStatus : std::uint32_t { Done = 0, Failed = 1, VolumeLost = 2 };

struct MftDone {
    std::uint32_t status = 0; // MftStatus
    std::wstring error; // when Failed
};

struct QueryJournal {
    std::wstring root;
    std::uint32_t serial = 0;
};

struct FollowJournal {
    std::wstring root;
    std::uint32_t serial = 0;
    std::uint64_t journalId = 0;
};

struct Read {
    std::int64_t usn = 0;
    bool wait = false;
};

struct JournalLost {
    std::uint32_t error = 0; // the Win32 error of the read
};

struct CloseVolume {
    std::wstring root;
};

struct Closed {
    std::uint32_t closed = 0; // connections let go of
};

struct Error {
    std::uint32_t code = 0; // Win32
    std::wstring message;
};

// ---- encoding ----------------------------------------------------------------

// Builds one frame: the header in front, patched on finish().
class Writer {
public:
    Writer();
    void u8(std::uint8_t v);
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i64(std::int64_t v);
    void string(std::u16string_view s); // u16 length (code units), then them
    void string(std::wstring_view s);
    void bytes(std::span<const std::byte> b);
    std::size_t payloadBytes() const noexcept { return m_data.size() - kHeaderBytes; }
    // The frame, header and all. The writer can go on being used for the next one (reset()).
    std::span<const std::byte> finish(Type type);
    void reset();
    // Patches a u32 / u64 written earlier at `at` (payload offset).
    void patchU32(std::size_t at, std::uint32_t v);
    void patchU64(std::size_t at, std::uint64_t v);

    static constexpr std::size_t kHeaderBytes = 8;

private:
    std::vector<std::byte> m_data;
};

// Reads a payload field by field; every read says whether the bytes were there.
class Reader {
public:
    explicit Reader(std::span<const std::byte> data) noexcept
        : m_data(data)
    {
    }
    bool u8(std::uint8_t& v) noexcept;
    bool u16(std::uint16_t& v) noexcept;
    bool u32(std::uint32_t& v) noexcept;
    bool u64(std::uint64_t& v) noexcept;
    bool i64(std::int64_t& v) noexcept;
    bool boolean(bool& v) noexcept;
    bool string(std::wstring& s); // as Writer::string wrote it
    bool string(std::u16string_view& s) noexcept; // a view into the payload
    bool bytes(std::span<const std::byte>& b, std::size_t n) noexcept;
    bool done() const noexcept { return m_pos == m_data.size(); }
    std::size_t position() const noexcept { return m_pos; }
    std::size_t left() const noexcept { return m_data.size() - m_pos; }

private:
    std::span<const std::byte> m_data;
    std::size_t m_pos = 0;
};

// The header of a frame: false when it is not one we would read (too long).
bool parseHeader(std::span<const std::byte, 8> header, Type& type, std::uint32_t& length) noexcept;

void encode(Writer& w, const Hello& m);
void encode(Writer& w, const HelloOk& m);
void encode(Writer& w, const HelloRefused& m);
void encode(Writer& w, const OpenMft& m);
void encode(Writer& w, const MftInfo& m);
void encode(Writer& w, const MftDone& m);
void encode(Writer& w, const QueryJournal& m);
void encode(Writer& w, const ntfs::JournalInfo& m);
void encode(Writer& w, const FollowJournal& m);
void encode(Writer& w, const Read& m);
void encode(Writer& w, const JournalLost& m);
void encode(Writer& w, const CloseVolume& m);
void encode(Writer& w, const Closed& m);
void encode(Writer& w, const Error& m);

// Each false on a payload that is cut short. Bytes past the fields known
// here are left alone: a later minor version may add some.
bool decode(std::span<const std::byte> payload, Hello& m);
bool decode(std::span<const std::byte> payload, HelloOk& m);
bool decode(std::span<const std::byte> payload, HelloRefused& m);
bool decode(std::span<const std::byte> payload, OpenMft& m);
bool decode(std::span<const std::byte> payload, MftInfo& m);
bool decode(std::span<const std::byte> payload, MftDone& m);
bool decode(std::span<const std::byte> payload, QueryJournal& m);
bool decode(std::span<const std::byte> payload, ntfs::JournalInfo& m);
bool decode(std::span<const std::byte> payload, FollowJournal& m);
bool decode(std::span<const std::byte> payload, Read& m);
bool decode(std::span<const std::byte> payload, JournalLost& m);
bool decode(std::span<const std::byte> payload, CloseVolume& m);
bool decode(std::span<const std::byte> payload, Closed& m);
bool decode(std::span<const std::byte> payload, Error& m);

// An MftBlock: { u32 firstRecord, u32 count, u64 bytesDone }, then `count`
// records in order, each { u8 kind } and, for a record in use, { u32
// baseRecord, u32 attributes, i64 modified, u8 flags, u8 nameCount } and
// each name { u64 parent, u16 length, the UTF-16 units }. Record kinds:
enum class RecordKind : std::uint8_t { Invalid = 0, Free = 1, InUse = 2 }; // Invalid: not an intact record
constexpr std::uint8_t kRecordDirectory = 1;
constexpr std::uint8_t kRecordAttributeList = 2;

// Writes an MftBlock record by record, as the service parses them.
class BlockWriter {
public:
    void begin(std::uint32_t firstRecord);
    // The next record in order: parsed, or not one (`parsed` false).
    void add(const ntfs::FileRecord& record, bool parsed);
    std::uint32_t count() const noexcept { return m_count; }
    std::size_t payloadBytes() const noexcept { return m_writer.payloadBytes(); }
    std::span<const std::byte> finish(std::uint64_t bytesDone); // the frame

private:
    Writer m_writer;
    std::uint32_t m_count = 0;
};

// Reads an MftBlock: checked whole on load(), then any record by number.
class BlockReader {
public:
    // Takes the payload; false when it is malformed (nothing is kept then).
    bool load(std::vector<std::byte>&& payload);
    std::uint32_t firstRecord() const noexcept { return m_firstRecord; }
    std::uint32_t count() const noexcept { return static_cast<std::uint32_t>(m_offsets.size()); }
    std::uint64_t bytesDone() const noexcept { return m_bytesDone; }
    // Record i of the block, as parseFileRecord would have given it. False
    // for one that was not an intact record. Names point into the payload.
    bool record(std::size_t i, ntfs::FileRecord& out) const;

private:
    std::vector<std::byte> m_payload;
    std::vector<std::uint32_t> m_offsets; // of each record's kind byte
    std::uint32_t m_firstRecord = 0;
    std::uint64_t m_bytesDone = 0;
};

// ---- the pipe -------------------------------------------------------------------

// Frames on one pipe handle (either end). Reads and writes are overlapped,
// so that a read can be waited on among other events (event()), and so
// that either can be given up after a while or on an event.
class Channel {
public:
    Channel() = default;
    explicit Channel(void* pipe); // takes the handle
    ~Channel();
    Channel(Channel&& other) noexcept;
    Channel& operator=(Channel&& other) noexcept;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    bool valid() const noexcept;
    void* handle() const noexcept { return m_pipe; }
    void close(); // cancels what is in flight

    // Writes a whole frame. False when the other end is gone, or did not
    // take it within `timeoutMs` (the connection is then no good: close it).
    bool write(std::span<const std::byte> frame, unsigned long timeoutMs);
    bool write(Type type, std::span<const std::byte> payload, unsigned long timeoutMs);
    // Reads a whole frame: false when the other end is gone, the header is
    // not a frame's, or nothing came within `timeoutMs` / `abort` was set
    // (the read is cancelled then; the connection is no good).
    bool read(Type& type, std::vector<std::byte>& payload, unsigned long timeoutMs, void* abort = nullptr);

    // Starts reading the next frame: event() is set once its first bytes are
    // in (or the pipe broke). finishRead() then reads the rest. One at a time.
    bool beginRead();
    bool readPending() const noexcept { return m_pending; }
    void* event() const noexcept { return m_readEvent; }
    bool finishRead(Type& type, std::vector<std::byte>& payload, unsigned long timeoutMs);
    void cancelRead();

    unsigned long lastError() const noexcept { return m_error; }

private:
    bool readAll(std::byte* into, std::size_t n, unsigned long timeoutMs, void* abort);
    bool waitRead(unsigned long timeoutMs, void* abort); // the overlapped read in m_overlapped, into m_got

    void* m_pipe = nullptr;
    void* m_readEvent = nullptr;
    void* m_writeEvent = nullptr;
    std::unique_ptr<_OVERLAPPED> m_overlapped; // the read in flight
    std::byte m_header[8] {};
    bool m_pending = false;
    unsigned long m_error = 0;
};

} // namespace ws::volproto
