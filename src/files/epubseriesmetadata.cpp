#include "epubseriesmetadata.h"

#include "filenamevalidator.h"
#include "scopeenumerator.h"
#include "sourcesnapshot.h"
#include "platform/nativefilecopy.h"

#include <QByteArray>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

extern "C" {
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
}

namespace filemanager::files {
namespace {

constexpr qint64 MaximumSourceBytes = 128 * 1024 * 1024;
constexpr int MaximumArchiveEntries = 256;
constexpr qint64 MaximumMetadataEntryBytes = 256 * 1024;
constexpr qint64 MaximumMetadataTotalBytes = 512 * 1024;
constexpr qint64 MaximumExpansionRatio = 64;
const QString containerNamespace = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:container");
const QString packageNamespace = QStringLiteral("http://www.idpf.org/2007/opf");
const QString dcNamespace = QStringLiteral("http://purl.org/dc/elements/1.1/");

bool isCancelled(const std::atomic_bool* cancelled)
{
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

EpubSeriesMetadata cancelledMetadata()
{
    EpubSeriesMetadata result;
    result.error = "EPUB metadata inspection was cancelled.";
    result.blocksFilenameFallback = true;
    return result;
}

bool archiveShapeIsSafe(void* reader, const std::atomic_bool* cancelled)
{
    QSet<QByteArray> names;
    int entries = 0;
    int32_t status = mz_zip_reader_goto_first_entry(reader);
    while (status == MZ_OK) {
        if (isCancelled(cancelled))
            return false;
        if (++entries > MaximumArchiveEntries)
            return false;
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(reader, &info) != MZ_OK || !info || !info->filename
                || info->uncompressed_size < 0 || info->compressed_size < 0
                || info->uncompressed_size > MaximumSourceBytes)
            return false;
        const QByteArray name(info->filename);
        if (names.contains(name))
            return false; // A duplicate OPF/container entry is ambiguous.
        names.insert(name);
        const qint64 compressed = qMax<qint64>(1, info->compressed_size);
        if (info->uncompressed_size > compressed * MaximumExpansionRatio)
            return false;
        status = mz_zip_reader_goto_next_entry(reader);
    }
    return !isCancelled(cancelled) && status == MZ_END_OF_LIST;
}

QByteArray readMetadataEntry(void* reader, const QByteArray& name, qint64& totalBytes,
    const std::atomic_bool* cancelled)
{
    if (isCancelled(cancelled) || mz_zip_reader_locate_entry(reader, name.constData(), 0) != MZ_OK)
        return {};
    mz_zip_file* info = nullptr;
    if (mz_zip_reader_entry_get_info(reader, &info) != MZ_OK || !info
            || (info->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0
            || (info->compression_method != MZ_COMPRESS_METHOD_STORE
                && info->compression_method != MZ_COMPRESS_METHOD_DEFLATE)
            || info->uncompressed_size < 0
            || info->uncompressed_size > MaximumMetadataEntryBytes
            || totalBytes + info->uncompressed_size > MaximumMetadataTotalBytes)
        return {};
    if (mz_zip_reader_entry_open(reader) != MZ_OK)
        return {};
    QByteArray bytes;
    bytes.reserve(static_cast<int>(info->uncompressed_size));
    char buffer[16 * 1024];
    int32_t read = 0;
    bool safe = true;
    while (!isCancelled(cancelled)
            && (read = mz_zip_reader_entry_read(reader, buffer, sizeof(buffer))) > 0) {
        if (bytes.size() + read > MaximumMetadataEntryBytes) {
            safe = false;
            break;
        }
        bytes.append(buffer, read);
    }
    const int32_t closeStatus = mz_zip_reader_entry_close(reader);
    if (isCancelled(cancelled) || read < 0 || closeStatus != MZ_OK)
        safe = false;
    if (!safe)
        return {};
    totalBytes += bytes.size();
    return bytes;
}

QString firstPackagePath(const QByteArray& xml)
{
    QXmlStreamReader reader(xml);
    QString packagePath;
    int depth = 0;
    bool validRoot = false;
    bool inRootfiles = false;
    bool sawRootfile = false;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD)
            return {};
        if (token == QXmlStreamReader::StartElement) {
            if (depth == 0) {
                validRoot = reader.name() == QStringLiteral("container")
                    && reader.namespaceUri() == containerNamespace;
            } else if (validRoot && depth == 1
                    && reader.name() == QStringLiteral("rootfiles")
                    && reader.namespaceUri() == containerNamespace) {
                inRootfiles = true;
            } else if (validRoot && inRootfiles && depth == 2
                    && reader.name() == QStringLiteral("rootfile")
                    && reader.namespaceUri() == containerNamespace
                    && !sawRootfile) {
                sawRootfile = true;
                if (reader.attributes().value(QStringLiteral("media-type"))
                        == QStringLiteral("application/oebps-package+xml")) {
                    packagePath = reader.attributes().value(QStringLiteral("full-path")).toString();
                }
            }
            ++depth;
        } else if (token == QXmlStreamReader::EndElement) {
            --depth;
            if (depth == 1)
                inRootfiles = false;
        }
    }
    if (reader.hasError() || !validRoot || depth != 0 || packagePath.size() > 255
            || !packagePath.endsWith(".opf", Qt::CaseInsensitive))
        return {};
    static const QRegularExpression safePath(QStringLiteral(R"(^[A-Za-z0-9._/-]+$)"));
    if (!safePath.match(packagePath).hasMatch() || packagePath.startsWith('/')
            || packagePath.contains("//"))
        return {};
    for (const QString& segment : packagePath.split('/')) {
        if (segment.isEmpty() || segment == "." || segment == "..")
            return {};
    }
    if (packagePath.startsWith("META-INF/", Qt::CaseInsensitive))
        return {};
    return packagePath;
}

EpubSeriesMetadata packageMetadata(const QByteArray& xml)
{
    EpubSeriesMetadata result;
    QXmlStreamReader reader(xml);
    QHash<QString, QString> collections;
    QHash<QString, QString> collectionTypes;
    QHash<QString, QString> positions;
    int depth = 0;
    bool validRoot = false;
    bool inMetadata = false;
    bool ambiguousRefinement = false;
    bool ambiguousTitle = false;
    bool sawTitle = false;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD) {
            result.error = "EPUB package metadata contains a document type declaration.";
            result.blocksFilenameFallback = true;
            return result;
        }
        if (token == QXmlStreamReader::StartElement) {
            if (depth == 0) {
                validRoot = reader.name() == QStringLiteral("package")
                    && reader.namespaceUri() == packageNamespace;
            } else if (validRoot && depth == 1
                    && reader.name() == QStringLiteral("metadata")
                    && reader.namespaceUri() == packageNamespace) {
                inMetadata = true;
            } else if (inMetadata && depth == 2) {
                const QString name = reader.name().toString();
                const QString ns = reader.namespaceUri().toString();
                const auto attributes = reader.attributes();
                if (name == "title" && ns == dcNamespace) {
                    const QString title = reader.readElementText(QXmlStreamReader::SkipChildElements).simplified();
                    if (!sawTitle) {
                        result.title = title;
                        sawTitle = true;
                    } else if (result.title != title) {
                        ambiguousTitle = true;
                    }
                    continue;
                }
                if (name == "meta" && ns == packageNamespace) {
                    const QString property = attributes.value(QStringLiteral("property")).toString();
                    const QString id = attributes.value(QStringLiteral("id")).toString();
                    const QString refines = attributes.value(QStringLiteral("refines")).toString();
                    const QString value = reader.readElementText(QXmlStreamReader::SkipChildElements).simplified();
                    if (property == "belongs-to-collection" && !id.isEmpty()) {
                        ambiguousRefinement = ambiguousRefinement || collections.contains(id);
                        collections.insert(id, value);
                    } else if (refines.startsWith('#') && refines.size() > 1) {
                        if (property == "collection-type") {
                            if (value == "series")
                                result.blocksFilenameFallback = true;
                            ambiguousRefinement = ambiguousRefinement
                                || collectionTypes.contains(refines.mid(1));
                            collectionTypes.insert(refines.mid(1), value);
                        } else if (property == "group-position") {
                            ambiguousRefinement = ambiguousRefinement
                                || positions.contains(refines.mid(1));
                            positions.insert(refines.mid(1), value);
                        }
                    }
                    continue;
                }
            }
            ++depth;
        } else if (token == QXmlStreamReader::EndElement) {
            --depth;
            if (depth == 1)
                inMetadata = false;
        }
    }
    if (reader.hasError() || !validRoot || depth != 0 || ambiguousRefinement || ambiguousTitle) {
        result.error = "EPUB package metadata is malformed or ambiguous.";
        result.blocksFilenameFallback = true;
        return result;
    }
    for (auto it = collections.cbegin(); it != collections.cend(); ++it) {
        if (collectionTypes.value(it.key()) != "series")
            continue;
        if (!result.series.isEmpty()) {
            result.error = "EPUB identifies multiple series; leave it for review.";
            return result;
        }
        result.series = it.value().simplified();
        const QString position = positions.value(it.key());
        static const QRegularExpression integer(QStringLiteral(R"(^[0-9]{1,3}$)"));
        if (integer.match(position).hasMatch())
            result.number = position.toInt();
    }
    if (result.series.isEmpty() || result.number < 1 || result.title.isEmpty()
            || !validateWindowsName(result.series).valid
            || !validateWindowsName(result.title).valid) {
        result.error = "EPUB lacks one safe title, series and integer series position.";
    }
    return result;
}

} // namespace

