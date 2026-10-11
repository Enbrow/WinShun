#include "FileDropTarget.h"

#include <QCoreApplication>
#include <QUrl>

#include <windows.h>
// shellapi.h and ole2.h after windows.h
#include <ole2.h>
#include <shellapi.h>

#include <string>

namespace ws {

namespace {

constexpr UINT kCopyGlobalData = 0x0049; // WM_COPYGLOBALDATA: how WM_DROPFILES carries its data across

} // namespace

FileDropTarget::FileDropTarget(QObject* parent)
    : QObject(parent)
{
    QCoreApplication::instance()->installNativeEventFilter(this);
}

FileDropTarget::~FileDropTarget()
{
    if (QCoreApplication* app = QCoreApplication::instance())
        app->removeNativeEventFilter(this);
}

void FileDropTarget::setWindow(QWindow* window)
{
    if (m_window == window)
        return;
    if (m_window)
        disconnect(m_window, nullptr, this, nullptr);
    m_window = window;
    if (m_window) {
        connect(m_window, &QWindow::visibleChanged, this, [this](bool visible) {
            if (visible)
                accept();
        });
        if (m_window->isVisible())
            accept();
    }
    emit windowChanged();
}

void FileDropTarget::accept()
{
    if (!m_window)
        return;
    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    // Qt's OLE drop target would be asked first, and an OLE drag from
    // Explorer never gets to it; without one, Explorer sends WM_DROPFILES.
    ::RevokeDragDrop(hwnd);
    ::DragAcceptFiles(hwnd, TRUE);
    for (const UINT message : {static_cast<UINT>(WM_DROPFILES), static_cast<UINT>(WM_COPYDATA), kCopyGlobalData})
        ::ChangeWindowMessageFilterEx(hwnd, message, MSGFLT_ALLOW, nullptr);
}

bool FileDropTarget::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    if (eventType != "windows_generic_MSG" || !m_window)
        return false;
    const auto* msg = static_cast<const MSG*>(message);
    if (msg->message != WM_DROPFILES || msg->hwnd != reinterpret_cast<HWND>(m_window->winId()))
        return false;
    const auto drop = reinterpret_cast<HDROP>(msg->wParam);
    QVariantList urls;
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(length, L'\0');
        if (length == 0 || ::DragQueryFileW(drop, i, path.data(), length + 1) != length)
            continue;
        urls.append(QUrl::fromLocalFile(QString::fromStdWString(path)));
    }
    POINT at {};
    ::DragQueryPoint(drop, &at); // in the window's client area, device pixels
    ::DragFinish(drop);
    const qreal ratio = m_window->devicePixelRatio();
    if (!urls.isEmpty())
        emit dropped(urls, at.x / ratio, at.y / ratio);
    if (result) // none for a posted message, which WM_DROPFILES is
        *result = 0;
    return true;
}

} // namespace ws
