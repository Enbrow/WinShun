#include "ZeroCode.h"

#include "WebShortcut.h"
#include "Win32Util.h"

#include <QDir>

using namespace Qt::StringLiterals;

namespace ws::plugin {

QString zeroCodeUrl(const Run& run, const QString& words)
{
    WebShortcut shortcut;
    shortcut.url = run.url;
    shortcut.home = run.home;
    if (words.isEmpty() || !shortcut.searches())
        return shortcut.homeUrl();
    return shortcut.searchUrl(words);
}

namespace {

QString expanded(const QString& path)
{
    return QString::fromStdWString(win32::expandEnvironment(path.toStdWString()));
}

Item base(const Command& command, const QString& locale)
{
    Item item;
    item.id = command.id;
    item.title = command.title.pick(locale);
    item.icon = parseIcon(command.icon);
    return item;
}

} // namespace

Items runZeroCode(const Command& command, const QString& text, const QString& locale)
{
    const QString words = text.trimmed();
    Items items;
    switch (command.run.kind) {
    case Run::Kind::None:
        break;
    case Run::Kind::Url: {
        Item item = base(command, locale);
        item.open.kind = Open::Kind::Url;
        item.open.target = zeroCodeUrl(command.run, words);
        if (item.title.isEmpty())
            item.title = words.isEmpty() ? displayWebUrl(item.open.target) : words;
        item.subtitle = displayWebUrl(item.open.target);
        item.copy = item.open.target;
        items.push_back(item);
        break;
    }
    case Run::Kind::Program: {
        Item item = base(command, locale);
        item.open.kind = Open::Kind::Path;
        item.open.target = QDir::toNativeSeparators(expanded(command.run.program));
        item.open.args = QString(command.run.args).replace(u"%s"_s, words);
        item.open.cwd = QDir::toNativeSeparators(expanded(command.run.cwd));
        if (item.title.isEmpty())
            item.title = QFileInfo(item.open.target).completeBaseName();
        item.subtitle = item.open.args.isEmpty() ? item.open.target : item.open.target + u' ' + item.open.args;
        items.push_back(item);
        break;
    }
    case Run::Kind::Folder: {
        Item item = base(command, locale);
        item.open.kind = Open::Kind::Path;
        item.open.target = QDir::toNativeSeparators(expanded(command.run.folder));
        if (item.title.isEmpty())
            item.title = QFileInfo(item.open.target).fileName();
        item.subtitle = item.open.target;
        item.copy = item.open.target;
        items.push_back(item);
        break;
    }
    case Run::Kind::Search: {
        Item item = base(command, locale);
        item.searchQuery = QString(command.run.query).replace(u"%s"_s, words).trimmed();
        item.searchScope = command.run.scope;
        if (item.title.isEmpty())
            item.title = item.searchQuery;
        item.subtitle = item.searchQuery;
        items.push_back(item);
        break;
    }
    case Run::Kind::Items: {
        for (const QJsonValue& v : command.run.items) {
            Item item = parseItem(v.toObject());
            if (item.title.isEmpty())
                continue;
            if (!words.isEmpty()) {
                const QString haystack = item.match.isEmpty() ? item.title + u'\n' + item.subtitle : item.match;
                bool all = true;
                for (const QString& word : words.split(u' ', Qt::SkipEmptyParts))
                    all = all && haystack.contains(word, Qt::CaseInsensitive);
                if (!all)
                    continue;
            }
            if (item.icon.kind == IconSpec::Kind::None)
                item.icon = parseIcon(command.icon);
            // A manifest is written once for every machine: %USERPROFILE%
            // and the like in a path are expanded here.
            if (item.open.kind == Open::Kind::Path) {
                item.open.target = QDir::toNativeSeparators(expanded(item.open.target));
                item.open.cwd = QDir::toNativeSeparators(expanded(item.open.cwd));
            }
            if (item.subtitle.contains(u'%'))
                item.subtitle = QDir::toNativeSeparators(expanded(item.subtitle));
            items.push_back(item);
            if (items.size() >= kMaxItems)
                break;
        }
        break;
    }
    }
    return items;
}

} // namespace ws::plugin
