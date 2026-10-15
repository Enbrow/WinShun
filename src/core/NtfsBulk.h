#pragma once

#include "Ntfs.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Reading files' contents straight off an NTFS volume, by their MFT records:
// the measurement prototype of the content index's bulk path (design: a
// first build opens every file, and the antivirus scans each one as it is
// opened; reading the clusters through the volume handle skips that). This
// is what wsbench --raw-read and --dirty-window measure with; the service
// would use the same code behind an access check. Administrator rights:
// it opens the volume itself.
namespace ws::ntfs {

// One run of a $DATA attribute, in clusters.
struct DataRun {
    std::uint64_t clusters;
    std::int64_t lcn; // -1: a sparse run, no clusters on disk (reads as zeros)
};

// Decodes a $DATA run list, sparse runs included (decodeRunList, for the
// MFT's own, refuses them). Nothing for a malformed list.
std::optional<std::vector<DataRun>> decodeDataRuns(std::span<const std::byte> pairs);

// Attribute flags of $DATA (offset 0x0C of the attribute header).
inline constexpr std::uint16_t kDataCompressed = 0x0001;
inline constexpr std::uint16_t kDataEncrypted = 0x4000;
inline constexpr std::uint16_t kDataSparse = 0x8000;

// What one file record says about the file, beyond FileRecord: for reading
// its unnamed $DATA off the volume, and for deciding not to.
struct FileInfo {
    // The record header.
    std::uint64_t lsn = 0; // $LogFile sequence number: changes with every change to the record
    std::uint16_t sequence = 0; // reuses of the record
    std::uint16_t links = 0; // hard links as the record counts them: a DOS 8.3 alias is one too
    std::uint16_t names = 0; // $FILE_NAME attributes in this record outside the DOS namespace: the real links (base record)
    bool inUse = false;
    bool directory = false;
    std::uint32_t baseRecord = 0;
    bool hasAttributeList = false;
    // $STANDARD_INFORMATION.
    std::uint32_t standardInfoBytes = 0; // the value's length: 0x48 (the Windows 2000 and later form) has the ids
    std::int64_t modified = 0;
    std::int64_t mftChanged = 0;
    std::uint32_t attributes = 0; // FILE_ATTRIBUTE_*
    std::uint32_t securityId = 0; // into $Secure, when standardInfoBytes >= 0x38
    bool hasSecurityDescriptor = false; // a $SECURITY_DESCRIPTOR attribute of its own (volumes from NTFS 1.x)
    // $REPARSE_POINT.
    bool hasReparse = false;
    std::uint32_t reparseTag = 0; // from a resident $REPARSE_POINT (IO_REPARSE_TAG_*)
    // The unnamed $DATA.
    enum class Data : std::uint8_t { None, Resident, NonResident, Several };
    Data data = Data::None; // Several: more than one unnamed $DATA in the base record
    std::uint16_t dataFlags = 0; // kData*
    std::uint64_t size = 0; // real size, what GetFileSizeEx says
    std::uint64_t initialized = 0; // valid data length: bytes past it read as zeros
    std::uint64_t allocated = 0;
    std::uint64_t lowestVcn = 0;
    std::uint64_t highestVcn = 0;
    std::span<const std::byte> resident; // the value, into the record (Data::Resident)
    std::span<const std::byte> runList; // the mapping pairs, into the record (Data::NonResident)
};

// Parses a record whose update sequence fixups are applied already
// (MftReader::parse or parseFileRecord did that). False for anything that is
// not an intact file record.
bool parseFileInfo(std::span<const std::byte> record, FileInfo& out);

// Reads files by their MFT records (BulkSkip, BulkWant: Ntfs.h): scan()
// finds the records of the files asked for (by the record number of their
// folder and their name, as the service is asked), readAll() reads their
// clusters in LCN order with several reads in flight. Everything unbuffered:
// the file cache is not consulted, so what comes back is what is on the disk
// right now (the caller keeps recently written files away from this).
class BulkReader {
public:
    struct Options {
        int readsInFlight = 8;
        std::uint32_t blockBytes = 4u << 20; // one read at most (rounded up to a cluster)
        bool quiet = false; // a hard disk while someone uses the computer: very low I/O priority
    };
    using Want = BulkWant;
    struct Found {
        std::uint32_t record = 0;
        BulkSkip skip = BulkSkip::NotFound;
        FileInfo info; // resident and runList spans are not valid: see below
        std::vector<std::byte> resident; // Data::Resident: the bytes
        std::vector<DataRun> runs; // Data::NonResident: decoded
        std::uint64_t want = 0; // bytes to deliver: min(size, maxBytes)
    };
    struct ScanStats {
        std::uint64_t records = 0;
        std::uint64_t inUse = 0;
        std::uint64_t matched = 0;
        double ms = 0;
    };
    struct ReadStats {
        std::uint64_t files = 0;
        std::uint64_t bytes = 0; // delivered
        std::uint64_t diskBytes = 0; // read from the volume (whole clusters, nothing past the valid data length)
        std::uint64_t reads = 0;
        double ms = 0;
    };

