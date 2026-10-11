#include "Query.h"

#include "PathText.h"
#include "TextUtil.h"
#include "Wtf8.h"

#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

std::string foldedUtf8(const QString& s)
{
    return text::foldAscii(wtf8::fromUtf16(wtf8::view(s)));
}

bool isPathSeparator(QChar c)
{
    return c == u'\\' || c == u'/';
}

const QRegularExpression& separatorRegex()
{
    static const QRegularExpression re(u"[\\\\/]"_s);
    return re;
}

} // namespace

QStringList queryWords(const QString& text)
{
    QStringList tokens;
    QString current;
    bool quoted = false;
    for (const QChar c : text) {
        if (c == u'"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && c.isSpace()) {
            if (!current.isEmpty())
                tokens.append(std::exchange(current, QString()));
            continue;
        }
        current.append(c);
    }
    if (!current.isEmpty())
        tokens.append(current);
    return tokens;
}

bool looksLikeCode(QStringView text)
{
    static constexpr QStringView kSigns[] {u"=", u";", u"(", u")", u"{", u"}", u"<", u">", u"[", u"]", u"->", u"::", u"&&", u"||"};
    return std::ranges::any_of(kSigns, [&](QStringView s) { return text.contains(s); });
}

QStringList contentTerms(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};
    if (!looksLikeCode(trimmed))
        return queryWords(trimmed); // an unclosed quote: to the end
    // Quotes around the whole line only make it one phrase, as for words.
    if (trimmed.count(u'"') == 2 && trimmed.size() > 2 && trimmed.startsWith(u'"') && trimmed.endsWith(u'"'))
        return {trimmed.mid(1, trimmed.size() - 2).trimmed()};
    return {trimmed};
}

ParsedQuery parseQuery(const QString& text)
{
    ParsedQuery q;
    // A path typed or pasted whole ("C:\Users\me\Tencent Files\x") is one
    // term: its spaces are in the names, not between terms.
    const pathtext::TypedPath typed = pathtext::splitTyped(text);
    const QStringList tokens = typed.folder.isEmpty()
        ? queryWords(text)
        : QStringList {(typed.folder.endsWith(u'\\') ? typed.folder : typed.folder + u'\\') + typed.name.trimmed()};
    for (QString token : tokens) {
        if (token.startsWith(u"ext:", Qt::CaseInsensitive)) {
            const QStringList exts = token.mid(4).split(QRegularExpression(u"[,;|]"_s), Qt::SkipEmptyParts);
            for (QString ext : exts) {
                while (ext.startsWith(u'*') || ext.startsWith(u'.'))
                    ext.remove(0, 1);
                if (!ext.isEmpty())
                    q.extensions.push_back(foldedUtf8(ext));
            }
            continue;
        }

        QueryTerm term;
        if (token.size() > 1 && token.startsWith(u'!')) {
            term.negated = true;
            token.remove(0, 1);
        }

        if (std::any_of(token.cbegin(), token.cend(), isPathSeparator)) {
            QStringList parts = token.split(separatorRegex());
            const QString last = parts.takeLast();
            for (const QString& part : parts) {
                if (!part.isEmpty())
                    term.ancestors.push_back(foldedUtf8(part));
            }
            token = last;
        }

        term.wildcard = token.contains(u'*') || token.contains(u'?');
        term.text = foldedUtf8(token);
        if (term.wildcard) {
            std::size_t start = 0;
            for (std::size_t i = 0; i <= term.text.size(); ++i) {
                if (i == term.text.size() || term.text[i] == '*' || term.text[i] == '?') {
                    if (i - start > term.literal.size())
                        term.literal = term.text.substr(start, i - start);
                    start = i + 1;
                }
            }
        }
        if (term.text.empty() && term.ancestors.empty())
            continue;
        if (!term.negated && !term.wildcard && !token.isEmpty())
            q.highlights.append(token);
        q.terms.push_back(std::move(term));
    }
    return q;
}

