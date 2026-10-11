#include "ResultModel.h"

#include "AppCatalog.h"
#include "FileIndex.h"
#include "FileIconProvider.h"
#include "Pinyin.h"
#include "SystemCatalog.h"
#include "WebShortcut.h"
#include "Wtf8.h"
#include "platform/SiteIcons.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLocale>
#include <QStyleHints>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// Wraps the given ranges of `text` in accent-coloured bold (Text.StyledText).
QString highlighted(const QString& text, QList<std::pair<qsizetype, qsizetype>> ranges, const QColor& color)
{
    std::sort(ranges.begin(), ranges.end());
    QString html;
    html.reserve(text.size() + 64);
    const QString open = u"<font color=\"%1\"><b>"_s.arg(color.name());
    qsizetype pos = 0;
    for (const auto& [start, length] : ranges) {
        if (start < pos || length <= 0)
            continue; // overlapping ranges: keep the first
        html += text.mid(pos, start - pos).toHtmlEscaped();
        html += open + text.mid(start, length).toHtmlEscaped() + u"</b></font>"_s;
        pos = start + length;
    }
    html += text.mid(pos).toHtmlEscaped();
    return html;
}

QString parentFolder(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(u'\\');
    if (slash < 0)
        return {};
    QString folder = path.left(slash);
    if (folder.endsWith(u':'))
        folder += u'\\';
    return folder;
}

// Where the terms are in a name: as they are, or read as pinyin ("bg" for
// 报告: the characters it matched).
QList<std::pair<qsizetype, qsizetype>> nameMatches(const QString& name, const QStringList& terms)
{
    QList<std::pair<qsizetype, qsizetype>> ranges;
    for (const QString& term : terms) {
        const qsizetype i = name.indexOf(term, 0, Qt::CaseInsensitive);
        if (i >= 0) {
            ranges.append({i, term.size()});
            continue;
        }
        const pinyin::Matcher matcher(term.toLower().toStdString());
        if (const auto span = matcher.findUtf16(wtf8::view(name)))
            ranges.append({static_cast<qsizetype>(span->start), static_cast<qsizetype>(span->length)});
    }
    return ranges;
}

// Whether a name has a term (see nameMatches).
bool nameHas(const QString& name, const QString& term)
{
    return name.contains(term, Qt::CaseInsensitive)
        || pinyin::Matcher(term.toLower().toStdString()).findUtf16(wtf8::view(name)).has_value();
}

// Where the terms a name lacks are in its folder, found there ("弯管机" in
// F:\模型\3轴弯管机模型): as they are, or as pinyin in a folder's name.
QList<std::pair<qsizetype, qsizetype>> folderMatches(
    const QString& name, const QString& folder, const QStringList& terms)
{
    QList<std::pair<qsizetype, qsizetype>> ranges;
    for (const QString& term : terms) {
        if (nameHas(name, term))
            continue;
        bool found = false;
        for (qsizetype i = folder.indexOf(term, 0, Qt::CaseInsensitive); i >= 0;
            i = folder.indexOf(term, i + term.size(), Qt::CaseInsensitive)) {
            ranges.append({i, term.size()});
            found = true;
        }
        const pinyin::Matcher matcher(term.toLower().toStdString());
        if (found || !matcher.valid())
            continue;
        const std::u16string_view all = wtf8::view(folder);
        for (qsizetype from = 0; from < folder.size();) {
            qsizetype to = folder.indexOf(u'\\', from);
            if (to < 0)
                to = folder.size();
            if (const auto span = matcher.findUtf16(all.substr(from, to - from)))
                ranges.append({from + static_cast<qsizetype>(span->start), static_cast<qsizetype>(span->length)});
            from = to + 1;
        }
    }
    std::sort(ranges.begin(), ranges.end());
    return ranges;
}

// A web search's row reads 在 GitHub 中搜索“WinShun”; `words`: where they are in it.
QString webSearchTitle(const SearchResult& r, std::pair<qsizetype, qsizetype>* words = nullptr)
{
    const QString title = ResultModel::tr("在 %1 中搜索“%2”").arg(r.name, r.words);
    if (words)
        *words = {title.lastIndexOf(r.words), r.words.size()};
    return title;
}

