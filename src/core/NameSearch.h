#pragma once

#include "FileIndex.h"
#include "Query.h"
#include "SearchTypes.h"
#include "WorkerPool.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ws {

struct NameHit {
    EntryId id = kNoEntry;
    int score = 0;
    FileTime modified = 0;
};

struct NameSearchOptions {
    std::size_t limit = 100;
    bool foldersOnly = false;
    RankBy rankBy = RankBy::Match;
    // The best `limit` files and the best `limit` folders, each apart: for a
    // list that puts one kind before the other (KindOrder).
    bool kindsApart = false;
};

struct NameSearchOutput {
    std::vector<NameHit> hits; // best first
    std::size_t totalMatches = 0;
    bool cancelled = false;
};

// Scans the whole index in parallel and keeps the best matches.
// Caller holds index.readLock().
NameSearchOutput searchNames(const FileIndex& index, const NameMatcher& matcher, const NameSearchOptions& options,
    WorkerPool& pool, const std::function<bool()>& isCancelled);

struct NewestOptions {
    std::size_t limit = 5;
    // The folders those files are in instead, each once (a folder's own time
    // changes with whatever comes and goes in it, a program's files too).
    bool folders = false;
    std::size_t perFolder = 2; // files: at most this many from one folder
    std::size_t perTree = 3; // ...and from one project or other tree of folders (F:\Project, Documents\Work)
    // Later than this is a clock gone wrong (a file from a camera or another
    // computer dated in the future): left out.
    FileTime notAfter = 0xFFFF'FFFFu;
    // Files: only those with one of these extensions (folded, without the
    // dot); empty: any.
    std::vector<std::string> extensions;
};

struct NewestOutput {
    std::vector<NameHit> hits; // newest first; a folder's `modified` is its newest file's
    bool cancelled = false;
};

// The files (or folders) written last that are the user's own work: not
// under system, program or tool folders (EntryFlag::LowPriority), not
// hidden, not what tools write as they go (isWorkFile). Caller holds
// index.readLock().
NewestOutput newestFiles(const FileIndex& index, const NewestOptions& options, WorkerPool& pool,
    const std::function<bool()>& isCancelled);

// A name that tools write as they go, not one a person works on: dot-names,
// Office's "~$" lock files, downloads under way, logs, compiler output.
bool isWorkFile(std::string_view name, bool isDir) noexcept;

// Where a path typed from some folder along the way leads ("tools/out/" ->
// every folder out in a folder tools): `names` (UTF-8, outermost first) one
// inside the other, each just so, ASCII case aside as NTFS has it. Folders
// alone for `foldersOnly`. At most `limit`, lowest ids first; none when
// cancelled. Caller holds index.readLock().
std::vector<EntryId> findRelativePath(const FileIndex& index, const std::vector<std::string>& names, bool foldersOnly,
    std::size_t limit, WorkerPool& pool, const std::function<bool()>& isCancelled);

} // namespace ws
