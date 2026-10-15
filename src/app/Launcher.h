#pragma once

#include "AppCatalog.h"
#include "ContentIndex.h"
#include "Favorites.h"
#include "ResultModel.h"
#include "SearchEngine.h"
#include "SearchTypes.h"
#include "WebShortcut.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QVariantList>
#include <QTimer>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ws {

class History;
class IndexService;
class SystemCatalog;
namespace plugin {
class Host;
class Merge;
struct Effect;
struct Item;
}

// The view-model behind the search window: query and scope in, results and
// status out, plus the actions on a result (open, reveal, copy, ...).
class Launcher : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged FINAL)
    Q_PROPERTY(Scope scope READ scope WRITE setScope NOTIFY scopeChanged FINAL)
    Q_PROPERTY(ws::ResultModel* results READ results CONSTANT FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY statusChanged FINAL)
    // Scrolled to the end, there is more to list (loadMore()): names beyond
    // those listed, a content scan stopped at its limit, more files written
    // lately.
    Q_PROPERTY(bool moreAvailable READ moreAvailable NOTIFY statusChanged FINAL)
    // What the search behind the rows passed over, once it is done, each with
    // a one-off action that searches it for this query alone (nothing is
    // saved): [{kind, text, action, running}], for the rows after the last
    // result (or the lines under 没有找到). Files over the size limit (kind
    // "large"), in 内容 the files in system and program folders ("system")
    // and the text of kinds not listed ("types"), and a removable drive the
    // index leaves out ("drive:E:"). While one runs, it alone, with how far
    // it is and 停止 (searchLeftOut, stopLeftOut).
    Q_PROPERTY(QVariantList leftOutHints READ leftOutHints NOTIFY statusChanged FINAL)
    // The rows were found by how their names read, not as typed
    // (SearchResult::approximate): the heading over them, "没有叫“年度报高”的，
    // 按读音找到："; empty otherwise.
    Q_PROPERTY(QString approximateHeading READ approximateHeading NOTIFY statusChanged FINAL)
    // How far the content scan is, 0–1 (the footer's divider fills); -1 while
    // that is not known (busy: the divider shows work going on).
    Q_PROPERTY(qreal progress READ progress NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool searching READ searching NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool stale READ stale NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString placeholder READ placeholder NOTIFY scopeChanged FINAL)
    Q_PROPERTY(QString contentFilesLabel READ contentFilesLabel NOTIFY scopeChanged FINAL)
    // File content search is on (Settings::contentSearch): the 内容 tab is there.
    Q_PROPERTY(bool contentEnabled READ contentEnabled NOTIFY scopeChanged FINAL)
    Q_PROPERTY(bool searchesContent READ searchesContent NOTIFY statusChanged FINAL)
    // History is kept and has something in it (the footer's clear button).
    Q_PROPERTY(bool canClearHistory READ canClearHistory NOTIFY historyChanged FINAL)
    // The apps opened most, for the launcher over the taskbar with nothing
    // typed: for each {name, icon}. Read again each time it is shown; none
    // while history is not kept.
    Q_PROPERTY(QVariantList frequentApps READ frequentApps NOTIFY frequentAppsChanged FINAL)
    // Files and folders, and content matches: best match first, or by when
    // last written (the footer's switch, Ctrl+S). 全部 keeps its apps,
    // places and web shortcuts first.
    Q_PROPERTY(Sort sort READ sort WRITE setSort NOTIFY sortChanged FINAL)
    // With nothing typed: what was opened last, or the files written last
    // (the footer's switch there, Ctrl+S).
    Q_PROPERTY(StartList startList READ startList WRITE setStartList NOTIFY startListChanged FINAL)
    // Rows say when each file or folder was last written.
    Q_PROPERTY(bool showModified READ showModified NOTIFY resultOptionsChanged FINAL)
    // The modifier that opens the nth row shown with a digit: "ctrl", "alt",
    // or "" for none (Settings::numberKeys).
    Q_PROPERTY(QString numberKeys READ numberKeys NOTIFY resultOptionsChanged FINAL)
    // Bits 1 << n of the digits n left to another program's global hotkey
    // (numberkeys::heldMask): their rows show the chord struck through.
    Q_PROPERTY(int numberKeysLeft READ numberKeysLeft NOTIFY resultOptionsChanged FINAL)
    // Opened over Win顺's search box on the taskbar (taskbar::SearchBox):
    // what is typed shows there, its own field is out of sight (SearchBar.qml
    // reports the field, the app draws it in the box).
    Q_PROPERTY(bool inTaskbarBox READ inTaskbarBox NOTIFY inTaskbarBoxChanged FINAL)
    // The row the keyboard is on (Main.qml's), -1 for none: content matches
    // coming in are placed by how well they match, but only below it.
    Q_PROPERTY(int currentRow READ currentRow WRITE setCurrentRow NOTIFY currentRowChanged FINAL)
    // The footer's key hints for the row the keyboard is on: what Enter
    // does ("打开", a plugin action's name; empty for a row Enter does
    // nothing on), what the second key does with its name ("Ctrl+Enter" /
    // "打开位置"; empty when the row has no location or second action),
    // and what Tab does ("切换范围", "补全"; empty when nothing).
    Q_PROPERTY(QVariantMap keyHints READ keyHints NOTIFY keyHintsChanged FINAL)
    // 文件 narrowed to a kind of file, as the strip under the search box
    // shows it: its index in fileTypes, or -1 for any (不限; 文件夹 is the
    // scope Folders). Kept while in another scope: 文件 comes back as it
    // was left, 文件夹 too (fileChoice, remembered between runs).
    Q_PROPERTY(int fileType READ fileType NOTIFY fileTypeChanged FINAL)
    // The kinds as the settings have them: [{name, extensions: "jpg png …"}].
    Q_PROPERTY(QVariantList fileTypes READ fileTypes NOTIFY fileTypesChanged FINAL)
    // Plugins (plugin::Host, the plugin design §4.1): a keyword typed first
    // ("kill note") gives the list to that command. The keyword as typed,
    // empty otherwise; the command's title and icon for the chip in the
    // scope tabs; its hint, shown after the keyword while nothing follows it.
    Q_PROPERTY(QString pluginKeyword READ pluginKeyword NOTIFY pluginChanged FINAL)
    Q_PROPERTY(QString pluginLabel READ pluginLabel NOTIFY pluginChanged FINAL)
    Q_PROPERTY(QString pluginIcon READ pluginIcon NOTIFY pluginChanged FINAL)
    Q_PROPERTY(QString pluginHint READ pluginHint NOTIFY pluginChanged FINAL)
    // In keyword mode, once the plugin takes a while: "正在等 结束进程…" (or
    // what it says it is doing); empty before that and otherwise.
    Q_PROPERTY(QString pluginWaiting READ pluginWaiting NOTIFY statusChanged FINAL)
    // Its user keeps favorites (Favorites): listed first with nothing typed.
    Q_PROPERTY(bool hasFavorites READ hasFavorites NOTIFY favoritesChanged FINAL)
    // 内容 in some folders, as the strip under the search box shows it (in
    // place of the kinds of 文件): [{type, name, tip, dot}], `type` the key
    // chooseFolderChip takes. With a folder named among the words ("报告
    // abc/"): every folder so named ("所有 abc · 3"), then each of them;
    // otherwise 不限, the favorite folders, the folder chosen. folderChip:
    // the current one's key. folderHint: a few words after them, how to
    // name one.
    Q_PROPERTY(QVariantList folderChips READ folderChips NOTIFY folderChipsChanged FINAL)
    Q_PROPERTY(QString folderChip READ folderChip NOTIFY folderChipsChanged FINAL)
    Q_PROPERTY(QString folderHint READ folderHint NOTIFY folderChipsChanged FINAL)
    // 内容: the words looked for, without the folders named among them, and
    // where it looks ("abc", "D:\Projects"; empty: everywhere), for the
    // empty state; whereMissing: that place is not in the index (or on a share).
    Q_PROPERTY(QString contentWords READ contentWords NOTIFY folderChipsChanged FINAL)
    Q_PROPERTY(QString contentWhere READ contentWhere NOTIFY folderChipsChanged FINAL)
    Q_PROPERTY(QString whereMissing READ whereMissing NOTIFY folderChipsChanged FINAL)

