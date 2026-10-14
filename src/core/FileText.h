#pragma once

// The text of one file, for whoever asks to read it whole (an AI client
// through MCP, a plugin with content:read): a document's (Word, Excel,
// PowerPoint, PDF) as the content index keeps it, or read out now by the
// extractor when the index has none; a text file's, decoded from the disk.
//
// Reading goes to the disk and may wait for an extractor: call it from a
// worker thread, never the GUI's.

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ws {

class ContentIndex;
class DocExtractor;
class FileIndex;

namespace filetext {

enum class Status {
    Ok,
    Missing, // no such file
    Folder,
    NotText, // a binary file
    TooLarge, // more bytes than `maxBytes`
    CloudOnly, // a cloud file not on this computer: reading it would download it
    Unreadable, // in use, no access, or the extractor failed on it
};

struct Result {
    Status status = Status::Unreadable;
    // The whole text. A document's has "[page 3]", "[slide 2]" and
    // "[sheet Name]" lines where those begin.
    QString text;
    bool document = false;
    bool fromIndex = false; // the content index had it: no file was opened
    bool cut = false; // the file has more than was read (the text limit)
};

struct Sources {
    std::shared_ptr<const FileIndex> index; // may be null
    std::shared_ptr<const ContentIndex> content; // may be null
    DocExtractor* extractor = nullptr; // null: documents the index has no text of are Unreadable
};

// `path`: full, native ("C:\dir\file.docx"). `maxBytes`: the largest file to
// read (a document's extraction; a text file is read up to its first 16 MB
// regardless).
Result read(const std::wstring& path, const Sources& sources, std::int64_t maxBytes, const std::function<bool()>& cancelled);

} // namespace filetext
} // namespace ws
