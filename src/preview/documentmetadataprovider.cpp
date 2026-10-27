#include "preview/documentmetadataprovider.h"

#include "files/scopeenumerator.h"
#include "platform/cloudfilestatus.h"
#include "preview/sandboxedpreviewprocess.h"

#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QThreadPool>
#include <QXmlStreamReader>

#include <climits>

extern "C" {
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
}

namespace filemanager::preview {
namespace {

QString decodedLiteral(const QByteArray& value)
{
    QByteArray decoded = value;
    decoded.replace("\\(", "(");
    decoded.replace("\\)", ")");
    decoded.replace("\\\\", "\\");
    return QString::fromUtf8(decoded).trimmed();
}

QString firstCapture(const QByteArray& bytes, const QRegularExpression& expression)
{
    const QRegularExpressionMatch match = expression.match(QString::fromLatin1(bytes));
    return match.hasMatch() ? decodedLiteral(match.captured(1).toLatin1()) : QString();
}

DocumentMetadata readMetadata(QIODevice* source, const QString& suffix)
{
    if (!source || !source->seek(0))
        return {};
    const QByteArray bytes = source->read(DocumentMetadataProvider::MaximumReadBytes);
    DocumentMetadata result;
    if (suffix == "pdf") {
        const QString text = QString::fromLatin1(bytes);
        const QRegularExpression pages(QStringLiteral(R"(/Type\s*/Page(?!s)\b)"));
        auto pageMatches = pages.globalMatch(text);
        while (pageMatches.hasNext()) {
            pageMatches.next();
            ++result.pageCount;
        }
        result.title = firstCapture(bytes,
            QRegularExpression(QStringLiteral(R"(/Title\s*\(([^\r\n)]*)\))")));
        result.authors = firstCapture(bytes,
            QRegularExpression(QStringLiteral(R"(/Author\s*\(([^\r\n)]*)\))")));
    } else if (suffix == "rtf") {
        result.title = firstCapture(bytes,
            QRegularExpression(QStringLiteral(R"(\\title\s+([^{}]*)\})")));
        result.authors = firstCapture(bytes,
            QRegularExpression(QStringLiteral(R"(\\author\s+([^{}]*)\})")));
    }
    return result;
}

bool archiveShapeIsSafe(void* reader)
{
    int entries = 0;
    int32_t status = mz_zip_reader_goto_first_entry(reader);
    while (status == MZ_OK) {
        if (++entries > DocumentMetadataProvider::MaximumArchiveEntries)
            return false;
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(reader, &info) != MZ_OK || !info
                || info->uncompressed_size < 0 || info->compressed_size < 0
                || info->uncompressed_size > DocumentMetadataProvider::MaximumSourceBytes)
            return false;
        const qint64 compressed = qMax<qint64>(1, info->compressed_size);
        if (info->uncompressed_size > compressed * DocumentMetadataProvider::MaximumExpansionRatio)
            return false;
        status = mz_zip_reader_goto_next_entry(reader);
    }
    return status == MZ_END_OF_LIST;
}

QByteArray readArchiveEntry(void* reader, const char* name, qint64& totalBytes)
{
    if (mz_zip_reader_locate_entry(reader, name, 0) != MZ_OK)
        return {};
    mz_zip_file* info = nullptr;
    if (mz_zip_reader_entry_get_info(reader, &info) != MZ_OK || !info
            || (info->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0
            || (info->compression_method != MZ_COMPRESS_METHOD_STORE
                && info->compression_method != MZ_COMPRESS_METHOD_DEFLATE)
            || info->uncompressed_size < 0
            || info->uncompressed_size > DocumentMetadataProvider::MaximumMetadataEntryBytes
            || totalBytes + info->uncompressed_size
                > DocumentMetadataProvider::MaximumMetadataTotalBytes)
        return {};
    if (mz_zip_reader_entry_open(reader) != MZ_OK)
        return {};
    QByteArray bytes;
    bytes.reserve(static_cast<int>(info->uncompressed_size));
    char buffer[16 * 1024];
    int32_t read = 0;
    bool valid = true;
    while ((read = mz_zip_reader_entry_read(reader, buffer, sizeof(buffer))) > 0) {
        if (bytes.size() + read > DocumentMetadataProvider::MaximumMetadataEntryBytes) {
            valid = false;
            break;
        }
        bytes.append(buffer, read);
    }
    if (read < 0)
        valid = false;
    if (mz_zip_reader_entry_close(reader) != MZ_OK)
        valid = false;
    if (!valid)
        return {};
    totalBytes += bytes.size();
    return bytes;
}

void readCoreProperties(const QByteArray& xml, DocumentMetadata& metadata)
{
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD) {
            reader.raiseError(QStringLiteral("Document type declarations are not accepted"));
            break;
        }
        if (token != QXmlStreamReader::StartElement)
            continue;
        if (reader.name() == QStringLiteral("title"))
            metadata.title = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
        else if (reader.name() == QStringLiteral("creator"))
            metadata.authors = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
    }
    if (reader.hasError()) {
        metadata.title.clear();
        metadata.authors.clear();
    }
}