NameMatcher::NameMatcher(const ParsedQuery& query)
    : m_extensions(query.extensions)
{
    for (const auto& t : query.terms) {
        if (t.negated) {
            m_negative.push_back(t); // exclusions stay literal: no surprises
            continue;
        }
        const std::string_view text = t.wildcard ? std::string_view() : std::string_view(t.text);
        Positive p {t, pinyin::Matcher(text), pinyin::Matcher(text, true), {}};
        for (const std::string& segment : t.ancestors)
            p.folders.emplace_back(segment);
        m_positive.push_back(std::move(p));
    }
    // Longest term first: it rejects the most names before the others are tried.
    std::stable_sort(m_positive.begin(), m_positive.end(),
        [](const Positive& a, const Positive& b) { return a.term.text.size() > b.term.text.size(); });

    // With something else to find in the name (another term, an extension),
    // a plain word may be a folder's instead: "src app" finds src\app\.
    if (m_positive.size() + (m_extensions.empty() ? 0 : 1) >= 2) {
        for (std::size_t i = 0; i < m_positive.size() && i < 64; ++i) {
            const QueryTerm& t = m_positive[i].term;
            if (!t.wildcard && !t.text.empty() && t.ancestors.empty())
                m_folderTerms |= std::uint64_t {1} << i;
        }
    }
    for (std::size_t i = 0; i < m_positive.size() && i < 64; ++i) {
        const QueryTerm& t = m_positive[i].term;
        if (!t.wildcard && !t.text.empty() && !(m_folderTerms >> i & 1)) {
            m_quick = static_cast<int>(i);
            break;
        }
    }
    m_quickDecides = m_quick >= 0 && m_positive.size() == 1 && m_negative.empty() && m_extensions.empty();
}

namespace {

// Index names are stored with padding, so they can use the SIMD search.
template <bool Padded> std::size_t find(std::string_view haystack, std::string_view folded)
{
    if constexpr (Padded)
        return text::findFoldedPadded(haystack, folded);
    else
        return text::findFolded(haystack, folded);
}

} // namespace

template <bool Padded, typename Han, typename AncestorsMatch, typename FolderTerms>
int NameMatcher::score(std::string_view name, bool isDir, Han&& han, std::size_t extLength,
    AncestorsMatch&& ancestorsMatch, FolderTerms&& folderTerms) const
{
    int score = 0;
    std::size_t covered = 0;
    std::optional<pinyin::Span> pinyinMatch; // of a lone term, for the exact-name bonus
    std::uint64_t elsewhere = 0; // terms not in the name, to be found in its folders
    bool inName = !m_extensions.empty(); // something is in the name (checked below)

    for (std::size_t i = 0; i < m_positive.size(); ++i) {
        const Positive& p = m_positive[i];
        const QueryTerm& t = p.term;
        if (!t.text.empty()) {
            if (t.wildcard) {
                if (!t.literal.empty() && find<Padded>(name, t.literal) == text::npos)
                    return -1;
                if (!text::globMatch(name, t.text))
                    return -1;
                score += 10;
                covered += static_cast<std::size_t>(
                    std::count_if(t.text.begin(), t.text.end(), [](char c) { return c != '*' && c != '?'; }));
            } else if (std::size_t pos = find<Padded>(name, t.text); pos == text::npos) {
                // Not spelled out: maybe as pinyin ("bg" for 报告), ranked a little lower,
                // and lower still when it takes a less common reading (解 as xie).
                auto span = p.pinyin.valid() && han() ? p.pinyin.findUtf8(name) : std::nullopt;
                if (!span) {
                    if (i < 64 && (m_folderTerms >> i & 1)) {
                        elsewhere |= std::uint64_t {1} << i;
                        continue;
                    }
                    return -1;
                }
                int bonus = -12;
                if (const auto common = p.commonPinyin.findUtf8(name)) {
                    span = common;
                    bonus = 0;
                }
                bonus += span->start == 0 ? 26 : text::isWordStart(name, span->start) ? 16 : 4;
                score += bonus;
                covered += span->length;
                pinyinMatch = span;
            } else {
                int bonus = 4;
                if (pos == 0) {
                    bonus = 30;
                } else {
                    // Prefer an occurrence that starts a word ("my_report" > "deportation").
                    for (int tries = 0; tries < 4 && pos != text::npos; ++tries) {
                        if (text::isWordStart(name, pos)) {
                            bonus = 18;
                            break;
                        }
                        pos = text::findFolded(name, t.text, pos + 1);
                    }
                }
                score += bonus;
                covered += t.text.size();
            }
            inName = true;
        }
        if (!t.ancestors.empty() && !ancestorsMatch(t.ancestors, p.folders.data()))
            return -1;
    }

    // "!draft" leaves out names with "draft"; "!node_modules\" everything in
    // such a folder; "!tmp\*.log" only the .log files in one.
    for (const auto& t : m_negative) {
        const bool nameHit
            = t.text.empty() || (t.wildcard ? text::globMatch(name, t.text) : find<Padded>(name, t.text) != text::npos);
        if (nameHit && (t.ancestors.empty() || ancestorsMatch(t.ancestors, nullptr)))
            return -1;
    }

    if (!m_extensions.empty()) {
        if (isDir || extLength == 0)
            return -1;
        const std::string_view ext = name.substr(name.size() - extLength);
        const bool ok = std::any_of(
            m_extensions.begin(), m_extensions.end(), [&](const std::string& e) { return text::equalsFolded(ext, e); });
        if (!ok)
            return -1;
    }

    // The rest in the folders it is in: a little, below any place in the name.
    if (elsewhere != 0) {
        if (!inName || (folderTerms() & elsewhere) != elsewhere)
            return -1;
        score += 3 * std::popcount(elsewhere);
    }

    if (m_positive.size() == 1 && !m_positive[0].term.wildcard && !m_positive[0].term.text.empty()) {
        const std::string& t = m_positive[0].term.text;
        const std::size_t stem
            = extLength > 0 && name.size() > extLength + 1 ? name.size() - extLength - 1 : name.size();
        if (pinyinMatch) {
            if (pinyinMatch->start == 0 && pinyinMatch->length >= stem)
                score += 40; // the whole name (without extension) as pinyin
        } else if (text::equalsFolded(name, t)) {
            score += 60; // exact name
        } else if (stem < name.size() && text::equalsFolded(name.substr(0, stem), t)) {
            score += 45; // exact name without extension
        }
    }

    if (!name.empty())
        score += static_cast<int>(25 * std::min(covered, name.size()) / name.size());
    return score;
}

