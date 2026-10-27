#pragma once

#include <QCache>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QSize>

#include <optional>

namespace filemanager::preview {

struct MediaMetadata
{
    qint64 durationMs = 0;
    QString title;
    QString artists;
    QSize resolution;

    QString durationText() const;
};

class MediaMetadataProvider final : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 MaximumSourceBytes = 8LL * 1024 * 1024 * 1024;
    static constexpr int MaximumPendingRequests = 128;
    explicit MediaMetadataProvider(QObject* parent = nullptr);

    std::optional<MediaMetadata> request(const QString& path);

signals:
    void metadataReady(const QString& path);

private:
    struct Request { QString key; QString path; };
    QString cacheKey(const QString& path) const;
    void startNext();
    void finishCurrent(const MediaMetadata& metadata);

    QCache<QString, MediaMetadata> cache_{256};
    QSet<QString> pending_;
    QQueue<Request> queue_;
    Request current_;
    bool active_ = false;
};

} // namespace filemanager::preview