public:
    // As ws::Scope. 文件夹 is shown within 文件, as a chip of its strip
    // (chooseFileType): Tab goes 全部, 文件, 内容 (cycleScope), Shift+Tab
    // through the strip under the box (cycleFileType, cycleFolderChip).
    enum Scope { All, Files, Content, Folders };
    Q_ENUM(Scope)
    // Alternate: Shift+Enter, a plugin row's third action (Open elsewhere).
    // FavoriteFolder: the folder a file is in into the favorites.
    // EditFavorite: the settings' 收藏夹 page, at the row's favorite.
    // OpenTerminal: a command prompt in a folder, or in the folder a file is in.
    // PluginAction + n: the nth of a plugin row's actions (its context menu).
    enum Action {
        Open,
        Reveal,
        RunAsAdmin,
        CopyPath,
        CopyName,
        CopyItem,
        Recycle,
        ForgetRecent,
        EditWebShortcuts,
        Alternate,
        AddFavorite,
        RemoveFavorite,
        FavoriteFolder,
        EditFavorite,
        OpenTerminal,
        SearchInFolder, // 内容 in a folder's row (the strip shows it chosen)
        PluginAction = 100
    };
    Q_ENUM(Action)
    // As ws::RankBy and ws::StartList.
    enum Sort { BestMatch, NewestFirst, OldestFirst };
    Q_ENUM(Sort)
    enum StartList { RecentlyOpened, RecentlyModified };
    Q_ENUM(StartList)

    Launcher(IndexService* index, AppCatalog* apps, SystemCatalog* places, SearchEngine* engine, History* history,
        QObject* parent = nullptr);
    ~Launcher() override;

    // The plugins (may be none): every query goes to them too (全部).
    void setPluginHost(plugin::Host* host);
    QString pluginKeyword() const;
    QString pluginLabel() const;
    QString pluginIcon() const;
    QString pluginHint() const;
    QString pluginWaiting() const;
    // Esc in keyword mode: the words after the keyword go, the keyword
    // stays (true); nothing to clear: false, and Esc closes the window.
    Q_INVOKABLE bool clearToKeyword();
    // Tab: the row's autocomplete into the box, if it has one; outside
    // keyword mode else the hint row offering a keyword, wherever it is
    // ("uu" → "uuid "; completionRow()). False when there is nothing.
    Q_INVOKABLE bool autocomplete(int row);
    Q_INVOKABLE void leaveKeyword(); // the chip clicked: the box is emptied

    QString query() const { return m_query; }
    void setQuery(const QString& query);
    Scope scope() const { return m_scope; }
    void setScope(Scope scope);
    int currentRow() const { return m_currentRow; }
    void setCurrentRow(int row);
    QVariantMap keyHints() const { return m_keyHints; }
    ResultModel* results() { return &m_results; }
    QString statusText() const { return m_flash.isEmpty() ? m_status : m_flash; }
    bool busy() const;
    qreal progress() const;
    bool moreAvailable() const;
    // The list scrolled to its end: the same search again for a page more,
    // what it finds that is not listed yet added after the rows (none of
    // them moves).
    Q_INVOKABLE void loadMore();
    QVariantList leftOutHints() const;
    QString approximateHeading() const;
    // A hint taken up (its kind, as leftOutHints names it): that part
    // searched for this query, and what it holds put among the rows (content
    // matches where they rank; a drive's files after the rows, as found).
    // Not offered again for this query; nothing is written to the settings or
    // the index. stopLeftOut() ends the one running (the rows found stay).
    Q_INVOKABLE void searchLeftOut(const QString& kind);
    Q_INVOKABLE void stopLeftOut();
    // More results for the query are on the way. Not while quietly
    // refreshing results that were already complete, nor for a hint's
    // one-off search (searchLeftOut): the rows are the query's, and the hint
    // itself says what is going on (with 停止); the divider shows its progress.
    bool searching() const
    {
        return !m_quiet && m_hintRunning.isEmpty()
            && (m_pending || m_contentRunning || m_contentDebounce.isActive() || !m_pluginPending.isEmpty());
    }
    // The rows on screen are for the previous query; the new one has found
    // nothing yet (内容 takes a while).
    bool stale() const { return m_replacePending && m_results.count() > 0; }
    // A name search for the current query is still running (window reveal
    // waits for it, see App::showLauncher). Never for a plugin's answer.
    bool waitingForNames() const { return m_pending && !m_contentRunning; }
    QString placeholder() const;
    QString contentFilesLabel() const; // ".txt / .md / .log 等文件"
    // Whether the current query also looks inside files: always in 内容,
    // and in 全部 unless the query is name syntax or starts with a web
    // shortcut's keyword (see contentNeedle()).
    bool searchesContent() const { return m_scope == Content || m_withContent; }
    bool contentEnabled() const { return m_contentEnabled; }

    void setWindow(QWindow* window) { m_window = window; }
    // `enabled` false: no 内容, and 全部 finds names only; a launcher in 内容 goes to 全部.
    void setContentOptions(bool enabled, QStringList extensions, const ContentSizeLimits& sizeLimits,
        bool inLowPriority, bool documents);
    void setWebShortcuts(const WebShortcuts& shortcuts);
    // Its user's favorites (may be none): with every request, and the
    // rows refreshed when they change.
    void setFavorites(Favorites* favorites);
    bool hasFavorites() const;
    void setResultOptions(KindOrder kindOrder, bool showModified, bool thumbnails, const QString& numberKeys);
    Sort sort() const { return m_sort; }
    void setSort(Sort sort);
    StartList startList() const { return m_startList; }
    void setStartList(StartList list);
    // Ctrl+S: with nothing typed, the other start list; else the next order
    // (best match, newest, oldest).
    Q_INVOKABLE void switchOrder();
    bool showModified() const { return m_showModified; }
    QString numberKeys() const { return m_numberKeys; }
    int numberKeysLeft() const { return static_cast<int>(m_numberKeysLeft); }
    void setNumberKeysLeft(unsigned digits);
    bool inTaskbarBox() const { return m_inTaskbarBox; }
    void setInTaskbarBox(bool on);
    // The search field changed (text, caret, selection, composition): fieldReported.
    Q_INVOKABLE void reportField(
        const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition);
    // Ctrl+n (or Alt+n): the nth row shown opens (numberPressed), once the
    // rows on screen are the query's (holdUntilShown).
    Q_INVOKABLE void pressNumber(int number);
    // Enter or a number pressed while the rows on screen are still the
    // previous query's (it was typed a moment ago): true, and heldKey()
    // brings it back once the new rows are shown. Typing on drops it.
    Q_INVOKABLE bool holdUntilShown(int action, int number);
    // Off: nothing opened is remembered, and what was is not shown (it stays
    // until cleared in the settings).
    void setRecordHistory(bool on);
    bool canClearHistory() const;
    Q_INVOKABLE void clearHistory();
    QVariantList frequentApps() const;
    Q_INVOKABLE void openFrequentApp(int index);
    void handleShown();
    void handleHidden();
    void retranslate(); // the language changed: status, placeholder, rows
    void flash(const QString& message); // a few words in the footer for a moment ("已复制")

    // An action on a row; on a row of a selection of several, on all of it.
    Q_INVOKABLE void trigger(int row, int action);
    Q_INVOKABLE void triggerSelection(int action); // the keyboard's, with rows selected
    // The row's file or folder dragged out of the window (into a chat, a
    // folder); the selection when the row is in it. Returns when it was let
    // go (see dragout::drag). Apps, places, web pages and plugins' rows stay.
    Q_INVOKABLE void dragOut(int row);
    // Selecting several rows: Ctrl toggles one, Shift takes the rows from an
    // anchor to here (instead of the selection, or with Ctrl added to it).
    Q_INVOKABLE void toggleSelected(int row);
    Q_INVOKABLE void selectRange(int from, int to, bool add);
    Q_INVOKABLE void clearSelection();
    // Context menu (ContextMenu.qml): its entries for a row, the usable
    // screen area around a point, and the native styling of its window.
    Q_INVOKABLE QVariantList menuItems(int row) const;
    Q_INVOKABLE QRectF screenArea(QPointF globalPos) const;
    Q_INVOKABLE void prepareMenuWindow(QWindow* menu) const;
    Q_INVOKABLE void cycleScope(int delta);
    int fileType() const { return m_fileType; }
    QVariantList fileTypes() const;
    void setFileTypes(const FileTypes& types); // named (Settings::namedFileTypes)
    // A chip of the strip: kFoldersChip (文件夹), -1 (不限), or a kind
    // (fileTypes); in 文件 (or 文件夹) from any scope.
    static constexpr int kFoldersChip = -2;
    Q_INVOKABLE void chooseFileType(int type);
    // Shift+Tab in 文件: the next chip, round (不限, 文件夹, then the kinds).
    Q_INVOKABLE void cycleFileType(int delta);
    Q_INVOKABLE void editFileTypes(); // the strip's +: fileTypeSettingsRequested
    // The strip's choice, to keep between runs: "folders", a kind's id
    // ("pictures") or "name:<its name>" for one of the user's own, or empty
    // (不限). Set after setFileTypes; in 文件 it applies at once.
    QString fileChoice() const;
    void setFileChoice(const QString& choice);
    QVariantList folderChips() const;
    QString folderChip() const;
    QString folderHint() const;
    QString contentWords() const;
    QString contentWhere() const;
    QString whereMissing() const;
    // A chip of the strip in 内容 (folderChips): searched there.
    Q_INVOKABLE void chooseFolderChip(const QString& key);
    // Shift+Tab in 内容: the next folder chip, round; nothing with none to go to.
    Q_INVOKABLE void cycleFolderChip(int delta);
    Q_INVOKABLE void editFolderChips(); // the strip's +: the favorites, where the folders come from
    // Esc in 内容 with a folder chosen: the words go, the folder stays
    // (true); nothing to clear: false, and Esc closes the window.
    Q_INVOKABLE bool clearContentWords();
    Q_INVOKABLE void dismiss();

