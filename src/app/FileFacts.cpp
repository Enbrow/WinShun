#include "FileFacts.h"

#include <QDateTime>
#include <QDir>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// When `path` was created, local time; empty when it cannot be read.
QString createdOf(const QString& path)
{
    QString native = QDir::toNativeSeparators(path);
    if (native.size() >= MAX_PATH && !native.startsWith(u"\\\\"_s))
        native.prepend(u"\\\\?\\"_s);
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!::GetFileAttributesExW(reinterpret_cast<LPCWSTR>(native.utf16()), GetFileExInfoStandard, &data))
        return {};
    const qint64 ticks = (qint64(data.ftCreationTime.dwHighDateTime) << 32) | data.ftCreationTime.dwLowDateTime;
    if (ticks <= 0)
        return {};
    constexpr qint64 kEpochTicks = 116444736000000000LL; // 1601 to 1970, in 100 ns
    const qint64 ms = (ticks - kEpochTicks) / 10000;
    return QDateTime::fromMSecsSinceEpoch(ms).toLocalTime().toString(u"yyyy-MM-dd HH:mm"_s);
}

// A drive letter mapped to a share ("Z:"): as slow as the share when it is
// gone, and the exit waits for what is being read.
bool onNetworkDrive(const QString& path)
{
    if (path.size() < 2 || path[1] != u':')
        return false;
    const wchar_t root[] {static_cast<wchar_t>(path[0].unicode()), L':', L'\\', L'\0'};
    return ::GetDriveTypeW(root) == DRIVE_REMOTE;
}

} // namespace

FileFacts::FileFacts(QObject* parent)
    : QObject(parent)
{
    m_pool.setMaxThreadCount(1); // one hover at a time; a second waits a moment
    m_pool.setExpiryTimeout(10000);
}

FileFacts::~FileFacts()
{
    m_pool.waitForDone(); // local files only (see the header): a moment at most
}

void FileFacts::ask(const QString& path)
{
    if (path.isEmpty() || path.startsWith(u"\\\\"_s) || path.startsWith(u"//"_s) || onNetworkDrive(path))
        return;
    if (const auto it = m_known.constFind(path); it != m_known.cend()) {
        if (!it->isEmpty())
            emit created(path, *it);
        return;
    }
    if (m_asking.contains(path))
        return;
    m_asking.insert(path);
    m_pool.start([this, path] {
        const QString when = createdOf(path);
        QMetaObject::invokeMethod(this, [this, path, when] { answer(path, when); }, Qt::QueuedConnection);
    });
}

void FileFacts::answer(const QString& path, const QString& when)
{
    m_asking.remove(path);
    if (m_known.size() > 4096)
        m_known.clear();
    m_known.insert(path, when);
    if (!when.isEmpty())
        emit created(path, when);
}

} // namespace ws