// The second line of an app's row: the program it starts, or where it comes
// from. A place's: where in Windows it is, or a tool's command. A web
// shortcut's: the address it opens, and for its site the keyword.
QString appOrigin(const SearchResult& r)
{
    switch (r.app) {
    case AppKind::Web:
        return r.words.isEmpty() ? ResultModel::tr("%1 · 关键词 %2").arg(displayWebUrl(r.target), r.keyword)
                                 : displayWebUrl(r.target);
    case AppKind::Store:
        return ResultModel::tr("Microsoft Store 应用");
    case AppKind::System:
        return ResultModel::tr("Windows 系统应用");
    case AppKind::Package:
        return ResultModel::tr("MSIX 应用");
    case AppKind::Setting:
        return ResultModel::tr("Windows 设置");
    case AppKind::ControlPanel:
        return ResultModel::tr("控制面板");
    case AppKind::Security:
        return ResultModel::tr("Windows 安全中心");
    case AppKind::Tool:
        return describeCommand(r.target); // "diskmgmt.msc", the folder
    case AppKind::Plugin:
        return r.plugin ? r.plugin->subtitle : QString();
    default:
        return r.target.isEmpty() ? ResultModel::tr("Windows 系统应用") : r.target; // no file: Control Panel, Run, ...
    }
}

} // namespace

ResultModel::ResultModel(QObject* parent)
    : QAbstractListModel(parent)
{
    // Store apps' logos come in a light and a dark variant; sites' icons may sit on a plate.
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (!m_items.isEmpty())
            emit dataChanged(index(0), index(count() - 1), {IconRole});
    });
    // A site's icon arrived: its rows trade the browser's for it.
    connect(siteicons::notifier(), &siteicons::Notifier::fetched, this, [this](const QString& site) {
        for (int row = 0; row < count(); ++row) {
            const SearchResult* r = at(row);
            if (r && r->isWeb() && siteIconKey(r->icon) == site)
                emit dataChanged(index(row), index(row), {IconRole});
        }
    });
}

int ResultModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

const SearchResult* ResultModel::at(int row) const
{
    return row >= 0 && row < m_items.size() ? &m_items[row] : nullptr;
}

int ResultModel::indexOf(const QString& path) const
{
    const auto it = std::find_if(m_items.cbegin(), m_items.cend(), [&](const SearchResult& r) { return r.path == path; });
    return it == m_items.cend() ? -1 : static_cast<int>(it - m_items.cbegin());
}

