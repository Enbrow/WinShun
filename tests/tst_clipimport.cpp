// Importing clipboard history: Ditto's database, text and CSV files, and the
// store's batch import under them.
#include "ClipImage.h"
#include "ClipImport.h"
#include "ClipImportJob.h"
#include "ClipStore.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

using namespace ws;
using namespace ws::clipimport;
using namespace Qt::StringLiterals;

namespace {

constexpr qint64 kStart = 1'790'000'000'000; // some time in 2026
constexpr qint64 kDay = 24ll * 60 * 60 * 1000;

QByteArray utf16(const QString& text, bool terminator = true)
{
    QByteArray bytes(reinterpret_cast<const char*>(text.utf16()), text.size() * 2);
    if (terminator)
        bytes.append(QByteArray(2, '\0'));
    return bytes;
}

QByteArray dibOf(const QImage& image)
{
    QByteArray bmp;
    QBuffer buffer(&bmp);
    buffer.open(QIODevice::WriteOnly);
    image.convertToFormat(QImage::Format_RGB888).save(&buffer, "BMP");
    return bmp.mid(14); // a DIB is a BMP file without its file header
}

QByteArray dropFiles(const QStringList& files)
{
    QByteArray drop(20, '\0');
    drop[0] = 20; // where the names start
    drop[16] = 1; // wide characters
    for (const QString& file : files)
        drop += utf16(file);
    drop += QByteArray(2, '\0');
    return drop;
}

// Ditto's tables as its own code creates them (src/DatabaseUtilities.cpp).
class DittoDb {
public:
    explicit DittoDb(const QString& file)
        : m_name(u"ditto-fixture-"_s + file)
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, m_name);
        db.setDatabaseName(file);
        QVERIFY2(db.open(), "cannot create the fixture");
        for (const QString& sql : {
                 u"CREATE TABLE Main(lID INTEGER PRIMARY KEY AUTOINCREMENT, lDate INTEGER, mText TEXT, lShortCut INTEGER, "
                 u"lDontAutoDelete INTEGER, CRC INTEGER, bIsGroup INTEGER, lParentID INTEGER, QuickPasteText TEXT, "
                 u"clipOrder REAL, clipGroupOrder REAL, globalShortCut INTEGER, lastPasteDate INTEGER, "
                 u"stickyClipOrder REAL, stickyClipGroupOrder REAL, MoveToGroupShortCut INTEGER, "
                 u"GlobalMoveToGroupShortCut INTEGER)"_s,
                 u"CREATE TABLE Data(lID INTEGER PRIMARY KEY AUTOINCREMENT, lParentID INTEGER, strClipBoardFormat TEXT, "
                 u"ooData BLOB)"_s}) {
            QSqlQuery query(db);
            QVERIFY2(query.exec(sql), qPrintable(query.lastError().text()));
        }
    }
    ~DittoDb()
    {
        QSqlDatabase::database(m_name).close();
        QSqlDatabase::removeDatabase(m_name);
    }

    // Returns the row's lID. parent: a group's lID, -1 for none.
    qint64 clip(const QString& description, qint64 date, qint64 parent, double order, double sticky,
        const QList<std::pair<QString, QByteArray>>& formats, bool isGroup = false)
    {
        QSqlDatabase db = QSqlDatabase::database(m_name);
        QSqlQuery query(db);
        query.prepare(u"INSERT INTO Main (lDate, mText, lShortCut, lDontAutoDelete, CRC, bIsGroup, lParentID, "
                      u"clipOrder, clipGroupOrder, lastPasteDate, stickyClipOrder, stickyClipGroupOrder) "
                      u"VALUES (?, ?, 0, 0, 0, ?, ?, ?, ?, ?, ?, ?)"_s);
        query.addBindValue(date);
        query.addBindValue(description);
        query.addBindValue(isGroup ? 1 : 0);
        query.addBindValue(parent);
        query.addBindValue(order);
        query.addBindValue(order);
        query.addBindValue(date + 5);
        query.addBindValue(sticky);
        query.addBindValue(sticky);
        if (!query.exec())
            return QTest::qFail(qPrintable(query.lastError().text()), __FILE__, __LINE__), 0;
        const qint64 id = query.lastInsertId().toLongLong();
        QSqlQuery data(db);
        data.prepare(u"INSERT INTO Data (lParentID, strClipBoardFormat, ooData) VALUES (?, ?, ?)"_s);
        for (const auto& [name, bytes] : formats) {
            data.addBindValue(id);
            data.addBindValue(name);
            data.addBindValue(bytes);
            data.exec();
        }
        return id;
    }

    static constexpr double kNotSticky = -2147483647;

