#include "Crawler.h"
#include "FileIndex.h"
#include "Favorites.h"
#include "History.h"
#include "NameSearch.h"
#include "PathText.h"
#include "Query.h"
#include "ShareIndexer.h"
#include "Snapshot.h"
#include "Unc.h"
#include "WorkerPool.h"
#include "Wsl.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStringEncoder>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>

using namespace ws;
using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

class WslTest : public QObject {
    Q_OBJECT

private slots:
    void paths()
    {
        QCOMPARE(
            unc::normalize(L"\\\\WSL$\\Ubuntu\\home\\Dev\\"), std::wstring(L"\\\\wsl.localhost\\Ubuntu\\home\\Dev"));
        QCOMPARE(unc::wslDistribution(L"\\\\wsl.localhost\\Ubuntu\\home"), std::wstring_view(L"Ubuntu"));
        QVERIFY(unc::wslDistribution(L"\\\\nas\\Ubuntu\\home").empty());
        QVERIFY(unc::wslDistribution(L"\\\\wsl.localhost.attacker\\Ubuntu").empty());
        QVERIFY(unc::contains(L"\\\\wsl$\\Ubuntu\\home", L"\\\\WSL.LOCALHOST\\ubuntu\\home\\Dev"));
        QVERIFY(!unc::contains(L"\\\\wsl$\\Ubuntu\\home", L"\\\\wsl.localhost\\Ubuntu\\Home\\Dev"));
        QVERIFY(!unc::contains(L"\\\\wsl$\\Ubuntu\\home", L"\\\\wsl.localhost\\Ubuntu\\home2"));
        QVERIFY(!unc::equal(L"\\\\wsl$\\Ubuntu\\home\\Dev", L"\\\\wsl.localhost\\Ubuntu\\home\\dev"));
        QVERIFY(unc::equal(L"\\\\NAS\\DOCS", L"\\\\nas\\docs"));
        QCOMPARE(pathtext::pathFromText(u"\\\\wsl$\\Ubuntu\\home\\Dev\\"_s), u"\\\\wsl.localhost\\Ubuntu\\home\\Dev"_s);
    }

    void cliNames()
    {
        const QString names = u"Ubuntu-24.04\r\nDebian\r\n测试\r\n"_s;
        const auto expected = std::vector<std::wstring> {L"Ubuntu-24.04", L"Debian", L"测试"};
        QCOMPARE(wsl::parseNames(names.toUtf8()), expected);
        const QByteArray wide = QStringEncoder(QStringEncoder::Utf16LE)(names);
        QCOMPARE(wsl::parseNames(wide), expected);
        QCOMPARE(wsl::parseNames(QByteArray("\xFF\xFE", 2) + wide), expected);
        QVERIFY(wsl::parseNames(QByteArray()).empty());
        const QByteArray emptyWide = QStringEncoder(QStringEncoder::Utf16LE)(u"\r\n"_s);
        QVERIFY(wsl::parseNames(emptyWide).empty());
    }

    void distinctCaseAndSnapshot()
    {
        FileIndex index;
        const auto root = ShareIndexer::ensureRoot(index, L"\\\\wsl$\\Ubuntu\\home");
        const auto upper = index.add(root, "Dev", EntryFlag::Directory | EntryFlag::Remote);
        const auto lower = index.add(root, "dev", EntryFlag::Directory | EntryFlag::Remote);
        const auto one = index.add(upper, "README.md", EntryFlag::Remote);
        const auto two = index.add(upper, "readme.md", EntryFlag::Remote);
        index.add(lower, "other.md", EntryFlag::Remote);
        QCOMPARE(index.findPath(L"\\\\wsl$\\Ubuntu\\home\\Dev\\README.md"), one);
        QCOMPARE(index.findPath(L"\\\\wsl.localhost\\UBUNTU\\home\\Dev\\readme.md"), two);
        QCOMPARE(index.findPath(L"\\\\wsl.localhost\\Ubuntu\\home\\DEV"), kNoEntry);
        QCOMPARE(index.pathForUpdate(L"\\\\wsl.localhost\\Ubuntu\\home\\Dev\\readme.md"), two);
        // Both the linked-list and the hash-table lookup must keep Linux case.
        for (int i = 0; i < 600; ++i)
            index.add(upper, "file" + std::to_string(i), EntryFlag::Remote);
        index.childForUpdate(upper, "not-here", true);
        QCOMPARE(index.findPath(L"\\\\wsl$\\Ubuntu\\home\\Dev\\README.md"), one);
        QCOMPARE(index.pathForUpdate(L"\\\\wsl$\\Ubuntu\\home\\Dev\\readme.md"), two);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString file = temp.filePath(u"index.bin"_s);
        QVERIFY(snapshot::save(index, {}, {}, {}, file));
        const auto loaded = snapshot::load(file);
        QVERIFY(loaded && loaded->index);
        QVERIFY(loaded->index->findPath(L"\\\\wsl$\\Ubuntu\\home\\Dev\\README.md")
            != loaded->index->findPath(L"\\\\wsl$\\Ubuntu\\home\\Dev\\readme.md"));
    }

