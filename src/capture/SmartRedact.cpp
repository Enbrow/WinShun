#include "SmartRedact.h"

#include <QDate>
#include <QRegion>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

// ---- the text the rules look at ----

// One UTF-16 unit for one, so that an index in the folded text is the same
// index in the line's text (the characters' places go by it).
QChar widthFolded(QChar c)
{
    const char16_t u = c.unicode();
    if (u >= 0xFF01 && u <= 0xFF5E)
        return QChar(char16_t(u - 0xFEE0)); // full-width letters, digits, signs
    switch (u) {
    case 0x3000:
        return u' ';
    case 0x3002:
        return u'.';
    case 0x2013:
    case 0x2014:
    case 0x2212:
        return u'-';
    case 0x201C:
    case 0x201D:
        return u'"';
    case 0x2018:
    case 0x2019:
        return u'\'';
    default:
        return c;
    }
}

QString foldedPlain(const QString& text)
{
    QString out = text;
    for (QChar& c : out)
        c = widthFolded(c);
    return out;
}

// Look-alikes read as the digit they are taken for, for the number rules only
// (a word keeps its letters).
QString foldedDigits(const QString& plain)
{
    QString out = plain;
    for (QChar& c : out) {
        switch (c.unicode()) {
        case u'O':
        case u'o':
            c = u'0';
            break;
        case u'l':
        case u'I':
        case u'|':
            c = u'1';
            break;
        default:
            break;
        }
    }
    return out;
}

bool asciiDigit(QChar c)
{
    return c.unicode() >= u'0' && c.unicode() <= u'9';
}

bool asciiAlnum(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z');
}

// No digit right before or after [from, to).
bool apartFromDigits(const QString& plain, int from, int to)
{
    return !(from > 0 && asciiDigit(plain[from - 1])) && !(to < plain.size() && asciiDigit(plain[to]));
}

QString digitsOnly(QStringView text)
{
    QString out;
    for (QChar c : text) {
        if (asciiDigit(c) || c == u'X' || c == u'x')
            out += c;
    }
    return out;
}

bool plausibleDate(int year, int month, int day)
{
    return QDate::isValid(year, month, day) && year >= 1900 && year <= QDate::currentDate().year();
}

// The region, the birth date and the shape of an 18- or 15-digit number,
// without the check digit.
bool idShaped(QStringView d)
{
    if (d.size() != 18 && d.size() != 15)
        return false;
    if (d[0] == u'0')
        return false;
    for (int i = 0; i < d.size() - (d.size() == 18 ? 1 : 0); ++i) {
        if (!asciiDigit(d[i]))
            return false;
    }
    const int year = d.size() == 18 ? d.mid(6, 4).toInt() : 1900 + d.mid(6, 2).toInt();
    const int at = d.size() == 18 ? 10 : 8;
    return plausibleDate(year, d.mid(at, 2).toInt(), d.mid(at + 2, 2).toInt());
}

// Text in a key's value that stands for a value rather than being one.
bool placeholder(const QString& value)
{
    QString v = value.trimmed();
    while (!v.isEmpty() && (v.front() == u'"' || v.front() == u'\'' || v.front() == u'`'))
        v.remove(0, 1);
    while (!v.isEmpty() && (v.back() == u'"' || v.back() == u'\'' || v.back() == u'`' || v.back() == u',' || v.back() == u';'))
        v.chop(1);
    if (v.isEmpty())
        return true;
    if (v.front() == u'$' || v.front() == u'<' || v.front() == u'[' || v.front() == u'{' || v.front() == u'(')
        return true;
    if (v.front() == u'%' && v.back() == u'%')
        return true;
    const QString lower = v.toLower();
    for (const QString& word : {u"process.env"_s, u"os.environ"_s, u"env("_s, u"your"_s, u"null"_s, u"none"_s, u"undefined"_s,
             u"redacted"_s, u"example"_s, u"changeme"_s, u"placeholder"_s, u"true"_s, u"false"_s}) {
        if (lower.startsWith(word) || (word == u"your"_s && lower.contains(word)))
            return true;
    }
    // Only one mark repeated or blanking marks: ****, xxxx, ......, ••••••.
    bool blanks = true;
    for (QChar c : v) {
        const char16_t u = c.unicode();
        if (!(u == u'*' || u == u'x' || u == u'X' || u == u'.' || u == u'-' || u == u'_' || u == u'#' || u == u'?' || u == 0x2022
                || u == 0x25CF || u == 0x00B7 || u == 0x2A2F))
            blanks = false;
    }
    if (blanks)
        return true;
    // sk-xxxxxxxx: a known start, then blanks.
    const qsizetype cut = std::max(v.lastIndexOf(u'-'), v.lastIndexOf(u'_'));
    if (cut > 0 && v.size() - cut - 1 >= 4) {
        bool tailBlank = true;
        for (qsizetype i = cut + 1; i < v.size(); ++i)
            tailBlank = tailBlank && (v[i] == u'x' || v[i] == u'X' || v[i] == u'*');
        if (tailBlank)
            return true;
    }
    bool same = v.size() >= 4;
    for (QChar c : v)
        same = same && c == v[0];
    return same;
}

