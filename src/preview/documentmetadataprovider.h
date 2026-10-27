#pragma once

#include "files/sourcesnapshot.h"

#include <QCache>
#include <QObject>
#include <QSet>

#include <atomic>
#include <optional>

class QIODevice;

namespace filemanager::preview {

struct DocumentMetadata
{
    int pageCount = 0;
    QString title;
    QString authors;
};

struct SelectedDocumentMetadata
{
    DocumentMetadata metadata;
    QString error;
    bool available = false;
};

// Called only by the restricted preview host with its inherited read-only
// source handle. The main app must not parse document bytes itself.
DocumentMetadata readDocumentMetadataFromDevice(QIODevice* source,
    const QString& fileName, qint64 sourceSize);

// Bounded synchronous review for an explicitly selected local document. The
// caller supplies its snapshot; a changed file invalidates the result. File
// bytes are decoded only in the restricted preview host.
SelectedDocumentMetadata readSelectedDocumentMetadata(const QString& path,
    const filemanager::files::SourceSnapshot& expected,
    const std::atomic_bool* cancelled = nullptr);

class DocumentMetadataProvider final : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 MaximumSourceBytes = 128 * 1024 * 1024;
    static constexpr qint64 MaximumReadBytes = 2 * 1024 * 1024;
    static constexpr int MaximumArchiveEntries = 256;
    static constexpr qint64 MaximumMetadataEntryBytes = 256 * 1024;
    static constexpr qint64 MaximumMetadataTotalBytes = 512 * 1024;
    static constexpr qint64 MaximumExpansionRatio = 64;
    explicit DocumentMetadataProvider(QObject* parent = nullptr);

    std::optional<DocumentMetadata> request(const QString& path);

signals:
    void metadataReady(const QString& path);

private:
    QString cacheKey(const QString& path) const;

    QCache<QString, DocumentMetadata> cache_{256};
    QSet<QString> pending_;
};

} // namespace filemanager::preview