    void caseSensitiveReconcile()
    {
        FileIndex index;
        const std::wstring path = L"\\\\wsl.localhost\\Ubuntu\\home";
        const auto root = ShareIndexer::ensureRoot(index, path);
        const auto old = index.add(root, "Readme", EntryFlag::Remote);
        const Crawler crawler(CrawlRules {});
        const auto list = [&](const Crawler::Root& dir, DirListing& out) {
            out.clear();
            crawler.addToListing(out, dir, "README", {}, 0, false, 0, 0);
            crawler.addToListing(out, dir, "Readme", {}, 0, false, 0, 0);
            return true;
        };
        QVERIFY(crawler.sync(index, {{root, path, EntryFlag::Remote}}, 1, false, {}, list));
        QCOMPARE(index.findPath(path + L"\\Readme"), old);
        QVERIFY(index.findPath(path + L"\\README") != old);
        QCOMPARE(index.liveCount(), std::size_t {3});
    }

    void favoritesAndHistoryKeepCase()
    {
        const QString upper = u"\\\\wsl.localhost\\Ubuntu\\home\\README"_s;
        const QString lower = u"\\\\wsl.localhost\\Ubuntu\\home\\readme"_s;
        const QString alias = u"\\\\wsl$\\UBUNTU\\home\\README"_s;
        QVERIFY(pathtext::samePath(upper, alias));
        QVERIFY(!pathtext::samePath(upper, lower));
        Favorite a;
        a.path = upper;
        Favorite b;
        b.path = lower;
        const FavoriteList favorites {a, b};
        QCOMPARE(indexOfFavorite(favorites, lower), qsizetype {1});
        QCOMPARE(indexOfFavorite(favorites, alias), qsizetype {0});
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        History history(temp.filePath(u"history.txt"_s));
        history.record(upper, 1000);
        history.record(lower, 2000);
        history.record(alias, 3000);
        QCOMPARE(history.items().size(), 2);
        QCOMPARE(history.use(upper).count, 2);
        QCOMPARE(history.use(lower).count, 1);
        history.remove(lower);
        QCOMPARE(history.items().size(), 1);
        QVERIFY(pathtext::samePath(history.items().front(), upper));
    }

    void stoppedDistroDoesNotStart()
    {
        const auto distros = wsl::distributions();
        if (distros.empty())
            QSKIP("WSL is not installed");
        const std::wstring path = L"\\\\wsl.localhost\\" + distros.front().name + L"\\tmp";
        if (wsl::probe(path, true) != wsl::State::Stopped)
            QSKIP("This test needs an already stopped distro; it never stops a user's distro");
        auto index = std::make_shared<FileIndex>();
        const auto root = ShareIndexer::ensureRoot(*index, path);
        index->add(root, "cached.txt", EntryFlag::Remote);
        ShareIndexer::Options options;
        options.shares = {{path, {}}};
        ShareIndexer service({[&] { return index; }, [] { return true; }}, options, {});
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(
            !service.statuses().empty() && service.statuses().front().problem == share::Problem::WslStopped, 6000);
        QVERIFY(service.offline(path + L"\\cached.txt"));
        service.stop();
        QCOMPARE(wsl::probe(path, true), wsl::State::Stopped);
        QVERIFY(index->findPath(path + L"\\cached.txt") != kNoEntry);
        WorkerPool pool(2);
        NameMatcher matcher(parseQuery(u"cached"_s));
        const auto found = searchNames(*index, matcher, {}, pool, [] { return false; });
        QCOMPARE(found.totalMatches, std::size_t {1});
    }

