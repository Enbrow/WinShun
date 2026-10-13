#include "Launcher.h"

#include "AppCatalog.h"
#include "FileIconProvider.h"
#include "History.h"
#include "Host.h"
#include "IndexService.h"
#include "Merge.h"
#include "PathText.h"
#include "Query.h"
#include "SearchEngine.h"
#include "ShareLister.h"
#include "SystemCatalog.h"
#include "Unc.h"
#include "platform/DragOut.h"
#include "platform/Shell.h"
#include "platform/WindowEffects.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QStyleHints>

#include <windows.h>

#include <algorithm>
#include <thread>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

// QML sees Launcher::Scope; the engine takes ws::Scope. One cast between them.
static_assert(Launcher::All == static_cast<int>(ws::Scope::All) && Launcher::Files == static_cast<int>(ws::Scope::Files)
    && Launcher::Content == static_cast<int>(ws::Scope::Content)
    && Launcher::Folders == static_cast<int>(ws::Scope::Folders));
static_assert(Launcher::BestMatch == static_cast<int>(RankBy::Match)
    && Launcher::NewestFirst == static_cast<int>(RankBy::Newest)
    && Launcher::OldestFirst == static_cast<int>(RankBy::Oldest));
static_assert(Launcher::RecentlyOpened == static_cast<int>(ws::StartList::Opened)
    && Launcher::RecentlyModified == static_cast<int>(ws::StartList::Modified));

namespace {

// Files 内容 reads at once. Opening a file waits for the antivirus to scan
// it, so throughput grows well past the processor count: about 2x at 16
// threads over 6, and little more beyond 32 (measured with Defender).
constexpr int kContentThreads = 16;

constexpr qsizetype kShownExtensions = 3; // named in the search box and the status line

// The most a content size limit can be set to (Settings: roundedSizeMB), the
// limit for the files asked for after being left out (searchLeftOut "large").
constexpr std::int64_t kLargestFile = 4096ll << 20;

// The hints' kinds (leftOutHints), and a drive's root from its kind.
constexpr QLatin1StringView kLargeHint("large");
constexpr QLatin1StringView kSystemHint("system");
constexpr QLatin1StringView kTypesHint("types");
constexpr QLatin1StringView kDriveHint("drive:"); // + "E:"

// Rows asked for at a time: names, content matches, files written lately.
// Scrolled to the end, a page more (loadMore), up to kMaxPages.
constexpr int kPage = 256;
constexpr int kFewRows = 8; // fewer found than the window shows: the content scan's left-out hints
constexpr int kMaxPages = 16;

// Ranked by time: where a content match goes among `count` rows (`at(i)`
// reads one). Among the files, name and content matches alike, in their
// order; not among apps, places, web shortcuts, plugins' rows or folders,
// and never above `floor`. With no file at or below it: before the folders
// at the end (files first), else at the end.
template <typename At>
int timePlace(int count, At at, const SearchResult& r, int floor, RankBy by, KindOrder kindOrder)
{
    int lastFile = -1;
    for (int row = floor; row < count; ++row) {
        const SearchResult& x = at(row);
        if (x.isApp() || x.isDir)
            continue;
        if (rankedBefore(r, x, by))
            return row;
        lastFile = row;
    }
    if (lastFile >= 0)
        return lastFile + 1;
    int row = count;
    while (kindOrder == KindOrder::FilesFirst && row > floor && at(row - 1).isDir)
        --row;
    return row;
}

// Whether the server of a network path answers (share::reachable: a few
// milliseconds, at most 1.5 seconds); true for a local path. Asked first and
// afresh, as asking Windows about a file on a server that went away (even
// seconds ago) holds the launcher up for half a minute. `fresh` false takes
// an answer of the last seconds: for many paths on one server.
bool serverAnswers(const QString& path, bool fresh = true)
{
    if (!path.startsWith(u"\\\\"))
        return true;
    const std::wstring p = path.toStdWString();
    return !unc::isUnc(p) || share::reachable(unc::host(p), std::chrono::milliseconds(1500), fresh);
}

// The drives being read from disk now, with how far: "D: 45%、E:".
QString readingDrives(const IndexService& index)
{
    QStringList drives;
    for (const IndexService::Reading& r : index.readingVolumes()) {
        const QString root = QString::fromStdWString(r.root);
        drives.append(r.percent >= 0 ? u"%1 %2%"_s.arg(root).arg(r.percent) : root);
    }
    return drives.join(Launcher::tr("、"));
}

// The folder a file is in: "D:\工作文档", "C:\" for one at a drive's root;
// empty for a path with no folder.
QString folderOf(const QString& file)
{
    const QString path = QDir::toNativeSeparators(file);
    const qsizetype slash = path.lastIndexOf(u'\\');
    if (slash <= 0)
        return {};
    return slash == 2 && path[1] == u':' ? path.left(3) : path.left(slash);
}

// The interface's language, as the plugins' texts are picked (Text::pick).
QString uiLocale()
{
    return QLocale().language() == QLocale::English ? u"en"_s : u"zh"_s;
}

} // namespace

// The result model as plugin::Merge writes it: a plugin's whole list goes
// through showRows (keyword mode); pushed rows go in and out one by one.
class Launcher::ModelSink : public plugin::RowSink {
public:
    explicit ModelSink(Launcher& launcher)
        : m_launcher(launcher)
    {
    }
    int count() const override { return m_launcher.m_results.count(); }
    const SearchResult& row(int index) const override { return *m_launcher.m_results.at(index); }
    int currentRow() const override { return m_launcher.m_currentRow; }
    void assign(SearchResults rows) override { m_launcher.showPluginRows(std::move(rows)); }
    void append(const SearchResults& rows) override { m_launcher.m_results.append(rows); }
    void insert(int index, const SearchResult& row) override { m_launcher.m_results.insert(index, row); }
    void update(int index, const SearchResult& row) override { m_launcher.m_results.update(index, row); }
    void remove(int index) override
    {
        if (const SearchResult* r = m_launcher.m_results.at(index))
            m_launcher.m_results.remove(r->path);
    }

private:
    Launcher& m_launcher;
};

Launcher::Launcher(IndexService* index, AppCatalog* apps, SystemCatalog* places, SearchEngine* engine, History* history,
    QObject* parent)
    : QObject(parent)
    , m_index(index)
    , m_apps(apps)
    , m_places(places)
    , m_engine(engine)
    , m_history(history)
    , m_sink(std::make_unique<ModelSink>(*this))
{
    m_pluginWait.setSingleShot(true);
    m_pluginWait.setInterval(150ms); // a plugin that answers within it shows nothing of the wait
    connect(&m_pluginWait, &QTimer::timeout, this, [this] {
        m_pluginSlow = true;
        refreshStatus();
    });
    m_pluginErrorTimer.setSingleShot(true);
    m_pluginErrorTimer.setInterval(3s);
    connect(&m_pluginErrorTimer, &QTimer::timeout, this, [this] {
        m_pluginError.clear();
        refreshStatus();
    });

    m_contentDebounce.setSingleShot(true);
    m_contentDebounce.setInterval(300ms); // content search reads files: wait for a pause in typing
    connect(&m_contentDebounce, &QTimer::timeout, this, &Launcher::startContentSearch);

    m_flashTimer.setSingleShot(true);
    m_flashTimer.setInterval(2s);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] {
        m_flash.clear();
        emit statusChanged();
    });

    m_statusPoll.setInterval(500ms); // live item count while the index is being built
    connect(&m_statusPoll, &QTimer::timeout, this, [this] {
        if (m_index->state() != IndexService::State::Ready || m_index->isRefreshing())
            refreshStatus();
    });

    m_slowSearch.setSingleShot(true);
    m_slowSearch.setInterval(1s);
    connect(&m_slowSearch, &QTimer::timeout, this, [this] {
        m_searchSlow = true;
        refreshStatus();
    });

    connect(&m_results, &ResultModel::selectionChanged, this, &Launcher::refreshStatus);
    connect(m_engine, &SearchEngine::resultsReady, this, &Launcher::onResults);
    connect(m_engine, &SearchEngine::contentResults, this, &Launcher::onContentResults);
    connect(m_engine, &SearchEngine::contentProgress, this, &Launcher::onContentProgress);
    connect(m_engine, &SearchEngine::contentLeftOut, this, &Launcher::onContentLeftOut);
    connect(m_engine, &SearchEngine::removableDrives, this, &Launcher::onRemovableDrives);
    connect(m_engine, &SearchEngine::driveResults, this, &Launcher::onDriveResults);
    connect(m_engine, &SearchEngine::driveProgress, this, &Launcher::onDriveProgress);
    connect(m_index, &IndexService::stateChanged, this, [this] {
        // A finished index can only improve the current results.
        if (m_index->state() == IndexService::State::Ready && m_window && m_window->isVisible() && m_scope != Content)
            search();
        refreshStatus();
    });
    connect(m_apps, &AppCatalog::changed, this, [this] {
        // Apps installed or removed (or the first list arrived): 全部 shows them.
        if (m_window && m_window->isVisible() && m_scope == All)
            search();
        refreshStatus();
    });
    connect(m_places, &SystemCatalog::changed, this, [this] {
        if (m_window && m_window->isVisible() && m_scope == All)
            search();
    });
    refreshStatus();
}

Launcher::~Launcher() = default;

void Launcher::setPluginHost(plugin::Host* host)
{
    m_plugins = host;
    m_merge.reset();
    if (!host)
        return;
    m_merge = std::make_unique<plugin::Merge>(*m_sink, host->catalog());
    m_merge->setLocale(uiLocale());
    m_merge->setMoreText(tr("（%1 查看全部）"));
    m_merge->setHintText(tr("按空格进入；Enter 或 Tab 也行"));
    m_merge->setPrefixHintText(tr("按 Tab 补全；Enter 直接进入"));
    connect(host, &plugin::Host::queryTaken, this, &Launcher::onPluginTaken);
    connect(host, &plugin::Host::results, this, &Launcher::onPluginResults);
    connect(host, &plugin::Host::queryFinished, this, &Launcher::onPluginFinished);
    connect(host, &plugin::Host::queryFailed, this, &Launcher::onPluginFailed);
    connect(host, &plugin::Host::progress, this, &Launcher::onPluginProgress);
    connect(host, &plugin::Host::notice, this, [this](const QString&, const QString& text) { flash(text); });
    connect(host, &plugin::Host::actionDone, this, &Launcher::onPluginActionDone);
}

void Launcher::setQuery(const QString& query)
{
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
    m_results.setSelection({});
    m_held.reset(); // typed on: a key held for the rows before is not for these
    search();
}

void Launcher::setScope(Scope scope)
{
    if (scope == Content && !m_contentEnabled)
        scope = All; // a plugin's row may still ask for it
    // Back in 文件: as it was left (文件夹, or the kind chosen kept meanwhile).
    if (scope == Files && m_scope != Folders && m_foldersChosen)
        scope = Folders;
    if (scope == m_scope)
        return;
    m_scope = scope;
    if ((scope == Files || scope == Folders) && m_foldersChosen != (scope == Folders)) {
        m_foldersChosen = scope == Folders;
        emit fileChoiceChanged();
    }
    emit scopeChanged();
    m_results.setSelection({});
    m_held.reset();
    refreshKeyHints(); // Tab: through the kinds in 文件
    // Keep the old rows until the new results replace them: clearing first
    // collapses the window and then grows it back (flicker).
    search();
}

bool Launcher::busy() const
{
    const auto state = m_index->state();
    return state == IndexService::State::Loading || state == IndexService::State::Building
        || (m_contentRunning && !m_quiet) || m_walkRunning;
}

qreal Launcher::progress() const
{
    // While the index is still being built, that is the work going on.
    if (!m_contentRunning || m_quiet || m_contentTotal <= 0 || m_index->state() != IndexService::State::Ready)
        return -1;
    return std::clamp(qreal(m_contentScanned) / m_contentTotal, 0.0, 1.0);
}

bool Launcher::moreAvailable() const
{
    // Not while a hint's one-off search runs: its rows reaching the end of
    // the list would ask for a page more, which would cut it short.
    return (m_moreNames || m_moreContent) && m_pages < kMaxPages && m_haveResults && !m_replacePending && !m_keyword
        && m_hintRunning.isEmpty();
}

