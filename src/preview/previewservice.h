#pragma once

#include <QImage>
#include <QDateTime>
#include <QString>

class QIODevice;

namespace filemanager::preview {

enum class PreviewKind { None, Folder, Image, Text, Unsupported, Error };

struct PreviewData
{
    PreviewKind kind = PreviewKind::None;
    QString title;
    QString metadata;
    QString text;
    QImage image;
};

class PreviewService final
{
public:
    static constexpr qint64 MaximumTextBytes = 128 * 1024;
    static constexpr qint64 MaximumImageFileBytes = 64 * 1024 * 1024;
    static constexpr int MaximumImageDimension = 2048;
    static constexpr qint64 MaximumImagePixels = 100'000'000;
    static constexpr qint64 MaximumArchivePreviewSourceBytes = 512LL * 1024 * 1024;
    static constexpr int MaximumArchivePreviewEntries = 1024;
    static constexpr int MaximumArchiveDisplayedEntries = 100;
    static constexpr int MaximumArchiveEntryNameBytes = 1024;

    PreviewData load(const QString& path) const;
    PreviewData load(QIODevice* source, const QString& fileName, qint64 fileSize,
        const QDateTime& modified = {}) const;
};

} // namespace filemanager::preview
