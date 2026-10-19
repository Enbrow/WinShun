#include "SwitchCommand.h"

#include <QString>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

using Kind = SwitchIntent::Kind;

enum class Polarity : quint8 { On, Off, Toggle };

struct Verb {
    QStringView text;
    Polarity polarity;
    bool suffixOnly = false; // "蓝牙开关": only after the switch
};

// Longest first. A verb spelled in Latin letters stands apart by a space
// ("turn on wifi", "bt off"); a Chinese one needs none ("关蓝牙", "蓝牙关").
constexpr Verb kVerbs[] = {
    {u"turn off", Polarity::Off},
    {u"turn on", Polarity::On},
    {u"disable", Polarity::Off},
    {u"enable", Polarity::On},
    {u"toggle", Polarity::Toggle},
    {u"switch", Polarity::Toggle},
    {u"开关", Polarity::Toggle, true},
    {u"切换", Polarity::Toggle},
    {u"打开", Polarity::On},
    {u"开启", Polarity::On},
    {u"启用", Polarity::On},
    {u"关闭", Polarity::Off},
    {u"关掉", Polarity::Off},
    {u"关上", Polarity::Off},
    {u"停用", Polarity::Off},
    {u"禁用", Polarity::Off},
    {u"取消", Polarity::Off},
    {u"off", Polarity::Off},
    {u"on", Polarity::On},
    {u"开", Polarity::On},
    {u"关", Polarity::Off},
};

struct Unit {
    QStringView text;
    int minutes;
};

constexpr Unit kUnits[] = {
    {u"个小时", 60},
    {u"小时", 60},
    {u"hours", 60},
    {u"hour", 60},
    {u"hrs", 60},
    {u"hr", 60},
    {u"h", 60},
    {u"分钟", 1},
    {u"minutes", 1},
    {u"minute", 1},
    {u"mins", 1},
    {u"min", 1},
    {u"分", 1},
    {u"m", 1},
};

constexpr QStringView kSettingsWords[] = {u"设置", u"选项", u"settings", u"setting", u"options", u"option"};

constexpr int kMaxMinutes = 99 * 60; // keep awake: a minute to 99 hours
constexpr qsizetype kLongestCommand = 64;

bool isAsciiWord(QChar c) noexcept
{
    return c.unicode() < 128 && c.isLetterOrNumber();
}

bool isDigit(QChar c) noexcept
{
    return c >= u'0' && c <= u'9';
}

bool takesLevel(const QString& id) noexcept
{
    return id == u"volume";
}

bool takesDuration(const QString& id) noexcept
{
    return id == u"keep-awake";
}

// Full-width forms to half-width, Latin letters to lower case, runs of
// spaces to one, and no "%" at the end ("音量 20%").
QString normalized(QStringView text)
{
    QString s = text.toString().normalized(QString::NormalizationForm_KC).toLower().simplified();
    while (s.endsWith(u'%')) {
        s.chop(1);
        s = s.trimmed();
    }
    return s;
}

bool listed(const QStringList& words, QStringView s)
{
    return std::any_of(words.cbegin(), words.cend(), [&](const QString& w) { return s.compare(w, Qt::CaseInsensitive) == 0; });
}

// What a piece of the query names: a switch itself (its name or a keyword),
// or a state word of one ("深色": the theme, on).
struct Subject {
    std::size_t place = 0;
    int state = -1; // -1 the switch, 1 its state word for on, 0 for off
};

// The one switch `s` is, whole; none when it is none, or when it is two.
std::optional<Subject> findSubject(
    QStringView s, const PlaceList& places, const std::vector<std::size_t>& switches, bool stateWords)
{
    std::optional<Subject> found;
    for (const std::size_t i : switches) {
        const PlaceInfo& p = places[i];
        std::optional<Subject> here;
        if (s.compare(p.name, Qt::CaseInsensitive) == 0 || listed(p.ownKeywords, s))
            here = Subject {i, -1};
        else if (stateWords && listed(p.onWords, s))
            here = Subject {i, 1};
        else if (stateWords && listed(p.offWords, s))
            here = Subject {i, 0};
        if (!here)
            continue;
        if (found)
            return std::nullopt;
        found = here;
    }
    return found;
}