namespace {

// `pinyin` is null for exclusions, which stay literal.
bool folderMatches(
    std::string_view folder, bool han, const std::string& segment, const pinyin::Matcher* pinyin, bool padded)
{
    if ((padded ? text::findFoldedPadded(folder, segment) : text::findFolded(folder, segment)) != text::npos)
        return true;
    return han && pinyin && pinyin->valid() && pinyin->findUtf8(folder);
}

} // namespace

std::uint64_t NameMatcher::termsInFolder(std::string_view name, bool han, bool padded) const
{
    std::uint64_t terms = 0;
    for (std::uint64_t left = m_folderTerms; left != 0; left &= left - 1) {
        const auto i = static_cast<std::size_t>(std::countr_zero(left));
        if (folderMatches(name, han, m_positive[i].term.text, &m_positive[i].pinyin, padded))
            terms |= std::uint64_t {1} << i;
    }
    return terms;
}

std::uint64_t NameMatcher::termsInFolders(const FileIndex& index, EntryId folder, Folders* folders) const
{
    if (folders && folders->m_slots.empty())
        folders->m_slots.resize(Folders::kSlots);
    const auto slotOf = [&](EntryId id) -> Folders::Slot& { return folders->m_slots[id % Folders::kSlots]; };
    const auto own = [&](EntryId id) {
        const Entry& e = index.entry(id);
        return termsInFolder(index.name(e), e.flags & EntryFlag::Han, true);
    };

    // Up to a folder known already (or the top), then down again, noting each.
    std::array<EntryId, 64> chain;
    std::size_t length = 0;
    std::uint64_t terms = 0;
    EntryId cur = folder;
    for (; cur != kNoEntry && length < chain.size(); cur = index.entry(cur).parent) {
        if (folders && slotOf(cur).folder == cur) {
            terms = slotOf(cur).terms;
            break;
        }
        chain[length++] = cur;
    }
    if (length == chain.size()) { // that deep: the rest unnoted
        for (; cur != kNoEntry; cur = index.entry(cur).parent)
            terms |= own(cur);
    }
    while (length > 0) {
        const EntryId id = chain[--length];
        terms |= own(id);
        if (folders)
            slotOf(id) = {id, terms};
    }
    return terms;
}

