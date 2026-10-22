#pragma once

#include "ClipStore.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

namespace ws::clipimport {

enum class Source { Ditto, Text, Csv };

// What a file is, by its name and its first bytes: Ditto's database (a
// SQLite file), a CSV table, or plain text.
Source sourceOf(const QString& path);

// The base name of the group an import goes in: "Ditto 导入", "notes 导入".
QString groupName(const QString& path);

// How a text file is cut into entries.
enum class Split { Lines, Paragraphs, Whole };

struct Options {
    Split split = Split::Paragraphs; // text files only
    // Entries of a file without times (text, CSV with no time column) are
    // given `now`, and one second less each, the first being the newest
    // (0: the time now).
    qint64 now = 0;
};

// Where the entries go, a batch at a time: the history's importClips() on its
// own thread, returning once the batch is on disk. `cancelled` is asked between entries; `progress` tells how many
// of the source's entries were looked at.
struct Hooks {
    std::function<ClipImportResult(std::vector<ClipImportItem>&&)> sink;
    std::function<bool()> cancelled;
    std::function<void(int done, int total)> progress;
};

enum class Error : std::uint8_t {
    None,
    Missing, // no such file
    Unreadable, // cannot be opened or read
    TooLarge, // a text or CSV file over 1 GB
    NotDitto, // a database, but not Ditto's (or protected, or damaged)
    CannotCopy, // Ditto's database could not be copied to read (space, or locked)
    NoHistory, // the clipboard history is not open
    Busy, // another import is running
};

struct Report {
    int added = 0;
    int duplicates = 0;
    int skipped = 0; // empty, too large, or in a format that cannot be used
    int failed = 0; // could not be saved
    bool ownGroupUsed = false; // entries went into the group named after the file
    bool cancelled = false;
    Error error = Error::None;
};

// Reads the file and hands its entries to hooks.sink. Any thread; nothing of
// the source is changed (Ditto's database is read from a copy, Ditto may be
// running).
Report importFile(const QString& path, const Options& options, const Hooks& hooks);

// The pieces, for tests.
QString decodeText(const QByteArray& bytes); // UTF-8, UTF-16 by its BOM, else GBK
QStringList splitText(const QString& text, Split split);
std::vector<QStringList> parseCsv(const QString& text, QChar delimiter = QChar());

} // namespace ws::clipimport