// ---- the matches ----

struct Match {
    int from = 0;
    int to = 0;
    int priority = 0;
    QString category;
};

void add(QList<Match>& out, int from, int to, int priority, const QString& category)
{
    if (to > from)
        out.append({from, to, priority, category});
}

void findPhones(const QString& plain, const QString& digits, QList<Match>& out)
{
    static const QRegularExpression re(u"(?:(?:\\+|00)86[ -]?)?1[3-9]\\d[ -]?\\d{4}[ -]?\\d{4}"_s);
    for (auto it = re.globalMatch(digits); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const int from = int(m.capturedStart());
        const int to = int(m.capturedEnd());
        if (apartFromDigits(plain, from, to))
            add(out, from, to, 3, u"phone"_s);
    }
}

void findIdCards(const QString& plain, const QString& digits, QList<Match>& out)
{
    static const QRegularExpression re(u"[1-9]\\d{16}[\\dXx]|[1-9]\\d{14}"_s);
    for (auto it = re.globalMatch(digits); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const int from = int(m.capturedStart());
        const int to = int(m.capturedEnd());
        if (apartFromDigits(plain, from, to) && validIdCard(QStringView(plain).mid(from, to - from)))
            add(out, from, to, 1, u"id"_s);
    }
}

void findBankCards(const QString& plain, const QString& digits, QList<Match>& out)
{
    static const QRegularExpression re(u"\\d{4}(?:[ -]\\d{4}){2,3}(?:[ -]\\d{1,3})?|\\d{13,19}"_s);
    for (auto it = re.globalMatch(digits); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const int from = int(m.capturedStart());
        const int to = int(m.capturedEnd());
        if (!apartFromDigits(plain, from, to))
            continue;
        const QString number = digitsOnly(QStringView(digits).mid(from, to - from));
        if (number.size() < 13 || number.size() > 19)
            continue;
        // Cards start 3 to 6 or 9 (Amex, Visa, Mastercard, UnionPay, the banks' own); a
        // timestamp or an order number rarely does and rarely passes the check.
        const QChar first = number[0];
        if (!((first >= u'3' && first <= u'6') || first == u'9'))
            continue;
        if (std::all_of(number.begin(), number.end(), [&](QChar c) { return c == number[0]; }))
            continue;
        if (idShaped(number)) // an identity number with a wrong check digit is not a card
            continue;
        if (validLuhn(number))
            add(out, from, to, 2, u"card"_s);
    }
}

void findEmails(const QString& plain, QList<Match>& out)
{
    static const QRegularExpression re(u"[A-Za-z0-9._%+\\-]+@[A-Za-z0-9\\-]+(?:\\.[A-Za-z0-9\\-]+)*\\.[A-Za-z]{2,}"_s);
    for (auto it = re.globalMatch(plain); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        add(out, int(m.capturedStart()), int(m.capturedEnd()), 4, u"email"_s);
    }
}