SwitchCommand withVerb(Polarity verb, const Subject& subject)
{
    SwitchCommand c {.place = subject.place};
    switch (verb) {
    case Polarity::Toggle:
        c.intent.kind = Kind::Toggle;
        break;
    case Polarity::On: // "开深色" is the state word's own; "开蓝牙" is on
        c.intent.kind = subject.state == 0 ? Kind::Off : Kind::On;
        break;
    case Polarity::Off: // "关闭深色": the other state than the word's
        c.intent.kind = subject.state == 1 || subject.state < 0 ? Kind::Off : Kind::On;
        break;
    }
    return c;
}

std::optional<SwitchCommand> byVerb(QStringView t, const PlaceList& places, const std::vector<std::size_t>& switches)
{
    for (const Verb& v : kVerbs) {
        const qsizetype n = v.text.size();
        if (t.size() <= n)
            continue;
        if (!v.suffixOnly && t.startsWith(v.text) && (!isAsciiWord(v.text.back()) || t[n] == u' ')) {
            if (const auto s = findSubject(t.sliced(n).trimmed(), places, switches, true))
                return withVerb(v.polarity, *s);
        }
        if (t.endsWith(v.text) && (!isAsciiWord(v.text.front()) || t[t.size() - n - 1] == u' ')) {
            if (const auto s = findSubject(t.first(t.size() - n).trimmed(), places, switches, true))
                return withVerb(v.polarity, *s);
        }
    }
    return std::nullopt;
}

// "音量 +10", "30", "-5".
std::optional<SwitchIntent> parseLevel(QStringView tail)
{
    int sign = 0;
    if (tail.startsWith(u'+'))
        sign = 1;
    else if (tail.startsWith(u'-'))
        sign = -1;
    const QStringView digits = (sign != 0 ? tail.sliced(1) : tail).trimmed();
    if (digits.isEmpty() || digits.size() > 4 || !std::all_of(digits.begin(), digits.end(), isDigit))
        return std::nullopt;
    const int value = digits.toString().toInt();
    return sign == 0 ? SwitchIntent {Kind::SetLevel, value} : SwitchIntent {Kind::StepLevel, sign * value};
}

struct Number {
    double value = 0;
    qsizetype used = 0;
};

// "1.5", "90", "三", "两", "半", "一个半": at the start of `t`.
std::optional<Number> readNumber(QStringView t)
{
    Number n;
    if (t.isEmpty())
        return std::nullopt;
    if (isDigit(t[0])) {
        qsizetype i = 0;
        while (i < t.size() && isDigit(t[i]))
            ++i;
        if (i + 1 < t.size() && t[i] == u'.' && isDigit(t[i + 1])) {
            ++i;
            while (i < t.size() && isDigit(t[i]))
                ++i;
        }
        n.value = t.first(i).toString().toDouble();
        n.used = i;
    } else if (t[0] == u'半') {
        return Number {0.5, 1};
    } else {
        static constexpr QStringView kDigits = u"一二三四五六七八九十";
        const qsizetype at = t[0] == u'两' ? 1 : kDigits.indexOf(t[0]);
        if (at < 0)
            return std::nullopt;
        n.value = t[0] == u'两' ? 2 : static_cast<double>(at + 1);
        n.used = 1;
    }
    if (t.sliced(n.used).startsWith(u"个半")) { // 一个半小时
        n.value += 0.5;
        n.used += 2;
    }
    return n;
}

// "1小时", "半小时", "一个半小时", "90m", "30 分钟": minutes, within the range.
std::optional<int> parseSpan(QStringView t)
{
    const std::optional<Number> n = readNumber(t);
    if (!n)
        return std::nullopt;
    const QStringView unit = t.sliced(n->used).trimmed();
    const auto it = std::find_if(std::begin(kUnits), std::end(kUnits), [&](const Unit& u) { return unit == u.text; });
    if (it == std::end(kUnits))
        return std::nullopt;
    const long long minutes = std::llround(n->value * it->minutes);
    return minutes >= 1 && minutes <= kMaxMinutes ? std::optional<int>(static_cast<int>(minutes)) : std::nullopt;
}

