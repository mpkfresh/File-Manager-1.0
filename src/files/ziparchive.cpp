#include "files/ziparchive.h"

#include <algorithm>
#include <limits>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace filemanager::files {
namespace {

struct Entry final { QString name; qint64 size = 0; bool directory = false; };

bool cancelled(const std::atomic_bool* flag) { return flag && flag->load(); }

bool isDeviceName(const QString& component)
{
    const QString stem = component.section('.', 0, 0).toUpper();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    if (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT")))
        return stem.at(3) >= '1' && stem.at(3) <= '9';
    return false;
}

bool safeEntryName(const QString& raw, QString* normalized)
{
    QString name = raw;
    name.replace('\\', '/');
    while (name.endsWith('/')) name.chop(1);
    if (name.isEmpty() || name.startsWith('/') || name.startsWith("//") ||
        (name.size() >= 2 && name.at(1) == ':') || name.contains(QChar::ReplacementCharacter))
        return false;
    const QStringList components = name.split('/', Qt::KeepEmptyParts);
    for (const QString& component : components) {
        if (component.isEmpty() || component == "." || component == ".." || component.contains(':') ||
            component.endsWith(' ') || component.endsWith('.') || isDeviceName(component))
            return false;
        for (const QChar ch : component) {
            if (ch.unicode() < 32 || QStringLiteral("<>\"|?*").contains(ch)) return false;
        }
    }
    *normalized = components.join('/');
    return true;
}

bool pathHasReparsePoint(const QString& path)
{
#ifdef Q_OS_WIN
    QString cursor = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    while (!cursor.isEmpty()) {
        const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(cursor.utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
        const QString parent = QFileInfo(cursor).absolutePath();
        if (parent == cursor) break;
        cursor = parent;
    }
#else
    Q_UNUSED(path);
#endif
    return false;
}

void rollback(QStringList files, QStringList directories)
{
    for (auto it = files.crbegin(); it != files.crend(); ++it) QFile::remove(*it);
    std::sort(directories.begin(), directories.end(), [](const QString& a, const QString& b) {
        return a.size() > b.size();
    });
    for (const QString& directory : directories) QDir().rmdir(directory);
}

ZipOperationResult failure(const QString& error, bool wasCancelled = false)
{
    ZipOperationResult result;
    result.cancelled = wasCancelled;
    result.error = error;
    return result;
}

} // namespace

ZipOperationResult extractZipSafely(const QString& archivePath, const QString& destinationDirectory,
                                    const std::atomic_bool* cancel, const ZipProgress& progress)
{
    if (cancelled(cancel)) return failure("Extraction cancelled.", true);
    const QFileInfo archiveInfo(archivePath);
    const QFileInfo destinationInfo(destinationDirectory);
    if (!archiveInfo.isFile() || !destinationInfo.isDir()) return failure("Archive or destination is unavailable.");
    if (pathHasReparsePoint(destinationDirectory)) return failure("The destination crosses a link or reparse point.");

    void* reader = mz_zip_reader_create();
    const QByteArray archiveName = QDir::toNativeSeparators(archiveInfo.absoluteFilePath()).toUtf8();
    if (!reader || mz_zip_reader_open_file(reader, archiveName.constData()) != MZ_OK) {
        if (reader) mz_zip_reader_delete(&reader);
        return failure("The ZIP archive is malformed or unsupported.");
    }

    QVector<Entry> entries;
    QSet<QString> occupied;
    QSet<QString> files;
    qint64 total = 0;
    int entryCount = 0;
    int status = mz_zip_reader_goto_first_entry(reader);
    while (status == MZ_OK) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(reader, &info) != MZ_OK || !info || !info->filename) {
            mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
            return failure("The ZIP directory is malformed.");
        }
        if (++entryCount > ZipArchivePolicy::maximumEntries) {
            mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
            return failure("The archive contains more than 50,000 entries.");
        }
        QString name;
        if (!safeEntryName(QString::fromUtf8(info->filename, info->filename_size), &name)) {
            mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
            return failure("The archive contains an unsafe path.");
        }
        const bool directory = mz_zip_reader_entry_is_dir(reader) == MZ_OK;
        const bool encrypted = (info->flag & 1U) != 0 || info->aes_version != 0;
        const bool link = info->linkname != nullptr && info->linkname[0] != '\0';
        if (encrypted || link || (!directory && info->compression_method != 0 && info->compression_method != 8) ||
            info->uncompressed_size < 0 || info->compressed_size < 0 ||
            info->uncompressed_size > ZipArchivePolicy::maximumEntryBytes) {
            mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
            return failure("The archive contains an encrypted, linked, oversized, or unsupported entry.");
        }
        if (!directory && info->uncompressed_size > 0 &&
            (info->compressed_size == 0 || info->uncompressed_size / info->compressed_size > ZipArchivePolicy::maximumExpansionRatio)) {
            mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
            return failure("The archive exceeds the 200:1 expansion limit.");
        }
        if (info->uncompressed_size > ZipArchivePolicy::maximumTotalBytes - total) {
            mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
            return failure("The archive exceeds the 64 GiB extraction limit.");
        }
        total += info->uncompressed_size;
        const QString key = name.toCaseFolded();
        if (occupied.contains(key)) {
            mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
            return failure("The archive contains duplicate or case-colliding paths.");
        }
        for (QString parent = name.section('/', 0, -2); !parent.isEmpty(); parent = parent.section('/', 0, -2)) {
            if (files.contains(parent.toCaseFolded())) {
                mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
                return failure("The archive contains a file/directory path collision.");
            }
        }
        if (!directory) {
            const QString prefix = key + '/';
            for (const QString& existing : std::as_const(occupied)) {
                if (existing.startsWith(prefix)) {
                    mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
                    return failure("The archive contains a file/directory path collision.");
                }
            }
            files.insert(key);
        }
        occupied.insert(key);
        entries.push_back({name, info->uncompressed_size, directory});
        status = mz_zip_reader_goto_next_entry(reader);
    }
    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    if (status != MZ_END_OF_LIST && !entries.isEmpty()) return failure("The ZIP directory is malformed.");

