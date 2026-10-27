#include "previewservice.h"
#include "ui/localizedformat.h"
#include "platform/nativefilecopy.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QMimeDatabase>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <array>
#include <cstddef>
#endif


extern "C" {
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
}

namespace filemanager::preview {
namespace {

QString commonMetadata(const QString& displayPath, qint64 size, const QDateTime& modified,
    bool isFile, const QString& type)
{
    return QString("%1\n%2\nModified %3")
        .arg(type,
            isFile ? filemanager::ui::localizedDataSize(size) : displayPath,
            filemanager::ui::localizedDateTime(modified, QLocale::LongFormat));
}

bool isTextExtension(const QString& extension)
{
    return QStringList{"json", "md", "cpp", "h", "hpp", "py", "js", "ts", "css",
        "html", "xml", "yaml", "yml", "toml", "ini", "log", "csv"}
        .contains(extension.toLower());
}

#ifdef Q_OS_WIN
struct DirectoryCount
{
    bool valid = false;
    int count = 0;
    bool truncated = false;
};

DirectoryCount countDirectoryFromHandle(const QString& path)
{
    // Check and enumerate one opened object. A path-based check followed by
    // QDirIterator could count a different folder if the path changed.
    const QString native = filemanager::platform::extendedLengthPath(path);
    HANDLE directory = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL,
        nullptr);
    if (directory == INVALID_HANDLE_VALUE)
        return {};
    struct CloseDirectory {
        HANDLE handle;
        ~CloseDirectory() { CloseHandle(handle); }
    } close{directory};
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &tag, sizeof(tag))
            || (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
            || (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE
                | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) != 0)
        return {};

    DirectoryCount result{true, 0, false};
    alignas(8) std::array<std::byte, 64 * 1024> entries{};
    for (;;) {
        if (!GetFileInformationByHandleEx(directory, FileIdBothDirectoryInfo,
                entries.data(), static_cast<DWORD>(entries.size()))) {
            if (GetLastError() == ERROR_NO_MORE_FILES)
                return result;
            return {};
        }
        size_t offset = 0;
        for (;;) {
            constexpr size_t HeaderBytes = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
            if (offset > entries.size() - HeaderBytes)
                return {};
            const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
                entries.data() + offset);
            if (entry->FileNameLength % sizeof(wchar_t) != 0
                    || entry->FileNameLength > entries.size() - offset - HeaderBytes)
                return {};
            const bool dot = entry->FileNameLength == sizeof(wchar_t)
                && entry->FileName[0] == L'.';
            const bool dotDot = entry->FileNameLength == 2 * sizeof(wchar_t)
                && entry->FileName[0] == L'.' && entry->FileName[1] == L'.';
            const bool hidden = (entry->FileAttributes
                & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
            if (!dot && !dotDot && !hidden && ++result.count > 500) {
                result.count = 500;
                result.truncated = true;
                return result;
            }
            if (entry->NextEntryOffset == 0)
                break;
            if (entry->NextEntryOffset < HeaderBytes
                    || entry->NextEntryOffset > entries.size() - offset)
                return {};
            offset += entry->NextEntryOffset;
        }
    }
}
#endif

