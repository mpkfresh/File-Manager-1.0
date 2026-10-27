#include "files/networkdiscoveryservice.h"

#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QThreadPool>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winnetwk.h>
#endif

namespace filemanager::files {
namespace {

#ifdef Q_OS_WIN
void enumerateResources(NETRESOURCEW* parent, int depth,
    const std::shared_ptr<std::atomic_bool>& cancellation,
    QVector<NetworkResource>& output, QSet<QString>& seen, QString& error)
{
    if (depth > NetworkDiscoveryService::MaximumDepth || cancellation->load()
        || output.size() >= NetworkDiscoveryService::MaximumResources)
        return;
    HANDLE enumeration = nullptr;
    const DWORD opened = WNetOpenEnumW(RESOURCE_GLOBALNET, RESOURCETYPE_DISK,
        RESOURCEUSAGE_ALL, parent, &enumeration);
    if (opened != NO_ERROR) {
        if (!parent && opened != ERROR_NO_NETWORK)
            error = QStringLiteral("Windows network enumeration failed (%1).").arg(opened);
        return;
    }
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (!cancellation->load() && output.size() < NetworkDiscoveryService::MaximumResources) {
        DWORD count = 0xFFFFFFFF;
        DWORD bytes = static_cast<DWORD>(buffer.size());
        const DWORD status = WNetEnumResourceW(
            enumeration, &count, buffer.data(), &bytes);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status == ERROR_MORE_DATA && bytes > static_cast<DWORD>(buffer.size())
            && bytes <= 1024 * 1024) {
            buffer.resize(static_cast<qsizetype>(bytes));
            continue;
        }
        if (status != NO_ERROR) {
            if (error.isEmpty())
                error = QStringLiteral("Windows network enumeration failed (%1).").arg(status);
            break;
        }
        auto* entries = reinterpret_cast<NETRESOURCEW*>(buffer.data());
        for (DWORD index = 0; index < count && !cancellation->load(); ++index) {
            NETRESOURCEW& entry = entries[index];
            const QString remote = entry.lpRemoteName
                ? QString::fromWCharArray(entry.lpRemoteName) : QString();
            const QString comment = entry.lpComment
                ? QString::fromWCharArray(entry.lpComment) : QString();
            const QString provider = entry.lpProvider
                ? QString::fromWCharArray(entry.lpProvider) : QString();
            const bool container = (entry.dwUsage & RESOURCEUSAGE_CONTAINER) != 0;
            const QString identity = remote.toCaseFolded() + '|'
                + QString::number(entry.dwDisplayType) + '|' + comment.toCaseFolded();
            if (!identity.trimmed().isEmpty() && !seen.contains(identity)) {
                seen.insert(identity);
                output.append({remote, comment.isEmpty() ? remote : comment, provider, container});
                if (output.size() >= NetworkDiscoveryService::MaximumResources)
                    break;
            }
            if (container)
                enumerateResources(&entry, depth + 1, cancellation, output, seen, error);
        }
    }
    WNetCloseEnum(enumeration);
}
#endif

} // namespace

NetworkDiscoveryService::NetworkDiscoveryService(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<QVector<NetworkResource>>();
}

NetworkDiscoveryService::~NetworkDiscoveryService()
{
    cancel();
}

void NetworkDiscoveryService::start()
{
    if (running_)
        return;
    running_ = true;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    QPointer<NetworkDiscoveryService> guard(this);
    QThreadPool::globalInstance()->start([guard, cancellation] {
        QVector<NetworkResource> resources;
        QString error;
#ifdef Q_OS_WIN
        QSet<QString> seen;
        enumerateResources(nullptr, 0, cancellation, resources, seen, error);
#else
        error = QStringLiteral("Network discovery is available only on Windows.");
#endif
        if (guard) {
            QMetaObject::invokeMethod(guard, [guard, cancellation, resources, error] {
                if (!guard || guard->cancellation_ != cancellation)
                    return;
                guard->running_ = false;
                emit guard->finished(resources,
                    cancellation->load() ? QStringLiteral("Scan cancelled.") : error);
            });
        }
    });
}

void NetworkDiscoveryService::cancel()
{
    if (cancellation_)
        cancellation_->store(true);
    running_ = false;
}

} // namespace filemanager::files
