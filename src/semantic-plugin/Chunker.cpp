#include "Chunker.h"

#include <string_view>

namespace semantic {

namespace {

bool continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

bool isMark(std::string_view line)
{
    return line.size() > 7 && line.back() == ']'
        && (line.starts_with("[page ") || line.starts_with("[slide ") || line.starts_with("[sheet "));
}

// The byte where `chars` code points of `s` (from `from`) end.
std::size_t advance(std::string_view s, std::size_t from, std::size_t chars)
{
    std::size_t at = from;
    for (std::size_t n = 0; at < s.size() && n < chars; ++n) {
        ++at;
        while (at < s.size() && continuation(static_cast<unsigned char>(s[at])))
            ++at;
    }
    return at;
}

std::size_t countOf(std::string_view s)
{
    std::size_t n = 0;
    for (const char c : s)
        n += !continuation(static_cast<unsigned char>(c));
    return n;
}

bool blank(std::string_view s)
{
    return s.find_first_not_of(" \t\r\f\v") == std::string_view::npos;
}

} // namespace

std::size_t codePoints(const std::string& text) { return countOf(text); }

std::vector<Passage> passages(const std::string& text)
{
    std::vector<Passage> out;
    Passage current;
    std::size_t chars = 0;
    std::string where;
    const auto close = [&] {
        if (chars > 0 && !blank(current.text) && out.size() < kMostPassages)
            out.push_back(std::move(current));
        current = {};
        chars = 0;
    };
    const auto add = [&](std::string_view piece) {
        const std::size_t n = countOf(piece);
        if (chars > 0 && chars + n > kMost)
            close();
        if (chars == 0)
            current.where = where;
        else
            current.text += '\n';
        current.text.append(piece);
        chars += n;
        if (chars >= kTarget)
            close();
    };
    const std::string_view all(text);
    for (std::size_t at = 0; at < all.size() && out.size() < kMostPassages;) {
        std::size_t end = all.find('\n', at);
        if (end == std::string_view::npos)
            end = all.size();
        std::string_view line = all.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (isMark(line)) {
            where = std::string(line);
            if (chars == 0)
                current.where = where;
            continue;
        }
        if (blank(line)) {
            if (chars >= kTarget / 2) // a paragraph ends: a good place for a passage to
                close();
            continue;
        }
        // A line longer than a passage goes in pieces.
        for (std::size_t from = 0; from < line.size();) {
            const std::size_t to = advance(line, from, kMost);
            add(line.substr(from, to - from));
            from = to;
        }
    }
    close();
    return out;
}

std::string snippet(const std::string& text, std::size_t chars)
{
    std::string out;
    bool space = false;
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !out.empty();
            continue;
        }
        if (!continuation(c)) {
            if (n >= chars) {
                out += "…";
                break;
            }
            if (space) {
                out += ' ';
                ++n;
            }
            space = false;
            ++n;
        }
        out += static_cast<char>(c);
    }
    return out;
}

} // namespace semantic
