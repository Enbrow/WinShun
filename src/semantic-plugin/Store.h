#pragma once

// The vectors of the files' passages, in the plugin's data folder. Segment
// files, each written once (to a temporary name, then renamed) and then only
// read, memory-mapped: the vectors are not the process's private memory,
// so a plugin that waits costs next to nothing, and Windows takes the pages
// back when it needs them. A later segment's record of a file replaces the
// earlier ones; a "removed" record takes it away. When more than a third
// of what the segments hold is replaced, or there are many of them, they
// are written again as one (compact).
//
// Vectors are kept as int8 with a scale each (a quarter of floats; the
// ranking hardly changes). Thread safety: search() alongside one writer.

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace semantic {

struct NewPassage {
    std::string where;
    std::string snippet;
    std::vector<float> vector; // normalized
};

struct NewFile {
    std::string path;
    std::uint32_t stamp = 0; // list_documents' stamp: changes when the file does
    std::vector<NewPassage> passages; // none: nothing to find in it (kept, so it is not read again)
};

struct Hit {
    std::string path;
    std::string where;
    std::string snippet;
    float score = 0; // cosine similarity of the best passage
    int passages = 0; // how many of the file's passages were among the best
};

class Store {
public:
    explicit Store(std::wstring folder);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // Opens what is there for `signature` (the model and how it is asked);
    // what was made for another is deleted. False if the folder is unusable.
    bool open(const std::string& signature);
    void wipe(); // everything gone (another model)

    // The files it has, path -> stamp.
    std::unordered_map<std::string, std::uint32_t> stamps() const;
    std::size_t files() const;
    std::size_t passages() const;

    // Written at the next flush(); add() of a file it has replaces it.
    void add(NewFile file);
    void remove(const std::string& path);
    std::size_t pending() const;
    bool flush();
    bool compactIfWorth();

    std::vector<Hit> search(const std::vector<float>& query, std::size_t files) const;

private:
    struct Segment;
    struct File {
        std::string path;
        std::uint32_t stamp = 0;
        std::uint32_t segment = 0;
        std::size_t offset = 0; // the record in the segment
        std::size_t size = 0;
        std::uint32_t firstPassage = 0;
        std::uint32_t passages = 0;
        bool live = true;
    };
    struct Passage {
        const std::int8_t* vector;
        float scale;
        std::uint32_t file;
        std::string_view where;
        std::string_view snippet;
    };

    bool load(); // the segments in the folder, in order
    bool mapSegment(const std::wstring& path);
    void unmapAll();
    bool writeSegment(const std::vector<std::string>& records, std::uint32_t& number);
    std::uint32_t nextNumber() const;
    void kill(const std::string& key);

    std::wstring m_folder;
    std::string m_signature;
    std::uint32_t m_dims = 0;
    std::vector<Segment*> m_segments;
    std::vector<File> m_files;
    std::vector<Passage> m_passages;
    std::unordered_map<std::string, std::uint32_t> m_byPath; // lower-case path -> live file
    std::size_t m_deadPassages = 0;
    std::vector<std::string> m_pending; // records to write
    std::uint32_t m_pendingDims = 0;
    mutable std::shared_mutex m_mutex;
};

} // namespace semantic