    const QDir destination(destinationInfo.absoluteFilePath());
    for (const Entry& entry : entries) {
        const QString target = destination.absoluteFilePath(entry.name);
        if (QFileInfo::exists(target)) return failure("Extraction would overwrite an existing item: " + entry.name);
    }
    const qint64 required = total + total / 10;
    if (QStorageInfo(destination.absolutePath()).bytesAvailable() < required) return failure("There is not enough free space to extract safely.");
    if (archiveInfo.size() != QFileInfo(archivePath).size() || archiveInfo.lastModified() != QFileInfo(archivePath).lastModified())
        return failure("The archive changed during validation.");

    reader = mz_zip_reader_create();
    if (!reader || mz_zip_reader_open_file(reader, archiveName.constData()) != MZ_OK) {
        if (reader) mz_zip_reader_delete(&reader);
        return failure("The archive changed before extraction.");
    }
    QStringList createdFiles;
    QStringList createdDirectories;
    qint64 writtenTotal = 0;
    for (int index = 0; index < entries.size(); ++index) {
        if (index == 0 ? mz_zip_reader_goto_first_entry(reader) != MZ_OK : mz_zip_reader_goto_next_entry(reader) != MZ_OK) {
            rollback(createdFiles, createdDirectories); mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
            return failure("The archive changed during extraction.");
        }
        if (cancelled(cancel)) {
            rollback(createdFiles, createdDirectories); mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
            return failure("Extraction cancelled.", true);
        }
        const Entry& entry = entries.at(index);
        const QString target = destination.absoluteFilePath(entry.name);
        const QString parent = QFileInfo(target).absolutePath();
        QString cursor = parent;
        QStringList missing;
        while (!QFileInfo::exists(cursor) && cursor.startsWith(destination.absolutePath(), Qt::CaseInsensitive)) {
            missing.prepend(cursor); cursor = QFileInfo(cursor).absolutePath();
        }
        if (pathHasReparsePoint(cursor)) {
            rollback(createdFiles, createdDirectories); mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
            return failure("An extraction path crosses a link or reparse point.");
        }
        for (const QString& directory : missing) {
            if (!QDir().mkdir(directory)) {
                rollback(createdFiles, createdDirectories); mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
                return failure("Could not create an extraction directory.");
            }
            createdDirectories.append(directory);
        }
        if (entry.directory) {
            if (!QFileInfo::exists(target)) {
                if (!QDir().mkdir(target)) {
                    rollback(createdFiles, createdDirectories); mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
                    return failure("Could not create an extraction directory.");
                }
                createdDirectories.append(target);
            }
            continue;
        }
        QSaveFile output(target);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly) || mz_zip_reader_entry_open(reader) != MZ_OK) {
            rollback(createdFiles, createdDirectories); mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
            return failure("Could not open an archive entry safely.");
        }
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        qint64 entryWritten = 0;
        int read = 0;
        while (!cancelled(cancel) && (read = mz_zip_reader_entry_read(reader, buffer.data(), buffer.size())) > 0) {
            entryWritten += read;
            writtenTotal += read;
            if (entryWritten > entry.size || writtenTotal > total || output.write(buffer.constData(), read) != read) break;
            if (progress) progress(index, writtenTotal);
        }
        const int closeStatus = mz_zip_reader_entry_close(reader);
        if (cancelled(cancel) || read < 0 || closeStatus != MZ_OK || entryWritten != entry.size || !output.commit()) {
            output.cancelWriting(); rollback(createdFiles, createdDirectories); mz_zip_reader_close(reader); mz_zip_reader_delete(&reader);
            return failure(cancelled(cancel) ? "Extraction cancelled." : "An archive entry failed integrity validation.", cancelled(cancel));
        }
        createdFiles.append(target);
    }
    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    ZipOperationResult result;
    result.success = true; result.entries = entries.size(); result.totalBytes = writtenTotal;
    return result;
}

