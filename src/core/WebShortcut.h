#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <functional>
#include <optional>
#include <vector>

namespace ws {

class NameMatcher;

// A web page opened by a keyword typed in the launcher's 全部: "gh" opens
// GitHub, "gh WinShun" searches GitHub for WinShun. Typing its name
// ("github", or "baidu" for 百度) finds it among the results too.
struct WebShortcut {
    QString keyword; // "gh": one word, matched ignoring case
    QString name; // "GitHub"
    // "https://github.com/search?q=%s": %s is where the words go. Without
    // %s, the page itself, and the keyword takes no words.
    QString url;
    QString home; // what the keyword alone opens; empty: see homeUrl()
    // A picture its user chose ("3f2a….png", see isValidCustomIconName),
    // kept by Win顺; empty: its site's own icon.
    QString icon;

    bool searches() const { return url.contains(u"%s"); }
    // What its rows' icon is asked for by (FileIconProvider::webIconUrl):
    // "custom:<icon>" for a picture of the user's, else the home page.
    QString iconRef() const;
    // `home`, else for a search its site ("https://github.com/"), or for a
    // link of an app's own scheme ("obsidian://search?query=%s") the link
    // with no words; for a page, the page.
    QString homeUrl() const;
    // `url` with %s replaced by the words, percent-encoded as UTF-8.
    QString searchUrl(const QString& words) const;

    friend bool operator==(const WebShortcut&, const WebShortcut&) = default;
};

using WebShortcuts = QList<WebShortcut>;

// What Win顺 comes with: the sites most used on the Chinese web ("baidu 天气"
// searches 百度), and "winshun" for its page on GitHub. The rest are the
// user's own.
WebShortcuts defaultWebShortcuts();
// Raised when defaults are added: an older settings file gets the new ones
// once (Settings, WebSearch/Defaults), those its user removed stay removed.
constexpr int kWebDefaultsVersion = 2;
// `shortcuts` with the defaults it lacks after them: those whose keyword is
// not taken, nor their site (a user's own "bd" for 百度 keeps 百度 once).
WebShortcuts withNewDefaults(WebShortcuts shortcuts, const WebShortcuts& defaults);

// "winshun-web:<keyword>": stands for a shortcut's row (SearchResult::path).
QString webPath(const QString& keyword, const QString& words = {});
bool isWebPath(QStringView path) noexcept;

struct TypedKeyword {
    qsizetype index = -1; // into the shortcuts
    QString words; // what follows the keyword, trimmed; empty: the keyword alone
};

// The shortcut whose keyword the text starts with, alone or followed by a
// space and the words to search for ("gh", "gh  WinShun"). A shortcut that
// does not search takes no words: "page x" is an ordinary query then.
std::optional<TypedKeyword> typedKeyword(const WebShortcuts& shortcuts, QStringView text);

struct WebHit {
    qsizetype index = 0; // into the shortcuts
    int score = 0; // as an app's (higher is better)
};

// The shortcuts whose name matches, best first (by pinyin too: "bd" for 百度).
std::vector<WebHit> searchWebShortcuts(const WebShortcuts& shortcuts, const NameMatcher& matcher);

// What a typed address becomes, or empty if it is not one Win顺 opens:
// trimmed; "github.com/search?q=%s" gets https://; a search for "%s" copied
// from the address bar ("q=%25s") gets its %s back. Any scheme of two
// letters or more but file: (a word typed into a file path could start a
// program); http and https need a host.
QString normalizeWebUrl(const QString& text);

// A keyword one can type: not empty, no spaces or quotes.
bool isValidWebKeyword(QStringView keyword) noexcept;

// An address as a row shows it: no https:// and www., %-escapes decoded
// ("github.com/search?q=Win Shun"); another scheme stays.
QString displayWebUrl(const QString& url);

// ---- import and export ---------------------------------------------------
// A file of web shortcuts, to take them to another computer or share them:
// JSON, {"format": "winshun-web-shortcuts", "version": 1, "shortcuts": [{
// "keyword", "name", "url", "home"?, "icon"? }]}, an icon as a PNG in a
// data: address, so the file is all one needs.

// A custom icon's name as Win顺 keeps it: 16 hex digits of its PNG's
// SHA-1 and ".png". Anything else (a path, "..") is not one.
bool isValidCustomIconName(QStringView name) noexcept;

struct WebShortcutFileEntry {
    WebShortcut shortcut; // `icon` empty
    QByteArray iconData; // the picture the file holds for it, still to be decoded; empty: none
};

// `iconPng(name)` gives the PNG of a custom icon, empty when it is gone.
QByteArray writeWebShortcutsFile(const WebShortcuts& shortcuts,
    const std::function<QByteArray(const QString& icon)>& iconPng);

struct WebShortcutFile {
    QList<WebShortcutFileEntry> entries;
    int skipped = 0; // entries with no keyword one can type or no address Win顺 opens
};
// Read as the settings file is (readWebShortcuts): addresses tidied, a
// missing name the keyword's. The later of two with one keyword wins. Null
// for a file that is not one of these, `newer` set when it is one a newer
// Win顺 wrote.
std::optional<WebShortcutFile> readWebShortcutsFile(QByteArrayView json, bool* newer = nullptr);

struct WebMerge {
    int added = 0;
    int updated = 0; // a keyword already there, with another address, name or icon
    int unchanged = 0;
};
// `from` into `into`: a keyword already there (ignoring case) is replaced in
// its place, a new one goes at the end.
WebMerge mergeWebShortcuts(WebShortcuts& into, const WebShortcuts& from);

// ---- site icons ----------------------------------------------------------
// A shortcut's row shows its site's own icon, fetched from the site the first
// time (nothing of other sites ships with Win顺) and kept on disk.

// What a site's icon is kept under: the host of the shortcut's home page,
// lower case ("fanyi.baidu.com", not "baidu.com": 百度翻译 has an icon of its
// own); empty for an address that is not http(s).
QString siteIconKey(const QString& homeUrl);

// The icons a page names, as absolute addresses. `icons` are what a browser
// shows beside the address: <link rel="icon"> with sizes of 64 or more
// first (largest first), then the other icons as the page has them, and last
// /favicon.ico of the page's site, which nearly every site has. `touchIcons`
// (apple-touch-icon, made for phones' home screens: 120-512 px, largest
// first) are sharper but often another picture (bilibili's says "WEB"): only
// for a site whose browser icon is too small. SVG is left out: Win顺 does
// not decode it. `pageUrl` is where the page was read from, after redirects.
struct SiteIconCandidates {
    QStringList icons;
    QStringList touchIcons;
};
SiteIconCandidates siteIconCandidates(QByteArrayView html, const QString& pageUrl);

} // namespace ws
