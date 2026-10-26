#include "platform/Integrity.h"
#include "platform/FileManagers.h"
#include "platform/Shell.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <objbase.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <memory>

using namespace Qt::StringLiterals;

namespace {
QString parentProgram()
{
    const ws::win32::UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry {sizeof entry};
    DWORD parent = 0;
    if (::Process32FirstW(snapshot.get(), &entry)) {
        do {
            if (entry.th32ProcessID == ::GetCurrentProcessId()) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (::Process32NextW(snapshot.get(), &entry));
    }
    const ws::win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent));
    wchar_t path[32768] {};
    DWORD size = static_cast<DWORD>(std::size(path));
    return process.valid() && ::QueryFullProcessImageNameW(process.get(), 0, path, &size)
        ? QString::fromWCharArray(path, size) : QString();
}
} // namespace

class TestShellLaunch : public QObject {
    Q_OBJECT
private slots:
    void oneCommanderLookalike()
    {
        // WPF's window class is shared by unrelated apps. Even a matching
        // assembly name and title must not be enough to identify a manager.
        constexpr const wchar_t* name = L"HwndWrapper[OneCommander.exe;;WinShunTest]";
        WNDCLASSW wc {};
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = name;
        QVERIFY(::RegisterClassW(&wc));
        auto unregister = qScopeGuard([&] { ::UnregisterClassW(name, wc.hInstance); });
        const HWND window = ::CreateWindowExW(
            0, name, L"OneCommander", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        QVERIFY(window);
        auto destroy = qScopeGuard([&] { ::DestroyWindow(window); });
        QVERIFY(!ws::filemanager::kindOf(window));
        QVERIFY(!ws::filemanager::kindOf(nullptr));
    }

    void oneCommanderNavigation_data()
    {
        QTest::addColumn<int>("model");
        QTest::newRow("production-STA") << int(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        QTest::newRow("MTA") << int(COINIT_MULTITHREADED);
    }

    // Opt-in: the HWND must belong to an isolated OneCommander copy. Uses
    // the same live active-pane query as Ctrl+G and the dialog suggestions.
    void oneCommanderNavigation()
    {
        bool valid = false;
        const qulonglong handle = qEnvironmentVariable("WINSHUN_ONECOMMANDER_TEST_WINDOW").toULongLong(&valid);
        if (!valid || !handle)
            QSKIP("Set WINSHUN_ONECOMMANDER_TEST_WINDOW to an isolated OneCommander main window");
        const HWND window = reinterpret_cast<HWND>(static_cast<quintptr>(handle));
        QVERIFY(ws::filemanager::kindOf(window) == ws::filemanager::Kind::OneCommander);
        QFETCH(int, model);
        const HRESULT com = ::CoInitializeEx(nullptr, static_cast<DWORD>(model));
        QVERIFY(SUCCEEDED(com));
        auto uninitialize = qScopeGuard([] { ::CoUninitialize(); });
        const std::wstring original = ws::filemanager::frontFolder(window);
        QVERIFY(!original.empty());
        QTemporaryDir temporary(QDir::tempPath() + u"/WinShun OneCommander-XXXXXX"_s);
        QVERIFY(temporary.isValid());
        const QString child = temporary.filePath(u"中文 空格 John's 文件夹"_s);
        QVERIFY(QDir().mkpath(child));
        const auto native = [](const QString& path) { return QDir::toNativeSeparators(path).toStdWString(); };
        const auto shows = [&](const std::wstring& path) {
            return QDir::fromNativeSeparators(QString::fromStdWString(ws::filemanager::frontFolder(window)))
                == QDir::fromNativeSeparators(QString::fromStdWString(path));
        };
        auto restore
            = qScopeGuard([&] { ws::filemanager::goToInFront(ws::filemanager::Kind::OneCommander, original); });
        QVERIFY(ws::filemanager::installed(ws::filemanager::Kind::OneCommander));
        QVERIFY(ws::filemanager::goToInFront(ws::filemanager::Kind::OneCommander, native(child)));
        QTRY_VERIFY_WITH_TIMEOUT(shows(native(child)), 5000);
        const DWORD clipboard = ::GetClipboardSequenceNumber();
        const auto folders = ws::filemanager::openFolders();
        QCOMPARE(::GetClipboardSequenceNumber(), clipboard);
        const auto found = std::ranges::find_if(folders, [&](const ws::filemanager::Folder& folder) {
            return folder.shownIn == ws::filemanager::Kind::OneCommander && folder.path == native(child);
        });
        QVERIFY(found != folders.end());
        QCOMPARE(folders.front().path, native(child));
        QCOMPARE(std::ranges::count_if(
                     folders, [&](const ws::filemanager::Folder& folder) { return folder.path == found->path; }),
            1);

        QFile file(temporary.filePath(u"选中 中文.txt"_s));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QVERIFY(ws::filemanager::reveal(ws::filemanager::Kind::OneCommander, {native(file.fileName())}));
        QTRY_VERIFY_WITH_TIMEOUT(shows(native(temporary.path())), 5000);
        QVERIFY(ws::filemanager::reveal(ws::filemanager::Kind::OneCommander, {native(child)}));
        QTRY_VERIFY_WITH_TIMEOUT(shows(native(temporary.path())), 5000);
        QFile nested(child + u"/另一个.txt"_s);
        QVERIFY(nested.open(QIODevice::WriteOnly));
        nested.close();
        QVERIFY(ws::filemanager::reveal(
            ws::filemanager::Kind::OneCommander, {native(file.fileName()), native(nested.fileName())}));
        QTRY_VERIFY_WITH_TIMEOUT(shows(native(child)), 5000);
        QVERIFY(ws::filemanager::openFolder(ws::filemanager::Kind::OneCommander, native(child)));
        QTRY_VERIFY_WITH_TIMEOUT(shows(native(child)), 5000);
        QVERIFY(ws::filemanager::goToInFront(ws::filemanager::Kind::OneCommander, L"C:\\"));
        QTRY_VERIFY_WITH_TIMEOUT(shows(L"C:\\"), 5000);
        QVERIFY(ws::filemanager::goToInFront(ws::filemanager::Kind::OneCommander, original));
        QTRY_VERIFY_WITH_TIMEOUT(shows(original), 5000);
        restore.dismiss();
    }

    void customCommandViaExplorer()
    {
        if (!::GetShellWindow())
            QSKIP("No desktop shell in this test session");
        QTemporaryDir temporary(QDir::tempPath() + u"/WinShun launch-XXXXXX"_s);
        QVERIFY(temporary.isValid());
        // Explorer uses its own PATH, not CTest's Qt PATH. Stage only this
        // helper and its Qt DLLs so the test also works on a clean build.
        const QString helper = temporary.filePath(u"launch-probe.exe"_s);
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), helper));
        const QDir qtBin(QLibraryInfo::path(QLibraryInfo::BinariesPath));
        for (const auto* module : {"Core", "Gui", "Sql", "Test"}) {
            const QString dll = u"Qt6"_s + QString::fromLatin1(module)
                + (QLibraryInfo::isDebugBuild() ? u"d.dll"_s : u".dll"_s);
            QVERIFY(QFile::copy(qtBin.filePath(dll), temporary.filePath(dll)));
        }
        const QString working = temporary.filePath(u"工作 folder"_s);
        QVERIFY(QDir().mkpath(working));
        const QString output = temporary.filePath(u"result.json"_s);
        const QString query = u"中文 \"引号\" C:\\末尾\\ {query} %TEMP% & text"_s;
        const QString command = u'"' + helper
            + u"\" --probe \""_s + output + u"\" {query}"_s;
        const auto done = std::make_shared<std::atomic<int>>(-1);
        ws::shell::runCustom(command, working, query, [done](bool ok) { done->store(ok ? 1 : 0); });
        QTRY_VERIFY_WITH_TIMEOUT(done->load() >= 0, 10000);
        QCOMPARE(done->load(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output), 10000);
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject result = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY2(result.value(u"parent"_s).toString().endsWith(u"\\explorer.exe", Qt::CaseInsensitive),
            qPrintable(result.value(u"parent"_s).toString()));
        QVERIFY(!result.value(u"elevated"_s).toBool());
        QCOMPARE(QDir::cleanPath(result.value(u"cwd"_s).toString()), QDir::cleanPath(working));
        QCOMPARE(result.value(u"arguments"_s).toArray().last().toString(), query);
        qInfo() << "Launched by" << result.value(u"parent"_s).toString() << "with the requested arguments and folder";
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 4 && args[1] == u"--probe") {
        const QJsonObject result {
            {u"parent"_s, parentProgram()}, {u"cwd"_s, QDir::currentPath()},
            {u"elevated"_s, ws::integrity::processElevated()},
            {u"arguments"_s, QJsonArray::fromStringList(args)},
        };
        QFile file(args[2]);
        return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(result).toJson()) > 0 ? 0 : 1;
    }
    TestShellLaunch test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_shelllaunch.moc"