void Launcher::loadMore()
{
    if (!moreAvailable() || searching() || !m_window || !m_window->isVisible())
        return;
    ++m_pages;
    search(true);
}

bool Launcher::hintsSettled() const
{
    if (!m_haveResults || m_replacePending || m_keyword || m_query.trimmed().isEmpty())
        return false;
    return m_quiet || (!m_pending && !m_contentRunning && !m_walkRunning && !m_contentDebounce.isActive());
}

// In the order they are offered: the content scan's parts (large files in
// any scope; system folders and other kinds of text in 内容, where contents
// are what was asked for: 全部 reads them as a by-product, and under its name
// rows the counts would read oddly), then the drives. The content parts once
// the scan has run to its end (stopped at its limit, more rows come first:
// loadMore), the drives once the names are all listed, for the same reason.
QVariantList Launcher::leftOutHints() const
{
    QVariantList hints;
    const auto add = [&](const QString& kind, const QString& text, const QString& action) {
        hints.append(QVariantMap {
            {u"kind"_s, kind}, {u"text"_s, text}, {u"action"_s, action}, {u"running"_s, kind == m_hintRunning}});
    };
    if (!m_hintRunning.isEmpty()) {
        QString text;
        if (m_hintRunning == kLargeHint)
            text = tr("正在搜这些大文件…");
        else if (m_hintRunning == kSystemHint)
            text = tr("正在搜系统和程序文件夹里的文件…");
        else if (m_hintRunning == kTypesHint)
            text = tr("正在搜这些类型的文件…");
        else
            text = tr("正在搜 %1… 已看 %Ln 项", nullptr, m_walkSeen).arg(m_hintRunning.mid(kDriveHint.size()));
        add(m_hintRunning, text, tr("停止"));
        return hints;
    }
    if (!hintsSettled())
        return hints;
    // The counts are of files not read, whatever was typed: offered under
    // every list they would be noise. Only when what was found does not fill
    // the window (Main.qml maxRows), where "is that all?" is the question.
    if (m_scanComplete && !m_moreContent && m_results.count() < kFewRows) {
        if (!m_leftOut.large.isEmpty()) {
            add(kLargeHint, tr("另有 %Ln 个大文件超过大小上限没搜", nullptr, static_cast<int>(m_leftOut.large.size())),
                tr("也搜这些"));
        }
        if (m_scope == Content && m_leftOut.lowPriority > 0)
            add(kSystemHint, tr("系统和程序文件夹里还有 %Ln 个文件没搜", nullptr, m_leftOut.lowPriority), tr("也搜这些"));
        if (m_scope == Content && m_leftOut.otherTypes > 0) {
            // The two most numerous kinds by name, "等" when there are more.
            QStringList kinds;
            for (const QString& ext : m_leftOut.otherExtensions.first(std::min<qsizetype>(2, m_leftOut.otherExtensions.size())))
                kinds.append(u'.' + ext);
            const QString named = kinds.join(u' ');
            add(kTypesHint,
                m_leftOut.otherExtensions.size() > 2
                    ? tr("还有 %Ln 个 %1 等文件不在内容搜索的类型里", nullptr, m_leftOut.otherTypes).arg(named)
                    : tr("还有 %Ln 个 %1 文件不在内容搜索的类型里", nullptr, m_leftOut.otherTypes).arg(named),
                tr("也搜这些"));
        }
    }
    if (m_scope != Content && !m_moreNames) {
        for (const RemovableDrive& drive : m_drives) {
            const QString kind = kDriveHint + drive.root;
            if (m_hintsTaken.contains(kind))
                continue;
            add(kind,
                drive.label.isEmpty() ? tr("U 盘 %1 没收录").arg(drive.root) : tr("U 盘 %1（%2）没收录").arg(drive.root, drive.label),
                tr("这次也搜它"));
        }
    }
    return hints;
}

QString Launcher::approximateHeading() const
{
    if (!m_approximate || !m_haveResults || m_shownText.isEmpty())
        return {};
    // What was typed, on one line (as Main.qml shows it when nothing is found).
    static const QRegularExpression kSpaces(u"\\s+"_s);
    return tr("没有叫“%1”的，按读音找到：").arg(QString(m_shownText).replace(kSpaces, u" "_s));
}

// A hint taken up: the same query over that part alone. The content parts
// are a content scan (SearchEngine::Request::contentSet) whose matches come
// in as content matches do: the files over the limit under the largest limit
// the settings allow, the others under the limits set. A drive is walked
// (Request::walkDrive); its rows go after the rows there are, as they come.
// Not while a search runs, but a quiet refresh (the window opened again) is
// stopped for it: its rows stay as they were, which is what a quiet refresh
// keeps anyway, and a scan that finished without finding them again would
// have taken them for gone.
void Launcher::searchLeftOut(const QString& kind)
{
    if (!m_window || !m_window->isVisible() || !m_hintRunning.isEmpty())
        return;
    const QVariantList offered = leftOutHints();
    if (std::none_of(offered.cbegin(), offered.cend(), [&](const QVariant& h) { return h.toMap()[u"kind"_s] == kind; }))
        return;
    if (m_pending || m_contentRunning) {
        m_engine->cancel();
        m_pending = false;
        m_contentRunning = false;
        m_loadingMore = false;
        m_unconfirmed.clear();
    }
    SearchEngine::Request request;
    request.rankBy = rankBy();
    if (kind.startsWith(kDriveHint)) {
        request.text = m_query;
        request.scope = m_scope == Folders ? ws::Scope::Folders : ws::Scope::Files; // 全部: its files and folders
        request.extensions = fileExtensions();
        request.limit = kPage;
        request.walkDrive = kind.mid(kDriveHint.size());
        m_walkRunning = true;
        m_walkSeen = 0;
    } else {
        request.text = m_scope == Content ? m_query.trimmed() : contentNeedle();
        request.scope = ws::Scope::Content;
        request.contentExtensions = m_contentExtensions;
        request.contentSizeLimits = m_contentSizeLimits;
        request.contentDocuments = m_contentDocuments;
        request.contentInLowPriority = m_contentInLowPriority;
        request.contentThreads = kContentThreads; // asked for: many files at once (each open mostly waits for the antivirus)
        request.maxContentResults = kPage * m_pages;
        if (kind == kLargeHint) {
            request.contentSet = SearchEngine::ContentSet::Given;
            request.contentOnly = m_leftOut.large;
            request.maxContentResults = static_cast<int>(m_leftOut.large.size()); // a match per file at most: all of them
            request.contentSizeLimits.bytes.fill(kLargestFile);
        } else {
            request.contentSet
                = kind == kSystemHint ? SearchEngine::ContentSet::LowPriority : SearchEngine::ContentSet::OtherTypes;
        }
        m_pending = true;
        m_contentRunning = true;
    }
    m_hintRunning = kind;
    m_hintsTaken.insert(kind);
    m_hintHits = 0;
    m_quiet = false; // the divider shows how far it is
    m_requestId = m_engine->submit(std::move(request));
    refreshStatus();
}

void Launcher::stopLeftOut()
{
    if (m_hintRunning.isEmpty())
        return;
    m_engine->cancel();
    m_requestId = 0;
    m_pending = false;
    m_contentRunning = false;
    m_walkRunning = false;
    m_contentScanned = m_contentTotal; // the scan behind the rows did run to its end (setSort reads the figures)
    finishLeftOut(true);
}

// The rows it brought went in where they rank (content matches), which may
// be out of sight: the footer says what came of it.
void Launcher::finishLeftOut(bool stopped)
{
    const QString kind = std::exchange(m_hintRunning, QString());
    const int hits = m_hintHits;
    if (kind == kLargeHint)
        m_leftOut.large.clear();
    else if (kind == kSystemHint)
        m_leftOut.lowPriority = 0;
    else if (kind == kTypesHint)
        m_leftOut.otherTypes = 0;
    QString said;
    if (stopped)
        said = hits > 0 ? tr("已停止，找到 %Ln 条", nullptr, hits) : tr("已停止");
    else if (kind == kLargeHint)
        said = hits > 0 ? tr("大文件里找到 %Ln 条", nullptr, hits) : tr("这些大文件里也没有");
    else if (kind == kSystemHint)
        said = hits > 0 ? tr("系统和程序文件夹里找到 %Ln 条", nullptr, hits) : tr("系统和程序文件夹里也没有");
    else if (kind == kTypesHint)
        said = hits > 0 ? tr("这些类型的文件里找到 %Ln 条", nullptr, hits) : tr("这些类型的文件里也没有");
    else {
        const QString root = kind.mid(kDriveHint.size());
        said = hits > 0 ? tr("%1 里找到 %Ln 项", nullptr, hits).arg(root) : tr("%1 里也没有").arg(root);
    }
    flash(said);
}

// The first few content extensions, ".txt / .md / .log": the full default
// list would overflow the search box.
QString Launcher::shownExtensions() const
{
    QStringList parts;
    for (const QString& ext : m_contentExtensions.first(std::min(kShownExtensions, m_contentExtensions.size())))
        parts.append(u'.' + ext);
    return parts.join(u" / "_s);
}

QString Launcher::contentFilesLabel() const
{
    if (m_contentDocuments)
        return tr("文档和 %1 等文件").arg(u'.' + m_contentExtensions.first());
    return m_contentExtensions.size() > kShownExtensions ? tr("%1 等文件").arg(shownExtensions())
                                                         : tr("%1 文件").arg(shownExtensions());
}

QString Launcher::placeholder() const
{
    switch (m_scope) {
    case Files:
        if (m_fileType >= 0)
            return tr("搜索%1", "a kind of file, as 图片").arg(m_fileTypes[m_fileType].name);
        return tr("搜索文件和文件夹");
    case Folders:
        return tr("搜索文件夹");
    case Content:
        if (m_contentDocuments)
            return tr("搜索 Word、Excel、PDF 和 %1 等文件中的文字").arg(u'.' + m_contentExtensions.first());
        return m_contentExtensions.size() > kShownExtensions ? tr("搜索 %1 等文件中的文字").arg(shownExtensions())
                                                             : tr("搜索 %1 文件中的文字").arg(shownExtensions());
    default:
        return m_contentEnabled ? tr("搜索应用、文件和文件内容") : tr("搜索应用和文件");
    }
}

void Launcher::setContentOptions(
    bool enabled, QStringList extensions, const ContentSizeLimits& sizeLimits, bool inLowPriority, bool documents)
{
    const bool wasEnabled = std::exchange(m_contentEnabled, enabled);
    m_contentInLowPriority = inLowPriority;
    m_contentDocuments = documents;
    for (QString& ext : extensions) {
        ext = ext.trimmed();
        while (ext.startsWith(u'.') || ext.startsWith(u'*'))
            ext.remove(0, 1);
    }
    extensions.removeAll(QString());
    if (extensions.isEmpty())
        extensions = {u"txt"_s};
    m_contentExtensions = extensions;
    m_contentSizeLimits = sizeLimits;
    if (!enabled && m_scope == Content) {
        setScope(All);
        return;
    }
    emit scopeChanged(); // placeholder text mentions the extensions
    if (enabled != wasEnabled && m_scope == All && m_window && m_window->isVisible())
        search(); // 全部: content matches below the names come or go
}

void Launcher::setWebShortcuts(const WebShortcuts& shortcuts)
{
    if (shortcuts == m_web)
        return;
    m_web = shortcuts;
    if (m_window && m_window->isVisible() && m_scope == All && !m_query.trimmed().isEmpty())
        search();
}

void Launcher::setFavorites(Favorites* favorites)
{
    if (m_favorites)
        disconnect(m_favorites, nullptr, this, nullptr);
    m_favorites = favorites;
    if (favorites) {
        connect(favorites, &Favorites::changed, this, [this] {
            emit favoritesChanged();
            if (m_window && m_window->isVisible() && !m_keyword && m_scope != Content)
                search(); // the rows refreshed in place: 收藏 comes or goes
        });
    }
    emit favoritesChanged();
}

bool Launcher::hasFavorites() const
{
    return m_favorites && !m_favorites->items().isEmpty();
}

