#include "PasteFile.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>

namespace ws::pastefile {

namespace {

// White space, the kinds pasted text has: no-break and ideographic spaces,
// the zero-width ones and a BOM besides.
constexpr bool isSpace(wchar_t c) noexcept
{
    return c == L' ' || (c >= L'\t' && c <= L'\r') || c == 0x00A0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200B)
        || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

// A character that says something: a letter, a digit, a Chinese character;
// not punctuation, symbols, arrows or box drawing (a line of "-----" or
// "【】" names nothing). A surrogate (emoji) is not one either.
constexpr bool isWordChar(wchar_t c) noexcept
{
    if (c < 0x80)
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
    return !isSpace(c) && !(c >= 0x00A1 && c <= 0x00BF) && c != 0x00D7 && c != 0x00F7 && !(c >= 0x2010 && c <= 0x205E)
        && !(c >= 0x2190 && c <= 0x2BFF) && !(c >= 0x3000 && c <= 0x303F) && !(c >= 0xD800 && c <= 0xDFFF)
        && !(c >= 0xFE10 && c <= 0xFE1F) && !(c >= 0xFE30 && c <= 0xFE4F) && !(c >= 0xFF01 && c <= 0xFF0F)
        && !(c >= 0xFF1A && c <= 0xFF20) && !(c >= 0xFF3B && c <= 0xFF40) && !(c >= 0xFF5B && c <= 0xFF65)
        && c < 0xFFF0;
}

constexpr bool isHighSurrogate(wchar_t c) noexcept
{
    return c >= 0xD800 && c <= 0xDBFF;
}

constexpr bool isLowSurrogate(wchar_t c) noexcept
{
    return c >= 0xDC00 && c <= 0xDFFF;
}

constexpr wchar_t lowerAscii(wchar_t c) noexcept
{
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

std::wstring_view trimmed(std::wstring_view text)
{
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

// `text` starts with the ASCII `prefix`, in any case.
bool startsWithNoCase(std::wstring_view text, std::wstring_view prefix)
{
    if (text.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (lowerAscii(text[i]) != lowerAscii(prefix[i]))
            return false;
    }
    return true;
}

// A tag's name ends there: "<svg>" and "<svg " are svg, "<svgx" is not.
bool startsWithTag(std::wstring_view text, std::wstring_view tag)
{
    return startsWithNoCase(text, tag)
        && (text.size() == tag.size() || text[tag.size()] == L'>' || text[tag.size()] == L'/' || isSpace(text[tag.size()]));
}

std::wstring lowerCopy(std::wstring_view text)
{
    std::wstring lower(text);
    for (wchar_t& c : lower)
        c = lowerAscii(c);
    return lower;
}

std::wstring_view firstLine(std::wstring_view text)
{
    const std::size_t end = text.find_first_of(L"\r\n");
    return end == std::wstring_view::npos ? text : text.substr(0, end);
}

// Each line of `text` in turn, its line break left off.
template <typename Visit> void forEachLine(std::wstring_view text, Visit visit)
{
    while (!text.empty()) {
        const std::size_t end = text.find_first_of(L"\r\n");
        if (!visit(text.substr(0, end)) || end == std::wstring_view::npos)
            return;
        text.remove_prefix(end + (text[end] == L'\r' && end + 1 < text.size() && text[end + 1] == L'\n' ? 2 : 1));
    }
}

// A single address and nothing else: what Firefox copies with a picture.
bool isLoneAddress(std::wstring_view text)
{
    for (const wchar_t c : text) {
        if (isSpace(c))
            return false;
    }
    for (const std::wstring_view scheme : {L"http://", L"https://", L"ftp://", L"file:", L"data:"}) {
        if (startsWithNoCase(text, scheme))
            return true;
    }
    return false;
}

// Line breaks all made `lineBreak`: CRLF, LF and CR alike.
std::wstring withLineBreaks(std::wstring_view text, std::wstring_view lineBreak)
{
    std::wstring out;
    out.reserve(text.size() + text.size() / 16);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r' || text[i] == L'\n') {
            if (text[i] == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n')
                ++i;
            out += lineBreak;
        } else {
            out += text[i];
        }
    }
    return out;
}

// RFC 8259, by recursive descent: whether `text` (from `at`) is one value.
class JsonReader {
public:
    explicit JsonReader(std::wstring_view text) : m_text(text) {}

    bool whole()
    {
        if (!value())
            return false;
        space();
        return m_at == m_text.size();
    }

private:
    static constexpr int kMaxDepth = 512;

    bool more() const { return m_at < m_text.size(); }
    wchar_t peek() const { return m_text[m_at]; }

    void space()
    {
        while (more() && (peek() == L' ' || peek() == L'\t' || peek() == L'\n' || peek() == L'\r'))
            ++m_at;
    }

    bool value()
    {
        space();
        if (!more())
            return false;
        switch (peek()) {
        case L'{':
            return container(L'}', true);
        case L'[':
            return container(L']', false);
        case L'"':
            return string();
        case L't':
            return literal(L"true");
        case L'f':
            return literal(L"false");
        case L'n':
            return literal(L"null");
        default:
            return number();
        }
    }

    // An object (`keyed`, members "key": value) or an array, up to `close`.
    bool container(wchar_t close, bool keyed)
    {
        if (++m_depth > kMaxDepth)
            return false;
        ++m_at;
        space();
        if (more() && peek() == close) {
            ++m_at;
            --m_depth;
            return true;
        }
        for (;;) {
            if (keyed) {
                space();
                if (!more() || peek() != L'"' || !string())
                    return false;
                space();
                if (!more() || peek() != L':')
                    return false;
                ++m_at;
            }
            if (!value())
                return false;
            space();
            if (!more())
                return false;
            if (peek() == L',') {
                ++m_at;
                continue;
            }
            if (peek() != close)
                return false;
            ++m_at;
            --m_depth;
            return true;
        }
    }

    bool string()
    {
        ++m_at; // the opening quote
        while (more()) {
            const wchar_t c = m_text[m_at++];
            if (c == L'"')
                return true;
            if (c < 0x20)
                return false;
            if (c != L'\\')
                continue;
            if (!more())
                return false;
            const wchar_t escaped = m_text[m_at++];
            if (escaped == L'u') {
                for (int i = 0; i < 4; ++i, ++m_at) {
                    if (!more() || !std::iswxdigit(peek()))
                        return false;
                }
            } else if (!std::wcschr(L"\"\\/bfnrt", escaped) || escaped == 0) {
                return false;
            }
        }
        return false;
    }

    bool digits()
    {
        const std::size_t from = m_at;
        while (more() && peek() >= L'0' && peek() <= L'9')
            ++m_at;
        return m_at > from;
    }

    bool number()
    {
        if (more() && peek() == L'-')
            ++m_at;
        if (more() && peek() == L'0')
            ++m_at;
        else if (!digits())
            return false;
        if (more() && peek() == L'.') {
            ++m_at;
            if (!digits())
                return false;
        }
        if (more() && (peek() == L'e' || peek() == L'E')) {
            ++m_at;
            if (more() && (peek() == L'+' || peek() == L'-'))
                ++m_at;
            if (!digits())
                return false;
        }
        return true;
    }

    bool literal(std::wstring_view word)
    {
        if (m_text.substr(m_at, word.size()) != word)
            return false;
        m_at += word.size();
        return true;
    }

    std::wstring_view m_text;
    std::size_t m_at = 0;
    int m_depth = 0;
};

// The first line of the words that says something, as a name: a Markdown
// heading's #, a quote's >, a list's bullet left off. Empty when none of the
// first lines does.
std::wstring firstLineName(std::wstring_view text)
{
    std::wstring name;
    int looked = 0;
    forEachLine(text, [&](std::wstring_view line) {
        while (!line.empty() && (isSpace(line.front()) || std::wcschr(L"#>-*+=~`|•·", line.front()) != nullptr))
            line.remove_prefix(1);
        for (const wchar_t c : line) {
            if (isWordChar(c)) {
                name = cleanName(line);
                break;
            }
        }
        return name.empty() && ++looked < 20;
    });
    return name;
}

// HTML's character references, the usual named ones and numbers; others stay.
std::wstring decodeEntities(std::wstring_view text)
{
    std::wstring out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const std::size_t end = text[i] == L'&' ? text.find(L';', i) : std::wstring_view::npos;
        if (end == std::wstring_view::npos || end - i > 10) {
            out += text[i];
            continue;
        }
        const std::wstring name = lowerCopy(text.substr(i + 1, end - i - 1));
        unsigned long code = 0;
        if (name == L"amp")
            code = L'&';
        else if (name == L"lt")
            code = L'<';
        else if (name == L"gt")
            code = L'>';
        else if (name == L"quot")
            code = L'"';
        else if (name == L"apos")
            code = L'\'';
        else if (name == L"nbsp")
            code = L' ';
        else if (name.size() > 1 && name[0] == L'#') {
            const bool hex = name[1] == L'x';
            wchar_t* stop = nullptr;
            code = std::wcstoul(name.c_str() + (hex ? 2 : 1), &stop, hex ? 16 : 10);
            if (!stop || *stop != 0)
                code = 0;
        }
        if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            out += text[i];
            continue;
        }
        if (code > 0xFFFF) {
            code -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (code >> 10));
            out += static_cast<wchar_t>(0xDC00 + (code & 0x3FF));
        } else {
            out += static_cast<wchar_t>(code);
        }
        i = end;
    }
    return out;
}