ZipOperationResult createZipWithWindowsTar(const QStringList& sourcePaths, const QString& destinationPath,
                                           const std::atomic_bool* cancel)
{
    if (cancelled(cancel)) return failure("Archive creation cancelled.", true);
    if (sourcePaths.isEmpty()) return failure("Select at least one item to archive.");
    if (QFileInfo::exists(destinationPath)) return failure("The destination archive already exists.");
    const QString tar = QStandardPaths::findExecutable("tar.exe");
    if (tar.isEmpty()) return failure("Windows tar.exe is unavailable.");
    const QFileInfo destination(destinationPath);
    if (!QDir(destination.absolutePath()).exists()) return failure("The destination folder is unavailable.");
    const QString temporary = destination.absolutePath() + "/." + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".zip";
    QStringList arguments{"-caf", QDir::toNativeSeparators(temporary)};
    QSet<QString> names;
    for (const QString& path : sourcePaths) {
        const QFileInfo source(path);
        if (!source.exists() || pathHasReparsePoint(source.absoluteFilePath())) { QFile::remove(temporary); return failure("An input is unavailable or crosses a link/reparse point."); }
        const QString key = source.fileName().toCaseFolded();
        if (names.contains(key)) { QFile::remove(temporary); return failure("Selected items have colliding top-level names."); }
        names.insert(key);
        arguments << "-C" << QDir::toNativeSeparators(source.absolutePath()) << source.fileName();
    }
    QProcess process;
    process.start(tar, arguments);
    if (!process.waitForStarted()) return failure("Windows could not start archive creation.");
    while (!process.waitForFinished(50)) {
        if (cancelled(cancel)) { process.kill(); process.waitForFinished(); QFile::remove(temporary); return failure("Archive creation cancelled.", true); }
    }
    if (cancelled(cancel)) {
        QFile::remove(temporary);
        return failure("Archive creation cancelled.", true);
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        QFile::remove(temporary); return failure(detail.isEmpty() ? "Windows archive creation failed." : detail);
    }
    if (QFileInfo::exists(destinationPath) || !QFile::rename(temporary, destinationPath)) {
        QFile::remove(temporary); return failure("The completed archive could not be committed without overwriting a file.");
    }
    ZipOperationResult result; result.success = true; result.entries = sourcePaths.size(); result.totalBytes = QFileInfo(destinationPath).size();
    return result;
}

} // namespace filemanager::files

