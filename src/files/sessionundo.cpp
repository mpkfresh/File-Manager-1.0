#include "files/sessionundo.h"
#include "files/batchrename.h"
#include "platform/recyclebinoperation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSet>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace filemanager::files {

void SessionUndoStack::discardTrashEntriesContaining(const QList<QByteArray>& recycledItemIds)
{
    const QSet<QByteArray> selected(recycledItemIds.cbegin(), recycledItemIds.cend());
    entries_.removeIf([&](const Entry& entry) {
        if (entry.kind != Entry::Kind::Trash)
            return false;
        return std::any_of(entry.items.cbegin(), entry.items.cend(), [&](const Entry::Item& item) {
            return selected.contains(item.recycledItemId);
        });
    });
}

void SessionUndoStack::discardAllTrashEntries()
{
    entries_.removeIf([](const Entry& entry) { return entry.kind == Entry::Kind::Trash; });
}

SessionUndoStack::Snapshot SessionUndoStack::capture(const QString& path)
{
    Snapshot result;
    const QFileInfo info(path);
    if (!info.exists())
        return result;
    result.directory = info.isDir();
    result.size = result.directory ? 0 : info.size();
    result.modifiedMsecs = info.lastModified().toMSecsSinceEpoch();
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(info.absoluteFilePath());
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return result;
    BY_HANDLE_FILE_INFORMATION identity{};
    if (GetFileInformationByHandle(handle, &identity)) {
        result.volumeSerial = identity.dwVolumeSerialNumber;
        result.fileIndex = (static_cast<quint64>(identity.nFileIndexHigh) << 32U)
            | identity.nFileIndexLow;
        result.valid = true;
    }
    CloseHandle(handle);
#else
    result.valid = true;
#endif
    return result;
}

bool SessionUndoStack::matches(const Snapshot& expected, const Snapshot& current)
{
    if (!expected.valid || !current.valid || expected.directory != current.directory
            || expected.size != current.size || expected.modifiedMsecs != current.modifiedMsecs)
        return false;
#ifdef Q_OS_WIN
    return expected.volumeSerial == current.volumeSerial && expected.fileIndex == current.fileIndex;
#else
    return true;
#endif
}

bool SessionUndoStack::recordRename(const QString& originalPath, const QString& renamedPath)
{
    const QString original = QFileInfo(originalPath).absoluteFilePath();
    const QString current = QFileInfo(renamedPath).absoluteFilePath();
    const Snapshot snapshot = capture(current);
    if (!snapshot.valid || QFileInfo::exists(original))
        return false;
    Entry entry;
    entry.kind = Entry::Kind::Rename;
    entry.items.append({original, current, snapshot, true});
    entry.id = ++lastAssignedEntryId_;
    entries_.append(std::move(entry));
    while (entries_.size() > MaximumEntries)
        entries_.removeFirst();
    return true;
}

bool SessionUndoStack::recordBatchRename(const BatchRenamePlan& plan)
{
    if (!plan.isValid())
        return false;
    Entry entry;
    entry.kind = Entry::Kind::BatchRename;
    for (const BatchRenameItem& item : plan.items) {
        if (item.source == item.destination)
            continue;
        const SourceSnapshot current = captureSourceSnapshot(item.destination);
        if (!current.valid || current != item.sourceSnapshot)
            return false;
        Entry::Item recorded;
        recorded.originalPath = QFileInfo(item.source).absoluteFilePath();
        recorded.currentPath = QFileInfo(item.destination).absoluteFilePath();
        recorded.currentSnapshot.directory = item.directory;
        recorded.batchSourceSnapshot = current;
        recorded.root = true;
        entry.items.append(std::move(recorded));
    }
    if (entry.items.isEmpty() || !preflight(entry))
        return false;
    entry.id = ++lastAssignedEntryId_;
    entries_.append(std::move(entry));
    while (entries_.size() > MaximumEntries)
        entries_.removeFirst();
    return true;
}