private:
    QString m_name;
};

Report runImport(ClipStore& store, const QString& path, Options options = {})
{
    Hooks hooks;
    const QString own = store.uniqueGroupName(groupName(path));
    hooks.sink = [&](std::vector<ClipImportItem>&& items) { return store.importClips(std::move(items), own); };
    options.now = options.now > 0 ? options.now : kStart;
    return importFile(path, options, hooks);
}

QStringList textsIn(const ClipStore& store, qint64 group)
{
    QStringList list;
    for (const qint64 id : store.groupOrder(group))
        list.append(store.clip(id)->text);
    return list;
}

qint64 groupNamed(const ClipStore& store, const QString& name)
{
    for (const ClipGroup& g : store.groups()) {
        if (g.id != ClipStore::kPinned && g.name == name)
            return g.id;
    }
    return 0;
}

QString write(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        QTest::qFail("cannot write a test file", __FILE__, __LINE__);
    return path;
}

} // namespace

class ClipImportTest : public QObject {
    Q_OBJECT

private slots:
    void storeBatch()
    {
        QTemporaryDir dir;
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        ClipCapture own;
        own.kind = ClipKind::Text;
        own.text = u"本来就有"_s;
        const qint64 existing = store.add(own, kStart + 10 * kDay);
        const qint64 mine = store.addGroup(u"我的"_s);
        store.setGroup(std::vector<qint64> {existing}, mine);

        std::vector<ClipImportItem> items;
        for (const QString& s : {u"甲"_s, u"乙"_s, u"本来就有"_s, u"甲"_s, u"丙"_s}) {
            ClipImportItem item;
            item.text = s;
            item.created = item.used = kStart + (s == u"乙"_s ? 3 * kDay : 0);
            item.group = s == u"丙"_s ? u"我的"_s : QString();
            items.push_back(item);
        }
        const ClipImportResult result = store.importClips(items, u"新来的"_s);
        QCOMPARE(result.added, 3);
        QCOMPARE(result.duplicates, 2);
        QCOMPARE(result.failed, 0);
        const qint64 fresh = groupNamed(store, u"新来的"_s);
        QVERIFY(fresh != 0);
        QCOMPARE(textsIn(store, fresh), (QStringList {u"甲"_s, u"乙"_s})); // in the order given
        QCOMPARE(textsIn(store, mine), (QStringList {u"本来就有"_s, u"丙"_s})); // after what it has
        // In the list by when they were used: the oldest last, the existing one on top.
        QCOMPARE(store.clips().front().text, u"本来就有"_s);
        QCOMPARE(store.clips().back().text, u"甲"_s);
        QCOMPARE(store.clips()[1].text, u"乙"_s);
        QCOMPARE(store.uniqueGroupName(u"新来的"_s), u"新来的 2"_s);

        // Nothing new: no group made.
        QCOMPARE(store.importClips({items.front()}, u"空的"_s).duplicates, 1);
        QCOMPARE(groupNamed(store, u"空的"_s), qint64(0));

        // On disk the same.
        store.flush();
        ClipStore again(dir.path());
        QVERIFY(again.open(kStart));
        QCOMPARE(again.clips().size(), store.clips().size());
        QCOMPARE(textsIn(again, groupNamed(again, u"新来的"_s)), (QStringList {u"甲"_s, u"乙"_s}));
        QCOMPARE(again.clip(store.groupOrder(fresh).front())->created, kStart);
    }