// A key is hidden to the end of the line: the reader often gets the tail wrong.
void findSecrets(const QString& plain, QList<Match>& out)
{
    static const QRegularExpression prefixed(
        u"(?<![A-Za-z0-9_\\-])(?:sk-ant-[A-Za-z0-9_\\-]{16,}|sk-[A-Za-z0-9_\\-]{16,}|gh[pousr]_[A-Za-z0-9]{16,}"
        u"|github_pat_[A-Za-z0-9_]{16,}|glpat-[A-Za-z0-9_\\-]{16,}|xox[bpas]-[A-Za-z0-9\\-]{10,}|AKIA[0-9A-Z]{16}"
        u"|AIza[0-9A-Za-z_\\-]{30,}|eyJ[A-Za-z0-9_\\-]{8,}\\.[A-Za-z0-9_\\-]{8,}\\.[A-Za-z0-9_\\-]{8,})"_s);
    for (auto it = prefixed.globalMatch(plain); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const QString token = m.captured();
        // sk-xxxxxxxxxxxxxxxx and the like stand for a key, they are not one.
        if (placeholder(token.mid(int(token.indexOf(QRegularExpression(u"[-_]"_s))) + 1)))
            continue;
        add(out, int(m.capturedStart()), int(plain.size()), 0, u"secret"_s);
        return; // the rest of the line is covered
    }
    static const QRegularExpression pem(u"-----BEGIN [A-Z ]*PRIVATE KEY-----"_s);
    if (const QRegularExpressionMatch m = pem.match(plain); m.hasMatch()) {
        add(out, int(m.capturedStart()), int(plain.size()), 0, u"secret"_s);
        return;
    }
    static const QRegularExpression keyed(
        u"(?:password|passwd|pwd|secret|token|api[_\\- ]?key|apikey|access[_\\- ]?key|密码|口令|令牌|密钥)[\"']?[ \\t]*[:=][ \\t]*"_s,
        QRegularExpression::CaseInsensitiveOption);
    for (auto it = keyed.globalMatch(plain); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const int start = int(m.capturedEnd());
        const QString rest = plain.mid(start);
        // The first word of the value: up to a blank, quote or comma.
        const QString first = rest.section(QRegularExpression(u"[ \\t,;]"_s), 0, 0);
        if (placeholder(first))
            continue;
        add(out, start, int(plain.size()), 0, u"secret"_s);
        return;
    }
}

bool boundedAscii(const QString& plain, int from, int to)
{
    if (from > 0 && asciiAlnum(plain[from]) && asciiAlnum(plain[from - 1]))
        return false;
    if (to < plain.size() && asciiAlnum(plain[to - 1]) && asciiAlnum(plain[to]))
        return false;
    return true;
}

void findNames(const QString& plain, const QStringList& names, QList<Match>& out)
{
    for (const QString& raw : names) {
        const QString name = foldedPlain(raw.trimmed());
        if (name.isEmpty())
            continue;
        for (qsizetype at = plain.indexOf(name, 0, Qt::CaseInsensitive); at >= 0;
             at = plain.indexOf(name, at + 1, Qt::CaseInsensitive)) {
            const int from = int(at);
            const int to = int(at + name.size());
            if (!boundedAscii(plain, from, to))
                continue;
            if (name.size() < 3) {
                // Too short to stand alone: only in a path or a prompt.
                const QString before = plain.left(from).toLower();
                const bool path = before.endsWith(u"users\\"_s) || before.endsWith(u"users/"_s);
                const bool prompt = (to < plain.size() && plain[to] == u'@') || (from > 0 && plain[from - 1] == u'@');
                if (!path && !prompt)
                    continue;
            }
            add(out, from, to, 5, u"identity"_s);
        }
    }
}

void findNetwork(const QString& plain, QList<Match>& out)
{
    static const QRegularExpression v4(u"(?<![0-9.])(?:\\d{1,3}\\.){3}\\d{1,3}(?![0-9.])"_s);
    static const QRegularExpression version(u"(?:version|ver\\.?|v|版本|版)[ ]*$"_s, QRegularExpression::CaseInsensitiveOption);
    for (auto it = v4.globalMatch(plain); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        bool ok = true;
        for (const QString& part : m.captured().split(u'.'))
            ok = ok && part.toInt() <= 255;
        if (ok && !version.match(plain.left(int(m.capturedStart()))).hasMatch())
            add(out, int(m.capturedStart()), int(m.capturedEnd()), 6, u"ip"_s);
    }
    static const QRegularExpression mac(u"(?<![0-9A-Fa-f:\\-])(?:[0-9A-Fa-f]{2}[:\\-]){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f])"_s);
    for (auto it = mac.globalMatch(plain); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        add(out, int(m.capturedStart()), int(m.capturedEnd()), 6, u"ip"_s);
    }
    static const QRegularExpression v6(u"(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}(?![0-9A-Fa-f:])"
                                       u"|(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{1,4}:){1,6}:(?:[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{1,4}){0,5})?(?![0-9A-Fa-f:])"_s);
    for (auto it = v6.globalMatch(plain); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        add(out, int(m.capturedStart()), int(m.capturedEnd()), 6, u"ip"_s);
    }
}