int NameMatcher::match(const FileIndex& index, const Entry& entry, Folders& folders) const
{
    return score<true>(
        index.name(entry), entry.isDir(), [&] { return (entry.flags & EntryFlag::Han) != 0; }, index.extLength(entry),
        [&](const std::vector<std::string>& segments, const pinyin::Matcher* folders) {
            // Walk up from the parent, consuming segments innermost-first.
            auto j = static_cast<std::ptrdiff_t>(segments.size()) - 1;
            for (EntryId cur = entry.parent; cur != kNoEntry && j >= 0; cur = index.entry(cur).parent) {
                const auto k = static_cast<std::size_t>(j);
                const Entry& folder = index.entry(cur);
                if (folderMatches(index.name(folder), folder.flags & EntryFlag::Han, segments[k],
                        folders ? &folders[k] : nullptr, true))
                    --j;
            }
            return j < 0;
        },
        [&] { return termsInFolders(index, entry.parent, &folders); });
}

bool NameMatcher::nameMayMatch(std::string_view name, std::size_t fileExtLength, bool han) const
{
    if (m_quick >= 0) {
        const Positive& p = m_positive[static_cast<std::size_t>(m_quick)];
        if (text::findFoldedPadded(name, p.term.text) == text::npos && !(han && p.pinyin.valid() && p.pinyin.findUtf8(name)))
            return false;
        if (m_quickDecides)
            return true; // the term in the name; its folders, if any, are the entry's
    }
    // What depends on the entry, at its most lenient: a file, in folders
    // that hold every term. Exclusions by folder rule nothing out here (they
    // are the calls without pinyin).
    return score<true>(
               name, false, [han] { return han; }, fileExtLength,
               [](const std::vector<std::string>&, const pinyin::Matcher* folders) { return folders != nullptr; },
               [] { return ~std::uint64_t {0}; })
        >= 0;
}

bool NameMatcher::filtersNames() const noexcept
{
    if (!m_extensions.empty())
        return true;
    const auto byName = [](const QueryTerm& t) { return !t.text.empty(); };
    return std::any_of(m_positive.begin(), m_positive.end(), [&](const Positive& p) { return byName(p.term); })
        || std::any_of(m_negative.begin(), m_negative.end(),
            [&](const QueryTerm& t) { return byName(t) && t.ancestors.empty(); });
}

int NameMatcher::match(const FileIndex& index, const Entry& entry) const
{
    Folders folders;
    return match(index, entry, folders);
}

int NameMatcher::matchPath(const QString& fullPath, bool isDir) const
{
    QStringList parts = fullPath.split(separatorRegex(), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return -1;
    const std::string name = wtf8::fromUtf16(wtf8::view(parts.takeLast()));
    const std::size_t extLength = extensionLength(name, isDir);
    return score<false>(
        name, isDir, [&] { return pinyin::hasHan(name); }, extLength,
        [&](const std::vector<std::string>& segments, const pinyin::Matcher* folders) {
            auto j = static_cast<std::ptrdiff_t>(segments.size()) - 1;
            for (auto it = parts.crbegin(); it != parts.crend() && j >= 0; ++it) {
                const auto k = static_cast<std::size_t>(j);
                const std::string folder = wtf8::fromUtf16(wtf8::view(*it));
                if (folderMatches(folder, pinyin::hasHan(folder), segments[k], folders ? &folders[k] : nullptr, false))
                    --j;
            }
            return j < 0;
        },
        [&] {
            std::uint64_t terms = 0;
            for (const QString& part : parts) {
                const std::string folder = wtf8::fromUtf16(wtf8::view(part));
                terms |= termsInFolder(folder, pinyin::hasHan(folder), false);
            }
            return terms;
        });
}

bool NameMatcher::isExactName(std::string_view name, bool isDir) const
{
    if (m_positive.size() != 1 || m_positive[0].term.wildcard || m_positive[0].term.text.empty())
        return false;
    const std::string& t = m_positive[0].term.text;
    if (text::equalsFolded(name, t))
        return true;
    const std::size_t extLength = extensionLength(name, isDir);
    return extLength > 0 && name.size() > extLength + 1
        && text::equalsFolded(name.substr(0, name.size() - extLength - 1), t);
}

int NameMatcher::matchName(std::string_view name) const
{
    return score<false>(
        name, false, [&] { return pinyin::hasHan(name); }, 0,
        [](const std::vector<std::string>&, const pinyin::Matcher*) { return false; },
        [] { return std::uint64_t {0}; });
}

} // namespace ws
