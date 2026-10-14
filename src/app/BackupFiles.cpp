#include "BackupFiles.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cstring>

#include <miniz.h>

using namespace Qt::StringLiterals;

namespace ws::backupfiles {

namespace {

constexpr qint64 kMaxZipBytes = 512ll << 20;
const QString kPrefix = u"WinShun-"_s;
const QString kTimeFormat = u"yyyy-MM-dd-HHmmss"_s;
const QString kManifest = u"backup.json"_s;
const QString kSettingsName = u"WinShun.ini"_s;

QString tr(const char* text)
{
    return QCoreApplication::translate("ws::backupfiles", text);
}

// The files of the data folder a backup holds, by their names in it.
constexpr const char16_t* kDataFiles[] {u"favorites.json", u"dialog-pins.txt"};
// Folders of it whose files are all held (but logs, big ones, and the
// plugins' "<id>.data" folders: their caches, such as the semantic index,
// are of this computer's files and rebuilt).
constexpr const char16_t* kDataFolders[] {u"icons/web/custom", u"plugins"};

struct Zip {
    mz_zip_archive archive {};
    bool reading = false;
    ~Zip()
    {
        if (reading)
            mz_zip_reader_end(&archive);
    }
};

// An entry's name, '/' separated; empty when it could leave the folder.
QString safeName(const mz_zip_archive_file_stat& stat)
{
    QString name = QString::fromUtf8(stat.m_filename, static_cast<qsizetype>(std::strlen(stat.m_filename)));
    name.replace(u'\\', u'/');
    if (name.startsWith(u'/') || name.contains(u':'))
        return {};
    const QStringList parts = name.split(u'/', Qt::SkipEmptyParts);
    if (std::any_of(parts.begin(), parts.end(), [](const QString& p) { return p == u".." || p == u"."; }))
        return {};
    return parts.join(u'/');
}

QJsonObject manifestOf(Zip& zip)
{
    const int i = mz_zip_reader_locate_file(&zip.archive, "backup.json", nullptr, 0);
    if (i < 0)
        return {};
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip.archive, static_cast<mz_uint>(i), &size, 0);
    if (!data)
        return {};
    const QJsonObject o = QJsonDocument::fromJson(QByteArray(static_cast<const char*>(data), static_cast<qsizetype>(size))).object();
    mz_free(data);
    return o.value(u"app"_s).toString() == u"WinShun" ? o : QJsonObject();
}

} // namespace

QList<File> collect(const QString& settingsFile, const QString& dataFolder)
{
    QList<File> files;
    if (QFileInfo(settingsFile).isFile())
        files.append({kSettingsName, settingsFile});
    const QDir data(dataFolder);
    for (const char16_t* name : kDataFiles) {
        const QString path = data.filePath(QString::fromUtf16(name));
        if (QFileInfo(path).isFile())
            files.append({u"data/"_s + QString::fromUtf16(name), path});
    }
    for (const char16_t* folder : kDataFolders) {
        const QString root = data.filePath(QString::fromUtf16(folder));
        QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo info = it.nextFileInfo();
            if (info.isSymLink() || info.size() > kMaxFileBytes || info.suffix().compare(u"log"_s, Qt::CaseInsensitive) == 0)
                continue;
            const QString name = data.relativeFilePath(info.filePath());
            if (name.section(u'/', 1, 1).endsWith(u".data"_s, Qt::CaseInsensitive) && name.startsWith(u"plugins/"_s))
                continue;
            files.append({u"data/"_s + name, info.filePath()});
        }
    }
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.name < b.name; });
    return files;
}

Packed pack(const QList<File>& files, const QString& version, const QDateTime& when)
{
    Zip zip;
    if (!mz_zip_writer_init_heap(&zip.archive, 0, 64 * 1024))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    bool ok = true;
    for (const File& f : files) {
        QFile file(f.path);
        if (!file.open(QIODevice::ReadOnly)) {
            ok = false;
            break;
        }
        const QByteArray bytes = file.readAll();
        const QByteArray name = f.name.toUtf8();
        hash.addData(name);
        hash.addData(QByteArrayView("\0", 1));
        hash.addData(bytes);
        hash.addData(QByteArrayView("\0", 1));
        if (!mz_zip_writer_add_mem(&zip.archive, name.constData(), bytes.constData(), static_cast<size_t>(bytes.size()),
                MZ_DEFAULT_COMPRESSION)) {
            ok = false;
            break;
        }
    }
    Packed packed;
    packed.hash = QString::fromLatin1(hash.result().toHex());
    const QByteArray manifest = QJsonDocument(QJsonObject {
                                                  {u"app"_s, u"WinShun"_s},
                                                  {u"format"_s, 1},
                                                  {u"version"_s, version},
                                                  {u"created"_s, when.toString(Qt::ISODate)},
                                                  {u"hash"_s, packed.hash},
                                              })
                                    .toJson();
    ok = ok
        && mz_zip_writer_add_mem(&zip.archive, "backup.json", manifest.constData(), static_cast<size_t>(manifest.size()),
            MZ_DEFAULT_COMPRESSION);
    void* buffer = nullptr;
    size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip.archive, &buffer, &size);
    mz_zip_writer_end(&zip.archive);
    if (!ok) {
        if (buffer)
            mz_free(buffer);
        return {};
    }
    packed.zip = QByteArray(static_cast<const char*>(buffer), static_cast<qsizetype>(size));
    mz_free(buffer);
    return packed;
}