signals:
    void queryChanged();
    void scopeChanged();
    void statusChanged();
    void shown();
    void resultsReplaced(); // results for a new query: select the first row
    void namesShown(); // a name search finished and its rows are in the model
    void dismissRequested();
    // Not started: it would have needed administrator rights to start at all
    // (see shell::open). The launcher is closed by then.
    void openFailed(const QString& name);
    void contextMenuKeyPressed(); // Menu key / Shift+F10 (arrive as a context-menu event, not a key)
    void historyChanged();
    void frequentAppsChanged();
    void webSettingsRequested(); // the settings window, on its 网页搜索 page
    void fileTypeSettingsRequested(); // ... on its 结果列表 page, at the file types
    void fileTypeChanged();
    void fileTypesChanged();
    void fileChoiceChanged();
    void favoriteSettingsRequested(const QString& path); // ... on its 收藏夹 page, at this favorite
    void favoritesChanged();
    // The settings window, on the 插件 page at this plugin's card: a plugin
    // row opening "winshun:settings" (its own card only; performPlugin).
    void pluginSettingsRequested(const QString& pluginId);
    void currentRowChanged();
    void keyHintsChanged();
    void sortChanged();
    void startListChanged();
    void resultOptionsChanged();
    void inTaskbarBoxChanged();
    void fieldReported(const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition);
    // Pressed in the box on the taskbar: the field's caret goes there, or
    // its word is selected; dragged there, the text from `anchor` is selected.
    void caretRequested(int position, bool word);
    void selectionRequested(int anchor, int position);
    void numberPressed(int number); // 1…9: open that row, counted from the first one shown
    void heldKey(int action, int number); // see holdUntilShown(); `number` 0: the action on the current row
    // A content match (or a plugin's row) better than the first row came in
    // above it while the keyboard was there: the first row is current again
    // (the list moved it down).
    void betterFirstRow();
    void pluginChanged();
    void folderChipsChanged();