// Whether a name, up to its first dot, is one of the devices Windows keeps
// for itself (CON.txt is the console too).
bool isDeviceName(std::wstring_view name)
{
    name = name.substr(0, name.find(L'.'));
    while (!name.empty() && name.back() == L' ')
        name.remove_suffix(1);
    const std::wstring lower = lowerCopy(name);
    for (const std::wstring_view device : {L"con", L"prn", L"aux", L"nul"}) {
        if (lower == device)
            return true;
    }
    if (lower.size() == 4 && (lower.starts_with(L"com") || lower.starts_with(L"lpt"))) {
        const wchar_t n = lower[3];
        return (n >= L'0' && n <= L'9') || n == 0x00B9 || n == 0x00B2 || n == 0x00B3; // COM¹ to COM³ too
    }
    return false;
}

void dropTrailingSpacesAndDots(std::wstring& name)
{
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.'))
        name.pop_back();
}

// The first `count` characters (code points) of `text`, a surrogate pair whole.
std::size_t unitsOfChars(std::wstring_view text, std::size_t count)
{
    std::size_t at = 0;
    for (std::size_t chars = 0; at < text.size() && chars < count; ++chars)
        at += isHighSurrogate(text[at]) && at + 1 < text.size() && isLowSurrogate(text[at + 1]) ? 2 : 1;
    return at;
}

} // namespace

