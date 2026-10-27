#include "scopeenumerator.h"
#include "filenamevalidator.h"

#include "platform/nativefilecopy.h"

#include <QDirIterator>
#include <QDir>
#include <QStringList>

namespace filemanager::files {

bool isRegularDirectoryChain(QString path, bool allowMissing)
{
    for (;;) {
        const auto kind = platform::nativePathKind(path);
        if (kind == platform::NativePathKind::Missing) {
            if (!allowMissing || !validateWindowsName(QFileInfo(path).fileName()).valid)
                return false;
        } else if (kind != platform::NativePathKind::Regular || !QFileInfo(path).isDir()) {
            return false;
        }
        const QString parent = QFileInfo(path).absolutePath();
        if (QDir::cleanPath(QFileInfo(parent).absoluteFilePath()).compare(
                QDir::cleanPath(QFileInfo(path).absoluteFilePath()), Qt::CaseInsensitive) == 0)
            return true;
        path = parent;
    }
}

ScopeEnumerationResult enumerateRegularTree(const QString& root, bool includeDirectories,
    const std::atomic_bool* cancelled, const std::function<bool(const QFileInfo&)>& visitor,
    const ScopeEnumerationOptions& options)
{
    ScopeEnumerationResult result;
    const auto stopped = [&] { return cancelled && cancelled->load(); };
    if (stopped()) {
        result.cancelled = true;
        return result;
    }
    if (filemanager::platform::nativePathKind(root)
            != filemanager::platform::NativePathKind::Regular
            || !QFileInfo(root).isDir()) {
        result.error = QStringLiteral("The selected scope is unavailable or is a reparse point.");
        return result;
    }

    QStringList pending{QFileInfo(root).absoluteFilePath()};
    while (!pending.isEmpty() && !stopped()) {
        const QString directory = pending.takeLast();
        bool excluded = false;
        for (const QString& excludedDirectory : options.excludedDirectories)
            excluded = excluded || isPathInside(directory, excludedDirectory);
        if (excluded)
            continue;
        if (filemanager::platform::nativePathKind(directory)
                != filemanager::platform::NativePathKind::Regular || !QFileInfo(directory).isDir()) {
            ++result.skippedBoundaries;
            continue;
        }
        QDirIterator entries(directory,
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDirIterator::NoIteratorFlags);
        while (entries.hasNext()) {
            if (stopped())
                break;
            entries.next();
            if (options.maxEntries > 0 && result.entriesVisited >= options.maxEntries) {
                result.limitReached = true;
                return result;
            }
            ++result.entriesVisited;
            const QFileInfo entry = entries.fileInfo();
            if (filemanager::platform::nativePathKind(entry.absoluteFilePath())
                    != filemanager::platform::NativePathKind::Regular) {
                ++result.skippedBoundaries;
                continue;
            }
            if (entry.isDir()) {
                if (includeDirectories && !visitor(entry))
                    return result;
                pending.append(entry.absoluteFilePath());
            } else if (entry.isFile() && !visitor(entry)) {
                return result;
            }
        }
    }
    result.cancelled = stopped();
    return result;
}

} // namespace filemanager::files