PreviewData zipPreview(QIODevice* source, qint64 sourceSize, PreviewData result)
{
    if (!source || sourceSize < 0 || sourceSize > PreviewService::MaximumArchivePreviewSourceBytes) {
        result.kind = PreviewKind::Error;
        result.text = "Archive inspection is disabled because the ZIP exceeds the 512 MB preview limit.";
        return result;
    }
    void* reader = mz_zip_reader_create();
    if (!reader)
        return {PreviewKind::Error, result.title, result.metadata, "The bounded ZIP reader is unavailable."};
    const QByteArray bytes = source->read(sourceSize + 1);
    if (bytes.size() != sourceSize || bytes.size() > INT_MAX
            || mz_zip_reader_open_buffer(reader,
                reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size(), 0) != MZ_OK) {
        mz_zip_reader_delete(&reader);
        return {PreviewKind::Error, result.title, result.metadata, "The ZIP directory is malformed or unsupported."};
    }
    QStringList names;
    int entries = 0;
    qint64 totalBytes = 0;
    bool safe = true;
    int32_t status = mz_zip_reader_goto_first_entry(reader);
    while (status == MZ_OK) {
        mz_zip_file* entry = nullptr;
        if (++entries > PreviewService::MaximumArchivePreviewEntries
                || mz_zip_reader_entry_get_info(reader, &entry) != MZ_OK || !entry
                || !entry->filename || entry->filename_size > PreviewService::MaximumArchiveEntryNameBytes
                || entry->compressed_size < 0 || entry->uncompressed_size < 0
                || entry->uncompressed_size > 16LL * 1024 * 1024 * 1024
                || entry->uncompressed_size > 64LL * 1024 * 1024 * 1024 - totalBytes
                || (entry->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0 || entry->aes_version != 0
                || (entry->uncompressed_size > 0 && (entry->compressed_size == 0
                    || entry->uncompressed_size / entry->compressed_size > 200))) {
            safe = false;
            break;
        }
        totalBytes += entry->uncompressed_size;
        if (names.size() < PreviewService::MaximumArchiveDisplayedEntries) {
            QString name = QString::fromUtf8(entry->filename, entry->filename_size);
            name.replace('\\', '/');
            if (name.contains(QChar::ReplacementCharacter)) { safe = false; break; }
            names.append(name);
        }
        status = mz_zip_reader_goto_next_entry(reader);
    }
    if (status != MZ_END_OF_LIST && status != MZ_OK)
        safe = false;
    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    if (!safe) {
        result.kind = PreviewKind::Error;
        result.text = "Archive contents were not displayed because the ZIP is malformed or exceeds preview safety limits.";
        return result;
    }
    result.kind = PreviewKind::Text;
    result.metadata += QString("\n%1 · %2 unpacked")
        .arg(filemanager::ui::localizedEntries(entries),
            filemanager::ui::localizedDataSize(totalBytes));
    result.text = names.join('\n');
    if (entries > names.size())
        result.text += QString("\n\n— Showing the first %1 of %2 —")
            .arg(QLocale().toString(names.size()), filemanager::ui::localizedEntries(entries));
    if (entries == 0)
        result.text = "This ZIP archive is empty.";
    return result;
}

} // namespace

PreviewData PreviewService::load(const QString& path) const
{
    const QFileInfo info(path);
    if (!info.exists())
        return {PreviewKind::None, "Select a file"};

    PreviewData result;
    result.title = info.fileName().isEmpty() ? info.absoluteFilePath() : info.fileName();
    if (info.isDir()) {
        result.metadata = commonMetadata(info.absoluteFilePath(), info.size(),
            info.lastModified(), false, "Folder");
        int count = 0;
        bool truncated = false;
#ifdef Q_OS_WIN
        const DirectoryCount counted = countDirectoryFromHandle(path);
        if (!counted.valid)
            return {PreviewKind::Error, result.title, result.metadata,
                "Folder preview is unavailable for a cloud placeholder, redirected folder, "
                "or unreadable directory."};
        count = counted.count;
        truncated = counted.truncated;
#else
        QDirIterator iterator(path, QDir::AllEntries | QDir::NoDotAndDotDot);
        while (iterator.hasNext()) {
            iterator.next();
            if (++count > 500) {
                truncated = true;
                break;
            }
        }
#endif
        result.kind = PreviewKind::Folder;
        result.text = QString("%1%2\n\n%3")
            .arg(truncated ? "More than " : "")
            .arg(filemanager::ui::localizedItems(count))
            .arg(info.absoluteFilePath());
        return result;
    }

    QMimeDatabase mimeDatabase;
    const auto mime = mimeDatabase.mimeTypeForFile(info);
    result.metadata = commonMetadata(info.absoluteFilePath(), info.size(), info.lastModified(),
        info.isFile(), mime.comment());

    if (info.suffix().compare("zip", Qt::CaseInsensitive) == 0) {
        QFile archive(path);
        if (!archive.open(QIODevice::ReadOnly))
            return {PreviewKind::Error, result.title, result.metadata, archive.errorString()};
        return zipPreview(&archive, info.size(), result);
    }

    if (mime.name().startsWith("image/")) {
        if (info.size() > MaximumImageFileBytes) {
            result.kind = PreviewKind::Error;
            result.text = "Image preview is disabled because the file exceeds the 64 MB safety limit.";
            return result;
        }
        QImageReader reader(path);
        reader.setAutoTransform(true);
        QSize size = reader.size();
        if (size.isValid() && (size.width() <= 0 || size.height() <= 0
                || static_cast<qint64>(size.width()) * size.height() > MaximumImagePixels)) {
            result.kind = PreviewKind::Error;
            result.text = "Image preview is disabled because its decoded dimensions exceed the safety limit.";
            return result;
        }
        if (size.isValid())
            size.scale(MaximumImageDimension, MaximumImageDimension, Qt::KeepAspectRatio);
        if (size.isValid())
            reader.setScaledSize(size);
        result.image = reader.read();
        if (result.image.isNull()) {
            result.kind = PreviewKind::Error;
            result.text = "The image decoder could not safely read this file.";
        } else {
            result.kind = PreviewKind::Image;
        }
        return result;
    }

    if (mime.name().startsWith("text/") || isTextExtension(info.suffix())) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            result.kind = PreviewKind::Error;
            result.text = file.errorString();
            return result;
        }
        const QByteArray bytes = file.read(MaximumTextBytes + 1);
        result.kind = PreviewKind::Text;
        result.text = QString::fromUtf8(bytes.first(MaximumTextBytes));
        if (bytes.size() > MaximumTextBytes)
            result.text += "\n\n— Preview truncated at 128 KB —";
        return result;
    }

    result.kind = PreviewKind::Unsupported;
    result.text = "Preview is not available for this file type.\n\nPress Ctrl+O to open it "
                  "in the default application.";
    return result;
}

