#pragma once

#include <QCache>
#include <QIcon>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QSize>

#include <optional>

namespace filemanager::preview {

class ThumbnailProvider final : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 MaximumSourceBytes = 64 * 1024 * 1024;
    explicit ThumbnailProvider(QObject* parent = nullptr);

    std::optional<QIcon> request(const QString& path, const QSize& logicalSize);

signals:
    void thumbnailReady(const QString& path);
    void thumbnailFailed(const QString& path, const QString& error);

private:
    QString cacheKey(const QString& path, const QSize& size) const;

    QCache<QString, QImage> cache_{128};
    QSet<QString> pending_;
};

} // namespace filemanager::preview
