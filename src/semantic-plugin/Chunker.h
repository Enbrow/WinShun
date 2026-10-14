#pragma once

// A file's text in passages for the model: whole lines, about kTarget
// characters each, never more than kMost; a page, slide or sheet mark
// ("[page 3]", from Win顺's read_file_text) is noted as where the passage is.

#include <string>
#include <vector>

namespace semantic {

struct Passage {
    std::string text; // UTF-8
    std::string where; // "[page 3]" (the mark it is under), or empty
};

constexpr std::size_t kTarget = 600; // characters (code points)
constexpr std::size_t kMost = 1000;
constexpr std::size_t kMostPassages = 48; // of one file: about 40 000 characters, the rest is not sent

std::vector<Passage> passages(const std::string& text);

// A short line from a passage for a result row: whitespace folded, at most
// `chars` characters.
std::string snippet(const std::string& text, std::size_t chars);

std::size_t codePoints(const std::string& text);

} // namespace semantic
