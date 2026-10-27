#include "preview/imagemetadataprovider.h"
#include "preview/sandboxedpreviewprocess.h"
#include "platform/cloudfilestatus.h"

#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QTimeZone>

namespace filemanager::preview {

ImageMetadataProvider::ImageMetadataProvider(QObject* parent)
    : QObject(parent)
{
}

QString ImageMetadataProvider::cacheKey(const QFileInfo& info) const
{
    return info.absoluteFilePath() + '|' + QString::number(info.size()) + '|'
        + QString::number(info.lastModified(QTimeZone::UTC).toMSecsSinceEpoch());
}

std::optional<QSize> ImageMetadataProvider::requestDimensions(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > MaximumSourceBytes
            || !filemanager::platform::isLocallyPreviewableFile(path))
        return std::nullopt;
    // Reuse this request's fresh stat data; creating another QFileInfo would
    // repeat filesystem work even when the dimensions are already cached.
    const QString key = cacheKey(info);
    if (const QSize* cached = dimensions_.object(key)) {
        if (cached->isValid())
            return *cached;
        return std::nullopt;
    }
    if (pending_.contains(key))
        return std::nullopt;

    pending_.insert(key);
    queue_.enqueue({key, info.absoluteFilePath()});
    startNext();
    return std::nullopt;
}

void ImageMetadataProvider::startNext()
{
    if (queue_.isEmpty() || activeRequests_ >= MaximumActiveRequests)
        return;
    const Request request = queue_.dequeue();
    ++activeRequests_;
    QPointer<ImageMetadataProvider> guard(this);
    QThreadPool::globalInstance()->start([guard, request] {
        QSize dimensions;
        // Query installed formats without opening the file. All source-byte
        // decoding stays in the restricted helper.
        if (QImageReader::supportedImageFormats().contains(
                QFileInfo(request.path).suffix().toLatin1().toLower())) {
            const auto helper = runSandboxedPreviewHost(request.path,
                QStringLiteral("image-metadata"), 6000, 16 * 1024);
            if (helper.success && helper.sandboxed && helper.jobRestricted) {
                QJsonParseError parseError;
                const QJsonDocument reply = QJsonDocument::fromJson(
                    helper.standardOutput, &parseError);
                if (parseError.error == QJsonParseError::NoError && reply.isObject()) {
                    const QJsonObject object = reply.object();
                    const int width = object.value(QStringLiteral("width")).toInt();
                    const int height = object.value(QStringLiteral("height")).toInt();
                    if (width > 0 && height > 0 && width <= 32768 && height <= 32768)
                        dimensions = QSize(width, height);
                }
            }
        }
        if (guard) {
            QMetaObject::invokeMethod(guard, [guard, request, dimensions] {
                if (!guard)
                    return;
                --guard->activeRequests_;
                guard->pending_.remove(request.key);
                // Cache rejections too, so repeated paints do not reschedule
                // text files or corrupt images. Size/mtime changes get a new key.
                guard->dimensions_.insert(request.key, new QSize(dimensions));
                QTimer::singleShot(0, guard, &ImageMetadataProvider::startNext);
                emit guard->metadataReady(request.path);
            });
        }
    });
}

} // namespace filemanager::preview
