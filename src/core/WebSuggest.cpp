#include "WebSuggest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringDecoder>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws::websuggest {

namespace {

// Through Windows rather than QStringDecoder: Qt without ICU (as Win顺 is
// built) knows no GBK.
QString fromCodePage(QByteArrayView bytes, UINT codePage)
{
    if (bytes.isEmpty())
        return {};
    const int size = static_cast<int>(bytes.size());
    const int length = ::MultiByteToWideChar(codePage, 0, bytes.data(), size, nullptr, 0);
    if (length <= 0)
        return {};
    QString text(length, Qt::Uninitialized);
    ::MultiByteToWideChar(codePage, 0, bytes.data(), size, reinterpret_cast<wchar_t*>(text.data()), length);
    return text;
}

// JavaScript's object literal as JSON: unquoted keys quoted ({q:"x"} ->
// {"q":"x"}), 'strings' in double quotes. What is inside strings stays.
QString relaxedJson(QStringView text)
{
    QString out;
    out.reserve(text.size() + 16);
    QChar quote; // the string being read, if any
    QChar lastOutside; // the last character outside strings that is not a space
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (!quote.isNull()) {
            if (c == u'\\' && i + 1 < text.size()) {
                const QChar next = text[++i];
                if (next != u'\'' || quote != u'\'')
                    out += c; // \' needs no escape in "…"
                out += next;
            } else if (c == quote) {
                out += u'"';
                quote = {};
            } else if (c == u'"') {
                out += u"\\\""_s; // a " inside a 'string'
            } else {
                out += c;
            }
            continue;
        }
        if (c == u'"' || c == u'\'') {
            quote = c;
            out += u'"';
            lastOutside = c;
            continue;
        }
        const bool keyStart = c.isLetter() || c == u'_' || c == u'$';
        if (keyStart && (lastOutside == u'{' || lastOutside == u',')) {
            qsizetype end = i;
            while (end < text.size() && (text[end].isLetterOrNumber() || text[end] == u'_' || text[end] == u'$'))
                ++end;
            qsizetype colon = end;
            while (colon < text.size() && text[colon].isSpace())
                ++colon;
            if (colon < text.size() && text[colon] == u':') {
                out += u'"' + text.sliced(i, end - i).toString() + u'"';
                i = end - 1;
                lastOutside = u'"';
                continue;
            }
        }
        out += c;
        if (!c.isSpace())
            lastOutside = c;
    }
    return out;
}

// One entry of a list of suggestions as its text, or empty.
QString itemText(const QJsonValue& item)
{
    if (item.isString())
        return item.toString();
    if (item.isArray()) {
        const QJsonArray array = item.toArray();
        return !array.isEmpty() && array.first().isString() ? array.first().toString() : QString();
    }
    if (item.isObject()) {
        const QJsonObject object = item.toObject();
        for (const QString key : {u"value"_s, u"q"_s, u"word"_s, u"term"_s, u"keyword"_s, u"suggestion"_s, u"query"_s,
                 u"text"_s, u"name"_s}) {
            if (const QJsonValue v = object.value(key); v.isString())
                return v.toString();
        }
    }
    return {};
}

QStringList textsOf(const QJsonArray& array)
{
    QStringList texts;
    for (const QJsonValue& item : array) {
        if (const QString t = itemText(item); !t.isEmpty())
            texts.append(t);
    }
    return texts;
}

// The first array, depth first, with any suggestion in it.
QStringList firstList(const QJsonValue& value, int depth = 0)
{
    if (depth > 8)
        return {};
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        if (QStringList texts = textsOf(array); !texts.isEmpty())
            return texts;
        for (const QJsonValue& item : array) {
            if (QStringList texts = firstList(item, depth + 1); !texts.isEmpty())
                return texts;
        }
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (QStringList texts = firstList(it.value(), depth + 1); !texts.isEmpty())
                return texts;
        }
    }
    return {};
}

// One line, no markup (哔哩哔哩's names carry <em>), spaces as one.
QString tidied(QString text)
{
    static const QRegularExpression tag(u"<[^>]{0,64}>"_s);
    text.remove(tag);
    return text.simplified();
}

} // namespace

QString decodeReply(QByteArrayView body, QStringView contentType)
{
    if (body.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(body.sliced(3));
    static const QRegularExpression charsetPattern(u"charset\\s*=\\s*\"?([A-Za-z0-9_.:-]+)"_s,
        QRegularExpression::CaseInsensitiveOption);
    const QString charset = charsetPattern.match(contentType).captured(1).toLower();
    if (charset.startsWith(u"gb") || charset == u"cp936" || charset == u"x-gbk")
        return fromCodePage(body, 54936); // GB18030: GBK and GB2312 are within it
    if (charset == u"big5")
        return fromCodePage(body, 950);
    if (charset == u"utf-8" || charset == u"utf8")
        return QString::fromUtf8(body);
    QStringDecoder utf8(QStringConverter::Utf8);
    const QString text = utf8(body);
    if (!utf8.hasError())
        return text;
    return fromCodePage(body, 54936);
}

QStringList parse(QStringView text, const QString& typed, int max)
{
    QStringView inner = text.trimmed();
    // A JSONP wrapper: "name(" … ")" and maybe ";".
    if (!inner.isEmpty() && inner.front() != u'[' && inner.front() != u'{') {
        const qsizetype open = inner.indexOf(u'(');
        const qsizetype close = inner.lastIndexOf(u')');
        if (open <= 0 || close <= open)
            return {};
        static const QRegularExpression callee(u"^[A-Za-z_$][\\w$.]*\\s*$"_s);
        if (!callee.matchView(inner.first(open)).hasMatch())
            return {};
        inner = inner.sliced(open + 1, close - open - 1).trimmed();
    }
    QJsonParseError error;
    QJsonDocument document = QJsonDocument::fromJson(inner.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError)
        document = QJsonDocument::fromJson(relaxedJson(inner).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError)
        return {};

    QStringList found;
    const QJsonArray root = document.array();
    if (document.isArray() && root.size() >= 2 && root[0].isString() && root[1].isArray())
        found = textsOf(root[1].toArray()); // OpenSearch
    else
        found = firstList(document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object()));

    const QString typedTidied = tidied(typed);
    QStringList out;
    for (const QString& s : std::as_const(found)) {
        const QString t = tidied(s);
        if (t.isEmpty() || t.size() > 200 || t.compare(typedTidied, Qt::CaseInsensitive) == 0
            || out.contains(t, Qt::CaseInsensitive))
            continue;
        out.append(t);
        if (out.size() >= max)
            break;
    }
    return out;
}

std::optional<QStringList> Cache::find(const QString& endpoint, const QString& words, qint64 nowMs) const
{
    const auto it = m_entries.constFind(key(endpoint, words));
    if (it == m_entries.cend() || nowMs - it->at > kKeepMs || nowMs < it->at)
        return std::nullopt;
    return it->suggestions;
}

void Cache::insert(const QString& endpoint, const QString& words, const QStringList& suggestions, qint64 nowMs)
{
    if (m_entries.size() >= kMaxEntries) {
        m_entries.removeIf([nowMs](QHash<QString, Entry>::iterator it) { return nowMs - it->at > kKeepMs; });
        while (m_entries.size() >= kMaxEntries) { // still full: the oldest go
            auto oldest = m_entries.begin();
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
                if (it->at < oldest->at)
                    oldest = it;
            }
            m_entries.erase(oldest);
        }
    }
    m_entries.insert(key(endpoint, words), {suggestions, nowMs});
}

} // namespace ws::websuggest