    void dittoFixture()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        const QString file = dir.filePath(u"Ditto.db"_s);
        QImage picture(4, 3, QImage::Format_RGB32);
        picture.fill(QColor(200, 30, 40));
        const QByteArray html = "Version:0.9\r\nStartHTML:0000000105\r\n<html><body><b>粗体</b></body></html>";
        const QByteArray rtf = R"({\rtf1\ansi bold})";
        {
            DittoDb ditto(file);
            const double plain = DittoDb::kNotSticky;
            // Newest first by clipOrder. An older one with ANSI text only.
            ditto.clip(u"hello 世界"_s, 1'700'000'100, -1, 10, plain,
                {{u"CF_UNICODETEXT"_s, utf16(u"hello 世界"_s)}, {u"CF_TEXT"_s, "hello ?"}});
            ditto.clip(u"粗体"_s, 1'700'000'200, -1, 20, plain,
                {{u"CF_UNICODETEXT"_s, utf16(u"粗体"_s)}, {u"HTML Format"_s, html + '\0'}, {u"Rich Text Format"_s, rtf + '\0'}});
            ditto.clip(u"https://example.com/a"_s, 1'700'000'300, -1, 30, plain,
                {{u"CF_UNICODETEXT"_s, utf16(u"https://example.com/a"_s)}});
            ditto.clip(u"[Image]"_s, 1'700'000'400, -1, 40, plain, {{u"CF_DIB"_s, dibOf(picture)}});
            ditto.clip(u"C:\\a.txt"_s, 1'700'000'500, -1, 50, plain,
                {{u"CF_HDROP"_s, dropFiles({u"C:\\a.txt"_s, u"D:\\b c\\d.txt"_s})}});
            ditto.clip(u"hello 世界"_s, 1'700'000'600, -1, 60, plain, // the same again
                {{u"CF_UNICODETEXT"_s, utf16(u"hello 世界"_s)}});
            ditto.clip(u"   "_s, 1'700'000'700, -1, 70, plain, {{u"CF_UNICODETEXT"_s, utf16(u"   "_s)}}); // nothing
            ditto.clip(u"仅 ANSI"_s, 1'700'000'750, -1, 75, plain, {{u"CF_TEXT"_s, "ansi only\0"_ba}});
            const qint64 work = ditto.clip(u"工作"_s, 1'700'000'800, -1, 0, plain, {}, true);
            const qint64 sub = ditto.clip(u"子"_s, 1'700'000'810, work, 0, plain, {}, true);
            ditto.clip(u"组里一"_s, 1'700'000'900, work, 1, plain, {{u"CF_UNICODETEXT"_s, utf16(u"组里一"_s)}});
            ditto.clip(u"组里二（置顶）"_s, 1'700'000'910, work, 2, 5, {{u"CF_UNICODETEXT"_s, utf16(u"组里二（置顶）"_s)}});
            ditto.clip(u"嵌套的"_s, 1'700'000'920, sub, 1, plain, {{u"CF_UNICODETEXT"_s, utf16(u"嵌套的"_s)}});
        }

        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        store.setLimits({1, 1}, kStart + 1000 * kDay); // everything but groups would go
        const Report report = runImport(store, file);
        QCOMPARE(int(report.error), int(Error::None));
        QCOMPARE(report.added, 9);
        QCOMPARE(report.duplicates, 1);
        QCOMPARE(report.skipped, 1);
        QCOMPARE(groupName(file), u"Ditto 导入"_s);

        const qint64 own = groupNamed(store, u"Ditto 导入"_s);
        const qint64 work = groupNamed(store, u"工作"_s);
        const qint64 sub = groupNamed(store, u"工作/子"_s);
        QVERIFY(own && work && sub);
        QCOMPARE(store.clips().size(), std::size_t(9));
        // Ditto's order inside: newest (highest clipOrder) first.
        QCOMPARE(textsIn(store, own).size(), 6);
        QCOMPARE(textsIn(store, own).front(), u"ansi only"_s);
        QCOMPARE(textsIn(store, work), (QStringList {u"组里二（置顶）"_s, u"组里一"_s})); // pinned first
        QCOMPARE(textsIn(store, sub), (QStringList {u"嵌套的"_s}));

        const auto find = [&](const QString& text) -> const Clip* {
            for (const Clip& c : store.clips()) {
                if (c.text == text)
                    return &c;
            }
            return nullptr;
        };
        const Clip* hello = find(u"hello 世界"_s);
        QVERIFY(hello);
        QCOMPARE(hello->kind, ClipKind::Text);
        // The same text twice: the newer one's times (Ditto lists it first).
        QCOMPARE(hello->created, qint64(1'700'000'600) * 1000);
        QCOMPARE(hello->used, qint64(1'700'000'605) * 1000); // when last pasted
        QCOMPARE(hello->group, own);
        const Clip* bold = find(u"粗体"_s);
        QVERIFY(bold && bold->html && bold->rtf);
        QCOMPARE(store.payload(bold->id).html, html);
        QCOMPARE(store.payload(bold->id).rtf, rtf);
        QCOMPARE(find(u"https://example.com/a"_s)->kind, ClipKind::Link);
        const Clip* files = find(u"C:\\a.txt\nD:\\b c\\d.txt"_s);
        QVERIFY(files);
        QCOMPARE(files->kind, ClipKind::Files);

        const Clip* image = nullptr;
        for (const Clip& c : store.clips()) {
            if (c.kind == ClipKind::Image)
                image = &c;
        }
        QVERIFY(image);
        QCOMPARE(image->width, 4);
        QCOMPARE(image->height, 3);
        store.flush();
        const QImage png(store.imagePath(image->id));
        QCOMPARE(png.size(), QSize(4, 3));
        QCOMPARE(png.pixelColor(1, 1), QColor(200, 30, 40));
        QCOMPARE(image->bytes, QFileInfo(store.imagePath(image->id)).size());
        QCOMPARE(image->hash, imagePixelHash(png));

        // Importing again: all already there. The limits took nothing away.
        const Report again = runImport(store, file);
        QCOMPARE(again.added, 0);
        QCOMPARE(again.duplicates, 10);
        QCOMPARE(store.clips().size(), std::size_t(9));
        QCOMPARE(groupNamed(store, u"Ditto 导入 2"_s), qint64(0));
        store.setLimits({1, 1}, kStart + 2000 * kDay);
        QCOMPARE(store.clips().size(), std::size_t(9));

        // After a restart too.
        store.flush();
        ClipStore reopened(data.path());
        QVERIFY(reopened.open(kStart + 3000 * kDay));
        reopened.setLimits({1, 1}, kStart + 3000 * kDay);
        QCOMPARE(reopened.clips().size(), std::size_t(9));
    }

    // A database Ditto itself made (set WINSHUN_TEST_DITTO_DB; with rows added by a script).
    void realDittoDb()
    {
        const QString file = qEnvironmentVariable("WINSHUN_TEST_DITTO_DB");
        if (file.isEmpty())
            QSKIP("WINSHUN_TEST_DITTO_DB is not set");
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        const Report report = runImport(store, file);
        QCOMPARE(int(report.error), int(Error::None));
        QCOMPARE(report.added, 2);
        QCOMPARE(textsIn(store, groupNamed(store, u"Ditto 导入"_s)), (QStringList {u"beta"_s, u"alpha 真实库"_s}));
    }

    void dittoNotDitto()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        // A SQLite file with other tables.
        const QString other = dir.filePath(u"other.db"_s);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"other"_s);
            db.setDatabaseName(other);
            QVERIFY(db.open());
            QSqlQuery(db).exec(u"CREATE TABLE x (a)"_s);
        }
        QSqlDatabase::removeDatabase(u"other"_s);
        Report report = runImport(store, other);
        QCOMPARE(int(report.error), int(Error::NotDitto));
        QCOMPARE(report.added, 0);
        // Not a database at all, named like one.
        report = runImport(store, write(dir, u"fake.db"_s, "just words"));
        QCOMPARE(report.added, 1); // read as text
        QCOMPARE(int(report.error), int(Error::None));
        QCOMPARE(int(runImport(store, dir.filePath(u"missing.txt"_s)).error), int(Error::Missing));
    }

    void decoding()
    {
        QCOMPARE(decodeText("plain"), u"plain"_s);
        QCOMPARE(decodeText("\xEF\xBB\xBF" "中文"), u"中文"_s);
        QCOMPARE(decodeText(QString(u"中文").toUtf8()), u"中文"_s);
        QCOMPARE(decodeText("\xFF\xFE" + utf16(u"中文"_s, false)), u"中文"_s);
        QByteArray be = "\xFE\xFF";
        be += QByteArray("\x4E\x2D\x65\x87", 4); // 中文
        QCOMPARE(decodeText(be), u"中文"_s);
        QCOMPARE(decodeText("\xD6\xD0\xCE\xC4"), u"中文"_s); // GBK
        QCOMPARE(decodeText("\xB1\xEA\xCC\xE2,\xC4\xDA\xC8\xDD"), u"标题,内容"_s);
    }

    void splitting()
    {
        const QString text = u"一\r\n二\r\n\r\n\r\n  三\n四\n\n五  \n"_s;
        QCOMPARE(splitText(text, Split::Lines), (QStringList {u"一"_s, u"二"_s, u"  三"_s, u"四"_s, u"五  "_s}));
        QCOMPARE(splitText(text, Split::Paragraphs), (QStringList {u"一\r\n二"_s, u"  三\n四"_s, u"五"_s}));
        QCOMPARE(splitText(text, Split::Whole), (QStringList {u"一\r\n二\r\n\r\n\r\n  三\n四\n\n五"_s}));
        QVERIFY(splitText(u" \n\n \n"_s, Split::Whole).isEmpty());
        QVERIFY(splitText(QString(), Split::Paragraphs).isEmpty());
    }

    void textFile()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        const QString path = write(dir, u"笔记.txt"_s, "\xEF\xBB\xBF" "第一段\r\n还是第一段\r\n\r\nhttps://example.com\r\n\r\n\r\nC:\\x\\y.txt\r\n");
        Options options;
        options.split = Split::Paragraphs;
        Report report = runImport(store, path, options);
        QCOMPARE(report.added, 3);
        const qint64 group = groupNamed(store, u"笔记 导入"_s);
        QVERIFY(group);
        QCOMPARE(textsIn(store, group), (QStringList {u"第一段\r\n还是第一段"_s, u"https://example.com"_s, u"C:\\x\\y.txt"_s}));
        QCOMPARE(store.clip(store.groupOrder(group)[1])->kind, ClipKind::Link);
        QCOMPARE(store.clip(store.groupOrder(group)[2])->kind, ClipKind::Path);
        // The first is the newest, a second apart.
        QCOMPARE(store.clip(store.groupOrder(group)[0])->used, kStart);
        QCOMPARE(store.clip(store.groupOrder(group)[2])->used, kStart - 2000);
        // The same file again: all there; with other lines a second group of that name.
        report = runImport(store, path, options);
        QCOMPARE(report.added, 0);
        QCOMPARE(report.duplicates, 3);
        options.split = Split::Lines;
        report = runImport(store, path, options);
        QCOMPARE(report.added, 2); // the url and the path are there
        QCOMPARE(report.duplicates, 2);
        QVERIFY(groupNamed(store, u"笔记 导入 2"_s) != 0);
        options.split = Split::Whole;
        QCOMPARE(runImport(store, path, options).added, 1);
        QCOMPARE(groupName(path), u"笔记 导入"_s);
    }

    void csvParsing()
    {
        auto rows = parseCsv(u"a,b\r\n\"x, y\",\"he said \"\"hi\"\"\"\r\n\"line1\nline2\",z\r\n"_s);
        QCOMPARE(rows.size(), std::size_t(3));
        QCOMPARE(rows[1], (QStringList {u"x, y"_s, u"he said \"hi\""_s}));
        QCOMPARE(rows[2], (QStringList {u"line1\nline2"_s, u"z"_s}));
        rows = parseCsv(u"a;b\n1;2"_s);
        QCOMPARE(rows[1], (QStringList {u"1"_s, u"2"_s}));
        rows = parseCsv(u"a\tb\n1\t2\n\n3\t4"_s);
        QCOMPARE(rows.size(), std::size_t(3)); // the blank line is no row
        QCOMPARE(rows[2], (QStringList {u"3"_s, u"4"_s}));
        QVERIFY(parseCsv(QString()).empty());
        rows = parseCsv(u"\"unclosed,1\n2"_s); // does not hang or lose the text
        QCOMPARE(rows.size(), std::size_t(1));
    }

    void csvFiles()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));

        // A header names the text and the time columns.
        const QString withHeader = write(dir, u"a.csv"_s,
            "id,time,\xE5\x86\x85\xE5\xAE\xB9\r\n"
            "1,2026-01-02 03:04:05,\"first, with comma\"\r\n"
            "2,1700000000,\"two\r\nlines\"\r\n"
            "3,,\r\n"
            "4,not a time,last\r\n");
        Report report = runImport(store, withHeader);
        QCOMPARE(report.added, 3);
        QCOMPARE(report.skipped, 1);
        const qint64 group = groupNamed(store, u"a 导入"_s);
        QCOMPARE(textsIn(store, group), (QStringList {u"first, with comma"_s, u"two\r\nlines"_s, u"last"_s}));
        const std::vector<qint64> order = store.groupOrder(group);
        QCOMPARE(store.clip(order[0])->created, QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5)).toMSecsSinceEpoch());
        QCOMPARE(store.clip(order[1])->created, qint64(1'700'000'000'000));
        QCOMPARE(store.clip(order[2])->created, kStart - 3000); // no time: by its place in the file (the 4th row)

        // No header: the first column, the first row is data.
        const QString plain = write(dir, u"b.csv"_s, "alpha,1\nbeta,2\n");
        report = runImport(store, plain);
        QCOMPARE(report.added, 2);
        QCOMPARE(textsIn(store, groupNamed(store, u"b 导入"_s)), (QStringList {u"alpha"_s, u"beta"_s}));

        // GBK, as Chinese Excel saves it; "text" in a later column.
        const QString gbk = write(dir, u"c.csv"_s, "\xB1\xEA\xCC\xE2,Text\n1,\xD6\xD0\xCE\xC4\n");
        report = runImport(store, gbk);
        QCOMPARE(report.added, 1);
        QCOMPARE(textsIn(store, groupNamed(store, u"c 导入"_s)), (QStringList {u"中文"_s}));

        // Only a time column named: the text is the first of the others.
        const QString onlyTime = write(dir, u"d.csv"_s, "Date,words\n2026-03-04,hi there\n");
        QCOMPARE(runImport(store, onlyTime).added, 1);
        QCOMPARE(textsIn(store, groupNamed(store, u"d 导入"_s)), (QStringList {u"hi there"_s}));
    }

    // The way the app does it: read on a thread of its own, each batch into the
    // store on this one.
    void jobOnAThread()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        QByteArray lines;
        for (int i = 0; i < 700; ++i)
            lines += "entry " + QByteArray::number(i) + "\n";
        const QString path = write(dir, u"job.txt"_s, lines);
        ClipImportJob job(&store);
        QSignalSpy finished(&job, &ClipImportJob::finished);
        job.start(path, Split::Lines);
        QCOMPARE(job.group(), u"job 导入"_s);
        QVERIFY(finished.wait(20000));
        const auto report = finished.first().first().value<clipimport::Report>();
        QCOMPARE(report.added, 700);
        QCOMPARE(int(report.error), int(Error::None));
        QCOMPARE(store.groupSize(groupNamed(store, u"job 导入"_s)), 700);
    }

    // Gone while reading: no hang, nothing left running.
    void jobDestroyedEarly()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        QByteArray lines;
        for (int i = 0; i < 5000; ++i)
            lines += "entry " + QByteArray::number(i) + "\n";
        const QString path = write(dir, u"early.txt"_s, lines);
        {
            ClipImportJob job(&store);
            job.start(path, Split::Lines);
        } // the batches it asked for are dropped with it
        QVERIFY(store.clips().size() <= 5000);
    }