void findWords(const QString& plain, const QStringList& words, QList<Match>& out)
{
    for (const QString& raw : words) {
        const QString word = foldedPlain(raw.trimmed());
        if (word.isEmpty())
            continue;
        for (qsizetype at = plain.indexOf(word, 0, Qt::CaseInsensitive); at >= 0;
             at = plain.indexOf(word, at + 1, Qt::CaseInsensitive)) {
            if (boundedAscii(plain, int(at), int(at + word.size())))
                add(out, int(at), int(at + word.size()), 7, u"word"_s);
        }
    }
}

// ---- where characters are ----

// How wide a character is, relative to the others: for where one ends and
// the next begins between two middles.
double advance(QChar c)
{
    const char16_t u = c.unicode();
    if ((u >= 0x2E80 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFF00 && u <= 0xFFEF) || c.isSurrogate())
        return 1.0;
    if (c.isSpace())
        return 0.3;
    if (u >= u'0' && u <= u'9')
        return 0.56;
    switch (u) {
    case u'i':
    case u'l':
    case u'j':
    case u'.':
    case u',':
    case u':':
    case u';':
    case u'\'':
    case u'!':
    case u'|':
    case u'(':
    case u')':
    case u'[':
    case u']':
        return 0.3;
    case u'm':
    case u'w':
    case u'W':
    case u'M':
    case u'@':
        return 0.85;
    default:
        return (u >= u'A' && u <= u'Z') ? 0.68 : 0.56;
    }
}

// The part of the line [from, to) of its text takes, as fractions of the
// line's length (0 left end, 1 right end), and where the middles of its first
// and last character and of the characters just outside it are (a missing
// one as far outside as the match's edge is from its nearest middle).
struct Span {
    double f0 = 0;
    double f1 = 1;
    double first = 0.5;
    double last = 0.5;
    double before = 0;
    double after = 1;
};

Span span(const RedactLine& line, int from, int to)
{
    const int n = int(line.text.size());
    from = std::clamp(from, 0, n);
    to = std::clamp(to, from, n);
    Span out;
    // The reader's places, when they cover the line in order.
    bool placed = !line.chars.isEmpty();
    for (qsizetype k = 0; placed && k < line.chars.size(); ++k) {
        placed = line.chars[k].first >= 0 && line.chars[k].first < n && (k == 0 || line.chars[k].first > line.chars[k - 1].first);
    }
    if (!placed) {
        // By widths alone: a full-width character counts for two half-width ones.
        double total = 0;
        for (int i = 0; i < n; ++i)
            total += advance(line.text[i]);
        if (total <= 0)
            return out;
        std::vector<double> middle(static_cast<std::size_t>(n), 0.0);
        double at = 0;
        double before = 0;
        double through = 0;
        for (int i = 0; i < n; ++i) {
            if (i == from)
                before = at;
            middle[std::size_t(i)] = (at + advance(line.text[i]) / 2) / total;
            at += advance(line.text[i]);
            if (i + 1 == to)
                through = at;
        }
        if (from == to)
            through = before;
        out.f0 = before / total;
        out.f1 = through / total;
        const int last = std::max(from, to - 1);
        out.first = from < n ? middle[std::size_t(from)] : out.f0;
        out.last = last < n ? middle[std::size_t(last)] : out.f1;
        out.before = from > 0 ? middle[std::size_t(from - 1)] : 2 * out.f0 - out.first;
        out.after = to < n ? middle[std::size_t(to)] : 2 * out.f1 - out.last;
        return out;
    }
    const qsizetype m = line.chars.size();
    // Entry k covers text from chars[k].first to the next one's.
    qsizetype k0 = 0;
    qsizetype k1 = 0;
    for (qsizetype k = 0; k < m; ++k) {
        if (line.chars[k].first <= from)
            k0 = k;
        if (line.chars[k].first < to)
            k1 = k;
    }
    k1 = std::max(k1, k0);
    const auto centre = [&](qsizetype k) { return double(line.chars[k].second); };
    const auto width = [&](qsizetype k) { return advance(line.text[line.chars[k].first]); };
    // Between two middles, the edge lies in proportion to the two widths.
    const auto left = [&](qsizetype k) {
        if (m == 1)
            return 0.0;
        if (k == 0)
            return centre(0) - (centre(1) - centre(0)) * width(0) / (width(0) + width(1));
        return centre(k - 1) + (centre(k) - centre(k - 1)) * width(k - 1) / (width(k - 1) + width(k));
    };
    const auto right = [&](qsizetype k) {
        if (m == 1)
            return 1.0;
        if (k == m - 1)
            return centre(k) + (centre(k) - centre(k - 1)) * width(k) / (width(k - 1) + width(k));
        return centre(k) + (centre(k + 1) - centre(k)) * width(k) / (width(k) + width(k + 1));
    };
    out.f0 = std::clamp(left(k0), 0.0, 1.0);
    out.f1 = std::clamp(right(k1), 0.0, 1.0);
    out.first = centre(k0);
    out.last = centre(k1);
    out.before = k0 > 0 ? centre(k0 - 1) : 2 * out.f0 - out.first;
    out.after = k1 < m - 1 ? centre(k1 + 1) : 2 * out.f1 - out.last;
    return out;
}