void Launcher::setResultOptions(KindOrder kindOrder, bool showModified, const QString& numberKeys)
{
    const QString keys = numberKeys == u"off" ? QString() : numberKeys;
    if (kindOrder == m_kindOrder && showModified == m_showModified && keys == m_numberKeys)
        return;
    const bool reorder = kindOrder != m_kindOrder;
    m_kindOrder = kindOrder;
    m_showModified = showModified;
    m_numberKeys = keys;
    emit resultOptionsChanged();
    if (reorder && m_window && m_window->isVisible() && m_scope != Content)
        search();
}

void Launcher::setNumberKeysLeft(unsigned digits)
{
    if (digits == m_numberKeysLeft)
        return;
    m_numberKeysLeft = digits;
    emit resultOptionsChanged();
}

void Launcher::setSort(Sort sort)
{
    if (sort == m_sort)
        return;
    m_sort = sort;
    emit sortChanged();
    if (!m_window || !m_window->isVisible() || m_query.trimmed().isEmpty() || m_keyword)
        return; // hidden: handleShown() searches
    // 内容, every match on screen: put in the other order, not searched again
    // (that would read the files once more).
    if (m_scope == Content && m_shownScope == Content && m_haveResults && !m_replacePending && m_scanComplete
        && !m_contentRunning && m_contentScanned >= m_contentTotal && m_shownText == m_query.trimmed()) {
        SearchResults rows;
        rows.reserve(m_results.count());
        for (int row = 0; row < m_results.count(); ++row)
            rows.append(*m_results.at(row));
        const RankBy by = rankBy();
        std::ranges::stable_sort(rows, [by](const SearchResult& a, const SearchResult& b) { return rankedBefore(a, b, by); });
        m_replacePending = true; // new rows: the first is selected
        showRows(std::move(rows), {});
        refreshStatus();
        return;
    }
    search();
}

void Launcher::setStartList(StartList list)
{
    if (list == m_startList)
        return;
    m_startList = list;
    emit startListChanged();
    if (m_window && m_window->isVisible() && showsStartList())
        search();
}

RankBy Launcher::rankBy() const
{
    return static_cast<RankBy>(m_sort);
}

bool Launcher::showsStartList() const
{
    return m_scope != Content && m_query.trimmed().isEmpty();
}

void Launcher::switchOrder()
{
    // The footer's switch shows what it is now; the status keeps its figures.
    if (showsStartList())
        setStartList(m_startList == RecentlyOpened ? RecentlyModified : RecentlyOpened);
    else if (!m_keyword) // a command's list is in the plugin's order
        setSort(static_cast<Sort>((m_sort + 1) % 3));
}

void Launcher::setComposing(bool composing)
{
    if (composing == m_composing)
        return;
    m_composing = composing;
    emit composingChanged();
}

void Launcher::setInTaskbarBox(bool on)
{
    if (on == m_inTaskbarBox)
        return;
    m_inTaskbarBox = on;
    emit inTaskbarBoxChanged();
}

void Launcher::reportField(
    const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition)
{
    emit fieldReported(text, cursor, selectionStart, selectionEnd, composition);
}

void Launcher::pressNumber(int number)
{
    if (number < 1 || number > 9 || m_numberKeys.isEmpty() || holdUntilShown(Open, number))
        return;
    emit numberPressed(number);
}

bool Launcher::holdUntilShown(int action, int number)
{
    if (!m_replacePending || !searching())
        return false; // the rows on screen are the query's
    m_held = {action, number};
    return true;
}

void Launcher::handleShown()
{
    m_index->contentIndexer()->setSearchBoxOpen(true); // the files it searches are read before the indexer's
    m_statusPoll.start();
    m_apps->refresh(); // reads the list again only if apps were installed or removed
    m_places->refresh(); // ... if Windows was updated or its language changed
    search(); // refresh: files may have changed, and "recent" certainly has
    emit historyChanged(); // things were opened since
    refreshFrequentApps();
    emit shown();
}

void Launcher::refreshFrequentApps()
{
    constexpr std::size_t kFrequentApps = 6; // a row over the taskbar
    std::vector<FrequentApp> picked;
    const std::shared_ptr<const AppList> apps = m_apps->apps();
    if (m_recordHistory && apps) {
        if (!m_windowsUsesAge.isValid() || m_windowsUsesAge.hasExpired(60'000)) {
            m_windowsUses = windowsAppUses(); // some 500 registry values: a few ms
            m_windowsUsesAge.start();
        }
        const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        const auto indexes
            = ws::frequentApps(*apps, *m_history, m_windowsUses, QDateTime::currentMSecsSinceEpoch(), kFrequentApps);
        for (const std::size_t i : indexes) {
            const AppInfo& app = (*apps)[i];
            const bool packaged = app.kind == AppKind::Store || app.kind == AppKind::System || app.kind == AppKind::Package;
            picked.push_back({app.name, app.launchPath(), FileIconProvider::appIconUrl(app.id, packaged, dark)});
        }
    }
    if (picked == m_frequent)
        return;
    m_frequent = std::move(picked);
    emit frequentAppsChanged();
}

QVariantList Launcher::frequentApps() const
{
    QVariantList list;
    for (const FrequentApp& app : m_frequent)
        list.append(QVariantMap {{u"name"_s, app.name}, {u"icon"_s, app.icon}});
    return list;
}

void Launcher::openFrequentApp(int index)
{
    if (index < 0 || index >= static_cast<int>(m_frequent.size()))
        return;
    const FrequentApp app = m_frequent[static_cast<std::size_t>(index)];
    remember(app.path);
    shell::launchApp(app.path, false, reportFailure(app.name));
    emit dismissRequested();
}

void Launcher::retranslate()
{
    m_results.retranslate();
    if (m_merge) {
        m_merge->setLocale(uiLocale());
        m_merge->setMoreText(tr("（%1 查看全部）"));
        m_merge->setHintText(tr("按空格进入；Enter 或 Tab 也行"));
        m_merge->setPrefixHintText(tr("按 Tab 补全；Enter 直接进入"));
    }
    emit scopeChanged(); // placeholder, contentFilesLabel
    refreshStatus();
}

void Launcher::handleHidden()
{
    m_index->contentIndexer()->setSearchBoxOpen(false);
    m_results.setSelection({});
    m_held.reset();
    m_statusPoll.stop();
    m_contentDebounce.stop();
    m_slowSearch.stop();
    m_searchSlow = false;
    if (m_contentRunning || m_pending || m_walkRunning) {
        m_engine->cancel();
        m_requestId = 0;
        m_pending = false;
        m_contentRunning = false;
        m_walkRunning = false;
    }
    m_hintRunning.clear(); // whatever was asked is over; the hint is not offered again (m_hintsTaken)
    if (m_plugins)
        m_plugins->cancelAll(); // nobody is looking: whatever they were asked is moot
    m_pluginPending.clear();
    m_pluginWait.stop();
    m_pluginSlow = false;
    // Opened again, a page, as quick as ever; more as it is scrolled again.
    m_pages = 1;
    m_loadingMore = false;
}

// Nothing is cleared up front: the rows on screen stay until results replace
// them. Searching what is already shown again (the window reopens, the index
// changes) is a refresh: the rows are updated in place, and content matches
// from the last scan stay until the new scan has had a chance to find them.
void Launcher::search(bool more)
{
    m_flash.clear();
    m_contentDebounce.stop();
    m_slowSearch.stop();
    m_searchSlow = false;
    const QString text = m_query.trimmed();
    // Sorted another way, or the other start list, the rows are new ones
    // (the first is selected). So is a plugin command's list in place of
    // the names' (or back): "=" and "= " are the same text trimmed.
    std::optional<KeywordMode> keyword = matchKeyword(m_query);
    const QString keywordKey = keyword ? keyword->pluginId + u'/' + keyword->commandId : QString();
    m_refresh = m_haveResults && !m_replacePending && text == m_shownText && m_scope == m_shownScope
        && fileExtensions() == m_shownExtensions && rankBy() == m_shownRankBy && m_kindOrder == m_shownKindOrder && keywordKey == m_shownKeyword
        && (!text.isEmpty() || m_startList == m_shownStartList);
    m_replacePending = !m_refresh;
    // A page more of what is listed, or (anything else asking again) the
    // pages there are; a new list starts with one.
    m_loadingMore = more && m_refresh;
    m_hintRunning.clear(); // superseded, as any search is by what follows
    m_walkRunning = false;
    if (!m_refresh) {
        m_pages = 1;
        m_moreNames = false;
        m_moreContent = false;
        m_leftOut = {};
        m_drives.clear();
        m_hintsTaken.clear();
        m_leftOutFound.clear();
        m_approximate = false;
    }
    // The plugins are asked first: a keyword makes the list theirs, and
    // then neither names nor contents are searched (nothing waits for them
    // either way: the window shows with whatever it has). As typed: the
    // space after a keyword is what opens its command (Catalog::match).
    startPluginQuery(m_query, std::move(keyword));
    m_withContent = m_contentEnabled && m_scope == All && !contentNeedle().isEmpty();
    m_unconfirmed.clear();
    // Quietly, unless the rows came from a content scan that was cut short
    // (window hidden mid-scan): that one still shows as the search it is.
    const bool scans = m_scope == Content || m_withContent;
    m_quiet = m_refresh && (!scans || m_scanComplete) && !m_loadingMore;
    if (!m_refresh)
        m_scanComplete = false;
    if (!m_quiet) { // a quiet refresh keeps showing the last scan's figures
        m_contentScanned = 0;
        m_contentTotal = 0;
    }

    if (m_keyword) { // the command's list: its answer replaces the rows (onPluginResults)
        m_engine->cancel();
        m_requestId = 0;
        m_pending = false;
        m_contentRunning = false;
        refreshStatus();
        return;
    }

    if (m_scope == Content) {
        m_engine->cancel();
        m_requestId = 0;
        m_pending = false;
        m_contentRunning = false;
        if (text.isEmpty()) {
            m_replacePending = false;
            m_results.clear();
            m_haveResults = false;
            emit resultsReplaced();
        } else if (m_refresh) {
            // Asked again, the rows not found again go (not those a one-off
            // brought: this scan does not read those files); for a page more,
            // what is listed stays (the new scan finds those first).
            if (!m_loadingMore) {
                for (int row = 0; row < m_results.count(); ++row) {
                    if (!m_leftOutFound.contains(m_results.at(row)->path))
                        m_unconfirmed.insert(m_results.at(row)->path);
                }
            }
            startContentSearch(); // at once: nobody is typing
        } else {
            m_contentDebounce.start();
        }
        refreshStatus();
        return;
    }

    SearchEngine::Request request;
    request.text = m_query;
    request.scope = static_cast<ws::Scope>(m_scope);
    request.extensions = fileExtensions();
    request.kindOrder = m_kindOrder;
    request.rankBy = rankBy();
    request.startList = static_cast<ws::StartList>(m_startList);
    request.limit = kPage * m_pages;
    if (m_recordHistory)
        request.history = m_history->items();
    request.web = m_web;
    if (m_favorites)
        request.favorites = m_favorites->items();
    m_requestId = m_engine->submit(std::move(request)); // also stops a running content scan
    m_nameRequestId = m_requestId;
    m_pending = true;
    m_contentRunning = false;
    if (m_replacePending)
        m_slowSearch.start();
    refreshStatus();
}

// Rows for the current query and scope. Replacing another query's rows
// moves the selection back to the first one; a refresh keeps it.
void Launcher::showRows(SearchResults rows, QStringList highlights)
{
    m_results.assign(std::move(rows), std::move(highlights));
    m_shownText = m_query.trimmed();
    m_shownScope = m_scope;
    m_shownExtensions = fileExtensions();
    m_shownRankBy = rankBy();
    m_shownStartList = m_startList;
    m_shownKindOrder = m_kindOrder;
    m_shownKeyword = m_keyword ? m_keyword->pluginId + u'/' + m_keyword->commandId : QString();
    m_haveResults = true;
    if (m_replacePending) {
        m_replacePending = false;
        m_slowSearch.stop();
        m_searchSlow = false;
        emit resultsReplaced();
        if (const auto held = std::exchange(m_held, std::nullopt))
            emit heldKey(held->first, held->second); // on the rows it was pressed for
    }
    // Plugins that answered before the names: their rows go in now, among
    // these (after the first row is current again, so they are placed by it).
    if (m_merge && !m_merge->keywordMode()) {
        m_merge->namesShown();
        if (m_merge->takeBetterFirstRow())
            emit betterFirstRow();
    }
}

// Keyword mode: the command's answer is the list (ModelSink::assign).
void Launcher::showPluginRows(SearchResults rows)
{
    showRows(std::move(rows), m_keyword ? ws::parseQuery(m_keyword->words).highlights : QStringList());
    refreshStatus();
}


// 全部 looks for the typed text inside files too, as the 内容 scope does
// (contentTerms: the words, "a phrase", or a line of code as it is). Not for
// name syntax (ext:, wildcards, !exclusions: "*.log", "report !draft"),
// unless the text reads as code anyway ("if (!ok)", "char* p = s;"). Nor
// after a web shortcut's keyword: that is a search on the web ("gh
// WinShun"), and reading files for it only costs time. Nor for a path
// ("C:\Users\me\x"): that is a place to go to. Nor for a favorite's keyword.
QString Launcher::contentNeedle() const
{
    const QString text = m_query.trimmed();
    const ParsedQuery query = ws::parseQuery(text);
    if (query.terms.empty() || m_keyword || typedKeyword(m_web, m_query) || !pathtext::splitTyped(text).folder.isEmpty())
        return {};
    if (m_favorites && typedFavoriteKeyword(m_favorites->items(), text) >= 0)
        return {};
    const bool nameSyntax = !query.extensions.empty()
        || std::any_of(query.terms.cbegin(), query.terms.cend(), [](const QueryTerm& t) { return t.negated || t.wildcard; });
    if (nameSyntax && !ws::looksLikeCode(text))
        return {};
    return text;
}

void Launcher::startContentSearch()
{
    SearchEngine::Request request;
    request.text = m_scope == Content ? m_query.trimmed() : contentNeedle();
    request.scope = ws::Scope::Content;
    request.rankBy = rankBy();
    request.maxContentResults = kPage * m_pages;
    request.contentExtensions = m_contentExtensions;
    request.contentSizeLimits = m_contentSizeLimits;
    request.contentDocuments = m_contentDocuments;
    if (m_scope == Content) {
        // Asked for: system and program folders too if so set, and many
        // files at once (each open mostly waits for the antivirus).
        request.contentInLowPriority = m_contentInLowPriority;
        request.contentThreads = kContentThreads;
    } else {
        // 全部 looks in your own files on its own, gently.
        request.contentThreads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 2, 6);
    }
    m_requestId = m_engine->submit(std::move(request));
    m_pending = true;
    m_contentRunning = true;
    refreshStatus();
}