EpubSeriesMetadata readEpubSeriesMetadata(const QString& path,
    const std::atomic_bool* cancelled)
{
    if (isCancelled(cancelled))
        return cancelledMetadata();
    EpubSeriesMetadata result;
    const QFileInfo info(path);
    const SourceSnapshot snapshot = captureSourceSnapshot(path);
    if (isCancelled(cancelled))
        return cancelledMetadata();
    if (info.suffix().compare("epub", Qt::CaseInsensitive) != 0 || !info.isFile()
            || info.size() < 1 || info.size() > MaximumSourceBytes
            || !isRegularDirectoryChain(info.absolutePath(), false)
            || platform::nativePathKind(path) != platform::NativePathKind::Regular
            || !snapshot.valid || (snapshot.attributes & (0x400U | 0x1000U)) != 0) {
        result.error = "EPUB is unavailable, too large, or not a local regular file.";
        result.blocksFilenameFallback = true;
        return result;
    }
    void* reader = mz_zip_reader_create();
    if (!reader) {
        result.error = "Could not start bounded EPUB metadata inspection.";
        result.blocksFilenameFallback = true;
        return result;
    }
    if (isCancelled(cancelled)) {
        mz_zip_reader_delete(&reader);
        return cancelledMetadata();
    }
    const QByteArray nativePath = path.toUtf8();
    if (mz_zip_reader_open_file(reader, nativePath.constData()) != MZ_OK
            || !archiveShapeIsSafe(reader, cancelled)) {
        result.error = "EPUB archive is unavailable or exceeds metadata safety limits.";
        result.blocksFilenameFallback = true;
    } else {
        qint64 totalBytes = 0;
        const QByteArray container = readMetadataEntry(reader, "META-INF/container.xml",
            totalBytes, cancelled);
        const QString packagePath = firstPackagePath(container);
        if (packagePath.isEmpty()) {
            result.error = "EPUB has no safe default package document.";
            result.blocksFilenameFallback = true;
        } else {
            const QByteArray package = readMetadataEntry(reader, packagePath.toUtf8(),
                totalBytes, cancelled);
            result = packageMetadata(package);
        }
    }
    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    if (isCancelled(cancelled))
        return cancelledMetadata();
    if (snapshot != captureSourceSnapshot(path)) {
        result = {};
        result.error = "EPUB changed during metadata inspection.";
        result.blocksFilenameFallback = true;
    }
    if (isCancelled(cancelled))
        return cancelledMetadata();
    return result;
}

} // namespace filemanager::files
