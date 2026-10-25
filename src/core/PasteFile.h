#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// 粘贴为文件 (the PasteEx request): what the clipboard holds, made a file in
// the folder shown, then selected there for its name to be typed. Pure
// logic, no Qt and no windows.h: which formats count, which kind of file the
// words or the picture make, its name and the text it holds. The stub reads
// the clipboard and writes the file (src/stub/PasteAsFile.cpp, for Explorer's
// right-click and for Ctrl+V); Win顺's keyboard hook asks takesCtrlV
// (platform/TypeToSearch). Unit-tested (tests/tst_pastefile.cpp).
namespace ws::pastefile {

// What the clipboard offers, by its formats (IsClipboardFormatAvailable:
// nothing is opened or read).
struct Offer {
    bool files = false; // CF_HDROP, "Shell IDList Array": Explorer's own Paste
    // "FileGroupDescriptorW" or "FileGroupDescriptor" with "FileContents":
    // files that are not on disk yet (Outlook's attachments, 7-Zip, a
    // browser's), which Explorer's Paste makes.
    bool virtualFiles = false;
    bool text = false; // CF_UNICODETEXT (Windows makes it from CF_TEXT)
    bool image = false; // "PNG", CF_DIB, CF_BITMAP
};

// Whether Ctrl+V on the files of an Explorer folder or of the desktop is
// ours: words or a picture to make a file of, and nothing Explorer pastes
// itself. Measured (2026-10-24, Windows 11 26200): a folder's drop target,
// which its Paste uses, takes none of text, pictures, "PNG", HTML, RTF, CSV
// and URLs (DROPEFFECT_NONE); CF_HDROP it copies. So Ctrl+V did nothing there.
constexpr bool takesCtrlV(const Offer& offer) noexcept
{
    return !offer.files && !offer.virtualFiles && (offer.text || offer.image);
}

// What the clipboard held, as read.
struct Clip {
    std::wstring text; // CF_UNICODETEXT up to its first NUL; empty without
    bool spreadsheet = false; // "Csv" alongside: cells of Excel or WPS
    bool image = false; // a picture is there too (read once chosen)
};

// The kinds of file made, each with its extension and how its text is written.
enum class Kind {
    Text, // .txt
    Table, // .csv: spreadsheet cells
    Web, // .html: a whole page
    Svg, // .svg
    Xml, // .xml
    Json, // .json: valid JSON, an object or an array
    Registry, // .reg: "Windows Registry Editor Version 5.00" (or REGEDIT4)
    Batch, // .bat: starts with "@echo off"
    Shell, // .sh: "#!" naming sh, bash or zsh
    Python, // .py: "#!" naming python
    Picture, // .png
};

// The picture when there is no word worth a file (none, or blank), or when
// the words are just the address of a picture copied from a browser (Firefox
// puts its URL alongside); else the words, as textKind says. Nothing when
// neither is there.
std::optional<Kind> choose(const Clip& clip);
// What the words make: by an unmistakable start alone (a JSON value is parsed
// whole); cells of a spreadsheet, more than one, make a table.
Kind textKind(std::wstring_view text, bool spreadsheet);
std::wstring_view extension(Kind kind); // ".txt"

// How a kind's text goes into its file.
enum class Encoding {
    Utf8, // without a BOM, as Notepad writes it
    Utf8Bom, // .csv: Excel takes a file without it for the ANSI code page
    Utf16Bom, // .reg: regedit reads version 5 files as UTF-16 LE
    // .bat: cmd reads batch files in the console's OEM code page; in UTF-8
    // when that cannot hold every character (no better choice there).
    OemOrUtf8,
};
struct TextFile {
    std::wstring text;
    Encoding encoding = Encoding::Utf8;
};
// The text written for `kind`: a table made CSV with `separator` between
// cells (the user's list separator: Excel splits by it), CRLF lines for
// batch and registry files (cmd's labels and goto misread lines ending in
// LF alone), LF lines for shell scripts; the rest as copied.
TextFile textFile(Kind kind, std::wstring_view text, wchar_t separator = L',');

// Spreadsheet cells as their programs copy them (CF_UNICODETEXT): a tab
// between cells, CRLF after each row, a cell holding a tab, a line break or
// a quote at its start in quotes with its quotes doubled. Made CSV (RFC
// 4180): cells holding the separator, a quote or a line break in quotes.
std::wstring csvFromCells(std::wstring_view cells, wchar_t separator);

// Whether the text is one JSON value (RFC 8259), an object or an array.
bool isJson(std::wstring_view text);

// The words a made-up name starts with, in the language Win顺 shows; the
// time of the paste goes after them ("图片 2026-10-24 151230").
struct Words {
    std::wstring picture = L"图片";
    std::wstring table = L"表格";
    std::wstring text = L"文本";
    std::wstring web = L"网页";
    std::wstring data = L"数据";
    std::wstring script = L"脚本";
    std::wstring registry = L"注册表";
};
Words englishWords();
// "2026-10-24 151230", as Windows' own screenshots are named.
std::wstring stamp(int year, int month, int day, int hour, int minute, int second);

// The file's name without its extension. Words name themselves by their
// first line that says something (a heading's # and a list's - left off),
// a web page by its <title>; the rest, and words that give no name, by the
// kind's word and `when` (stamp). Fit for a file name (cleanName).
std::wstring baseName(Kind kind, std::wstring_view text, const Words& words, std::wstring_view when);
// A line made a file name of at most `maxChars` characters: no characters
// Windows refuses (< > : " / \ | ? * and controls), white space run into one
// space, no space or dot at either end (Windows drops those at the end), cut
// whole (not inside a surrogate pair), at a space when one comes near the
// end; a device name (CON, NUL, COM1...) gets a "_". Empty when nothing is left.
std::wstring cleanName(std::wstring_view line, std::size_t maxChars = 40);
// The text of a page's <title>, its entities decoded; empty without one.
std::wstring htmlTitle(std::wstring_view html);
// `base` shortened so that the whole path, with `extension` and room for
// Explorer's " (2)", stays inside MAX_PATH; at least one character is kept.
std::wstring fitName(std::wstring_view base, std::wstring_view extension, std::size_t folderLength);

// UTF-16 to UTF-8; a lone surrogate becomes U+FFFD.
std::string utf8(std::wstring_view text);

} // namespace ws::pastefile