void Launcher::onResults(quint64 id, const SearchResults& results, qint64 total, qint64 elapsedUs)
{
    if (id != m_requestId)
        return; // superseded while in flight
    m_pending = false;
    if (m_contentRunning) {
        refreshStatus();
        return; // the (empty) start of a content scan: rows come with contentResults
    }

    // More to list than came: names beyond the page, or the files written
    // lately filling it (the favorites before them not counted).
    if (m_query.trimmed().isEmpty()) {
        m_moreNames = m_startList == RecentlyModified
            && std::ranges::count_if(results, [](const SearchResult& r) { return !r.favorite; }) >= kPage * m_pages;
    } else {
        m_moreNames = total > results.size();
    }
    if (m_loadingMore) {
        // A page more: what is not listed yet goes after the rows, in its
        // order; nothing listed moves (it is where its user is looking).
        SearchResults fresh;
        for (const SearchResult& r : results) {
            if (!r.isPlugin() && m_results.indexOf(r.path) < 0)
                fresh.append(r);
        }
        m_results.append(fresh);
        if (m_withContent)
            startContentSearch(); // its page more too
        else
            m_loadingMore = false;
        refreshStatus();
        return;
    }

    SearchResults rows = results;
    QSet<QString> names;
    if (m_refresh) {
        for (const SearchResult& r : rows)
            names.insert(r.path);
    }
    if (m_refresh && m_withContent) {
        // Keep last scan's content matches below the names until the new
        // scan finds them again (or finishes without them), rather than
        // dropping them only to add them back a moment later.
        // By time, each among the files where it belongs.
        const RankBy by = rankBy();
        for (int row = 0; row < m_results.count(); ++row) {
            const SearchResult* r = m_results.at(row);
            if (r->line > 0 && !names.contains(r->path)) {
                const int at = by == RankBy::Match
                    ? static_cast<int>(rows.size())
                    : timePlace(static_cast<int>(rows.size()), [&](int i) -> const SearchResult& { return rows.at(i); },
                          *r, 0, by, m_kindOrder);
                rows.insert(at, *r);
                if (!m_leftOutFound.contains(r->path)) // a one-off brought it: the scan will not read that file
                    m_unconfirmed.insert(r->path);
            }
        }
    }
    if (m_refresh) {
        // Likewise the rows a walk of a drive brought (searchLeftOut): the
        // index has nothing to find them by again. After the names, as they came.
        for (int row = 0; row < m_results.count(); ++row) {
            const SearchResult* r = m_results.at(row);
            if (r->line == 0 && !r->isPlugin() && m_leftOutFound.contains(r->path) && !names.contains(r->path))
                rows.append(*r);
        }
    }
    if (m_refresh) {
        // Likewise the plugins' rows, where they are: their new answer
        // rewrites them in place (plugin::Merge) instead of taking them
        // out and putting them back.
        for (int row = 0; row < m_results.count(); ++row) {
            const SearchResult* r = m_results.at(row);
            if (r->isPlugin())
                rows.insert(std::min<qsizetype>(row, rows.size()), *r);
        }
    }
    m_contentHits = static_cast<int>(m_unconfirmed.size());
    m_totalMatches = total;
    if (!m_refresh)
        m_elapsedUs = elapsedUs; // a refresh keeps the figure the user has seen
    // Content search starts only now: a new request cancels the running one,
    // and the name search must finish first. Before the rows change, so the
    // view knows more are on the way and does not shrink in between.
    if (m_withContent) {
        if (m_refresh)
            startContentSearch();
        else
            m_contentDebounce.start();
    }
    // Rows found by how their names read (nothing is named as typed): the
    // heading says so (approximateHeading), and the names are highlighted by
    // the pinyin, as the result list reads "bg" back into 报告.
    m_approximate = std::ranges::any_of(results, [](const SearchResult& r) { return r.approximate; });
    const ParsedQuery query = ws::parseQuery(m_query);
    QStringList highlights = query.highlights;
    if (m_approximate) {
        highlights.clear();
        for (const ParsedQuery& reading : ws::homophoneQueries(query)) {
            for (const QString& word : reading.highlights) {
                if (!highlights.contains(word))
                    highlights.append(word);
            }
        }
    }
    showRows(std::move(rows), std::move(highlights));
    refreshStatus();
    emit namesShown();
}

void Launcher::onContentResults(quint64 id, const SearchResults& results)
{
    if (id != m_requestId)
        return;
    if (m_replacePending) { // 内容, new query: its first matches replace the previous query's rows
        showRows(results, {});
        refreshStatus();
        return;
    }
    SearchResults fresh;
    for (const SearchResult& r : results) {
        const int row = m_results.indexOf(r.path);
        if (row < 0)
            fresh.append(r);
        else if (m_unconfirmed.remove(r.path))
            m_results.update(row, r); // kept from the last scan and found again; the line may have moved
        // else a file whose name matched already has a row
    }
    if (m_scope != Content)
        m_contentHits += static_cast<int>(fresh.size());
    if (!m_hintRunning.isEmpty()) {
        for (const SearchResult& r : fresh)
            m_leftOutFound.insert(r.path);
        m_hintHits += static_cast<int>(fresh.size());
    }
    if (m_loadingMore) { // a page more: after the rows, as they come (loadMore)
        m_results.append(fresh);
        return;
    }
    // Each among the content rows by how well it matches (they come best
    // first, a tie after those already there); by time, among the files
    // (timePlace). Never above the row the keyboard is on, nor the rows
    // above it: they stay where they are seen; on the first row, a better
    // match takes its place.
    const int current = m_currentRow;
    const int floor = current > 0 ? current + 1 : 0;
    const RankBy by = rankBy();
    bool aboveFirst = false;
    for (const SearchResult& r : fresh) {
        int row = m_results.count();
        if (by != RankBy::Match) {
            row = timePlace(
                row, [&](int i) -> const SearchResult& { return *m_results.at(i); }, r, floor, by, m_kindOrder);
        } else {
            while (row > floor) {
                const SearchResult* before = m_results.at(row - 1);
                if (before->line == 0 || before->relevance >= r.relevance)
                    break; // a name match (they come first), or as good
                --row;
            }
        }
        aboveFirst |= row == 0 && current == 0;
        m_results.insert(row, r);
    }
    if (aboveFirst)
        emit betterFirstRow();
}

void Launcher::onContentProgress(quint64 id, int scanned, int total, bool finished)
{
    if (id != m_requestId)
        return;
    m_contentRunning = !finished;
    if (!finished && m_quiet)
        return; // the status keeps the last scan's figures
    m_contentScanned = scanned;
    m_contentTotal = total;
    if (finished && !m_hintRunning.isEmpty()) {
        // A part read after all (searchLeftOut): the scan behind the rows
        // is as it was (this one may have stopped at its limit, which says
        // nothing about that one).
        finishLeftOut(false);
    } else if (finished) {
        m_scanComplete = true;
        m_loadingMore = false;
        // Stopped at its limit before every file was read: more to list.
        m_moreContent = scanned < total;
        if (m_replacePending)
            showRows({}, {}); // 内容, new query: nothing found
        if (!m_unconfirmed.isEmpty()) { // kept from the last scan, but gone now
            const int removed = m_results.removeAll(m_unconfirmed);
            if (m_scope != Content)
                m_contentHits -= removed;
            m_unconfirmed.clear();
        }
    }
    refreshStatus();
}

void Launcher::onContentLeftOut(quint64 id, const ContentLeftOut& leftOut)
{
    if (id != m_requestId || !m_hintRunning.isEmpty())
        return; // superseded
    // Shown once the finishing progress arrives (leftOutHints); the parts
    // read already for this query are not offered again.
    m_leftOut = leftOut;
    if (m_hintsTaken.contains(kLargeHint))
        m_leftOut.large.clear();
    if (m_hintsTaken.contains(kSystemHint))
        m_leftOut.lowPriority = 0;
    if (m_hintsTaken.contains(kTypesHint))
        m_leftOut.otherTypes = 0;
}

void Launcher::onRemovableDrives(quint64 id, const RemovableDrives& drives)
{
    if (id != m_nameRequestId || drives == m_drives)
        return;
    m_drives = drives;
    refreshStatus();
}

// A drive's rows, as the walk finds them: after the rows there are (the hint
// taken up stands there), each file once.
void Launcher::onDriveResults(quint64 id, const SearchResults& results)
{
    if (id != m_requestId)
        return;
    SearchResults fresh;
    for (const SearchResult& r : results) {
        if (m_results.indexOf(r.path) < 0) {
            fresh.append(r);
            m_leftOutFound.insert(r.path);
        }
    }
    m_hintHits += static_cast<int>(fresh.size());
    m_results.append(fresh);
}

void Launcher::onDriveProgress(quint64 id, int seen, bool finished)
{
    if (id != m_requestId)
        return;
    m_walkSeen = seen;
    if (finished) {
        m_walkRunning = false;
        if (!m_hintRunning.isEmpty())
            finishLeftOut(false);
    }
    refreshStatus();
}