void readAppProperties(const QByteArray& xml, DocumentMetadata& metadata)
{
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD) {
            reader.raiseError(QStringLiteral("Document type declarations are not accepted"));
            break;
        }
        if (token == QXmlStreamReader::StartElement
                && (reader.name() == QStringLiteral("Pages")
                    || reader.name() == QStringLiteral("Slides"))) {
            bool valid = false;
            const int count = reader.readElementText().toInt(&valid);
            if (valid && count > 0)
                metadata.pageCount = count;
        }
    }
    if (reader.hasError())
        metadata.pageCount = 0;
}

DocumentMetadata readOfficeMetadata(QIODevice* source, qint64 sourceSize)
{
    DocumentMetadata metadata;
    if (!source || sourceSize < 0
            || sourceSize > DocumentMetadataProvider::MaximumSourceBytes
            || !source->seek(0))
        return metadata;
    const QByteArray bytes = source->read(sourceSize + 1);
    if (bytes.size() != sourceSize || bytes.size() > INT_MAX)
        return metadata;
    void* reader = mz_zip_reader_create();
    if (!reader)
        return metadata;
    if (mz_zip_reader_open_buffer(reader,
            reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size(), 0) == MZ_OK
            && archiveShapeIsSafe(reader)) {
        qint64 totalBytes = 0;
        readCoreProperties(readArchiveEntry(reader, "docProps/core.xml", totalBytes), metadata);
        readAppProperties(readArchiveEntry(reader, "docProps/app.xml", totalBytes), metadata);
    }
    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    return metadata;
}

std::optional<DocumentMetadata> isolatedMetadata(const QString& path,
    const std::atomic_bool* cancelled = nullptr)
{
    const auto helper = runSandboxedPreviewHost(path, QStringLiteral("document-metadata"),
        6000, 128 * 1024, {}, cancelled);
    if (!helper.success || !helper.sandboxed || !helper.jobRestricted)
        return std::nullopt;
    QJsonParseError parseError;
    const QJsonDocument reply = QJsonDocument::fromJson(helper.standardOutput, &parseError);
    if (parseError.error != QJsonParseError::NoError || !reply.isObject())
        return std::nullopt;
    const QJsonObject object = reply.object();
    if (!object.value(QStringLiteral("pageCount")).isDouble()
            || !object.value(QStringLiteral("title")).isString()
            || !object.value(QStringLiteral("authors")).isString())
        return std::nullopt;
    const int pageCount = object.value(QStringLiteral("pageCount")).toInt(-1);
    const QString title = object.value(QStringLiteral("title")).toString();
    const QString authors = object.value(QStringLiteral("authors")).toString();
    if (pageCount < 0 || pageCount > 1000000
            || title.size() > 32768 || authors.size() > 32768)
        return std::nullopt;
    return DocumentMetadata{pageCount, title, authors};
}

} // namespace

