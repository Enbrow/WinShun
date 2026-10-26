#include "LauncherNavigation.h"

#include <QtTest>

using namespace ws;
using namespace ws::navigation;
using namespace Qt::StringLiterals;

class LauncherNavigationTest : public QObject {
    Q_OBJECT

private slots:
    void favoritesKeepScopeByDefault()
    {
        SearchResult folder {.name = u"Work"_s, .path = u"C:\\Work"_s, .isDir = true, .favorite = true};
        for (const Scope scope : {Scope::All, Scope::Files, Scope::Folders, Scope::Content}) {
            QCOMPARE(tabAction(scope, false, false, &folder, false), TabAction::SwitchScope);
            QCOMPARE(tabAction(scope, false, false, nullptr, false), TabAction::SwitchScope);
            folder.favorite = false;
            QCOMPARE(tabAction(scope, false, false, &folder, false), TabAction::SwitchScope);
            folder.favorite = true;
        }
    }

    void favoriteBrowsingIsExplicitAndScoped()
    {
        SearchResult folder {.path = u"C:\\Work"_s, .isDir = true, .favorite = true};
        QCOMPARE(tabAction(Scope::All, false, true, &folder, false), TabAction::BrowseFolder);
        // Turning the setting off applies to the same selected row at once.
        QCOMPARE(tabAction(Scope::All, false, false, &folder, false), TabAction::SwitchScope);
        for (const Scope scope : {Scope::Files, Scope::Folders, Scope::Content})
            QCOMPARE(tabAction(scope, false, true, &folder, false), TabAction::SwitchScope);
        folder.favorite = false;
        QVERIFY(canBrowseFolder(folder)); // an ordinary folder still has the menu action
        QCOMPARE(tabAction(Scope::All, false, true, &folder, false), TabAction::SwitchScope);
        folder.favorite = true;
        folder.isDir = false;
        QCOMPARE(tabAction(Scope::All, false, true, &folder, false), TabAction::SwitchScope);
        QVERIFY(!canBrowseFolder(folder));
        QCOMPARE(tabAction(Scope::All, false, true, nullptr, false), TabAction::SwitchScope);
    }

    void unavailableFoldersAreNotEntered()
    {
        SearchResult folder {.path = u"\\\\server\\share"_s, .isDir = true, .favorite = true};
        folder.offline = true;
        QVERIFY(!canBrowseFolder(folder));
        QCOMPARE(tabAction(Scope::All, false, true, &folder, false), TabAction::SwitchScope);
        folder.offline = false;
        folder.missing = true;
        QVERIFY(!canBrowseFolder(folder));
        QCOMPARE(tabAction(Scope::All, false, true, &folder, false), TabAction::SwitchScope);
        folder.missing = false;
        folder.app = AppKind::Desktop;
        QVERIFY(!canBrowseFolder(folder));
        QCOMPARE(tabAction(Scope::All, false, true, &folder, false), TabAction::SwitchScope);
    }

    void completionKeepsItsOwnBehavior()
    {
        const SearchResult folder {.path = u"C:\\Work"_s, .isDir = true, .favorite = true};
        for (const bool enterFolder : {false, true}) {
            for (const Scope scope : {Scope::All, Scope::Files, Scope::Folders, Scope::Content}) {
                QCOMPARE(tabAction(scope, true, enterFolder, &folder, true), TabAction::Complete);
                QCOMPARE(tabAction(scope, true, enterFolder, &folder, false), TabAction::None);
            }
            // A web suggestion or keyword hint outside keyword mode still completes.
            QCOMPARE(tabAction(Scope::All, false, enterFolder, nullptr, true), TabAction::Complete);
        }
        QCOMPARE(tabAction(Scope::All, false, false, &folder, true), TabAction::Complete);
        QCOMPARE(tabAction(Scope::All, false, true, &folder, true), TabAction::BrowseFolder);
    }
};

QTEST_GUILESS_MAIN(LauncherNavigationTest)
#include "tst_launchernavigation.moc"