void Launcher::refreshStatus()
{
    // Only what is worth reading: the left of the footer is the order's
    // switch, and how far a content scan is shows on the divider above
    // (progress()). Not the figures of a search that went as it should.
    const auto state = m_index->state();
    const int items = static_cast<int>(m_index->itemCount());
    QString s;
    if (m_query.trimmed().isEmpty() || (!m_haveResults && !m_contentRunning)) {
        switch (state) {
        case IndexService::State::Idle:
        case IndexService::State::Loading:
            s = tr("正在加载索引…");
            break;
        case IndexService::State::Building:
            s = tr("正在建立索引… 已收录 %Ln 项", nullptr, items);
            if (const QString drives = readingDrives(*m_index); !drives.isEmpty())
                s += tr(" · 正在读 %1").arg(drives); // a hard disk takes a while: which one it is
            break;
        case IndexService::State::Ready:
            break;
        }
    } else if (m_scope == Content) {
        // Nothing to say: the divider shows the scan, and more matches than
        // listed come as the list is scrolled (loadMore).
    } else if (m_keyword) {
        // The command's list: nothing of the wait for the first 150 ms, then
        // who is being waited for; its failure for a few seconds.
        if (!m_pluginError.isEmpty())
            s = m_pluginError;
        else if (!m_pluginPending.isEmpty() && m_pluginSlow)
            s = pluginWaiting();
    } else {
        QStringList parts;
        if (m_searchSlow && m_replacePending)
            parts.append(tr("正在搜索…")); // the rows on screen are the last query's
        if (state == IndexService::State::Building)
            parts.append(tr("索引尚未建完"));
        // Plugins pushing into the list: who is still being waited for
        // (once it takes a while), who gave up.
        if (!m_pluginPending.isEmpty() && m_pluginSlow) {
            QStringList names;
            for (const QString& id : m_pluginPending)
                names.append(pluginName(id));
            names.sort();
            parts.append(tr("正在等 %1…").arg(names.join(tr("、"))));
        }
        if (!m_pluginError.isEmpty())
            parts.append(m_pluginError);
        s = parts.join(u" · "_s);
    }
    if (const int selected = m_results.selectedCount(); selected > 0)
        s = tr("已选择 %Ln 项", nullptr, selected);
    if (s != m_status) {
        m_status = s;
        emit statusChanged();
    } else {
        emit statusChanged(); // busy/searching may have changed
    }
    refreshKeyHints(); // the rows changed with whatever brought us here
}

void Launcher::setCurrentRow(int row)
{
    if (row == m_currentRow)
        return;
    m_currentRow = row;
    emit currentRowChanged();
    refreshKeyHints();
}

// What the keys do on the row the keyboard is on (keyHints), from the same
// cases as menuItems(). Enter on a plugin's row is its first action by
// name, else what its default does; the second key is its second action,
// or 打开位置 for a row with a location; Tab completes the row that offers
// it (the hint row of a keyword, completionRow), else switches the scope,
// which means nothing in keyword mode.
void Launcher::refreshKeyHints()
{
    QString enter;
    QString secondKeys;
    QString second;
    // In 文件, Tab goes through the strip of kinds (not into a folder).
    const bool narrowing = (m_scope == Files || m_scope == Folders) && !m_keyword;
    QString tab = enterableFolder(m_currentRow) && !narrowing ? tr("进入文件夹")
        : completionRow(m_currentRow)                          ? tr("补全")
        : narrowing                                            ? tr("切换类型")
        : m_keyword                                            ? QString()
                                                               : tr("切换范围");
    const SearchResult* r = m_results.at(m_currentRow);
    if (!r) {
        if (!m_keyword) {
            enter = tr("打开");
            secondKeys = u"Ctrl+Enter"_s;
            second = tr("打开位置");
        }
    } else if (r->isPlugin()) {
        const PluginRow& p = *r->plugin;
        if (p.valid) {
            if (!p.actions.isEmpty()) {
                enter = p.actions[0].title;
                if (p.actions.size() > 1) {
                    secondKeys = p.actions[1].key.isEmpty() ? u"Ctrl+Enter"_s : p.actions[1].key;
                    second = p.actions[1].title;
                }
            } else {
                enter = p.openKind == PluginRow::OpenKind::Url ? (p.openTarget.compare(u"winshun:settings", Qt::CaseInsensitive) == 0 ? tr("打开设置") : tr("打开网址"))
                    : p.openKind == PluginRow::OpenKind::Path ? tr("打开")
                    : !p.searchQuery.isEmpty()                ? tr("搜索")
                    : plugin::isHintRow(*r)                   ? tr("进入")
                    : !p.setQuery.isEmpty()                   ? tr("填入搜索框")
                                                              : tr("复制"); // the plugin's default action copies the row's text
            }
        }
    } else if (r->isWeb()) {
        enter = r->words.isEmpty() ? tr("打开网站") : tr("搜索");
    } else if (r->isPlace()) {
        enter = tr("打开");
    } else if (r->isApp()) {
        enter = tr("打开");
        if (!r->target.isEmpty()) {
            secondKeys = u"Ctrl+Enter"_s;
            second = r->isPackagedApp() ? tr("打开安装文件夹") : tr("打开位置");
        }
    } else {
        enter = r->isDir ? tr("打开文件夹") : tr("打开");
        secondKeys = u"Ctrl+Enter"_s;
        second = tr("打开位置");
    }
    const QVariantMap hints {{u"enter"_s, enter}, {u"secondKeys"_s, secondKeys}, {u"second"_s, second}, {u"tab"_s, tab}};
    if (hints == m_keyHints)
        return;
    m_keyHints = hints;
    emit keyHintsChanged();
}

void Launcher::flash(const QString& message)
{
    m_flash = message;
    m_flashTimer.start();
    emit statusChanged();
}

void Launcher::cycleScope(int delta)
{
    static constexpr Scope kOrder[] {All, Files, Content}; // as the tabs show them (文件夹 within 文件)
    const int scopes = static_cast<int>(std::size(kOrder)) - (m_contentEnabled ? 0 : 1); // Content last
    const Scope tab = m_scope == Folders ? Files : m_scope;
    const int at = static_cast<int>(std::find(std::begin(kOrder), std::end(kOrder), tab) - std::begin(kOrder));
    setScope(kOrder[((at + delta) % scopes + scopes) % scopes]);
}

QVariantList Launcher::fileTypes() const
{
    QVariantList types;
    for (const FileType& type : m_fileTypes)
        types.append(QVariantMap {{u"name"_s, type.name}, {u"extensions"_s, type.extensions.join(u' ')}});
    return types;
}

void Launcher::setFileTypes(const FileTypes& types)
{
    if (types == m_fileTypes)
        return;
    // The kind chosen stays chosen where it went (moved up, renamed: by its
    // id or name); gone, any kind.
    const QString chosen = m_fileType >= 0 ? fileTypeKey(m_fileTypes[m_fileType]) : QString();
    m_fileTypes = types;
    const int at = chosen.isEmpty() ? -1 : fileTypeIndex(chosen);
    if (at != m_fileType) {
        m_fileType = at;
        emit fileTypeChanged();
        emit fileChoiceChanged();
    }
    emit fileTypesChanged();
    emit scopeChanged(); // the placeholder names the kind
    // Its extensions changed: listed again (a refresh when they did not).
    if (m_fileType >= 0 && m_window && m_window->isVisible())
        search();
}

QStringList Launcher::fileExtensions() const
{
    return m_scope == Files && m_fileType >= 0 ? m_fileTypes[m_fileType].extensions : QStringList();
}

void Launcher::chooseFileType(int type)
{
    const Scope scope = type == kFoldersChip ? Folders : Files;
    if (type < -1 || type >= m_fileTypes.size())
        type = -1;
    if (scope == m_scope && type == m_fileType)
        return;
    m_scope = scope;
    if (type != m_fileType || m_foldersChosen != (scope == Folders)) {
        m_fileType = type;
        m_foldersChosen = scope == Folders;
        emit fileTypeChanged();
        emit fileChoiceChanged();
    }
    emit scopeChanged();
    m_results.setSelection({});
    m_held.reset();
    refreshKeyHints();
    search(); // the rows stay until the new ones come (see setScope)
}

void Launcher::cycleFileType(int delta)
{
    QList<int> chips {-1, kFoldersChip};
    for (int i = 0; i < m_fileTypes.size(); ++i)
        chips.append(i);
    const int at = std::max(0, static_cast<int>(chips.indexOf(m_scope == Folders ? kFoldersChip : m_fileType)));
    const int count = static_cast<int>(chips.size());
    chooseFileType(chips[((at + delta) % count + count) % count]);
}

void Launcher::editFileTypes()
{
    emit fileTypeSettingsRequested();
}

QString Launcher::fileTypeKey(const FileType& type)
{
    return type.id.isEmpty() ? u"name:"_s + type.name : type.id;
}

int Launcher::fileTypeIndex(const QString& key) const
{
    for (int i = 0; i < m_fileTypes.size(); ++i) {
        if (fileTypeKey(m_fileTypes[i]) == key)
            return i;
    }
    return -1;
}

QString Launcher::fileChoice() const
{
    return m_foldersChosen ? u"folders"_s : m_fileType >= 0 ? fileTypeKey(m_fileTypes[m_fileType]) : QString();
}

void Launcher::setFileChoice(const QString& choice)
{
    const int type = choice == u"folders" || choice.isEmpty() ? -1 : fileTypeIndex(choice);
    if (m_scope == Files || m_scope == Folders) {
        chooseFileType(choice == u"folders" ? kFoldersChip : type);
        return;
    }
    m_foldersChosen = choice == u"folders";
    if (type != m_fileType) {
        m_fileType = type;
        emit fileTypeChanged();
    }
}

void Launcher::dismiss()
{
    emit dismissRequested();
}

void Launcher::trigger(int row, int action)
{
    // A terminal opens for the row it was asked on, not for each of a selection.
    if (action != OpenTerminal && m_results.isSelected(row) && m_results.selectedCount() > 1) {
        performMany(m_results.selection(), static_cast<Action>(action));
        return;
    }
    if (const SearchResult* r = m_results.at(row))
        perform(*r, static_cast<Action>(action));
}

void Launcher::triggerSelection(int action)
{
    const SearchResults items = m_results.selection();
    if (items.size() == 1)
        perform(items.first(), static_cast<Action>(action));
    else if (!items.isEmpty())
        performMany(items, static_cast<Action>(action));
}

void Launcher::dragOut(int row)
{
    const SearchResult* at = m_results.at(row);
    if (!at)
        return;
    const SearchResults items = m_results.isSelected(row) && m_results.selectedCount() > 1 ? m_results.selection()
                                                                                         : SearchResults {*at};
    QStringList paths;
    for (const SearchResult& r : items) {
        // A share that does not answer would hold the shell up for long.
        if (!r.isApp() && !r.offline && serverAnswers(r.path))
            paths.append(r.path);
    }
    if (paths.isEmpty() || !m_window)
        return;
    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    dragout::drag(paths, hwnd);
    // OLE's loop took the button's release: Qt still holds the press. Let it
    // go where the pointer is now (over another program, mostly).
    POINT p {};
    ::GetCursorPos(&p);
    ::ScreenToClient(hwnd, &p);
    ::PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<short>(p.x), static_cast<short>(p.y)));
}

void Launcher::toggleSelected(int row)
{
    const SearchResult* r = m_results.at(row);
    if (!r)
        return;
    QSet<QString> paths = m_results.selectedPaths();
    if (!paths.remove(r->path))
        paths.insert(r->path);
    m_results.setSelection(std::move(paths));
}

void Launcher::selectRange(int from, int to, bool add)
{
    QSet<QString> paths = add ? m_results.selectedPaths() : QSet<QString>();
    for (int row = std::max(0, std::min(from, to)); row <= std::max(from, to); ++row) {
        if (const SearchResult* r = m_results.at(row))
            paths.insert(r->path);
    }
    m_results.setSelection(std::move(paths));
}

void Launcher::clearSelection()
{
    m_results.setSelection({});
}