DocumentMetadata readDocumentMetadataFromDevice(QIODevice* source,
    const QString& fileName, qint64 sourceSize)
{
    if (!source || sourceSize < 0
            || sourceSize > DocumentMetadataProvider::MaximumSourceBytes)
        return {};
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == QStringLiteral("pdf") || suffix == QStringLiteral("rtf"))
        return readMetadata(source, suffix);
    if (suffix == QStringLiteral("docx") || suffix == QStringLiteral("xlsx")
            || suffix == QStringLiteral("pptx"))
        return readOfficeMetadata(source, sourceSize);
    return {};
}

SelectedDocumentMetadata readSelectedDocumentMetadata(const QString& path,
    const filemanager::files::SourceSnapshot& expected,
    const std::atomic_bool* cancelled)
{
    SelectedDocumentMetadata result;
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        result.error = QStringLiteral("Document metadata review was cancelled.");
        return result;
    }
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    if (!info.isAbsolute() || !info.isFile()
            || info.size() > DocumentMetadataProvider::MaximumSourceBytes
            || !filemanager::files::isRegularDirectoryChain(info.absolutePath())
            || !filemanager::platform::isLocallyPreviewableFile(path)
            || (suffix != "pdf" && suffix != "rtf" && suffix != "docx"
                && suffix != "xlsx" && suffix != "pptx")) {
        result.error = QStringLiteral(
            "Only selected, locally available PDF, RTF and Office documents are supported.");
        return result;
    }
    if (!expected.valid || expected.fileIndex == 0
            || filemanager::files::captureSourceSnapshot(path) != expected) {
        result.error = QStringLiteral("The selected document changed before metadata review.");
        return result;
    }
    const auto metadata = isolatedMetadata(path, cancelled);
    if (!metadata) {
        result.error = QStringLiteral("The restricted document metadata helper was unavailable.");
        return result;
    }
    result.metadata = *metadata;
    if ((cancelled && cancelled->load(std::memory_order_relaxed))
            || !filemanager::platform::isLocallyPreviewableFile(path)
            || filemanager::files::captureSourceSnapshot(path) != expected) {
        result.metadata = {};
        result.error = QStringLiteral("The selected document changed during metadata review.");
        return result;
    }
    result.available = true;
    return result;
}

DocumentMetadataProvider::DocumentMetadataProvider(QObject* parent) : QObject(parent) {}

QString DocumentMetadataProvider::cacheKey(const QString& path) const
{
    const QFileInfo info(path);
    return info.absoluteFilePath() + '|' + QString::number(info.size()) + '|'
        + QString::number(info.lastModified().toMSecsSinceEpoch());
}

std::optional<DocumentMetadata> DocumentMetadataProvider::request(const QString& path)
{
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    if (!info.isFile() || info.size() > MaximumSourceBytes
            || !filemanager::platform::isLocallyPreviewableFile(path)
            || (suffix != "pdf" && suffix != "rtf" && suffix != "docx"
                && suffix != "xlsx" && suffix != "pptx"))
        return std::nullopt;
    const QString key = cacheKey(path);
    if (const DocumentMetadata* cached = cache_.object(key))
        return *cached;
    if (pending_.contains(key))
        return std::nullopt;
    pending_.insert(key);
    const QString absolutePath = info.absoluteFilePath();
    QPointer<DocumentMetadataProvider> guard(this);
    QThreadPool::globalInstance()->start([guard, key, absolutePath] {
        const DocumentMetadata metadata = isolatedMetadata(absolutePath).value_or(DocumentMetadata{});
        if (guard) {
            QMetaObject::invokeMethod(guard, [guard, key, absolutePath, metadata] {
                if (!guard)
                    return;
                guard->pending_.remove(key);
                guard->cache_.insert(key, new DocumentMetadata(metadata));
                emit guard->metadataReady(absolutePath);
            });
        }
    });
    return std::nullopt;
}

} // namespace filemanager::preview
