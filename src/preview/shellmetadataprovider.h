#pragma once

#include "preview/mediametadataprovider.h"

#include <QCache>
#include <QObject>
#include <QSet>

#include <optional>

namespace filemanager::preview {

class ShellMetadataProvider final : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 MaximumSourceBytes = 8LL * 1024 * 1024 * 1024;
    explicit ShellMetadataProvider(QObject* parent = nullptr);

    std::optional<MediaMetadata> request(const QString& path);

signals:
    void metadataReady(const QString& path);

private:
    QString cacheKey(const QString& path) const;

    QCache<QString, MediaMetadata> cache_{256};
    QSet<QString> pending_;
};

} // namespace filemanager::preview
