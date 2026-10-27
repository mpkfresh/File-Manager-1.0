#include "thumbnailprovider.h"
#include "preview/previewcontroller.h"
#include "platform/cloudfilestatus.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>

namespace filemanager::preview {

ThumbnailProvider::ThumbnailProvider(QObject* parent)
    : QObject(parent)
{
}

QString ThumbnailProvider::cacheKey(const QString& path, const QSize& size) const
{
    const QFileInfo info(path);
    return path + '|' + QString::number(info.size()) + '|'
        + QString::number(info.lastModified().toMSecsSinceEpoch()) + '|'
        + QString::number(size.width()) + 'x' + QString::number(size.height());
}

std::optional<QIcon> ThumbnailProvider::request(const QString& path, const QSize& logicalSize)
{
    // Reject filename-only non-candidates before cache-key metadata reads. Keep the full
    // existing checks below for every possibly matching image, including ambiguous names.
    const QMimeDatabase mimeDatabase;
    const auto candidates = mimeDatabase.mimeTypesForFileName(path);
    if (std::none_of(candidates.cbegin(), candidates.cend(), [](const QMimeType& type) {
            return type.name().startsWith("image/");
        }))
        return std::nullopt;
    // Metadata-only availability preflight precedes thumbnail cache-key reads.
    if (!filemanager::platform::isLocallyPreviewableFile(path))
        return std::nullopt;
    const QString key = cacheKey(path, logicalSize);
    if (const QImage* cached = cache_.object(key)) {
        // A null image records a completed rejection. Keep the model's fallback
        // icon without launching another helper on each repaint.
        if (cached->isNull())
            return std::nullopt;
        return QIcon(QPixmap::fromImage(*cached));
    }
    if (pending_.contains(key))
        return std::nullopt;
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > MaximumSourceBytes)
        return std::nullopt;
    if (!mimeDatabase.mimeTypeForFile(info, QMimeDatabase::MatchExtension)
            .name().startsWith("image/"))
        return std::nullopt;

    pending_.insert(key);
    QPointer<ThumbnailProvider> guard(this);
    const QString absolutePath = info.absoluteFilePath();
    QThreadPool::globalInstance()->start([guard, key, absolutePath, logicalSize] {
        auto isolated = PreviewController::loadIsolated(absolutePath);
        // Windows can time out the first isolated helper launch in a fresh
        // profile while loading its runtime. Keep the per-process boundary
        // unchanged and give that one transient failure a bounded retry.
        if (!isolated.success
                && isolated.error == QStringLiteral("The isolated preview helper timed out.")) {
            isolated = PreviewController::loadIsolated(absolutePath);
        }
        QImage image;
        if (isolated.success && isolated.preview.kind == PreviewKind::Image
                && !isolated.preview.image.isNull()) {
            image = isolated.preview.image.scaled(logicalSize * 2,
                Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QString error = isolated.error;
        if (image.isNull() && error.isEmpty()) {
            error = isolated.preview.text.isEmpty()
                ? QStringLiteral("The preview helper did not return a usable thumbnail.")
                : isolated.preview.text;
        }
        if (guard) {
            QMetaObject::invokeMethod(guard, [guard, key, absolutePath, image, error] {
                if (!guard)
                    return;
                guard->pending_.remove(key);
                // Cache failures within the same bounded cache as successes.
                // A source size/mtime change produces a fresh key and retries.
                guard->cache_.insert(key, new QImage(image));
                if (image.isNull())
                    emit guard->thumbnailFailed(absolutePath, error);
                if (guard)
                    emit guard->thumbnailReady(absolutePath);
            });
        }
    });
    return std::nullopt;
}

} // namespace filemanager::preview
