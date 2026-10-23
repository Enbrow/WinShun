// Explicit desktop integration test: WINSHUN_TEST_NATIVE_DIALOG=1 tst_filepaste.
// Opens only its own file picker, selects temporary files, and closes it.
#include "ClipFiles.h"
#include "platform/FileDialog.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <chrono>
#include <thread>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using namespace Microsoft::WRL;

namespace {

class FillDialog : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IFileDialogEvents> {
public:
    QString names;
    bool filled = false;
    bool openDialog = false;
    bool started = false;
    std::jthread worker;

    IFACEMETHODIMP OnFolderChange(IFileDialog* dialog) override
    {
        if (started)
            return S_OK;
        ComPtr<IOleWindow> window;
        HWND hwnd = nullptr;
        if (FAILED(dialog->QueryInterface(IID_PPV_ARGS(&window))) || FAILED(window->GetWindow(&hwnd)))
            return E_FAIL;
        started = true;
        worker = std::jthread([this, hwnd] {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            // Let the picker finish constructing its controls and enter Show's
            // message loop, which receives setFileName's window messages.
            while (std::chrono::steady_clock::now() < deadline) {
                openDialog = ws::filedialog::kind(hwnd) == ws::filedialog::Kind::Open;
                if (openDialog && ws::filedialog::setFileName(hwnd, names.toStdWString())) {
                    filled = true;
                    ws::filedialog::accept(hwnd); // only this test's local picker
                    break;
                }
                std::this_thread::sleep_for(20ms);
            }
            while (::IsWindow(hwnd) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(20ms);
            if (::IsWindow(hwnd))
                ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
        });
        return S_OK;
    }
    IFACEMETHODIMP OnFileOk(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP OnSelectionChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE*) override { return S_OK; }
    IFACEMETHODIMP OnTypeChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE*) override { return S_OK; }
};

} // namespace

class FilePasteTest : public QObject {
    Q_OBJECT
private slots:
    void uploadPicker_data()
    {
        QTest::addColumn<int>("count");
        QTest::newRow("one Unicode path with spaces") << 1;
        QTest::newRow("multiple paths from different folders") << 2;
    }
    void uploadPicker()
    {
        if (!qEnvironmentVariableIsSet("WINSHUN_TEST_NATIVE_DIALOG"))
            QSKIP("Set WINSHUN_TEST_NATIVE_DIALOG=1 to exercise a real Windows file picker");
        QFETCH(int, count);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QStringList paths;
        for (int i = 0; i < count; ++i) {
            const QString folder = dir.filePath(QString::number(i));
            QVERIFY(QDir().mkpath(folder));
            const QString path = folder + u"/附件 甲.txt"_s;
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("test attachment");
            paths.append(QDir::toNativeSeparators(path));
        }
        QVERIFY(SUCCEEDED(::OleInitialize(nullptr)));
        {
            ComPtr<IFileOpenDialog> dialog;
            QVERIFY(SUCCEEDED(
                ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))));
            FILEOPENDIALOGOPTIONS options;
            QVERIFY(SUCCEEDED(dialog->GetOptions(&options)));
            QVERIFY(SUCCEEDED(
                dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_DONTADDTORECENT | FOS_FORCEFILESYSTEM)));
            dialog->SetTitle(L"WinShun upload paste regression test");
            ComPtr<IShellItem> folder;
            const auto nativeFolder = QDir::toNativeSeparators(dir.path()).toStdWString();
            QVERIFY(SUCCEEDED(::SHCreateItemFromParsingName(nativeFolder.c_str(), nullptr, IID_PPV_ARGS(&folder))));
            dialog->SetFolder(folder.Get());
            auto fill = Make<FillDialog>();
            fill->names = ws::clipfiles::fileNames(paths);
            DWORD cookie = 0;
            QVERIFY(SUCCEEDED(dialog->Advise(fill.Get(), &cookie)));
            const HRESULT result = dialog->Show(nullptr);
            if (fill->worker.joinable())
                fill->worker.join();
            dialog->Unadvise(cookie);
            QVERIFY(fill->openDialog);
            QVERIFY(fill->filled);
            QVERIFY2(SUCCEEDED(result), qPrintable(QString::number(result, 16)));
            ComPtr<IShellItemArray> items;
            QVERIFY(SUCCEEDED(dialog->GetResults(&items)));
            DWORD total = 0;
            items->GetCount(&total);
            QCOMPARE(total, DWORD(count));
            for (DWORD i = 0; i < total; ++i) {
                ComPtr<IShellItem> item;
                QVERIFY(SUCCEEDED(items->GetItemAt(i, &item)));
                PWSTR name = nullptr;
                QVERIFY(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)));
                const QString selected = QString::fromWCharArray(name);
                ::CoTaskMemFree(name);
                QCOMPARE(selected, paths[i]);
            }
            QCOMPARE(
                QDir(dir.path()).entryList(QDir::Files).size(), 0); // no accidental file paste into the shown folder
        }
        ::OleUninitialize();
    }
};

QTEST_GUILESS_MAIN(FilePasteTest)
#include "tst_filepaste.moc"
