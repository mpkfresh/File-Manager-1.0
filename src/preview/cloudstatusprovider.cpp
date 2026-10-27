#include "preview/cloudstatusprovider.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

namespace filemanager::preview {

CloudStatusProvider::CloudStatusProvider(QObject* parent)
    : QObject(parent)
{
}

QString CloudStatusProvider::cacheKey(const QString& path) const
{
    const QFileInfo info(path);
    return info.absoluteFilePath() + '|' + QString::number(info.size()) + '|'
        + QString::number(info.lastModified().toMSecsSinceEpoch()) + '|'
        + QString::number(info.metadataChangeTime().toMSecsSinceEpoch());
}

std::optional<filemanager::platform::CloudFileStatus>
CloudStatusProvider::request(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return std::nullopt;
    const QString key = cacheKey(path);
    if (const auto* cached = cache_.object(key))
        return *cached;
    if (pending_.contains(key))
        return std::nullopt;

    pending_.insert(key);
    const QString absolutePath = info.absoluteFilePath();
    emit inspectionStarted(absolutePath);
    QPointer<CloudStatusProvider> guard(this);
    QThreadPool::globalInstance()->start([guard, key, absolutePath] {
        const auto status = filemanager::platform::inspectCloudFileStatus(absolutePath);
        if (guard) {
            QMetaObject::invokeMethod(guard,
                [guard, key, absolutePath, status] {
                    if (!guard)
                        return;
                    guard->pending_.remove(key);
                    guard->cache_.insert(key,
                        new filemanager::platform::CloudFileStatus(status));
                    emit guard->statusReady(absolutePath);
                });
        }
    });
    return std::nullopt;
}

} // namespace filemanager::preview