PreviewData PreviewService::load(QIODevice* source, const QString& fileName, qint64 fileSize,
    const QDateTime& modified) const
{
    PreviewData result;
    result.title = QFileInfo(fileName).fileName();
    if (!source || !source->isReadable() || result.title.isEmpty() || fileSize < 0)
        return {PreviewKind::Error, result.title, {}, "The brokered preview input is invalid."};
    QMimeDatabase mimeDatabase;
    const auto mime = mimeDatabase.mimeTypeForFile(result.title, QMimeDatabase::MatchExtension);
    result.metadata = commonMetadata(result.title, fileSize, modified, true, mime.comment());
    const QString suffix = QFileInfo(result.title).suffix();
    if (suffix.compare("zip", Qt::CaseInsensitive) == 0)
        return zipPreview(source, fileSize, result);
    if (mime.name().startsWith("image/")) {
        if (fileSize > MaximumImageFileBytes) {
            result.kind = PreviewKind::Error;
            result.text = "Image preview is disabled because the file exceeds the 64 MB safety limit.";
            return result;
        }
        QImageReader reader(source, suffix.toLatin1());
        reader.setAutoTransform(true);
        QSize size = reader.size();
        if (size.isValid() && (size.width() <= 0 || size.height() <= 0
                || static_cast<qint64>(size.width()) * size.height() > MaximumImagePixels)) {
            result.kind = PreviewKind::Error;
            result.text = "Image preview is disabled because its decoded dimensions exceed the safety limit.";
            return result;
        }
        if (size.isValid()) size.scale(MaximumImageDimension, MaximumImageDimension, Qt::KeepAspectRatio);
        if (size.isValid()) reader.setScaledSize(size);
        result.image = reader.read();
        result.kind = result.image.isNull() ? PreviewKind::Error : PreviewKind::Image;
        if (result.image.isNull()) result.text = "The image decoder could not safely read this file.";
        return result;
    }
    if (mime.name().startsWith("text/") || isTextExtension(suffix)) {
        const QByteArray bytes = source->read(MaximumTextBytes + 1);
        result.kind = PreviewKind::Text;
        result.text = QString::fromUtf8(bytes.first(MaximumTextBytes));
        if (bytes.size() > MaximumTextBytes) result.text += "\n\n— Preview truncated at 128 KB —";
        return result;
    }
    result.kind = PreviewKind::Unsupported;
    result.text = "Preview is not available for this file type.\n\nPress Ctrl+O to open it "
                  "in the default application.";
    return result;
}

} // namespace filemanager::preview
