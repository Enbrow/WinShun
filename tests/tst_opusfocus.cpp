#include "platform/FolderViewHit.h"

#include <QtTest>

class OpusFocusTest : public QObject {
    Q_OBJECT

private slots:
    void onlyTheFileDisplay()
    {
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        const wchar_t* classes[] {
            L"dopus.lister", L"dopus.filedisplaycontainer", L"dopus.filedisplay", L"WinShun.Other"};
        for (const wchar_t* name : classes) {
            WNDCLASSW windowClass {};
            windowClass.lpfnWndProc = ::DefWindowProcW;
            windowClass.hInstance = instance;
            windowClass.lpszClassName = name;
            QVERIFY(::RegisterClassW(&windowClass));
        }
        const auto create = [instance](const wchar_t* name, HWND parent = nullptr) {
            return ::CreateWindowExW(
                0, name, L"", parent ? WS_CHILD : WS_OVERLAPPED, 0, 0, 100, 100, parent, nullptr, instance, nullptr);
        };
        const HWND root = create(L"dopus.lister");
        const HWND other = create(L"WinShun.Other");
        const HWND container = create(L"dopus.filedisplaycontainer", root);
        const HWND view = create(L"dopus.filedisplay", container);
        const HWND rename = create(L"Edit", view);
        const HWND address = create(L"Edit", container);
        const HWND fakeView = create(L"dopus.filedisplay", root); // wrong parent
        QVERIFY(root && other && container && view && rename && address && fakeView);
        QVERIFY(ws::folderview::isOpusLister(root));
        QVERIFY(!ws::folderview::isOpusLister(other));
        QVERIFY(ws::folderview::isOpusFileDisplay(root, view));
        for (const HWND focus : {root, container, rename, address, fakeView, HWND(nullptr)})
            QVERIFY(!ws::folderview::isOpusFileDisplay(root, focus));
        QVERIFY(!ws::folderview::isOpusFileDisplay(other, view));
        ::DestroyWindow(root); // all its children, without ever showing or activating them
        ::DestroyWindow(other);
        for (const wchar_t* name : classes)
            QVERIFY(::UnregisterClassW(name, instance));
    }
};

QTEST_GUILESS_MAIN(OpusFocusTest)
#include "tst_opusfocus.moc"