double lineHeight(const QPolygonF& q)
{
    if (q.size() < 4)
        return 0;
    return (std::hypot(q[3].x() - q[0].x(), q[3].y() - q[0].y()) + std::hypot(q[2].x() - q[1].x(), q[2].y() - q[1].y())) / 2;
}

QRectF partOfLine(const RedactLine& line, double f0, double f1)
{
    const QPolygonF& q = line.quad;
    const auto top = [&](double f) { return q[0] + (q[1] - q[0]) * f; };
    const auto bottom = [&](double f) { return q[3] + (q[2] - q[3]) * f; };
    return QPolygonF({top(f0), top(f1), bottom(f1), bottom(f0)}).boundingRect();
}

// Which columns round `x` (within `reach`) of rows [top, bottom) hold ink:
// a pixel far from the window's usual brightness. `first` is the column the
// answer starts at.
struct InkWindow {
    int first = 0;
    std::vector<bool> ink;
};

InkWindow inkAround(const QImage& gray, int x, int reach, int top, int bottom)
{
    InkWindow w;
    const int left = std::max(0, x - reach - 1);
    const int right = std::min(gray.width() - 1, x + reach + 1);
    top = std::max(0, top);
    bottom = std::min(gray.height(), bottom);
    if (left > right || top >= bottom)
        return w;
    // The ground: the mean of the most common eighth of the brightness scale.
    int histogram[32] = {};
    for (int y = top; y < bottom; ++y) {
        const uchar* line = gray.constScanLine(y);
        for (int c = left; c <= right; ++c)
            ++histogram[line[c] >> 3];
    }
    const int modal = int(std::max_element(histogram, histogram + 32) - histogram);
    qint64 sum = 0;
    qint64 count = 0;
    for (int y = top; y < bottom; ++y) {
        const uchar* line = gray.constScanLine(y);
        for (int c = left; c <= right; ++c) {
            if ((line[c] >> 3) == modal) {
                sum += line[c];
                ++count;
            }
        }
    }
    const int ground = int(sum / std::max<qint64>(1, count));
    w.first = left;
    w.ink.assign(std::size_t(right - left + 1), false);
    for (int y = top; y < bottom; ++y) {
        const uchar* line = gray.constScanLine(y);
        for (int c = left; c <= right; ++c) {
            if (std::abs(int(line[c]) - ground) >= 48)
                w.ink[std::size_t(c - left)] = true;
        }
    }
    return w;
}

// Where the middles are on the picture's x axis (see Span).
struct Middles {
    double first = 0;
    double last = 0;
    double before = 0;
    double after = 0;
};

