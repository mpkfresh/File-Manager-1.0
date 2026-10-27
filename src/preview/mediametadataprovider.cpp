#include "preview/mediametadataprovider.h"
#include "preview/sandboxedpreviewprocess.h"
#include "platform/cloudfilestatus.h"

#include <QFileInfo>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

namespace filemanager::preview {
namespace {

bool isSupportedMedia(const QString& suffix)
{
    static const QSet<QString> supported = {
        "aac", "flac", "m4a", "m4v", "mkv", "mov", "mp3", "mp4", "ogg",
        "opus", "wav", "webm", "wma", "wmv"};
    return supported.contains(suffix.toLower());
}

} // namespace

QString MediaMetadata::durationText() const
{
    if (durationMs <= 0)
        return {};
    const qint64 totalSeconds = durationMs / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    return hours > 0
        ? QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0'))
              .arg(seconds, 2, 10, QLatin1Char('0'))
        : QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

MediaMetadataProvider::MediaMetadataProvider(QObject* parent)
    : QObject(parent)
{
}

QString MediaMetadataProvider::cacheKey(const QString& path) const
{
    const QFileInfo info(path);
    return info.absoluteFilePath() + '|' + QString::number(info.size()) + '|'
        + QString::number(info.lastModified().toMSecsSinceEpoch());
}

std::optional<MediaMetadata> MediaMetadataProvider::request(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > MaximumSourceBytes || !isSupportedMedia(info.suffix())
            || !filemanager::platform::isLocallyPreviewableFile(path))
        return std::nullopt;
    const QString key = cacheKey(path);
    if (const MediaMetadata* cached = cache_.object(key))
        return *cached;
    if (pending_.contains(key) || pending_.size() >= MaximumPendingRequests)
        return std::nullopt;
    pending_.insert(key);
    queue_.enqueue({key, info.absoluteFilePath()});
    startNext();
    return std::nullopt;
}

void MediaMetadataProvider::startNext()
{
    if (active_ || queue_.isEmpty())
        return;
    current_ = queue_.dequeue();
    active_ = true;
    const Request request = current_;
    QPointer<MediaMetadataProvider> guard(this);
    QThreadPool::globalInstance()->start([guard, request] {
        MediaMetadata metadata;
        const auto helper = runSandboxedPreviewHost(request.path, "media", 6000,
            1024 * 1024);
        if (helper.success) {
            const QByteArray payload = helper.standardOutput;
            QJsonParseError error;
            const QJsonDocument document = payload.size() <= 1024 * 1024
                ? QJsonDocument::fromJson(payload, &error) : QJsonDocument{};
            if (error.error == QJsonParseError::NoError && document.isObject()) {
                const QJsonObject object = document.object();
                metadata.durationMs = object.value("durationMs").toInteger();
                metadata.title = object.value("title").toString().left(32768);
                metadata.artists = object.value("artists").toString().left(32768);
                const int width = object.value("width").toInt();
                const int height = object.value("height").toInt();
                if (width > 0 && height > 0 && width <= 32768 && height <= 32768)
                    metadata.resolution = QSize(width, height);
            }
        }
        if (guard) QMetaObject::invokeMethod(guard, [guard, metadata] {
            if (guard) guard->finishCurrent(metadata);
        });
    });
}

void MediaMetadataProvider::finishCurrent(const MediaMetadata& metadata)
{
    if (!active_)
        return;
    const Request finished = current_;
    pending_.remove(finished.key);
    cache_.insert(finished.key, new MediaMetadata(metadata));
    active_ = false;
    emit metadataReady(finished.path);
    QTimer::singleShot(0, this, &MediaMetadataProvider::startNext);
}

} // namespace filemanager::preview