bool SessionUndoStack::recordMove(const QStringList& originalSources,
    const QList<TransferManifestEntry>& manifest)
{
    if (originalSources.isEmpty() || manifest.isEmpty())
        return false;
    Entry entry;
    entry.kind = Entry::Kind::Move;
    for (const TransferManifestEntry& recorded : manifest) {
        if (!recorded.destinationCreated || !recorded.sourceRemoved
                || !recorded.identityAvailable || !recorded.sourceIdentityAvailable)
            return false;
        if (recorded.volumeSerial != recorded.sourceVolumeSerial)
            entry.requiresQueuedTransfer = true;
        Snapshot snapshot;
        snapshot.valid = true;
        snapshot.directory = recorded.directory;
        snapshot.volumeSerial = recorded.volumeSerial;
        snapshot.fileIndex = recorded.fileIndex;
        snapshot.size = recorded.size;
        snapshot.modifiedMsecs = recorded.lastModifiedUtcMs;
        const bool root = std::any_of(originalSources.cbegin(), originalSources.cend(),
            [&](const QString& source) {
                return QDir::cleanPath(QFileInfo(source).absoluteFilePath()).compare(
                    QDir::cleanPath(QFileInfo(recorded.sourcePath).absoluteFilePath()),
                    Qt::CaseInsensitive) == 0;
            });
        entry.items.append({QFileInfo(recorded.sourcePath).absoluteFilePath(),
            QFileInfo(recorded.destinationPath).absoluteFilePath(), snapshot, root});
    }
    for (const QString& source : originalSources) {
        const QString absoluteSource = QDir::cleanPath(QFileInfo(source).absoluteFilePath());
        const bool found = std::any_of(entry.items.cbegin(), entry.items.cend(),
            [&](const Entry::Item& item) {
                return item.root && QDir::cleanPath(item.originalPath).compare(
                    absoluteSource, Qt::CaseInsensitive) == 0;
            });
        if (!found)
            return false;
    }
    if (entry.requiresQueuedTransfer) {
        QString originalParent;
        for (const Entry::Item& item : entry.items) {
            if (!item.root)
                continue;
            const QFileInfo original(item.originalPath);
            if (QFileInfo(item.currentPath).fileName().compare(
                    original.fileName(), Qt::CaseInsensitive) != 0)
                return false;
            if (originalParent.isEmpty())
                originalParent = QDir::cleanPath(original.absolutePath());
            else if (QDir::cleanPath(original.absolutePath()).compare(
                         originalParent, Qt::CaseInsensitive) != 0)
                return false;
        }
    }
    if (!preflight(entry))
        return false;
    entry.id = ++lastAssignedEntryId_;
    entries_.append(std::move(entry));
    while (entries_.size() > MaximumEntries)
        entries_.removeFirst();
    return true;
}

bool SessionUndoStack::recordTrash(const QList<TrashResult>& results)
{
    if (results.isEmpty())
        return false;
    Entry entry;
    entry.kind = Entry::Kind::Trash;
    for (const TrashResult& result : results) {
        if (!result.ok || result.originalPath.isEmpty() || result.recycledItemId.isEmpty()
                || QFileInfo::exists(result.originalPath))
            return false;
        Entry::Item item;
        item.originalPath = QFileInfo(result.originalPath).absoluteFilePath();
        item.currentPath = result.pathInTrash;
        item.root = true;
        item.recycledItemId = result.recycledItemId;
        if (!filemanager::platform::RecycleBinOperation::isAvailable(
                {item.originalPath, item.recycledItemId}))
            return false;
        entry.items.append(std::move(item));
    }
    entry.id = ++lastAssignedEntryId_;
    entries_.append(std::move(entry));
    while (entries_.size() > MaximumEntries)
        entries_.removeFirst();
    return true;
}

bool SessionUndoStack::preflight(const Entry& entry)
{
    QSet<QString> batchCurrentPaths;
    if (entry.kind == Entry::Kind::BatchRename) {
        for (const Entry::Item& item : entry.items)
            batchCurrentPaths.insert(QDir::cleanPath(item.currentPath).toCaseFolded());
    }
    for (const Entry::Item& item : entry.items) {
        if (entry.kind == Entry::Kind::Trash) {
            if (!filemanager::platform::RecycleBinOperation::isAvailable(
                    {item.originalPath, item.recycledItemId}))
                return false;
            continue;
        }
        if (entry.kind == Entry::Kind::BatchRename) {
            if (captureSourceSnapshot(item.currentPath) != item.batchSourceSnapshot)
                return false;
        } else if (!matches(item.currentSnapshot, capture(item.currentPath))) {
            return false;
        }
        if (item.root && QFileInfo::exists(item.originalPath)
                && !batchCurrentPaths.contains(
                    QDir::cleanPath(item.originalPath).toCaseFolded()))
            return false;
    }
    return true;
}

bool SessionUndoStack::renamePath(const Entry::Item& item, bool undo)
{
    const QString source = undo ? item.currentPath : item.originalPath;
    const QString destination = undo ? item.originalPath : item.currentPath;
    return item.currentSnapshot.directory
        ? QDir().rename(source, destination) : QFile::rename(source, destination);
}

bool SessionUndoStack::canUndo() const
{
    if (entries_.isEmpty())
        return false;
    return preflight(entries_.constLast());
}

QString SessionUndoStack::nextDescription() const
{
    if (entries_.isEmpty())
        return {};
    const Entry& entry = entries_.constLast();
    const int rootCount = static_cast<int>(std::count_if(entry.items.cbegin(), entry.items.cend(),
        [](const Entry::Item& item) { return item.root; }));
    if (entry.kind == Entry::Kind::Move)
        return QStringLiteral("Undo move of %1 %2")
            .arg(QLocale().toString(rootCount), rootCount == 1 ? "item" : "items");
    if (entry.kind == Entry::Kind::Trash)
        return QStringLiteral("Restore %1 %2 from Recycle Bin")
            .arg(QLocale().toString(rootCount), rootCount == 1 ? "item" : "items");
    if (entry.kind == Entry::Kind::BatchRename)
        return QStringLiteral("Undo batch rename of %1 %2")
            .arg(QLocale().toString(rootCount), rootCount == 1 ? "item" : "items");
    return QStringLiteral("Undo rename of %1")
        .arg(QFileInfo(entry.items.constFirst().currentPath).fileName());
}

