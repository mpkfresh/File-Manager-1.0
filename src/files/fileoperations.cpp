#include "fileoperations.h"
#include "filenamevalidator.h"
#include "platform/recyclebinoperation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

namespace filemanager::files {
namespace {

OperationResult copyRecursive(const QString& source, const QString& destination)
{
    const QFileInfo info(source);
    if (!info.exists())
        return OperationResult::failure("Source no longer exists: " + source);
    if (info.isSymbolicLink())
        return OperationResult::failure(
            "Symbolic-link transfer requires an explicit link policy: " + source);
    if (QFileInfo::exists(destination))
        return OperationResult::failure("Destination already exists: " + destination);
    if (info.isDir() && isPathInside(destination, source))
        return OperationResult::failure("A folder cannot be copied inside itself.");

    if (info.isFile()) {
        if (QFile::copy(source, destination))
            return OperationResult::success();
        return OperationResult::failure("Could not copy " + info.fileName());
    }

    if (!QDir().mkpath(destination))
        return OperationResult::failure("Could not create destination folder: " + destination);

    const QDir directory(source);
    const auto entries = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& child : entries) {
        const auto result = copyRecursive(
            child.absoluteFilePath(), QDir(destination).filePath(child.fileName()));
        if (!result.ok) {
            QDir(destination).removeRecursively();
            return result;
        }
    }
    return OperationResult::success();
}

} // namespace

OperationResult copy(const QString& source, const QString& destination)
{
    return copyRecursive(source, destination);
}

OperationResult move(const QString& source, const QString& destination)
{
    if (QFileInfo(source).isSymbolicLink())
        return OperationResult::failure(
            "Symbolic-link transfer requires an explicit link policy: " + source);
    if (QFileInfo(source).isDir() && isPathInside(destination, source))
        return OperationResult::failure("A folder cannot be moved inside itself.");
    if (QFile::rename(source, destination))
        return OperationResult::success();

    const auto copied = copyRecursive(source, destination);
    if (!copied.ok)
        return copied;

    const QFileInfo info(source);
    const bool removed = info.isDir() ? QDir(source).removeRecursively() : QFile::remove(source);
    if (removed)
        return OperationResult::success();

    QFileInfo(destination).isDir()
        ? QDir(destination).removeRecursively()
        : QFile::remove(destination);
    return OperationResult::failure("Move was rolled back because the source could not be removed.");
}

OperationResult renameItem(const QString& source, const QString& destination)
{
    const QFileInfo sourceInfo(source);
    if (!sourceInfo.exists())
        return OperationResult::failure("Source no longer exists: " + source);
    const ValidationResult destinationValidation = validateWindowsPath(destination);
    if (!destinationValidation.valid)
        return OperationResult::failure(destinationValidation.error);
    const QString sourcePath = sourceInfo.absoluteFilePath();
    const QString destinationPath = QFileInfo(destinationValidation.normalizedName).absoluteFilePath();
    const auto renamePath = [directory = sourceInfo.isDir()](const QString& from,
                                                            const QString& to) {
        return directory ? QDir().rename(from, to) : QFile::rename(from, to);
    };
    if (sourceInfo.dir().absolutePath().compare(
            QFileInfo(destinationPath).dir().absolutePath(), Qt::CaseInsensitive) != 0) {
        return OperationResult::failure("Rename cannot move an item to another folder.");
    }
    if (sourcePath == destinationPath)
        return OperationResult::success();

    const bool caseOnly = sourcePath.compare(destinationPath, Qt::CaseInsensitive) == 0;
    if (!caseOnly && QFileInfo::exists(destinationPath))
        return OperationResult::failure("Destination already exists: " + destinationPath);
    if (!caseOnly)
        return renamePath(sourcePath, destinationPath)
            ? OperationResult::success()
            : OperationResult::failure("The item could not be renamed.");

    QString staging;
    for (int attempt = 0; attempt < 100 && staging.isEmpty(); ++attempt) {
        const QString candidate = sourceInfo.dir().filePath(
            ".file-manager-case-rename-"
            + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12) + ".tmp");
        if (!QFileInfo::exists(candidate))
            staging = candidate;
    }
    if (staging.isEmpty() || !renamePath(sourcePath, staging))
        return OperationResult::failure("The case-only rename could not be staged.");
    if (renamePath(staging, destinationPath))
        return OperationResult::success();
    if (!renamePath(staging, sourcePath)) {
        return OperationResult::failure(
            "The case-only rename failed and requires manual recovery: " + staging);
    }
    return OperationResult::failure("The case-only rename failed and was rolled back.");
}

OperationResult moveToTrash(const QString& path)
{
    const TrashResult result = moveToTrashDetailed(path);
    return result.ok ? OperationResult::success() : OperationResult::failure(result.error);
}

TrashResult moveToTrashDetailed(const QString& path)
{
    const QFileInfo source(path);
    if (!source.exists())
        return {false, source.absoluteFilePath(), {}, {}, "Item no longer exists: " + path};
    const auto native = filemanager::platform::RecycleBinOperation::recycle(
        source.absoluteFilePath());
    return {native.success, native.item.originalPath, {}, native.item.absolutePidl, native.error};
}

} // namespace filemanager::files