    void progressAndCancel()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        QByteArray lines;
        for (int i = 0; i < 450; ++i)
            lines += "line " + QByteArray::number(i) + "\n";
        const QString path = write(dir, u"many.txt"_s, lines);
        Hooks hooks;
        int batches = 0;
        int lastDone = 0;
        hooks.sink = [&](std::vector<ClipImportItem>&& items) {
            ++batches;
            return store.importClips(std::move(items), u"many"_s);
        };
        hooks.progress = [&](int done, int total) {
            QCOMPARE(total, 450);
            lastDone = done;
        };
        Options options;
        options.split = Split::Lines;
        Report report = importFile(path, options, hooks);
        QCOMPARE(report.added, 450);
        QCOMPARE(batches, 3); // 200, 200, 50
        QCOMPARE(lastDone, 450);
        QCOMPARE(store.groupSize(groupNamed(store, u"many"_s)), 450);
        QCOMPARE(store.groupOrder(groupNamed(store, u"many"_s)).size(), std::size_t(450));

        // Cancelled after the first batch.
        int asked = 0;
        hooks.cancelled = [&] { return ++asked > 250; };
        hooks.sink = [&](std::vector<ClipImportItem>&& items) { return store.importClips(std::move(items), u"cut"_s); };
        lines.replace("line", "Line"); // other entries
        report = importFile(write(dir, u"more.txt"_s, lines), options, hooks);
        QVERIFY(report.cancelled);
        QVERIFY(report.added < 450);
        QVERIFY(report.added > 0);
    }
};

QTEST_GUILESS_MAIN(ClipImportTest)
#include "tst_clipimport.moc"