    void realWslFolder()
    {
        // Explicit opt-in: starts WSL, writes only a unique temporary directory.
        const QString distro = qEnvironmentVariable("WINSHUN_TEST_WSL");
        if (distro.isEmpty())
            QSKIP("Set WINSHUN_TEST_WSL to a registered distro to run the real filesystem test");
        const QString distroRoot = u"\\\\wsl.localhost\\"_s + distro;
        qInfo() << "Initial WSL state:" << static_cast<int>(wsl::probe(distroRoot.toStdWString(), true));
        QProcess keeper;
        const auto stopKeeper = qScopeGuard([&] {
            if (keeper.state() != QProcess::NotRunning) {
                keeper.kill(); // our own CLI, never the user's distribution
                keeper.waitForFinished(2000);
            }
        });
        keeper.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
        keeper.start(wsl::executable(), {u"--distribution"_s, distro, u"--exec"_s, u"/bin/sleep"_s, u"90"_s});
        QVERIFY(keeper.waitForStarted(3000));
        QTRY_COMPARE_WITH_TIMEOUT(wsl::probe(distroRoot.toStdWString(), true), wsl::State::Running, 15'000);
        const auto linux = [&](const QString& program, QStringList args, QByteArray* output = nullptr) {
            args.prepend(program);
            args.prepend(u"--exec"_s);
            args.prepend(distro);
            args.prepend(u"--distribution"_s);
            QProcess process;
            process.setCreateProcessArgumentsModifier(
                [](QProcess::CreateProcessArguments* p) { p->flags |= CREATE_NO_WINDOW; });
            process.start(wsl::executable(), args);
            const bool success = process.waitForFinished(15'000) && process.exitCode() == 0;
            if (!success)
                qWarning().noquote() << program << process.errorString()
                                     << QString::fromUtf8(process.readAllStandardError());
            if (output)
                *output = process.readAllStandardOutput();
            return success;
        };
        // Linux creates the fixture in its own home directory. /tmp can be
        // cleaned or remounted during a distro's cold start; do not infer that
        // a successful Windows CreateDirectory made it visible to Linux.
        QByteArray output;
        QVERIFY(linux(u"/usr/bin/printenv"_s, {u"HOME"_s}, &output));
        const QString home = QString::fromUtf8(output).trimmed();
        QVERIFY(home.startsWith(u'/') && !home.contains(u'\n'));
        QVERIFY(linux(u"/usr/bin/mktemp"_s, {u"-d"_s, home + u"/.winshun-wsl-XXXXXX"_s}, &output));
        const QString linuxPath = QString::fromUtf8(output).trimmed();
        // Only the freshly created fixture may be removed, on success or failure.
        QVERIFY(linuxPath.startsWith(home + u"/.winshun-wsl-"_s));
        QVERIFY(QRegularExpression(u"/\\.winshun-wsl-[A-Za-z0-9]{6}$"_s).match(linuxPath).hasMatch());
        auto cleanup = qScopeGuard([&] { linux(u"/bin/rm"_s, {u"-rf"_s, u"--"_s, linuxPath}); });
        QString native = distroRoot + linuxPath;
        native.replace(u'/', u'\\');
        qInfo().noquote() << "WSL test directory:" << native;
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(native), 15'000);
        const std::wstring path = native.toStdWString();
        QVERIFY(linux(u"/bin/mkdir"_s, {linuxPath + u"/项目 资料"_s}));
        QVERIFY(linux(u"/usr/bin/touch"_s,
            {linuxPath + u"/README.md"_s, linuxPath + u"/readme.md"_s, linuxPath + u"/项目 资料/年度报告.txt"_s}));
        QVERIFY(linux(u"/bin/ln"_s, {u"-s"_s, u".."_s, linuxPath + u"/项目 资料/loop"_s}));

        auto index = std::make_shared<FileIndex>();
        ShareIndexer::Options options;
        options.shares = {{path, {}}};
        ShareIndexer service({[&] { return index; }, [] { return true; }}, options, {});
        const auto online = [&] {
            const auto states = service.statuses();
            return !states.empty() && states.front().state == ShareIndexer::State::Online;
        };
        const auto has = [&](const std::wstring& suffix) {
            const auto lock = index->readLock();
            return index->findPath(path + suffix) != kNoEntry;
        };
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(online(), 15'000);
        QVERIFY(has(L"\\README.md") && has(L"\\readme.md") && has(L"\\项目 资料\\年度报告.txt"));
        {
            const auto lock = index->readLock();
            QVERIFY(index->liveCount() < 20); // the symlink back to the parent was not traversed
            WorkerPool pool(2);
            NameMatcher matcher(parseQuery(u"ndbg"_s));
            const auto found = searchNames(*index, matcher, {}, pool, [] { return false; });
            QCOMPARE(found.totalMatches, std::size_t {1});
        }
        QVERIFY(
            linux(u"/bin/mv"_s, {linuxPath + u"/项目 资料/年度报告.txt"_s, linuxPath + u"/项目 资料/已更新.txt"_s}));
        QVERIFY(linux(u"/bin/rm"_s, {linuxPath + u"/readme.md"_s}));
        QVERIFY(linux(u"/usr/bin/touch"_s, {linuxPath + u"/new.txt"_s}));
        // No manual verify: exercise the actual scheduled WSL reconciliation.
        QTRY_VERIFY_WITH_TIMEOUT(has(L"\\new.txt") && has(L"\\项目 资料\\已更新.txt") && !has(L"\\readme.md")
                && !has(L"\\项目 资料\\年度报告.txt"),
            45'000);
        QVERIFY(has(L"\\README.md")); // deleting the other spelling must not delete this one
        service.stop();
        QVERIFY(linux(u"/bin/rm"_s, {u"-rf"_s, u"--"_s, linuxPath}));
        cleanup.dismiss();
        QVERIFY(!QFileInfo::exists(native));
    }
};

QTEST_GUILESS_MAIN(WslTest)
#include "tst_wsl.moc"