std::optional<Kind> choose(const Clip& clip)
{
    const std::wstring_view words = trimmed(clip.text);
    if (!words.empty() && !(clip.image && isLoneAddress(words)))
        return textKind(clip.text, clip.spreadsheet);
    if (clip.image)
        return Kind::Picture;
    return std::nullopt;
}

Kind textKind(std::wstring_view text, bool spreadsheet)
{
    const std::wstring_view t = trimmed(text);
    if (t.empty())
        return Kind::Text;
    if (spreadsheet && t.find_first_of(L"\t\n") != std::wstring_view::npos)
        return Kind::Table;
    if (startsWithNoCase(t, L"Windows Registry Editor Version 5.00") || startsWithNoCase(t, L"REGEDIT4"))
        return Kind::Registry;
    const std::wstring_view first = trimmed(firstLine(t));
    if (first.size() == 9 && startsWithNoCase(first, L"@echo off"))
        return Kind::Batch;
    if (first.starts_with(L"#!")) {
        const std::wstring program = lowerCopy(first);
        if (program.find(L"python") != std::wstring::npos)
            return Kind::Python;
        for (const std::wstring_view shell : {L"/sh", L" sh", L"bash", L"zsh"}) {
            if (program.find(shell) != std::wstring::npos)
                return Kind::Shell;
        }
        return Kind::Text;
    }
    if (startsWithNoCase(t, L"<?xml"))
        return lowerCopy(t.substr(0, 4096)).find(L"<svg") != std::wstring::npos ? Kind::Svg : Kind::Xml;
    if (startsWithTag(t, L"<svg"))
        return Kind::Svg;
    if (startsWithNoCase(t, L"<!doctype html") || startsWithTag(t, L"<html"))
        return Kind::Web;
    if ((t.front() == L'{' || t.front() == L'[') && isJson(t))
        return Kind::Json;
    return Kind::Text;
}