// "18:00", "9:05", "18点", "18点半", "18点30分": minutes after midnight.
std::optional<int> parseClock(QStringView t)
{
    qsizetype i = 0;
    while (i < t.size() && i < 2 && isDigit(t[i]))
        ++i;
    if (i == 0 || i >= t.size())
        return std::nullopt;
    const int hour = t.first(i).toString().toInt();
    int minute = 0;
    const QStringView rest = t.sliced(i + 1).trimmed();
    if (t[i] == u':') {
        if (rest.size() != 2 || !isDigit(rest[0]) || !isDigit(rest[1]))
            return std::nullopt;
        minute = rest.toString().toInt();
    } else if (t[i] == u'点') {
        if (rest == u"半") {
            minute = 30;
        } else if (!rest.isEmpty() && rest != u"整") {
            const QStringView digits = rest.endsWith(u'分') ? rest.chopped(1) : rest;
            if (digits.isEmpty() || digits.size() > 2 || !std::all_of(digits.begin(), digits.end(), isDigit))
                return std::nullopt;
            minute = digits.toString().toInt();
        }
    } else {
        return std::nullopt;
    }
    return hour < 24 && minute < 60 ? std::optional<int>(hour * 60 + minute) : std::nullopt;
}

// "1小时", "到 18:00", "until 18:00".
std::optional<SwitchIntent> parseDuration(QStringView tail)
{
    QStringView at;
    if (tail.startsWith(u"until"))
        at = tail.sliced(5);
    else if (tail.startsWith(u'到'))
        at = tail.sliced(1);
    if (!at.isNull()) {
        if (const auto minutes = parseClock(at.trimmed()))
            return SwitchIntent {Kind::AwakeUntil, *minutes};
        return std::nullopt;
    }
    if (const auto minutes = parseSpan(tail))
        return SwitchIntent {Kind::AwakeFor, *minutes};
    return std::nullopt;
}

// What follows the switch: "设置", a level, a time.
std::optional<SwitchIntent> parseTail(QStringView tail, const QString& id)
{
    if (std::find(std::begin(kSettingsWords), std::end(kSettingsWords), tail) != std::end(kSettingsWords))
        return SwitchIntent {Kind::OpenSettings, 0};
    if (takesLevel(id))
        return parseLevel(tail);
    if (takesDuration(id))
        return parseDuration(tail);
    return std::nullopt;
}

std::optional<SwitchCommand> byTail(QStringView t, const PlaceList& places, const std::vector<std::size_t>& switches)
{
    for (qsizetype cut = 1; cut < t.size(); ++cut) {
        const QStringView head = t.first(cut).trimmed();
        const QStringView tail = t.sliced(cut).trimmed();
        if (head.isEmpty() || tail.isEmpty())
            continue;
        const auto s = findSubject(head, places, switches, false);
        if (!s || s->state >= 0)
            continue;
        if (const auto intent = parseTail(tail, places[s->place].switchId))
            return SwitchCommand {.place = s->place, .intent = *intent};
    }
    return std::nullopt;
}

} // namespace

std::optional<SwitchCommand> parseSwitchCommand(QStringView text, const PlaceList& places)
{
    std::vector<std::size_t> switches;
    for (std::size_t i = 0; i < places.size(); ++i) {
        if (!places[i].switchId.isEmpty())
            switches.push_back(i);
    }
    if (switches.empty())
        return std::nullopt;
    const QString t = normalized(text);
    if (t.isEmpty() || t.size() > kLongestCommand)
        return std::nullopt;

    // The whole of it is one switch or one state word.
    if (const auto whole = findSubject(t, places, switches, true)) {
        if (whole->state < 0)
            return std::nullopt; // the switch alone: an ordinary row
        return SwitchCommand {
            .place = whole->place, .intent = {whole->state == 1 ? Kind::On : Kind::Off, 0}, .onTop = false};
    }
    if (auto c = byVerb(t, places, switches))
        return c;
    return byTail(t, places, switches);
}

} // namespace ws