void Launcher::perform(const SearchResult& result, Action action)
{
    const SearchResult r = result; // the model may change underneath us
    if (action == EditWebShortcuts) {
        emit webSettingsRequested();
        return;
    }
    if (action == AddFavorite || action == RemoveFavorite || action == FavoriteFolder || action == EditFavorite) {
        performFavorite(r, action);
        return;
    }
    if (r.isPlugin()) {
        performPlugin(r, action);
        return;
    }
    if (action == Alternate)
        action = Open; // Shift+Enter means something to a plugin's row only
    // A favorite app uninstalled, a place gone with an update of Windows.
    if (r.missing && r.isApp() && (action == Open || action == RunAsAdmin || action == Reveal || action == CopyItem)) {
        flash(tr("“%1” 已不存在；右键可从收藏夹移除").arg(r.name));
        return;
    }
    if (r.isWeb()) {
        performWeb(r, action);
        return;
    }
    if (r.isPlace()) {
        performPlace(r, action);
        return;
    }
    if (r.isApp()) {
        performApp(r, action);
        return;
    }
    const bool needsFile
        = action == Open || action == Reveal || action == RunAsAdmin || action == Recycle || action == OpenTerminal;
    if (needsFile && !serverAnswers(r.path)) {
        flash(tr("连不上 %1：它离线了，或不在同一网络").arg(QString::fromStdWString(std::wstring(unc::host(r.path.toStdWString())))));
        return;
    }
    if (needsFile && r.offline)
        m_index->shares().networkChanged(); // it answers again: its share is back
    if (needsFile && !QFileInfo::exists(r.path)) {
        if (action == Recycle)
            m_results.remove(r.path);
        m_history->remove(r.path);
        emit historyChanged();
        // A favorite stays: it may be back (a drive plugged in again), or be picked anew.
        flash(r.favorite ? tr("“%1” 已不存在；右键可从收藏夹移除").arg(r.name) : tr("“%1” 已不存在").arg(r.name));
        return;
    }
    switch (action) {
    case Open:
    case RunAsAdmin:
        remember(r.path);
        shell::open(r.path, action == RunAsAdmin, reportFailure(r.name));
        emit dismissRequested();
        break;
    case Reveal:
        remember(r.path);
        shell::reveal(r.path);
        emit dismissRequested();
        break;
    case CopyPath:
        shell::copyText(r.path);
        flash(tr("已复制路径"));
        break;
    case CopyName:
        shell::copyText(r.name);
        flash(tr("已复制名称"));
        break;
    case CopyItem:
        shell::copyFiles({r.path});
        flash(tr("已复制，可在资源管理器中粘贴"));
        break;
    case ForgetRecent:
        forgetRecent({r.path});
        break;
    case Recycle: {
        const auto owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
        // Back on the GUI thread, where `self` can be checked safely.
        shell::recycle({r.path}, owner, [self = QPointer(this), r](bool ok) {
            QMetaObject::invokeMethod(qApp, [self, r, ok] {
                if (self)
                    self->onRecycled(r, ok);
            }, Qt::QueuedConnection);
        });
        break;
    }
    case EditWebShortcuts:
    case AddFavorite:
    case RemoveFavorite:
    case OpenTerminal:
        remember(r.path);
        shell::openTerminal(r.isDir ? r.path : folderOf(r.path), reportFailure(r.name));
        emit dismissRequested();
        break;
    case FavoriteFolder:
    case EditFavorite:
        break; // not for a file, or done above
    }
}

// An installed app opens through the shell (shell:AppsFolder\<id>), as the
// Start menu opens it; its "location" is its program or install folder.
void Launcher::performApp(const SearchResult& app, Action action)
{
    switch (action) {
    case Open:
    case RunAsAdmin:
        if (action == RunAsAdmin && !app.elevatable) {
            flash(tr("“%1”不能以管理员身份运行").arg(app.name));
            return;
        }
        remember(app.path);
        shell::launchApp(app.path, action == RunAsAdmin, reportFailure(app.name));
        emit dismissRequested();
        break;
    case Reveal:
        if (app.target.isEmpty() || !QFileInfo::exists(app.target)) {
            flash(tr("“%1”没有可以打开的位置").arg(app.name));
            return;
        }
        remember(app.path);
        if (app.isPackagedApp())
            shell::open(app.target); // its install folder; the folder above it is not readable
        else
            shell::reveal(app.target);
        emit dismissRequested();
        break;
    case CopyItem:
        if (app.hasCopyableTarget() && QFileInfo::exists(app.target)) {
            shell::copyFiles({app.target});
            flash(tr("已复制，可在资源管理器中粘贴"));
            break;
        }
        [[fallthrough]]; // nothing to paste: copy where it is instead
    case CopyPath:
        shell::copyText(app.target.isEmpty() ? app.path : app.target);
        flash(tr("已复制路径"));
        break;
    case CopyName:
        shell::copyText(app.name);
        flash(tr("已复制名称"));
        break;
    case ForgetRecent:
        forgetRecent({app.path});
        break;
    case Recycle: // apps are uninstalled in Windows Settings
    case EditWebShortcuts:
    case AddFavorite:
    case RemoveFavorite:
    case FavoriteFolder:
    case EditFavorite:
    case OpenTerminal:
        break;
    }
}

// A place in Windows opens by its command, with the user's normal rights
// (Settings and Control Panel ask for more themselves when they need it).
// It has no file of its own to reveal or copy: copying takes the command.
void Launcher::performPlace(const SearchResult& place, Action action)
{
    switch (action) {
    case Open:
    case RunAsAdmin:
        remember(place.path);
        shell::run(place.target, reportFailure(place.name));
        emit dismissRequested();
        break;
    case Reveal:
        flash(tr("“%1”没有可以打开的位置").arg(place.name));
        break;
    case CopyItem:
    case CopyPath:
        shell::copyText(place.target);
        flash(tr("已复制打开它的命令"));
        break;
    case CopyName:
        shell::copyText(place.name);
        flash(tr("已复制名称"));
        break;
    case ForgetRecent:
        forgetRecent({place.path});
        break;
    case Recycle:
    case EditWebShortcuts:
    case AddFavorite:
    case RemoveFavorite:
    case FavoriteFolder:
    case EditFavorite:
    case OpenTerminal:
        break;
    }
}

// A web shortcut's site or search opens in the default browser, with the
// user's normal rights like everything else. Nothing of it is remembered:
// what one searches the web for stays out of the list of recent items.
void Launcher::performWeb(const SearchResult& web, Action action)
{
    switch (action) {
    case Open:
    case RunAsAdmin:
        shell::openUrl(web.target);
        emit dismissRequested();
        break;
    case Reveal:
        flash(tr("“%1”没有可以打开的位置").arg(web.name));
        break;
    case CopyItem:
    case CopyPath:
        shell::copyText(web.target);
        flash(tr("已复制网址"));
        break;
    case CopyName:
        shell::copyText(web.name);
        flash(tr("已复制名称"));
        break;
    case ForgetRecent:
    case Recycle:
    case EditWebShortcuts: // see perform()
    case AddFavorite:
    case RemoveFavorite:
    case FavoriteFolder:
    case EditFavorite:
    case OpenTerminal:
        break;
    }
}

// The favorites: a row in or out, the folder a file is in, or the row's
// favorite in the settings. The rows refresh on their own (setFavorites);
// the footer says what was done after that (a search clears it).
void Launcher::performFavorite(const SearchResult& result, Action action)
{
    if (!m_favorites)
        return;
    switch (action) {
    case AddFavorite:
        if (const std::optional<Favorite> f = favoriteOf(result))
            flash(m_favorites->add(*f) ? tr("已加入收藏夹") : tr("已在收藏夹里"));
        break;
    case RemoveFavorite:
        if (m_favorites->remove(result.path))
            flash(tr("已从收藏夹移除"));
        break;
    case FavoriteFolder: {
        Favorite f;
        f.kind = Favorite::Kind::Folder;
        f.path = result.isApp() || result.isDir ? QString() : folderOf(result.path);
        if (f.path.isEmpty())
            break;
        f.name = f.path.size() == 3 ? f.path : f.path.mid(f.path.lastIndexOf(u'\\') + 1);
        flash(m_favorites->add(f) ? tr("已把“%1”加入收藏夹").arg(f.name) : tr("“%1”已在收藏夹里").arg(f.name));
        break;
    }
    case EditFavorite:
        emit favoriteSettingsRequested(result.path);
        break;
    default:
        break;
    }
}

// Several selected results at once. Those an action does not apply to
// (running a folder as administrator, recycling an app) are left out.
void Launcher::performMany(const SearchResults& items, Action action)
{
    // Each server asked afresh once, then taken at its word.
    QSet<QString> asked;
    const auto exists = [&asked](const QString& path) {
        const QString host = QString::fromStdWString(std::wstring(unc::host(path.toStdWString()))).toLower();
        const bool fresh = !asked.contains(host);
        asked.insert(host);
        return serverAnswers(path, fresh) && QFileInfo::exists(path);
    };
    if (action == Alternate)
        action = Open;
    if (action == AddFavorite && m_favorites) {
        FavoriteList favorites;
        for (const SearchResult& r : items) {
            if (const std::optional<Favorite> f = favoriteOf(r))
                favorites.append(*f);
        }
        const int added = m_favorites->addAll(favorites);
        flash(added > 0 ? tr("已加入收藏夹 %Ln 项", nullptr, added) : tr("选中的项目都已在收藏夹里"));
        return;
    }
    if (action == RemoveFavorite && m_favorites) {
        QStringList paths;
        for (const SearchResult& r : items)
            paths.append(r.path);
        if (const int removed = m_favorites->removeAll(paths); removed > 0)
            flash(tr("已从收藏夹移除 %Ln 项", nullptr, removed));
        return;
    }
    switch (action) {
    case Open:
    case RunAsAdmin: {
        const bool admin = action == RunAsAdmin;
        int started = 0;
        for (const SearchResult& r : items) {
            if (r.isPlugin()) { // its own default action (which may close the window itself)
                if (admin)
                    continue;
                performPlugin(r, Open);
                ++started;
                continue;
            }
            if (r.isWeb()) {
                if (admin)
                    continue;
                shell::openUrl(r.target);
                ++started;
                continue; // not remembered (see performWeb)
            }
            if (r.isPlace()) {
                if (admin)
                    continue;
                shell::run(r.target, reportFailure(r.name));
            } else if (r.isApp()) {
                if (admin && !r.elevatable)
                    continue;
                shell::launchApp(r.path, admin, reportFailure(r.name));
            } else {
                if (!exists(r.path) || (admin && (r.isDir || !shell::canRunAsAdministrator(r.path))))
                    continue;
                shell::open(r.path, admin, reportFailure(r.name));
            }
            remember(r.path);
            ++started;
        }
        if (started == 0) {
            flash(admin ? tr("选中的项目都不能以管理员身份运行") : tr("选中的项目都已不存在"));
            return;
        }
        emit dismissRequested();
        break;
    }
    case Reveal: {
        QStringList paths;
        int shown = 0;
        for (const SearchResult& r : items) {
            const QString& place = r.isApp() ? r.target : r.path;
            if (r.isPlace() || place.isEmpty() || !exists(place))
                continue;
            remember(r.path);
            if (r.isPackagedApp())
                shell::open(place); // its install folder; the folder above it is not readable
            else
                paths.append(place);
            ++shown;
        }
        if (shown == 0) {
            flash(tr("选中的项目没有可以打开的位置"));
            return;
        }
        shell::reveal(paths);
        emit dismissRequested();
        break;
    }
    case CopyItem: {
        QStringList files;
        for (const SearchResult& r : items) {
            const QString file = r.isApp() ? (r.hasCopyableTarget() ? r.target : QString()) : r.path;
            if (!file.isEmpty() && exists(file))
                files.append(file);
        }
        if (files.isEmpty()) {
            flash(tr("选中的项目没有可以复制的文件"));
            return;
        }
        shell::copyFiles(files);
        flash(tr("已复制 %Ln 项，可在资源管理器中粘贴", nullptr, static_cast<int>(files.size())));
        break;
    }
    case CopyPath: {
        QStringList paths;
        for (const SearchResult& r : items) {
            if (r.isPlugin())
                paths.append(r.plugin->copy.isEmpty() ? r.name : r.plugin->copy);
            else
                paths.append(r.isApp() && !r.target.isEmpty() ? r.target : r.path);
        }
        shell::copyText(paths.join(u'\n'));
        flash(tr("已复制 %Ln 个路径", nullptr, static_cast<int>(paths.size())));
        break;
    }
    case CopyName: {
        QStringList names;
        for (const SearchResult& r : items)
            names.append(r.name);
        shell::copyText(names.join(u'\n'));
        flash(tr("已复制 %Ln 个名称", nullptr, static_cast<int>(names.size())));
        break;
    }
    case ForgetRecent: {
        QStringList paths;
        for (const SearchResult& r : items) {
            if (r.recent)
                paths.append(r.path);
        }
        forgetRecent(paths);
        break;
    }
    case Recycle: {
        SearchResults files;
        QStringList paths;
        for (const SearchResult& r : items) {
            if (r.isApp()) // apps are uninstalled in Windows Settings
                continue;
            if (!exists(r.path)) {
                forgetRecycled(r);
                continue;
            }
            files.append(r);
            paths.append(r.path);
        }
        if (files.isEmpty()) {
            refreshStatus();
            flash(tr("选中的项目不能删除"));
            return;
        }
        const auto owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
        shell::recycle(paths, owner, [self = QPointer(this), files](bool ok) {
            QMetaObject::invokeMethod(qApp, [self, files, ok] {
                if (self)
                    self->onRecycledMany(files, ok);
            }, Qt::QueuedConnection);
        });
        break;
    }
    case EditWebShortcuts:
        emit webSettingsRequested();
        break;
    }
}