QVariant ResultModel::data(const QModelIndex& index, int role) const
{
    const SearchResult* r = at(index.row());
    if (!r)
        return {};
    switch (role) {
    case NameRole:
        return r->isWeb() && !r->words.isEmpty() ? webSearchTitle(*r) : r->name;
    case NameHtmlRole: {
        if (r->isWeb() && !r->words.isEmpty()) { // the words stand out, whatever the query's terms
            std::pair<qsizetype, qsizetype> words;
            const QString title = webSearchTitle(*r, &words);
            return highlighted(title, {words}, m_highlightColor);
        }
        return highlighted(r->name, nameMatches(r->name, m_highlights), m_highlightColor);
    }
    case NameMatchesRole: {
        QVariantList out;
        if (r->isApp()) // short names; a web search's title is its own
            return out;
        for (const auto& [start, length] : nameMatches(r->name, m_highlights))
            out << start << length;
        return out;
    }
    case FolderRole:
        return r->isApp() ? appOrigin(*r) : parentFolder(r->path);
    case PathRole:
        return r->path;
    case IsDirRole:
        return r->isDir;
    case IconRole:
        if (r->isWeb() || r->isApp()) {
            const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
            if (r->isWeb())
                return FileIconProvider::webIconUrl(r->icon.isEmpty() ? r->target : r->icon, dark);
            if (r->isPlugin())
                return FileIconProvider::pluginIconUrl(r->plugin->pluginId, r->plugin->icon, dark);
            if (r->isPlace() && r->icon.startsWith(u"app:")) // the logo of Settings, of Windows Security
                return FileIconProvider::appIconUrl(r->icon.mid(4), true, dark);
            if (r->isPlace())
                return FileIconProvider::placeIconUrl(r->icon);
            return FileIconProvider::appIconUrl(appIdOf(r->path), r->isPackagedApp(), dark);
        }
        return FileIconProvider::iconUrl(r->path, r->isDir);
    case SnippetHtmlRole:
        if (r->snippet.isEmpty())
            return QString();
        return highlighted(r->snippet, r->snippetMatches, m_highlightColor);
    case LineRole:
        return r->line;
    case LocationRole:
        return location(*r);
    case RecentRole:
        return r->recent;
    case IsAppRole:
        return r->isApp();
    case PlaceRole:
        return r->isPlace();
    case WebRole:
        return r->isWeb();
    case PackagedAppRole:
        return r->isPackagedApp();
    case ElevatableRole:
        return r->elevatable;
    case RevealableRole:
        return !r->isApp() || (!r->isPlace() && !r->target.isEmpty());
    case CopyableRole:
        return !r->isApp() || r->hasCopyableTarget();
    case SelectedRole:
        return m_selected.contains(r->path);
    case ModifiedRole:
        return r->isApp() ? QString() : modifiedText(r->modified, QDateTime::currentDateTime());
    case ModifiedFullRole:
        return r->isApp() ? QString() : modifiedFullText(r->modified);
    case OfflineRole:
        return r->offline;
    case PluginRole:
        return r->isPlugin();
    case SourceRole:
        return r->isPlugin() ? r->plugin->source : QString();
    case FavoriteRole:
        return r->favorite;
    case FavoriteKeywordRole:
        return r->favoriteKeyword;
    case MissingRole:
        return r->missing;
    case FolderMatchesRole: {
        QVariantList out;
        if (r->isApp() || r->line > 0) // a content match has every term in the file
            return out;
        for (const auto& [start, length] : folderMatches(r->name, parentFolder(r->path), m_highlights))
            out << start << length;
        return out;
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> ResultModel::roleNames() const
{
    return {
        {NameRole, "name"},
        {NameHtmlRole, "nameHtml"},
        {FolderRole, "folder"},
        {PathRole, "path"},
        {IsDirRole, "isDir"},
        {IconRole, "icon"},
        {SnippetHtmlRole, "snippetHtml"},
        {LineRole, "line"},
        {LocationRole, "location"},
        {RecentRole, "recent"},
        {IsAppRole, "isApp"},
        {PlaceRole, "place"},
        {WebRole, "web"},
        {PackagedAppRole, "packagedApp"},
        {ElevatableRole, "elevatable"},
        {RevealableRole, "revealable"},
        {CopyableRole, "copyable"},
        {SelectedRole, "selected"},
        {ModifiedRole, "modified"},
        {ModifiedFullRole, "modifiedFull"},
        {OfflineRole, "offline"},
        {PluginRole, "plugin"},
        {SourceRole, "source"},
        {FavoriteRole, "favorite"},
        {FavoriteKeywordRole, "favoriteKeyword"},
        {MissingRole, "missing"},
        {FolderMatchesRole, "folderMatches"},
        {NameMatchesRole, "nameMatches"},
    };
}

QString ResultModel::pathAt(int row) const
{
    const SearchResult* r = at(row);
    return r ? r->path : QString();
}

QString ResultModel::elidedHtml(
    const QString& text, const QVariantList& matches, qreal width, const QFont& font, const QColor& color)
{
    QList<std::pair<qsizetype, qsizetype>> ranges;
    for (qsizetype i = 0; i + 1 < matches.size(); i += 2) {
        const qsizetype start = matches[i].toLongLong();
        const qsizetype length = matches[i + 1].toLongLong();
        if (start >= 0 && length > 0 && start + length <= text.size())
            ranges.append({start, length});
    }
    std::sort(ranges.begin(), ranges.end());
    if (ranges.isEmpty())
        return text.toHtmlEscaped();

    // Each character's advance, the highlighted ones bold; a surrogate pair's on its first half.
    QFont bold = font;
    bold.setBold(true);
    const QFontMetricsF plain(font);
    const QFontMetricsF strong(bold);
    std::vector<qreal> before(static_cast<std::size_t>(text.size()) + 1); // of the text up to each place
    for (qsizetype i = 0, r = 0; i < text.size(); ++i) {
        while (r < ranges.size() && ranges[r].first + ranges[r].second <= i)
            ++r;
        const bool strongHere = r < ranges.size() && ranges[r].first <= i;
        qreal advance = 0;
        if (text[i].isHighSurrogate() && i + 1 < text.size())
            advance = (strongHere ? strong : plain).horizontalAdvance(text.mid(i, 2));
        else if (!text[i].isLowSurrogate())
            advance = (strongHere ? strong : plain).horizontalAdvance(text[i]);
        before[i + 1] = before[i] + advance;
    }
    const auto widthOf = [&](qsizetype from, qsizetype to) { return before[to] - before[from]; };
    const qreal room = width - 2; // what the advances leave out (kerning, rounding)
    const qsizetype coreFrom = ranges.front().first;
    qsizetype coreTo = 0;
    for (const auto& [start, length] : ranges)
        coreTo = std::max(coreTo, start + length);
    const qreal spare = room - widthOf(coreFrom, coreTo);
    if (widthOf(0, text.size()) <= room || spare <= 0)
        return highlighted(text, ranges, color); // fits, or the Text elides it

    // Each side of them shortened in its middle: the start of the text and
    // what is just before them; just after them and the end.
    const qreal ellipsis = plain.horizontalAdvance(u"…"_s);
    const qreal leftWidth = widthOf(0, coreFrom);
    const qreal rightWidth = widthOf(coreTo, text.size());
    qreal leftRoom = spare / 2;
    qreal rightRoom = spare / 2;
    if (leftWidth < leftRoom) {
        rightRoom += leftRoom - leftWidth;
        leftRoom = leftWidth;
    } else if (rightWidth < rightRoom) {
        leftRoom += rightRoom - rightWidth;
        rightRoom = rightWidth;
    }
    // Cut where a folder's name or the extension starts, if one is near:
    // "E:\…\弯管机资料\…\pdf", "很长的…弯管机说明….pdf".
    const qsizetype dot = text.lastIndexOf(u'.');
    const qsizetype extension = dot > text.lastIndexOf(u'\\') ? dot : -1;
    const auto starts = [&](qsizetype i) { return text[i] == u'\\' || i == extension; };
    const auto shortened = [&](qsizetype from, qsizetype to, qreal fit) {
        if (widthOf(from, to) <= fit)
            return text.mid(from, to - from);
        const qreal left = fit - ellipsis;
        // The end: about half, from the start of a folder's name or the
        // extension a little before it, or else just after it.
        qsizetype tail = to;
        while (tail > from && widthOf(tail - 1, to) <= left / 2)
            --tail;
        qsizetype back = tail;
        while (back > from && !starts(back) && widthOf(back - 1, to) <= left * 2 / 3)
            --back;
        if (back < tail && starts(back)) {
            tail = back;
        } else {
            qsizetype on = tail;
            while (on < to && !starts(on))
                ++on;
            if (on < to)
                tail = on;
        }
        // The start: the rest, to the end of a folder's name in it.
        qsizetype head = from;
        while (head < tail && widthOf(from, head + 1) + widthOf(tail, to) <= left)
            ++head;
        if (head < tail) {
            qsizetype cut = head;
            while (cut > from && text[cut - 1] != u'\\')
                --cut;
            if (cut > from)
                head = cut;
        }
        if (head > from && text[head - 1].isHighSurrogate())
            --head;
        if (tail < to && text[tail].isLowSurrogate())
            ++tail;
        return text.mid(from, head - from) + u"…"_s + text.mid(tail, to - tail);
    };
    const QString left = shortened(0, coreFrom, leftRoom);
    const QString shown = left + text.mid(coreFrom, coreTo - coreFrom) + shortened(coreTo, text.size(), rightRoom);
    for (auto& range : ranges)
        range.first += left.size() - coreFrom;
    return highlighted(shown, ranges, color);
}

void ResultModel::assign(SearchResults items, QStringList highlights)
{
    const int oldCount = count();
    const int newCount = static_cast<int>(items.size());
    const int common = std::min(oldCount, newCount);

    // Rows present before and after: overwrite in place. The name HTML of
    // every row depends on the highlights.
    int first = -1;
    int last = -1;
    if (highlights != m_highlights && common > 0) {
        first = 0;
        last = common - 1;
    }
    m_highlights = std::move(highlights);
    for (int i = 0; i < common; ++i) {
        if (m_items[i] == items[i])
            continue;
        m_items[i] = std::move(items[i]);
        if (first < 0)
            first = i;
        last = std::max(last, i);
    }
    if (first >= 0)
        emit dataChanged(index(first), index(last));

    if (newCount > oldCount) {
        beginInsertRows({}, oldCount, newCount - 1);
        for (int i = oldCount; i < newCount; ++i)
            m_items.append(std::move(items[i]));
        endInsertRows();
    } else if (newCount < oldCount) {
        beginRemoveRows({}, newCount, oldCount - 1);
        m_items.resize(newCount);
        endRemoveRows();
    }
    if (newCount != oldCount)
        emit countChanged();
    keepSelectionInList();
}

void ResultModel::update(int row, const SearchResult& item)
{
    if (row < 0 || row >= count() || m_items[row] == item)
        return;
    m_items[row] = item;
    emit dataChanged(index(row), index(row));
}

void ResultModel::append(const SearchResults& items)
{
    if (items.isEmpty())
        return;
    beginInsertRows({}, count(), count() + static_cast<int>(items.size()) - 1);
    m_items.append(items);
    endInsertRows();
    emit countChanged();
}

void ResultModel::insert(int row, const SearchResult& item)
{
    row = std::clamp(row, 0, count());
    beginInsertRows({}, row, row);
    m_items.insert(row, item);
    endInsertRows();
    emit countChanged();
}

bool ResultModel::remove(const QString& path)
{
    const int row = indexOf(path);
    if (row < 0)
        return false;
    beginRemoveRows({}, row, row);
    m_items.removeAt(row);
    endRemoveRows();
    emit countChanged();
    keepSelectionInList();
    return true;
}

int ResultModel::removeAll(const QSet<QString>& paths)
{
    int removed = 0;
    // From the end, one run of adjacent rows at a time.
    for (int row = count() - 1; row >= 0;) {
        if (!paths.contains(m_items[row].path)) {
            --row;
            continue;
        }
        int start = row;
        while (start > 0 && paths.contains(m_items[start - 1].path))
            --start;
        beginRemoveRows({}, start, row);
        m_items.remove(start, row - start + 1);
        endRemoveRows();
        removed += row - start + 1;
        row = start - 1;
    }
    if (removed > 0) {
        emit countChanged();
        keepSelectionInList();
    }
    return removed;
}

void ResultModel::clear()
{
    assign({}, {});
}

void ResultModel::retranslate()
{
    if (!m_items.isEmpty()) // a web search's title and a content match's place ("第 2 页") too
        emit dataChanged(index(0), index(count() - 1),
            {FolderRole, NameRole, NameHtmlRole, LocationRole, ModifiedRole, ModifiedFullRole});
}

// Briefly, as a mail list dates its mail: the time today and yesterday, the
// day of the week within the last week, the day this year, else the date.
QString ResultModel::modifiedText(std::uint32_t modified, const QDateTime& now)
{
    if (knownTime(modified) == 0)
        return {}; // 1970-01-01 and the like: not a time anyone wrote it
    const QLocale locale;
    const QDateTime when = QDateTime::fromSecsSinceEpoch(modified).toLocalTime();
    const QDate day = when.date();
    const qint64 daysAgo = day.daysTo(now.date());
    const QString time = locale.toString(when.time(), QLocale::ShortFormat);
    if (daysAgo < 0)
        return locale.toString(when, QLocale::ShortFormat); // ahead of the clock: no guessing
    if (daysAgo == 0)
        return tr("今天 %1").arg(time);
    if (daysAgo == 1)
        return tr("昨天 %1").arg(time);
    if (daysAgo < 7)
        return locale.dayName(day.dayOfWeek(), QLocale::ShortFormat) + u' ' + time;
    //: A date this year, as QLocale::toString formats it: "Oct 3"
    if (day.year() == now.date().year())
        return locale.toString(day, tr("M月d日"));
    //: An earlier date, as QLocale::toString formats it: "Mar 5, 2024"
    return locale.toString(day, tr("yyyy年M月d日"));
}

QString ResultModel::modifiedFullText(std::uint32_t modified)
{
    if (knownTime(modified) == 0)
        return {};
    const QDateTime when = QDateTime::fromSecsSinceEpoch(modified).toLocalTime();
    //: When a file was last written, in full, as QLocale::toString formats it: "Oct 3, 2026, 14:32"
    return tr("修改于 %1").arg(QLocale().toString(when, tr("yyyy年M月d日 HH:mm")));
}

QString ResultModel::location(const SearchResult& r)
{
    using Where = SearchResult::Where;
    switch (r.where) {
    case Where::Line:
        return r.line > 0 ? tr("第 %1 行").arg(r.line) : QString();
    case Where::Document:
        return {};
    case Where::Page:
        return tr("第 %1 页").arg(r.placeNumber);
    case Where::Slide:
        return tr("第 %1 张幻灯片").arg(r.placeNumber);
    case Where::Row: {
        constexpr qsizetype kSheetChars = 12; // a long sheet name would squeeze the snippet
        const QString sheet = r.sheet.size() > kSheetChars ? r.sheet.left(kSheetChars - 1) + u'…' : r.sheet;
        return sheet.isEmpty() ? tr("第 %1 行").arg(r.placeNumber) : tr("%1 第 %2 行").arg(sheet).arg(r.placeNumber);
    }
    }
    return {};
}

bool ResultModel::isSelected(int row) const
{
    const SearchResult* r = at(row);
    return r && m_selected.contains(r->path);
}

void ResultModel::setSelection(QSet<QString> paths)
{
    QSet<QString> inList;
    for (const SearchResult& r : std::as_const(m_items)) {
        if (paths.contains(r.path))
            inList.insert(r.path);
    }
    if (inList == m_selected)
        return;
    std::swap(m_selected, inList); // inList: the old selection now
    for (int row = 0; row < count(); ++row) {
        const QString& path = m_items[row].path;
        if (m_selected.contains(path) != inList.contains(path))
            emit dataChanged(index(row), index(row), {SelectedRole});
    }
    emit selectionChanged();
}

SearchResults ResultModel::selection() const
{
    SearchResults items;
    QSet<QString> seen;
    for (const SearchResult& r : m_items) {
        if (m_selected.contains(r.path) && !seen.contains(r.path)) {
            seen.insert(r.path);
            items.append(r);
        }
    }
    return items;
}

void ResultModel::keepSelectionInList()
{
    if (m_selected.isEmpty())
        return;
    QSet<QString> inList;
    for (const SearchResult& r : std::as_const(m_items)) {
        if (m_selected.contains(r.path))
            inList.insert(r.path);
    }
    if (inList.size() == m_selected.size())
        return;
    m_selected = std::move(inList); // rows that went away need no dataChanged
    emit selectionChanged();
}

void ResultModel::setHighlightColor(const QColor& color)
{
    if (color == m_highlightColor)
        return;
    m_highlightColor = color;
    emit highlightColorChanged();
    if (!m_items.isEmpty()) // a web search's title too, and where content matches are
        emit dataChanged(index(0), index(count() - 1), {NameHtmlRole, SnippetHtmlRole});
}

} // namespace ws
