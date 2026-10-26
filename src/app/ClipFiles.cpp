#include "ClipFiles.h"

#include <QDateTime>
#include <QDir>
#include <QFile>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::clipfiles {

Export images(const ClipStore& store, const std::vector<const Clip*>& clips, const QString& folder, const QString& name)
{
    if (clips.empty()
        || std::ranges::any_of(clips, [](const Clip* clip) { return !clip || clip->kind != ClipKind::Image; }))
        return {{}, Error::NotImages};
    QStringList paths;
    for (const Clip* clip : clips) {
        const QString original = store.payload(clip->id).imagePath; // waits for a just-captured image
        // IDs also distinguish pictures copied in the same second when Explorer
        // flattens all the source folders into the destination.
        const QString directory = folder + u'/' + QString::number(clip->id);
        const QString path = directory + u'/' + name + u' '
            + QDateTime::fromMSecsSinceEpoch(clip->created).toString(u"yyyy-MM-dd HH.mm.ss"_s) + u' '
            + QString::number(clip->id) + u'.' + clipImageExtension(clip->imageFormat);
        if (!QFile::exists(path) && (!QDir().mkpath(directory) || !QFile::copy(original, path)))
            return {{}, Error::WriteFailed};
        paths.append(QDir::toNativeSeparators(path));
    }
    return {paths, Error::None};
}

QString fileNames(const QStringList& paths)
{
    QStringList names;
    for (const QString& path : paths) {
        // Quotes and newlines cannot name Windows files. Do not turn malformed
        // imported entries into extra selections in another program's dialog.
        if (path.isEmpty() || path.contains(u'"') || path.contains(u'\r') || path.contains(u'\n')
            || path.contains(QChar()))
            return {};
        names.append(u'"' + QDir::toNativeSeparators(path) + u'"');
    }
    return names.join(u' ');
}

} // namespace ws::clipfiles