private:
    class ModelSink; // the result model as plugin::Merge sees it
    // Keyword mode: the command the keyword opens, and what it shows.
    struct KeywordMode {
        QString pluginId;
        QString commandId;
        QString keyword; // as typed
        QString words; // after it
        QString label; // the command's title
        QString hint;
        QString icon; // an image URL
    };

    void search(bool more = false); // more: a page more (loadMore)
    // The command whose keyword opens the text ("kill note"), if any: what
    // keyword mode shows for it. No side effects (search() asks before it
    // decides whether the rows are new ones).
    std::optional<KeywordMode> matchKeyword(const QString& text) const;
    // Part of search(): what the text triggers; `keyword` as matchKeyword() found it.
    void startPluginQuery(const QString& text, std::optional<KeywordMode> keyword);
    void refreshKeyHints(); // keyHints for the current row; keyHintsChanged when they differ
    const SearchResult* completionRow(int row) const; // what Tab completes from `row`, or null (see autocomplete)
    void onPluginTaken(std::uint64_t queryId, const QString& pluginId);
    void onPluginResults(std::uint64_t queryId, const QString& pluginId, const QString& commandId,
        const QList<ws::plugin::Item>& items, bool partial, bool replace);
    void onPluginFinished(std::uint64_t queryId, const QString& pluginId);
    void onPluginFailed(std::uint64_t queryId, const QString& pluginId, const QString& why);
    void onPluginProgress(std::uint64_t queryId, const QString& pluginId, const QString& text);
    void onPluginActionDone(std::uint64_t requestId, const QList<ws::plugin::Effect>& effects, bool ok, const QString& error);
    void pluginSettled(const QString& pluginId); // answered, or failed
    QString pluginName(const QString& pluginId) const;
    void performPlugin(const SearchResult& result, Action action);
    void applyEffects(const QList<ws::plugin::Effect>& effects);
    void showPluginRows(SearchResults rows); // keyword mode: the whole list (showRows)
    RankBy rankBy() const; // as the current scope has it
    bool showsStartList() const; // nothing typed, and a list for it
    QString contentNeedle() const;
    QString shownExtensions() const;
    void startContentSearch();
    void showRows(SearchResults rows, QStringList highlights);
    void perform(const SearchResult& result, Action action);
    void performApp(const SearchResult& app, Action action);
    void performPlace(const SearchResult& place, Action action);
    void performWeb(const SearchResult& web, Action action);
    void performFavorite(const SearchResult& result, Action action); // AddFavorite … EditFavorite
    void performMany(const SearchResults& items, Action action);
    // A favorite folder's row, outside keyword mode: Tab goes into it (or null).
    const SearchResult* enterableFolder(int row) const;
    void remember(const QString& path); // opened: into the history, if it is kept
    void forgetRecent(const QStringList& paths);
    bool forgetRecycled(const SearchResult& result); // its row and count; false if it is still there
    std::function<void(bool)> reportFailure(const QString& name);
    void onRecycled(const SearchResult& result, bool ok);
    void onRecycledMany(const SearchResults& items, bool ok);
    void onResults(quint64 id, const SearchResults& results, qint64 total, qint64 elapsedUs);
    void onContentResults(quint64 id, const SearchResults& results);
    void onContentProgress(quint64 id, int scanned, int total, bool finished);
    void onContentLeftOut(quint64 id, const ContentLeftOut& leftOut);
    void onContentScope(quint64 id, const ContentScope& scope);
    // A content request where the strip in 内容 says to look (m_contentFolder, m_contentPick).
    void setContentPlace(SearchEngine::Request& request) const;
    // The scan's report on the folders typed, if it is for the text there is (else none yet).
    const ContentScope::Place* typedPlace() const;
    static QString folderLabel(const QString& path); // its last names, "项目\abc"
    static QString pickKey(const SearchEngine::Request::ContentPick& pick); // for m_shownPick; empty: none
    void onRemovableDrives(quint64 id, const RemovableDrives& drives);
    void onDriveResults(quint64 id, const SearchResults& results);
    void onDriveProgress(quint64 id, int seen, bool finished);
    // Whether the search behind the rows is done (a quiet refresh keeps them
    // as they are, so it counts as done): only then is what it passed over
    // known, and the hints shown.
    bool hintsSettled() const;
    void finishLeftOut(bool stopped); // the one running is over: the hint goes, the footer says what came of it
    void refreshStatus();
    void refreshFrequentApps();

    struct FrequentApp {
        QString name;
        QString path; // launch path
        QString icon;
        bool operator==(const FrequentApp&) const = default;
    };

    IndexService* m_index;
    AppCatalog* m_apps;
    SystemCatalog* m_places;
    SearchEngine* m_engine;
    History* m_history;
    ResultModel m_results;
    QPointer<QWindow> m_window;

    QString m_query;
    Scope m_scope = All;
    quint64 m_requestId = 0;
    // The last name search's: its report of the removable drives
    // (onRemovableDrives) may arrive after 全部 has gone on to its content
    // scan, under a new m_requestId.
    quint64 m_nameRequestId = 0;
    bool m_pending = false;
    bool m_haveResults = false;
    // What the rows on screen are for. Searching it again (window reopened,
    // index updated) refreshes them in place instead of starting over.
    QString m_shownText;
    Scope m_shownScope = All;
    RankBy m_shownRankBy = RankBy::Match;
    StartList m_shownStartList = RecentlyOpened;
    KindOrder m_shownKindOrder = KindOrder::FilesFirst;
    QStringList m_shownExtensions; // of the kind of file chosen (fileExtensions())
    // The command whose list the rows are ("<plugin>/<command>"; empty: the
    // name search's): "=" and "= " trim to the same text, but one lists
    // files, the other the calculator's answer, and that is a new list.
    QString m_shownKeyword;
    bool m_refresh = false; // the current search is such a refresh
    bool m_quiet = false; // ...of complete results: it shows no progress
    bool m_scanComplete = false; // the content scan behind the rows on screen ran to the end
    bool m_replacePending = false; // the current search is for a new query and has shown nothing yet
    // Refresh: content matches kept from the last scan that the running one
    // has not found again yet. Removed if it finishes without them.
    QSet<QString> m_unconfirmed;
    qint64 m_totalMatches = 0;
    qint64 m_elapsedUs = 0;
    bool m_contentRunning = false;
    bool m_withContent = false; // 全部: content matches follow the name matches
    int m_contentScanned = 0;
    int m_contentTotal = 0;
    int m_contentHits = 0; // 全部: content rows added below the name matches
    // Pages of rows asked for (kPage each): back to 1 for a new list.
    int m_pages = 1;
    bool m_loadingMore = false; // the current search is loadMore()'s: its new rows go at the end
    bool m_moreNames = false; // the last name search (or files written last) filled its page
    bool m_moreContent = false; // the last content scan stopped at its limit
    // What the search behind the rows passed over (leftOutHints): the content
    // scan's report (SearchEngine::contentLeftOut), and the removable drives
    // the index leaves out (SearchEngine::removableDrives). A hint taken up
    // (searchLeftOut) is the one running (m_hintRunning, its kind) and then
    // not offered again for this query (m_hintsTaken); the rows a one-off
    // brought (m_leftOutFound) stay through refreshes, which cannot find them
    // again (the limits, the settings, or the index leave them out). Back to
    // nothing with a new query.
    ContentLeftOut m_leftOut;
    RemovableDrives m_drives;
    QString m_hintRunning;
    QSet<QString> m_hintsTaken;
    QSet<QString> m_leftOutFound;
    int m_hintHits = 0; // rows the one running has brought
    bool m_walkRunning = false; // ... it is a walk of a drive (SearchEngine::driveProgress ends it)
    int m_walkSeen = 0; // files and folders it has looked at
    bool m_approximate = false; // the rows were found by reading (approximateHeading)
    int m_currentRow = -1;
    QVariantMap m_keyHints; // see keyHints
    FileTypes m_fileTypes;
    int m_fileType = -1; // see fileType
    bool m_foldersChosen = false; // 文件夹 was the strip's last choice (fileChoice)
    static QString fileTypeKey(const FileType& type); // as fileChoice names it
    int fileTypeIndex(const QString& key) const; // -1: none
    QStringList fileExtensions() const; // the kind chosen's, in 文件; none otherwise
    // 内容 in a folder chosen (a chip, a folder's row: SearchInFolder), as a
    // full path; empty: everywhere. Kept while the app runs, as the kind of
    // file is. A folder named among the words goes first (ContentFolderSpec).
    QString m_contentFolder;
    // One of the folders a name typed stands for, Tab along them ("abc/":
    // 所有 abc, D:\工作\abc, E:\备份\abc).
    SearchEngine::Request::ContentPick m_contentPick;
    ContentScope m_contentScope; // as the last content scan made it out (onContentScope)
    QString m_shownContentFolder; // what the rows are for, as m_shownText
    QString m_shownPick;

    bool m_recordHistory = true;
    KindOrder m_kindOrder = KindOrder::FilesFirst;
    Sort m_sort = BestMatch;
    StartList m_startList = RecentlyOpened;
    bool m_showModified = true;
    QString m_numberKeys = QStringLiteral("ctrl");
    unsigned m_numberKeysLeft = 0;
    bool m_inTaskbarBox = false;
    std::optional<std::pair<int, int>> m_held; // holdUntilShown(): action, number
    std::vector<FrequentApp> m_frequent;
    std::vector<AppUse> m_windowsUses; // Windows' record of what was started, read at most every minute
    QElapsedTimer m_windowsUsesAge;
    QStringList m_contentExtensions {QStringLiteral("txt")};
    ContentSizeLimits m_contentSizeLimits;
    bool m_contentInLowPriority = false; // 内容 also looks in system, program and tool folders
    WebShortcuts m_web;
    QPointer<Favorites> m_favorites;
    bool m_contentDocuments = true; // and in Word, Excel, PowerPoint and PDF files
    bool m_contentEnabled = true;

    QString m_status;
    QString m_flash;
    QTimer m_contentDebounce;
    bool m_contentAtOnce = false; // search(): the text is the one shown, no pause in typing to wait for
    QTimer m_flashTimer;
    QTimer m_statusPoll;
    // A new query's rows a while in coming: the status says so, not the last
    // query's figures under its dimmed rows.
    QTimer m_slowSearch;
    bool m_searchSlow = false;

    // Plugins. The query ids are the launcher's own (keyword mode has no
    // engine request); the merge places their rows (plugin::Merge).
    QPointer<plugin::Host> m_plugins;
    std::unique_ptr<ModelSink> m_sink;
    std::unique_ptr<plugin::Merge> m_merge;
    std::uint64_t m_pluginQueryId = 0;
    std::optional<KeywordMode> m_keyword;
    QSet<QString> m_pluginPending; // answering the current query
    QElapsedTimer m_pluginTimer; // since they were asked
    qint64 m_pluginElapsedUs = 0; // keyword mode: until the answer
    QTimer m_pluginWait; // 150 ms: only then does the status say who is being waited for
    bool m_pluginSlow = false;
    QString m_pluginProgress; // winshun/progress: what the keyword's plugin says it is doing
    QString m_pluginError; // "X 没有响应": shown for a few seconds
    QTimer m_pluginErrorTimer;
    std::uint64_t m_pluginRequest = 0; // actions sent
};

} // namespace ws