std::wstring_view extension(Kind kind)
{
    switch (kind) {
    case Kind::Table:
        return L".csv";
    case Kind::Web:
        return L".html";
    case Kind::Svg:
        return L".svg";
    case Kind::Xml:
        return L".xml";
    case Kind::Json:
        return L".json";
    case Kind::Registry:
        return L".reg";
    case Kind::Batch:
        return L".bat";
    case Kind::Shell:
        return L".sh";
    case Kind::Python:
        return L".py";
    case Kind::Picture:
        return L".png";
    case Kind::Text:
        break;
    }
    return L".txt";
}

TextFile textFile(Kind kind, std::wstring_view text, wchar_t separator)
{
    switch (kind) {
    case Kind::Table:
        return {csvFromCells(text, separator), Encoding::Utf8Bom};
    case Kind::Registry:
        return {withLineBreaks(text, L"\r\n"), Encoding::Utf16Bom};
    case Kind::Batch:
        return {withLineBreaks(text, L"\r\n"), Encoding::OemOrUtf8};
    case Kind::Shell:
        return {withLineBreaks(text, L"\n"), Encoding::Utf8};
    default:
        return {std::wstring(text), Encoding::Utf8};
    }
}

std::wstring csvFromCells(std::wstring_view cells, wchar_t separator)
{
    std::wstring out;
    out.reserve(cells.size() + cells.size() / 8);
    bool firstInRow = true;
    const auto put = [&](std::wstring_view cell) {
        if (!firstInRow)
            out += separator;
        firstInRow = false;
        if (cell.find_first_of(std::wstring {separator, L'"', L'\r', L'\n'}) == std::wstring_view::npos) {
            out += cell;
            return;
        }
        out += L'"';
        for (const wchar_t c : cell) {
            if (c == L'"')
                out += L'"';
            out += c;
        }
        out += L'"';
    };
    const std::size_t n = cells.size();
    std::size_t i = 0;
    while (i < n) {
        // One cell: in quotes when it starts with one and the closing quote
        // ends it; else as it stands, up to the next tab or line break.
        std::wstring cell;
        bool quoted = false;
        if (cells[i] == L'"') {
            std::size_t j = i + 1;
            std::wstring inside;
            bool closed = false;
            while (j < n) {
                if (cells[j] == L'"') {
                    if (j + 1 < n && cells[j + 1] == L'"') {
                        inside += L'"';
                        j += 2;
                        continue;
                    }
                    closed = true;
                    ++j;
                    break;
                }
                inside += cells[j++];
            }
            if (closed && (j == n || cells[j] == L'\t' || cells[j] == L'\r' || cells[j] == L'\n')) {
                cell = std::move(inside);
                i = j;
                quoted = true;
            }
        }
        if (!quoted) {
            const std::size_t end = std::min(cells.find_first_of(L"\t\r\n", i), n);
            cell.assign(cells.substr(i, end - i));
            i = end;
        }
        put(cell);
        if (i < n && cells[i] == L'\t') {
            if (++i < n)
                continue;
            put({}); // the last cell, left empty, and the end
        }
        // The row ends (at a line break, or the end): none made after the last one.
        if (i < n)
            i += cells[i] == L'\r' && i + 1 < n && cells[i + 1] == L'\n' ? 2 : 1;
        out += L"\r\n";
        firstInRow = true;
    }
    return out;
}

bool isJson(std::wstring_view text)
{
    const std::wstring_view t = trimmed(text);
    return !t.empty() && (t.front() == L'{' || t.front() == L'[') && JsonReader(t).whole();
}

Words englishWords()
{
    return {L"Image", L"Table", L"Text", L"Web page", L"Data", L"Script", L"Registry"};
}