// The match's box with its left and right edge moved to the ink. An edge in
// blank space goes in to the nearest glyph on the match's side, so only blank
// is trimmed. An edge inside a glyph goes to whichever end keeps the glyph
// with the character it is nearest to: out past it when its middle is nearer
// the match's end character than the one outside, else back before it. An
// edge with no ink within reach stays where it was.
QRectF atTheInk(QRectF box, const QImage& gray, double height, const Middles& m)
{
    const int reach = std::max(3, int(std::lround(0.45 * height)));
    const int room = 3 * reach; // a whole glyph, a wide one, has to fit
    const int top = int(std::floor(box.top()));
    const int bottom = int(std::ceil(box.bottom()));
    double left = box.left();
    double right = box.right();
    {
        const int x = int(std::lround(box.left()));
        const InkWindow w = inkAround(gray, x, room, top, bottom);
        const int end = w.first + int(w.ink.size());
        const auto ink = [&](int c) { return c >= w.first && c < end && w.ink[std::size_t(c - w.first)]; };
        if (ink(x)) {
            int g0 = x;
            while (ink(g0 - 1) && x - g0 < room)
                --g0;
            int g1 = x;
            while (ink(g1 + 1) && g1 - x < room)
                ++g1;
            if (!ink(g0 - 1) && !ink(g1 + 1)) {
                const double centre = (g0 + g1) / 2.0;
                if (std::abs(centre - m.first) <= std::abs(centre - m.before)) {
                    left = g0; // the glyph is the match's first character's
                } else {
                    // The character before: the match begins at the next ink.
                    for (int c = g1 + 1; c < end && c <= g1 + 1 + reach; ++c) {
                        if (ink(c)) {
                            left = c;
                            break;
                        }
                    }
                }
            }
        } else {
            for (int c = x + 1; c <= x + reach && c < end; ++c) {
                if (ink(c)) {
                    left = c; // the first ink to the right: only blank is crossed
                    break;
                }
            }
        }
    }
    {
        const int x = int(std::lround(box.right())); // the edge: the last column covered is x - 1
        const InkWindow w = inkAround(gray, x, room, top, bottom);
        const int end = w.first + int(w.ink.size());
        const auto ink = [&](int c) { return c >= w.first && c < end && w.ink[std::size_t(c - w.first)]; };
        if (ink(x - 1)) {
            int g0 = x - 1;
            while (ink(g0 - 1) && x - 1 - g0 < room)
                --g0;
            int g1 = x - 1;
            while (ink(g1 + 1) && g1 - (x - 1) < room)
                ++g1;
            if (!ink(g0 - 1) && !ink(g1 + 1)) {
                const double centre = (g0 + g1) / 2.0;
                if (std::abs(centre - m.last) <= std::abs(centre - m.after)) {
                    right = g1 + 1; // the glyph is the match's last character's
                } else {
                    // The character after: the match ends at the ink before it.
                    for (int c = g0 - 2; c >= w.first && c >= g0 - 2 - reach; --c) {
                        if (ink(c)) {
                            right = c + 1;
                            break;
                        }
                    }
                }
            }
        } else {
            for (int c = x - 2; c >= x - 1 - reach && c >= w.first; --c) {
                if (ink(c)) {
                    right = c + 1; // the first ink to the left
                    break;
                }
            }
        }
    }
    if (right <= left)
        return box;
    box.setLeft(left);
    box.setRight(right);
    return box;
}

// The span's middles as x positions on the picture.
Middles middlesOf(const RedactLine& line, const Span& f)
{
    const QPolygonF& q = line.quad;
    const auto x = [&](double at) { return ((q[0].x() + (q[1].x() - q[0].x()) * at) + (q[3].x() + (q[2].x() - q[3].x()) * at)) / 2; };
    return {x(f.first), x(f.last), x(f.before), x(f.after)};
}

} // namespace

bool validLuhn(QStringView digits)
{
    int sum = 0;
    bool second = false;
    for (qsizetype i = digits.size() - 1; i >= 0; --i) {
        if (!asciiDigit(digits[i]))
            return false;
        int d = digits[i].unicode() - u'0';
        if (second) {
            d *= 2;
            if (d > 9)
                d -= 9;
        }
        sum += d;
        second = !second;
    }
    return digits.size() > 1 && sum % 10 == 0;
}