void Launcher::setRecordHistory(bool on)
{
    if (m_recordHistory == on)
        return;
    m_recordHistory = on;
    emit historyChanged();
    refreshFrequentApps();
    if (m_window && m_window->isVisible())
        search(); // "最近" rows come or go
}

bool Launcher::canClearHistory() const
{
    return m_recordHistory && !m_history->items().isEmpty();
}

void Launcher::clearHistory()
{
    m_history->clear();
    emit historyChanged();
    refreshFrequentApps();
    m_results.setSelection({});
    search(); // the list of recent ones empties, "最近" goes from the rest
    flash(tr("已清除最近使用记录"));
}

void Launcher::remember(const QString& path)
{
    if (m_recordHistory)
        m_history->record(path, QDateTime::currentMSecsSinceEpoch());
}

void Launcher::forgetRecent(const QStringList& paths)
{
    for (const QString& path : paths)
        m_history->remove(path);
    emit historyChanged();
    m_results.setSelection({});
    search(); // refreshes the rows in place: no "最近", or gone from the list of recent ones
    flash(paths.size() == 1 ? tr("已从最近使用中移除") : tr("已从最近使用中移除 %Ln 项", nullptr, static_cast<int>(paths.size())));
}

// For shell::open() and launchApp(), which call it on a worker thread.
std::function<void(bool)> Launcher::reportFailure(const QString& name)
{
    return [self = QPointer(this), name](bool ok) {
        if (ok)
            return;
        QMetaObject::invokeMethod(qApp, [self, name] {
            if (self)
                emit self->openFailed(name);
        }, Qt::QueuedConnection);
    };
}

void Launcher::onRecycled(const SearchResult& result, bool ok)
{
    if (!forgetRecycled(result)) { // cancelled, in use, access denied...
        flash(tr("没有删除“%1”").arg(result.name));
        return;
    }
    refreshStatus();
    flash((ok ? tr("已将“%1”移到回收站") : tr("已删除“%1”")).arg(result.name));
}

void Launcher::onRecycledMany(const SearchResults& items, bool ok)
{
    int gone = 0;
    for (const SearchResult& r : items) {
        if (forgetRecycled(r))
            ++gone;
    }
    refreshStatus();
    const int kept = static_cast<int>(items.size()) - gone;
    if (gone == 0)
        flash(tr("没有删除选中的项目"));
    else if (kept > 0)
        flash(tr("已将 %Ln 项移到回收站，%1 项没有删除", nullptr, gone).arg(kept));
    else if (ok)
        flash(tr("已将 %Ln 项移到回收站", nullptr, gone));
    else
        flash(tr("已删除 %Ln 项", nullptr, gone));
}

bool Launcher::forgetRecycled(const SearchResult& result)
{
    if (QFileInfo::exists(result.path))
        return false;
    m_history->remove(result.path);
    emit historyChanged();
    const SearchResult* row = m_results.at(m_results.indexOf(result.path));
    const bool contentRow = row && row->line > 0;
    if (m_results.remove(result.path) && m_scope != Content) {
        if (contentRow)
            --m_contentHits;
        else if (m_totalMatches > 0)
            --m_totalMatches;
    }
    return true;
}

