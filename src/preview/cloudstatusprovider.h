#pragma once

#include "platform/cloudfilestatus.h"

#include <QCache>
#include <QObject>
#include <QSet>

#include <optional>

namespace filemanager::preview {

class CloudStatusProvider final : public QObject
{
    Q_OBJECT

public:
    explicit CloudStatusProvider(QObject* parent = nullptr);

    std::optional<filemanager::platform::CloudFileStatus> request(const QString& path);

signals:
    void inspectionStarted(const QString& path);
    void statusReady(const QString& path);

private:
    QString cacheKey(const QString& path) const;

    QCache<QString, filemanager::platform::CloudFileStatus> cache_{2048};
    QSet<QString> pending_;
};

} // namespace filemanager::preview