bool validIdCard(QStringView d)
{
    if (!idShaped(d))
        return false;
    if (d.size() == 15)
        return true;
    static constexpr int weights[17] = {7, 9, 10, 5, 8, 4, 2, 1, 6, 3, 7, 9, 10, 5, 8, 4, 2};
    int sum = 0;
    for (int i = 0; i < 17; ++i)
        sum += (d[i].unicode() - u'0') * weights[i];
    const QChar expected = u"10X98765432"[sum % 11];
    return d[17].toUpper() == expected;
}

QRect matchBox(const RedactLine& line, int from, int to, const QImage& gray)
{
    if (line.quad.size() < 4 || line.text.isEmpty())
        return {};
    const Span f = span(line, from, to);
    QRectF box = partOfLine(line, f.f0, f.f1);
    if (!gray.isNull())
        box = atTheInk(box, gray, lineHeight(line.quad), middlesOf(line, f));
    return box.toAlignedRect();
}

QList<RedactHit> findSensitive(const QList<RedactLine>& lines, const RedactRules& rules, const QList<QRect>& alreadyCovered,
    const QImage& gray)
{
    QList<RedactHit> hits;
    QRegion covered;
    for (const QRect& r : alreadyCovered)
        covered += r;
    for (const RedactLine& line : lines) {
        if (line.quad.size() < 4 || line.text.isEmpty())
            continue;
        QList<Match> found;
        if (rules.allText) {
            add(found, 0, int(line.text.size()), 8, u"text"_s);
        } else {
            const QString plain = foldedPlain(line.text);
            const QString digits = foldedDigits(plain);
            if (rules.secret)
                findSecrets(plain, found);
            if (rules.idCard)
                findIdCards(plain, digits, found);
            if (rules.bankCard)
                findBankCards(plain, digits, found);
            if (rules.phone)
                findPhones(plain, digits, found);
            if (rules.email)
                findEmails(plain, found);
            if (rules.identity)
                findNames(plain, rules.names, found);
            if (rules.ipMac)
                findNetwork(plain, found);
            if (!rules.words.isEmpty())
                findWords(plain, rules.words, found);
        }
        if (found.isEmpty())
            continue;
        // The stronger rule keeps a stretch two rules both found.
        std::stable_sort(found.begin(), found.end(), [](const Match& a, const Match& b) {
            return a.priority != b.priority ? a.priority < b.priority : a.from < b.from;
        });
        QList<Match> kept;
        for (const Match& m : std::as_const(found)) {
            const bool overlaps = std::any_of(kept.cbegin(), kept.cend(), [&](const Match& k) { return m.from < k.to && k.from < m.to; });
            if (!overlaps)
                kept.append(m);
        }
        const double height = lineHeight(line.quad);
        const double sideways = std::max(2.0, 0.15 * height);
        const double upDown = 0.1 * height;
        QList<RedactHit> row;
        for (const Match& m : std::as_const(kept)) {
            const bool whole = m.category == u"text"_s;
            const Span f = whole ? Span() : span(line, m.from, m.to);
            QRectF part = partOfLine(line, f.f0, f.f1);
            if (!gray.isNull() && !whole)
                part = atTheInk(part, gray, height, middlesOf(line, f));
            const QRectF box = part.adjusted(-sideways, -upDown, sideways, upDown);
            row.append({box.toAlignedRect(), m.category});
        }
        // Boxes that touch, or lie closer than half the line's height, become one.
        std::sort(row.begin(), row.end(), [](const RedactHit& a, const RedactHit& b) { return a.box.left() < b.box.left(); });
        QList<RedactHit> merged;
        for (const RedactHit& h : std::as_const(row)) {
            if (!merged.isEmpty() && h.box.left() <= merged.last().box.right() + 0.5 * height) {
                merged.last().box |= h.box;
            } else {
                merged.append(h);
            }
        }
        for (const RedactHit& h : std::as_const(merged)) {
            if (!covered.isEmpty()) {
                const QRegion inside = covered & QRegion(h.box);
                qint64 area = 0;
                for (const QRect& r : inside)
                    area += qint64(r.width()) * r.height();
                if (area * 10 >= qint64(h.box.width()) * h.box.height() * 8)
                    continue;
            }
            hits.append(h);
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const RedactHit& a, const RedactHit& b) {
        return a.box.top() != b.box.top() ? a.box.top() < b.box.top() : a.box.left() < b.box.left();
    });
    return hits;
}

} // namespace ws::capture
