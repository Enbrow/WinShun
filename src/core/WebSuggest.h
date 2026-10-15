#pragma once

#include <QByteArrayView>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

// Search suggestions for a web shortcut (WebShortcut::suggest): what its
// site's suggestion address answers for the words typed after the keyword
// ("baidu 显卡" asks 百度 for "显卡"), read here; asked by the app
// (WebSuggester), shown as rows under the search's own.
namespace ws::websuggest {

constexpr int kMaxSuggestions = 5;

// The text of a reply: in the charset its Content-Type names (百度's is
// GBK), else UTF-8 when the bytes are that, else GB18030 (GBK's superset:
// a Chinese site that names none).
QString decodeReply(QByteArrayView body, QStringView contentType);

// The suggestions in a reply's text, best first, at most `max`, each once
// (ignoring case), without `typed` itself (the row above searches that
// already). Read, in this order:
// - a JSONP wrapper ("window.baidu.sug({...});") taken off; JavaScript's
//   unquoted keys and 'strings' read as JSON's ({q:"x",s:["a"]});
// - OpenSearch, ["typed", ["s1", "s2", …], …] (必应, Google, 百度's
//   action=opensearch);
// - else the first array, depth first (an object's keys in alphabetical
//   order: QJsonObject keeps no other), of strings, of arrays starting with
//   one (淘宝's [["显卡支架","100"], …]), or of objects with one under value,
//   q, word, term, keyword, suggestion, query, text or name (哔哩哔哩's
//   result.tag[].value, 百度's sugrec g[].q).
// Empty for anything else: no rows, nothing said.
QStringList parse(QStringView text, const QString& typed, int max = kMaxSuggestions);

// Replies kept a short while, by suggestion address and words: typing back
// over a word asks again for nothing. An address changed in the settings is
// another key, so its first answer is never an old one's; the app clears it
// all the same whenever the shortcuts change. Only answers are kept: a
// failure (no network, a timeout) is asked again next time.
class Cache {
public:
    static constexpr qint64 kKeepMs = 2 * 60 * 1000;
    static constexpr int kMaxEntries = 256;

    std::optional<QStringList> find(const QString& endpoint, const QString& words, qint64 nowMs) const;
    void insert(const QString& endpoint, const QString& words, const QStringList& suggestions, qint64 nowMs);
    void clear() { m_entries.clear(); }
    qsizetype size() const { return m_entries.size(); }

private:
    struct Entry {
        QStringList suggestions;
        qint64 at = 0;
    };
    static QString key(const QString& endpoint, const QString& words) { return endpoint + u'\n' + words; }
    QHash<QString, Entry> m_entries;
};

} // namespace ws::websuggest