std::wstring stamp(int year, int month, int day, int hour, int minute, int second)
{
    wchar_t text[32] {};
    std::swprintf(text, std::size(text), L"%04d-%02d-%02d %02d%02d%02d", year, month, day, hour, minute, second);
    return text;
}

std::wstring baseName(Kind kind, std::wstring_view text, const Words& words, std::wstring_view when)
{
    const auto stamped = [when](const std::wstring& word) { return word + L' ' + std::wstring(when); };
    switch (kind) {
    case Kind::Text:
        if (std::wstring name = firstLineName(text); !name.empty())
            return name;
        return stamped(words.text);
    case Kind::Web:
        if (std::wstring name = cleanName(htmlTitle(text)); !name.empty())
            return name;
        return stamped(words.web);
    case Kind::Table:
        return stamped(words.table);
    case Kind::Json:
    case Kind::Xml:
        return stamped(words.data);
    case Kind::Registry:
        return stamped(words.registry);
    case Kind::Batch:
    case Kind::Shell:
    case Kind::Python:
        return stamped(words.script);
    case Kind::Svg:
    case Kind::Picture:
        break;
    }
    return stamped(words.picture);
}

std::wstring cleanName(std::wstring_view line, std::size_t maxChars)
{
    std::wstring name;
    name.reserve(line.size());
    bool gap = false;
    for (const wchar_t c : line) {
        if (c < 0x20 || c == 0x7F || isSpace(c) || std::wcschr(L"<>:\"/\\|?*", c) != nullptr) {
            gap = !name.empty();
            continue;
        }
        if (gap)
            name += L' ';
        gap = false;
        name += c;
    }
    dropTrailingSpacesAndDots(name);
    const std::size_t cut = unitsOfChars(name, maxChars);
    if (cut < name.size()) {
        // At a space near the end, so that a word stays whole.
        const std::size_t space = name.rfind(L' ', cut);
        name.resize(space != std::wstring::npos && space >= cut * 2 / 3 ? space : cut);
        dropTrailingSpacesAndDots(name);
    }
    if (!name.empty() && isDeviceName(name))
        name.insert(std::min(name.find(L'.'), name.size()), L"_");
    return name;
}

std::wstring htmlTitle(std::wstring_view html)
{
    const std::wstring lower = lowerCopy(html.substr(0, 64 * 1024));
    std::size_t open = 0;
    while ((open = lower.find(L"<title", open)) != std::wstring::npos) {
        const std::wstring_view after = std::wstring_view(lower).substr(open);
        if (startsWithTag(after, L"<title"))
            break;
        open += 6;
    }
    if (open == std::wstring::npos)
        return {};
    const std::size_t start = lower.find(L'>', open);
    const std::size_t end = start == std::wstring::npos ? start : lower.find(L"</title", start);
    if (end == std::wstring::npos)
        return {};
    return decodeEntities(html.substr(start + 1, end - start - 1));
}

std::wstring fitName(std::wstring_view base, std::wstring_view extension, std::size_t folderLength)
{
    // MAX_PATH less its NUL, the backslash before the name, " (99)".
    constexpr std::size_t kMaxPath = 260 - 1;
    constexpr std::size_t kCollision = 5;
    const std::size_t taken = folderLength + 1 + extension.size() + kCollision;
    const std::size_t room = taken < kMaxPath ? kMaxPath - taken : 0;
    if (base.size() <= room)
        return std::wstring(base);
    const std::size_t first = unitsOfChars(base, 1);
    std::size_t keep = room;
    if (keep > 0 && keep < base.size() && isLowSurrogate(base[keep]))
        --keep; // not half a pair
    std::wstring name(base.substr(0, std::max(keep, first)));
    dropTrailingSpacesAndDots(name);
    return name.empty() ? std::wstring(base.substr(0, first)) : name;
}

std::string utf8(std::wstring_view text)
{
    std::string out;
    out.reserve(text.size() * 3);
    for (std::size_t i = 0; i < text.size(); ++i) {
        unsigned long c = text[i];
        if (isHighSurrogate(text[i]) && i + 1 < text.size() && isLowSurrogate(text[i + 1])) {
            c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

} // namespace ws::pastefile