SessionUndoResult SessionUndoStack::undoLast()
{
    if (entries_.isEmpty())
        return {false, {}, QStringLiteral("There is no session operation to undo."), std::nullopt};
    const Entry entry = entries_.constLast();
    if (!preflight(entry)) {
        entries_.removeLast();
        return {false, {},
            QStringLiteral("Undo refused because an item changed, was replaced, or its original "
                "location is occupied."), std::nullopt};
    }
    if (entry.kind == Entry::Kind::Move && entry.requiresQueuedTransfer) {
        QStringList sources;
        QString destinationDirectory;
        QString restoredPath;
        for (const Entry::Item& item : entry.items) {
            if (!item.root)
                continue;
            sources.append(item.currentPath);
            if (destinationDirectory.isEmpty())
                destinationDirectory = QFileInfo(item.originalPath).absolutePath();
            if (restoredPath.isEmpty())
                restoredPath = item.originalPath;
        }
        entries_.removeLast();
        return {true, restoredPath, {},
            TransferRequest{sources, destinationDirectory, TransferOperation::Move}};
    }
    if (entry.kind == Entry::Kind::Trash) {
        Entry& stored = entries_.last();
        QList<int> restored;
        for (int index = 0; index < stored.items.size(); ++index) {
            Entry::Item& item = stored.items[index];
            const auto restore = filemanager::platform::RecycleBinOperation::restore(
                {item.originalPath, item.recycledItemId});
            if (restore.success) {
                restored.append(index);
                continue;
            }
            bool rollbackOk = true;
            for (auto rollback = restored.crbegin(); rollback != restored.crend(); ++rollback) {
                Entry::Item& restoredItem = stored.items[*rollback];
                const TrashResult recycled = moveToTrashDetailed(restoredItem.originalPath);
                if (recycled.ok && !recycled.recycledItemId.isEmpty())
                    restoredItem.recycledItemId = recycled.recycledItemId;
                else
                    rollbackOk = false;
            }
            if (!rollbackOk)
                entries_.removeLast();
            return {false, {}, rollbackOk
                ? QStringLiteral("Recycle Bin restore failed; restored items were returned to the Recycle Bin.")
                : QStringLiteral("Recycle Bin restore failed and rollback was incomplete; inspect the original locations and Recycle Bin."),
                std::nullopt};
        }
        const QString restoredPath = stored.items.constFirst().originalPath;
        entries_.removeLast();
        return {true, restoredPath, {}, std::nullopt};
    }
    if (entry.kind == Entry::Kind::BatchRename) {
        BatchRenamePlan inverse;
        for (const Entry::Item& item : entry.items) {
            inverse.items.append({item.currentPath, item.originalPath,
                item.currentSnapshot.directory, item.batchSourceSnapshot});
        }
        const BatchRenameResult result = executeBatchRename(inverse);
        if (!result.ok) {
            if (!result.rollbackComplete)
                entries_.removeLast();
            QString error = result.errors.join(QChar(10));
            if (!result.manualRecoveryPaths.isEmpty())
                error += QChar(10) + QStringLiteral("Inspect: ") + result.manualRecoveryPaths.join(", ");
            return {false, {}, error.isEmpty()
                ? QStringLiteral("Batch rename undo was refused; inspect the files before retrying.")
                : error, std::nullopt};
        }
        entries_.removeLast();
        QList<QPair<QString, QString>> restored;
        for (const Entry::Item& item : entry.items)
            restored.append({item.currentPath, item.originalPath});
        return {true, entry.items.constFirst().originalPath, {}, std::nullopt, restored};
    }
    QList<int> completedRoots;
    for (int index = entry.items.size() - 1; index >= 0; --index) {
        const Entry::Item& item = entry.items.at(index);
        if (!item.root)
            continue;
        if (!renamePath(item, true)) {
            bool rollbackOk = true;
            for (auto rollback = completedRoots.crbegin(); rollback != completedRoots.crend(); ++rollback)
                rollbackOk = renamePath(entry.items.at(*rollback), false) && rollbackOk;
            return {false, {}, rollbackOk
                ? QStringLiteral("Windows could not restore the moved item; completed steps were rolled back.")
                : QStringLiteral("Move undo failed and its rollback was incomplete; inspect both locations."),
                std::nullopt};
        }
        completedRoots.append(index);
    }
    entries_.removeLast();
    QList<QPair<QString, QString>> restored;
    for (const Entry::Item& item : entry.items) {
        if (entry.kind != Entry::Kind::Move || !item.currentSnapshot.directory)
            restored.append({item.currentPath, item.originalPath});
    }
    return {true, entry.items.constFirst().originalPath, {}, std::nullopt, restored};
}

} // namespace filemanager::files
