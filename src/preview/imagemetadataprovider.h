#pragma once

#include <QCache>
#include <QFileInfo>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QSize>

#include <optional>

namespace filemanager::preview {

class ImageMetadataProvider final : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 MaximumSourceBytes = 64 * 1024 * 1024;
    explicit ImageMetadataProvider(QObject* parent = nullptr);

    std::optional<QSize> requestDimensions(const QString& path);

signals:
    void metadataReady(const QString& path);

private:
    struct Request {
        QString key;
        QString path;
    };
    static constexpr int MaximumActiveRequests = 4;
    QString cacheKey(const QFileInfo& info) const;
    void startNext();

    QCache<QString, QSize> dimensions_{512};
    QSet<QString> pending_;
    QQueue<Request> queue_;
    int activeRequests_ = 0;
};

} // namespace filemanager::preview