    explicit BulkReader(std::wstring_view root, Options options = {}); // "C:"
    ~BulkReader();
    BulkReader(const BulkReader&) = delete;
    BulkReader& operator=(const BulkReader&) = delete;

    bool valid() const noexcept { return m_error.empty(); }
    const std::wstring& error() const noexcept { return m_error; }
    const Geometry& geometry() const noexcept { return m_geometry; }

    // Reads the whole MFT once: found[i] answers wants[i].
    bool scan(std::span<const Want> wants, std::vector<Found>& found, ScanStats* stats = nullptr);
    // Reads every Found that is readable and non-resident (or only those at
    // `only`, indices into `found`), in the order of their first cluster;
    // `sink` gets each file's bytes (`want` of them, zeros past the valid data
    // length and in sparse runs) once it is whole. Resident files are not
    // touched: their bytes are in Found::resident. `abort` (a manual-reset
    // event) set: stops between reads, false.
    bool readAll(std::span<const Found> found,
        const std::function<void(std::size_t index, std::span<const std::byte> bytes)>& sink,
        ReadStats* stats = nullptr, std::span<const std::uint32_t> only = {}, void* abort = nullptr);
    bool readOne(const Found& found, std::vector<std::byte>& out); // the same for one file
    // Where the file's first cluster is (the order to read files in); the
    // largest value for one with no cluster of its own.
    static std::int64_t firstLcn(const Found& found) noexcept;

    // One record as it is on the disk (fixups applied), through the MFT's
    // extents; empty when it cannot be read.
    std::vector<std::byte> readRecordRaw(std::uint32_t record);
    // The same record as NTFS hands it out (FSCTL_GET_NTFS_FILE_RECORD):
    // from its cache, so possibly newer than the disk. `fixupsApplied` says
    // whether the bytes still carried the update sequence array (then it was
    // applied here), or came without one.
    struct Cached {
        std::vector<std::byte> record;
        std::uint64_t reference = 0; // the record NTFS answered about (a lower one when `record` is not in use)
        bool fixupsApplied = false;
    };
    bool readRecordCached(std::uint32_t record, Cached& out);

private:
    struct Op; // one read planned

    bool control(unsigned long code, const void* in, unsigned long inBytes, void* out, unsigned long outBytes);
    bool readSectors(std::uint64_t offset, std::uint32_t bytes, std::byte* into); // aligned, waited for

    std::wstring m_root;
    Options m_options;
    std::wstring m_error;
    Geometry m_geometry;
    std::uint64_t m_mftStart = 0; // bytes
    std::vector<Extent> m_mftExtents;
    void* m_volume = nullptr;
    void* m_event = nullptr; // for control() and readSectors()
};

// Whether a client may read a file that BulkReader found: an AccessCheck of
// the file's security descriptor against the client's token, for what its
// own CreateFileW(GENERIC_READ) would ask (FILE_GENERIC_READ). The
// descriptor comes from an open of the file by its id for READ_CONTROL only
// (no data access: nothing is scanned or recalled; files with a reparse
// point or offline are never opened). Verdicts are kept by security id
// (files sharing one descriptor have one id), each kept only once the file's
// record, as NTFS has it now, is seen to carry that id and to have been
// left alone for a while.
class AccessGate {
public:
    // `token`: the client's impersonation token (identification level is
    // enough); null: no check, everything allowed (an elevated program's own
    // reads). `reader`: the volume's, for the records.
    AccessGate(std::wstring_view root, void* token, BulkReader& reader);
    ~AccessGate();
    AccessGate(const AccessGate&) = delete;
    AccessGate& operator=(const AccessGate&) = delete;

    bool valid() const noexcept { return m_error.empty(); }
    const std::wstring& error() const noexcept { return m_error; }
    // False when the client may not read it, or that could not be made sure of.
    bool allowed(const BulkReader::Found& found);
    std::size_t checks() const noexcept { return m_checks; } // descriptors fetched and checked

private:
    std::wstring m_error;
    void* m_token;
    void* m_volume = nullptr; // a plain handle on the volume: OpenFileById's hint
    BulkReader& m_reader;
    std::unordered_map<std::uint32_t, bool> m_bySecurityId;
    std::size_t m_checks = 0;
};

// The client's token for an AccessGate: the pipe client's, as an
// identification-level impersonation token of its own (the holder can ask
// who it is, not act as it). Null when the client is anonymous, or has no
// SeChangeNotifyPrivilege (then the folders on the way would have to be
// checked too: not served). Call on the thread serving the pipe, after the
// client wrote something.
void* clientIdentificationToken(void* pipe, std::wstring* why = nullptr);

} // namespace ws::ntfs