QString fileName(const QDateTime& when)
{
    return kPrefix + when.toString(kTimeFormat) + u".zip"_s;
}

QList<Found> list(const QString& folder)
{
    QList<Found> found;
    const QDir dir(folder);
    for (const QFileInfo& info : dir.entryInfoList({kPrefix + u"*.zip"_s}, QDir::Files)) {
        const QString stamp = info.completeBaseName().mid(kPrefix.size());
        const QDateTime when = QDateTime::fromString(stamp, kTimeFormat);
        if (when.isValid())
            found.append({info.filePath(), when});
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.when > b.when; });
    return found;
}

QString hashOf(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxZipBytes)
        return {};
    const QByteArray bytes = file.readAll();
    Zip zip;
    zip.reading = mz_zip_reader_init_mem(&zip.archive, bytes.constData(), static_cast<size_t>(bytes.size()), 0);
    return zip.reading ? manifestOf(zip).value(u"hash"_s).toString() : QString();
}

QString write(const QString& folder, const Packed& packed, const QDateTime& when)
{
    if (packed.zip.isEmpty())
        return tr("有文件读不出来");
    if (!QDir().mkpath(folder))
        return tr("建不了文件夹 %1").arg(QDir::toNativeSeparators(folder));
    const QDir dir(folder);
    const QString target = dir.filePath(fileName(when));
    const QString part = target + u".part"_s;
    QFile file(part);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(packed.zip) != packed.zip.size()
        || !file.flush()) {
        file.close();
        QFile::remove(part);
        return tr("写不进 %1").arg(QDir::toNativeSeparators(folder));
    }
    file.close();
    QFile::remove(target);
    if (!QFile::rename(part, target)) {
        QFile::remove(part);
        return tr("写不进 %1").arg(QDir::toNativeSeparators(folder));
    }
    // Only the newest kept; and no ".part" left over by one cut short.
    const QList<Found> all = list(folder);
    for (qsizetype i = kKept; i < all.size(); ++i)
        QFile::remove(all[i].path);
    for (const QFileInfo& info : dir.entryInfoList({kPrefix + u"*.zip.part"_s}, QDir::Files))
        QFile::remove(info.filePath());
    return {};
}

QString unpack(const QString& path, const QString& folder)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return tr("打不开 %1").arg(QDir::toNativeSeparators(path));
    if (file.size() > kMaxZipBytes)
        return tr("这不是 Win顺 的备份");
    const QByteArray bytes = file.readAll();
    file.close();
    Zip zip;
    zip.reading = mz_zip_reader_init_mem(&zip.archive, bytes.constData(), static_cast<size_t>(bytes.size()), 0);
    if (!zip.reading || manifestOf(zip).isEmpty())
        return tr("这不是 Win顺 的备份");

    struct Entry {
        mz_uint index;
        QString name;
    };
    QList<Entry> entries;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip.archive); ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip.archive, i, &stat))
            return tr("备份文件已损坏");
        if (mz_zip_reader_is_file_a_directory(&zip.archive, i))
            continue;
        const QString name = safeName(stat);
        if (name.isEmpty())
            return tr("备份文件已损坏");
        if (name == kSettingsName || name.startsWith(u"data/"_s))
            entries.append({i, name});
    }
    QDir(folder).removeRecursively();
    if (!QDir().mkpath(folder))
        return tr("建不了文件夹 %1").arg(QDir::toNativeSeparators(folder));
    for (const Entry& e : entries) {
        const QString target = folder + u'/' + e.name;
        QDir().mkpath(QFileInfo(target).path());
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip.archive, e.index, &size, 0);
        if (!data)
            return tr("备份文件已损坏");
        QFile out(target);
        const bool written = out.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && out.write(static_cast<const char*>(data), static_cast<qint64>(size)) == static_cast<qint64>(size);
        mz_free(data);
        if (!written)
            return tr("写不进 %1").arg(QDir::toNativeSeparators(folder));
    }
    return {};
}

void apply(const QString& staged, const QString& settingsFile, const QString& dataFolder)
{
    const auto put = [](const QString& from, const QString& to) {
        QDir().mkpath(QFileInfo(to).path());
        QFile::remove(to);
        if (!QFile::rename(from, to))
            QFile::copy(from, to);
    };
    const QDir dir(staged);
    if (QFileInfo(dir.filePath(kSettingsName)).isFile())
        put(dir.filePath(kSettingsName), settingsFile);
    const QDir data(dir.filePath(u"data"_s));
    QDirIterator it(data.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString from = it.next();
        put(from, QDir(dataFolder).filePath(data.relativeFilePath(from)));
    }
    QDir(staged).removeRecursively();
}

} // namespace ws::backupfiles