QVariantList Launcher::menuItems(int row) const
{
    const SearchResult* r = m_results.at(row);
    if (!r)
        return {};
    const auto entry = [](Action action, const QString& text, const QString& shortcut, const QString& glyph) {
        return QVariantMap {
            {u"action"_s, static_cast<int>(action)},
            {u"text"_s, text},
            {u"shortcut"_s, shortcut},
            {u"glyph"_s, glyph},
        };
    };
    const QVariantMap separator {{u"separator"_s, true}};

    if (m_results.isSelected(row) && m_results.selectedCount() > 1) { // for the whole selection
        const SearchResults selection = m_results.selection();
        const auto any = [&](auto test) { return std::any_of(selection.cbegin(), selection.cend(), test); };
        QVariantList items;
        items.append(entry(Open, tr("打开 %Ln 项", nullptr, static_cast<int>(selection.size())), u"Enter"_s, u"\uE8E5"_s));
        if (any([](const SearchResult& r) { return !r.isApp() || (!r.isPlace() && !r.target.isEmpty()); }))
            items.append(entry(Reveal, tr("打开所在位置"), u"Ctrl+Enter"_s, u"\uE8DA"_s));
        items.append(separator);
        if (any([](const SearchResult& r) { return !r.isApp() || r.hasCopyableTarget(); }))
            items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u"\uE8C8"_s));
        items.append(entry(CopyPath, tr("复制完整路径"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        if (any([](const SearchResult& r) { return r.recent; })) {
            items.append(separator);
            items.append(entry(ForgetRecent, tr("从最近使用中移除"), QString(), u"\uE711"_s)); // Cancel
        }
        if (m_favorites) {
            int addable = 0;
            int kept = 0;
            for (const SearchResult& s : selection) {
                if (favoriteOf(s))
                    ++(m_favorites->contains(s.path) ? kept : addable);
            }
            if (addable > 0 || kept > 0)
                items.append(separator);
            if (addable > 0)
                items.append(entry(AddFavorite, tr("加入收藏夹（%Ln 项）", nullptr, addable), QString(), u"\uE734"_s));
            else if (kept > 0)
                items.append(entry(RemoveFavorite, tr("从收藏夹移除（%Ln 项）", nullptr, kept), QString(), u"\uE8D9"_s));
        }
        return items;
    }

    // Glyphs: Segoe Fluent Icons / MDL2 Assets, or the name of an icon
    // Glyph.qml draws.
    QVariantList items;
    // The favorites' entries, after the copying ones: in or out (and edit),
    // and for a file the folder it is in.
    const auto favoriteItems = [&] {
        if (!m_favorites || !favoriteOf(*r))
            return;
        items.append(separator);
        if (m_favorites->contains(r->path)) {
            items.append(entry(RemoveFavorite, tr("从收藏夹移除"), QString(), u"\uE8D9"_s)); // Unfavorite
            items.append(entry(EditFavorite, tr("编辑收藏（名称、关键词）…"), QString(), u"\uE70F"_s)); // Edit
        } else {
            items.append(entry(AddFavorite, tr("加入收藏夹"), QString(), u"\uE734"_s)); // FavoriteStar
        }
        if (const QString folder = r->isApp() || r->isDir ? QString() : folderOf(r->path);
            !folder.isEmpty() && !m_favorites->contains(folder)) {
            items.append(entry(FavoriteFolder, tr("收藏所在的文件夹"), QString(), u"\uE8B7"_s)); // Folder
        }
    };
    if (r->isPlugin()) {
        // Its actions as the plugin named them (Enter, Ctrl+Enter, Shift+Enter
        // in that order), else what its default does; then copying its text.
        const PluginRow& p = *r->plugin;
        static constexpr QStringView kKeys[] {u"Enter", u"Ctrl+Enter", u"Shift+Enter"};
        if (p.valid) {
            for (int i = 0; i < p.actions.size(); ++i) {
                const QString key = !p.actions[i].key.isEmpty() ? p.actions[i].key
                    : i < 3                                     ? kKeys[i].toString()
                                                                : QString();
                items.append(entry(static_cast<Action>(PluginAction + i), p.actions[i].title, key,
                    i == 0 ? u"\uE8A7"_s : u"\uE768"_s));
            }
            if (p.actions.isEmpty()) {
                const QString text = p.openKind == PluginRow::OpenKind::Url ? tr("打开网址")
                    : !p.searchQuery.isEmpty()                              ? tr("搜索")
                    : !p.setQuery.isEmpty()                                 ? tr("填入搜索框")
                                                                            : tr("打开");
                items.append(entry(Open, text, u"Enter"_s, u"\uE8A7"_s));
            }
            items.append(separator);
        }
        items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u"\uE8C8"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        return items;
    }
    if (r->isWeb()) {
        items.append(r->words.isEmpty() ? entry(Open, tr("打开网站"), u"Enter"_s, u"\uE774"_s) // Globe
                                        : entry(Open, tr("搜索"), u"Enter"_s, u"\uE721"_s)); // Search
        items.append(separator);
        items.append(entry(CopyPath, tr("复制网址"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        items.append(separator);
        items.append(entry(EditWebShortcuts, tr("编辑网页搜索…"), QString(), u"\uE70F"_s)); // Edit
        return items;
    }
    if (r->isPlace()) {
        items.append(entry(Open, tr("打开"), u"Enter"_s, u"\uE8A7"_s)); // OpenInNewWindow
        items.append(separator);
        items.append(entry(CopyPath, tr("复制打开命令"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        favoriteItems();
        if (r->recent) {
            items.append(separator);
            items.append(entry(ForgetRecent, tr("从最近使用中移除"), QString(), u"\uE711"_s));
        }
        return items;
    }
    if (r->isApp()) {
        items.append(entry(Open, tr("打开"), u"Enter"_s, u"\uE8A7"_s)); // OpenInNewWindow
        if (r->elevatable)
            items.append(entry(RunAsAdmin, tr("以管理员身份运行"), u"Ctrl+Shift+Enter"_s, u"\uE7EF"_s));
        if (!r->target.isEmpty())
            items.append(entry(Reveal, r->isPackagedApp() ? tr("打开安装文件夹") : tr("打开所在位置"), u"Ctrl+Enter"_s,
                u"\uE8DA"_s));
        items.append(separator);
        if (r->hasCopyableTarget())
            items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u"\uE8C8"_s));
        if (!r->target.isEmpty())
            items.append(entry(CopyPath, tr("复制完整路径"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
        items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
        favoriteItems();
        if (r->recent) {
            items.append(separator);
            items.append(entry(ForgetRecent, tr("从最近使用中移除"), QString(), u"\uE711"_s));
        }
        return items;
    }
    items.append(entry(Open, r->isDir ? tr("打开文件夹") : tr("打开"), u"Enter"_s, r->isDir ? u"\uE838"_s : u"\uE8E5"_s));
    items.append(entry(Reveal, tr("打开所在位置"), u"Ctrl+Enter"_s, u"\uE8DA"_s));
    items.append(entry(OpenTerminal, tr("在终端中打开"), QString(), u"\uE756"_s)); // CommandPrompt
    if (!r->isDir && shell::canRunAsAdministrator(r->path))
        items.append(entry(RunAsAdmin, tr("以管理员身份运行"), u"Ctrl+Shift+Enter"_s, u"\uE7EF"_s));
    items.append(separator);
    items.append(entry(CopyItem, tr("复制"), u"Ctrl+C"_s, u"\uE8C8"_s));
    items.append(entry(CopyPath, tr("复制完整路径"), u"Ctrl+Shift+C"_s, u"copyPath"_s));
    items.append(entry(CopyName, tr("复制名称"), QString(), u"\uE8AC"_s));
    favoriteItems();
    if (r->recent) {
        items.append(separator);
        items.append(entry(ForgetRecent, tr("从最近使用中移除"), QString(), u"\uE711"_s));
    }
    return items;
}

// ---- plugins ------------------------------------------------------------------

QString Launcher::pluginKeyword() const
{
    return m_keyword ? m_keyword->keyword : QString();
}

QString Launcher::pluginLabel() const
{
    return m_keyword ? m_keyword->label : QString();
}

QString Launcher::pluginIcon() const
{
    return m_keyword ? m_keyword->icon : QString();
}

// After the keyword, while nothing follows it: the user is about to type
// the words, so the hint sits where they go.
QString Launcher::pluginHint() const
{
    return m_keyword && m_keyword->words.isEmpty() && m_query.endsWith(u' ') ? m_keyword->hint : QString();
}

QString Launcher::pluginWaiting() const
{
    if (!m_keyword || m_pluginPending.isEmpty() || !m_pluginSlow)
        return {};
    return m_pluginProgress.isEmpty() ? tr("正在等 %1…").arg(m_keyword->label) : m_pluginProgress;
}

QString Launcher::pluginName(const QString& pluginId) const
{
    if (m_plugins) {
        if (const plugin::Entry* e = m_plugins->catalog().find(pluginId); e && e->manifest)
            return e->manifest->name.pick(uiLocale());
    }
    return pluginId;
}

bool Launcher::clearToKeyword()
{
    if (!m_keyword || m_keyword->words.isEmpty())
        return false;
    setQuery(m_keyword->keyword + u' ');
    return true;
}

// The row with an autocomplete: `row` itself, else (outside keyword mode,
// where the row's own is all Tab means) the hint row offering a keyword
// that begins with what was typed, which sits among the rows, not under
// the keyboard ("uu": Tab gives "uuid " from the app row above it).
const SearchResult* Launcher::completionRow(int row) const
{
    const SearchResult* r = m_results.at(row);
    if (r && r->isPlugin() && !r->plugin->autocomplete.isEmpty())
        return r;
    if (m_keyword)
        return nullptr;
    for (int i = 0; i < m_results.count(); ++i) {
        const SearchResult* h = m_results.at(i);
        if (plugin::isHintRow(*h) && !h->plugin->autocomplete.isEmpty())
            return h;
    }
    return nullptr;
}

// A favorite folder's row: Tab lists what it holds, as typing its path does
// ("D:\工作文档\"). Other folders' rows keep Tab for the scopes.
const SearchResult* Launcher::enterableFolder(int row) const
{
    const SearchResult* r = m_results.at(row);
    return r && !m_keyword && r->favorite && r->isDir && !r->isApp() && !r->offline && !r->missing ? r : nullptr;
}

bool Launcher::autocomplete(int row)
{
    // In 文件, Tab goes through the strip of kinds instead (cycleFileType).
    const bool narrowing = m_scope == Files || m_scope == Folders;
    if (const SearchResult* folder = narrowing ? nullptr : enterableFolder(row)) {
        setQuery(folder->path.endsWith(u'\\') ? folder->path : folder->path + u'\\');
        return true;
    }
    const SearchResult* r = completionRow(row);
    if (!r)
        return false;
    setQuery(r->plugin->autocomplete);
    return true;
}

void Launcher::leaveKeyword()
{
    if (m_keyword)
        setQuery({});
}

std::optional<Launcher::KeywordMode> Launcher::matchKeyword(const QString& text) const
{
    if (!m_plugins || !m_merge || m_scope != All || text.trimmed().isEmpty())
        return std::nullopt;
    for (const plugin::Match& m : m_plugins->catalog().match(text)) {
        if (m.kind != plugin::Match::Kind::Keyword)
            continue;
        const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        const QString locale = uiLocale();
        const QString icon = !m.command->icon.isEmpty() ? m.command->icon : m.entry->manifest->icon;
        return KeywordMode {m.entry->id, m.command->id, m.keyword, m.text,
            m.command->title.pick(locale).isEmpty() ? m.entry->manifest->name.pick(locale) : m.command->title.pick(locale),
            m.command->hint.pick(locale), FileIconProvider::pluginIconUrl(m.entry->id, icon, dark)};
    }
    return std::nullopt;
}

// Part of search(): the plugins the text triggers are asked (and the
// earlier query cancelled); a keyword's command takes the list (m_keyword).
void Launcher::startPluginQuery(const QString& text, std::optional<KeywordMode> keyword)
{
    const bool was = m_keyword.has_value();
    m_keyword.reset();
    m_pluginPending.clear();
    m_pluginWait.stop();
    m_pluginSlow = false;
    m_pluginProgress.clear();
    if (!m_plugins || !m_merge) {
        if (was)
            emit pluginChanged();
        return;
    }
    const auto scope = static_cast<ws::Scope>(m_scope);
    const std::uint64_t id = ++m_pluginQueryId;
    m_keyword = std::move(keyword);
    m_merge->begin(id, text, m_keyword ? m_keyword->pluginId : QString(), m_keyword ? m_keyword->commandId : QString());
    m_pluginTimer.start();
    m_plugins->query(text, scope, id); // queryTaken() for each plugin asked; zero-code ones answer within
    if (!m_pluginPending.isEmpty())
        m_pluginWait.start();
    if (was || m_keyword)
        emit pluginChanged();
}

void Launcher::onPluginTaken(std::uint64_t queryId, const QString& pluginId)
{
    if (queryId == m_pluginQueryId)
        m_pluginPending.insert(pluginId);
}

void Launcher::onPluginResults(std::uint64_t queryId, const QString& pluginId, const QString& commandId,
    const QList<ws::plugin::Item>& items, bool partial, bool replace)
{
    if (queryId != m_pluginQueryId || !m_merge)
        return;
    m_merge->results(queryId, pluginId, commandId, items, partial, replace);
    if (m_merge->takeBetterFirstRow())
        emit betterFirstRow();
    refreshStatus();
}

void Launcher::onPluginFinished(std::uint64_t queryId, const QString& pluginId)
{
    if (queryId == m_pluginQueryId)
        pluginSettled(pluginId);
}

void Launcher::onPluginFailed(std::uint64_t queryId, const QString& pluginId, const QString& why)
{
    if (queryId != m_pluginQueryId)
        return;
    m_pluginError = tr("%1：%2").arg(pluginName(pluginId), why);
    m_pluginErrorTimer.start();
    // The keyword's command gave nothing: an empty list, not the last query's rows.
    if (m_keyword && m_keyword->pluginId == pluginId && m_replacePending)
        showPluginRows({});
    pluginSettled(pluginId);
}

void Launcher::onPluginProgress(std::uint64_t queryId, const QString& pluginId, const QString& text)
{
    if (queryId != m_pluginQueryId || !m_keyword || m_keyword->pluginId != pluginId)
        return;
    m_pluginProgress = text;
    refreshStatus();
}

void Launcher::pluginSettled(const QString& pluginId)
{
    m_pluginPending.remove(pluginId);
    if (m_pluginPending.isEmpty()) {
        m_pluginWait.stop();
        m_pluginSlow = false;
        m_pluginElapsedUs = m_pluginTimer.isValid() ? m_pluginTimer.nsecsElapsed() / 1000 : 0;
    }
    refreshStatus();
}

// A plugin row's action: Enter, Ctrl+Enter and Shift+Enter are the first
// three of its actions (its own, else its command's); the menu names them
// by number. The default one is done here when the row says how (open,
// setQuery, search: no round trip, and no process needed); every other
// goes to the plugin, whose effects come back (onPluginActionDone). Never
// as administrator: a plugin's rows open with the user's rights, always.
void Launcher::performPlugin(const SearchResult& result, Action action)
{
    const PluginRow& p = *result.plugin;
    const QString copyText = p.copy.isEmpty() ? result.name : p.copy;
    switch (action) {
    case CopyItem:
    case CopyPath:
        shell::copyText(copyText);
        flash(tr("已复制"));
        return;
    case CopyName:
        shell::copyText(result.name);
        flash(tr("已复制名称"));
        return;
    case RunAsAdmin:
        flash(tr("插件的结果不以管理员身份运行"));
        return;
    case Recycle:
    case ForgetRecent:
    case EditWebShortcuts:
        return;
    default:
        break;
    }
    const int index = action == Open ? 0 : action == Reveal ? 1 : action == Alternate ? 2 : action >= PluginAction ? action - PluginAction : -1;
    if (index < 0 || !p.valid)
        return;
    if (index == 0) {
        switch (p.openKind) {
        case PluginRow::OpenKind::Path:
            // A program with arguments or a working folder is started as
            // such; anything else opens as the shell would open it.
            if (p.openArgs.isEmpty() && p.openCwd.isEmpty())
                shell::open(p.openTarget, false, reportFailure(result.name));
            else if (!shell::startUnelevated(p.openTarget.toStdWString(), p.openArgs.toStdWString(), p.openCwd.toStdWString()).valid())
                emit openFailed(result.name);
            emit dismissRequested();
            return;
        case PluginRow::OpenKind::Url:
            // "winshun:settings": Win顺's own settings at the plugin's card
            // (a row saying what to set up); never handed to the shell.
            if (p.openTarget.compare(u"winshun:settings", Qt::CaseInsensitive) == 0)
                emit pluginSettingsRequested(p.pluginId);
            else
                shell::openUrl(p.openTarget);
            emit dismissRequested();
            return;
        case PluginRow::OpenKind::None:
            break;
        }
        if (!p.searchQuery.isEmpty()) {
            const QString scope = p.searchScope.toLower();
            setScope(scope == u"files" ? Files : scope == u"folders" ? Folders : scope == u"content" ? Content : All);
            setQuery(p.searchQuery);
            return;
        }
        if (!p.setQuery.isEmpty()) {
            setQuery(p.setQuery);
            return;
        }
    }
    if (index >= p.actions.size() && !(index == 0 && p.actions.isEmpty()))
        return; // no such action
    if (!m_plugins)
        return;
    const QString actionId = index < p.actions.size() ? p.actions[index].id : u"default"_s;
    const QString words = m_keyword ? m_keyword->words : m_query.trimmed();
    m_plugins->action(++m_pluginRequest, p.pluginId, p.commandId, actionId, p.item, words);
}

void Launcher::onPluginActionDone(std::uint64_t requestId, const QList<ws::plugin::Effect>& effects, bool ok, const QString& error)
{
    (void)requestId;
    if (!ok) {
        flash(error.isEmpty() ? tr("插件没有完成这个操作") : error);
        return;
    }
    applyEffects(effects);
}

// What a plugin asked for after an action, in its order: hide (the
// default), keep, setQuery, notify, copy, open, replace (its items as the list).
void Launcher::applyEffects(const QList<ws::plugin::Effect>& effects)
{
    bool hide = false;
    for (const plugin::Effect& e : effects) {
        switch (e.kind) {
        case plugin::Effect::Kind::Hide:
            hide = true;
            break;
        case plugin::Effect::Kind::Keep:
            break;
        case plugin::Effect::Kind::SetQuery:
            setQuery(e.text);
            break;
        case plugin::Effect::Kind::Notify:
            flash(e.text);
            break;
        case plugin::Effect::Kind::Copy:
            shell::copyText(e.text);
            break;
        case plugin::Effect::Kind::Open:
            if (e.open.kind == plugin::Open::Kind::Url && e.open.target.compare(u"winshun:settings", Qt::CaseInsensitive) == 0) {
                if (m_keyword)
                    emit pluginSettingsRequested(m_keyword->pluginId); // its own card (see performPlugin)
            } else if (e.open.kind == plugin::Open::Kind::Url)
                shell::openUrl(e.open.target);
            else if (e.open.args.isEmpty() && e.open.cwd.isEmpty())
                shell::open(e.open.target, false);
            else
                shell::startUnelevated(e.open.target.toStdWString(), e.open.args.toStdWString(), e.open.cwd.toStdWString());
            break;
        case plugin::Effect::Kind::Replace:
            if (m_merge && m_keyword)
                m_merge->replace(m_keyword->pluginId, m_keyword->commandId, e.items);
            break;
        }
    }
    if (hide)
        emit dismissRequested();
}

QRectF Launcher::screenArea(QPointF globalPos) const
{
    QScreen* screen = QGuiApplication::screenAt(globalPos.toPoint());
    if (!screen && m_window)
        screen = m_window->screen();
    return screen ? QRectF(screen->availableGeometry()) : QRectF();
}

void Launcher::prepareMenuWindow(QWindow* menu) const
{
    if (!menu)
        return;
    // Showing it must not take the focus from the launcher: losing it
    // dismisses the launcher, and the search box keeps handling the keys.
    menu->setProperty("_q_showWithoutActivating", true);
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    win::styleFramelessWindow(menu);
    win::setDarkFrame(menu, dark);
}

} // namespace ws
