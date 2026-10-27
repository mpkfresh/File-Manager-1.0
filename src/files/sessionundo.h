#pragma once

#include "files/transfercontroller.h"
#include "files/fileoperations.h"
#include "files/sourcesnapshot.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace filemanager::files {

struct BatchRenamePlan;

struct SessionUndoResult
{
    bool ok = false;
    QString restoredPath;
    QString error;
    std::optional<TransferRequest> queuedRequest;
    // Paths restored by a completed in-process undo. A queued inverse is
    // reported by its transfer manifest after it actually commits.
    QList<QPair<QString, QString>> restoredPathChanges;
};

class SessionUndoStack final
{
public:
    static constexpr int MaximumEntries = 100;

    bool recordRename(const QString& originalPath, const QString& renamedPath);
    bool recordBatchRename(const BatchRenamePlan& plan);
    bool recordMove(const QStringList& originalSources,
        const QList<TransferManifestEntry>& manifest);
    bool recordTrash(const QList<TrashResult>& results);
    bool canUndo() const;
    QString nextDescription() const;
    SessionUndoResult undoLast();
    int size() const { return entries_.size(); }
    quint64 topEntryId() const { return entries_.isEmpty() ? 0 : entries_.constLast().id; }
    void clear() { entries_.clear(); }
    void discardTrashEntriesContaining(const QList<QByteArray>& recycledItemIds);
    void discardAllTrashEntries();

private:
    struct Snapshot
    {
        bool valid = false;
        bool directory = false;
        quint64 volumeSerial = 0;
        quint64 fileIndex = 0;
        qint64 size = -1;
        qint64 modifiedMsecs = 0;
    };
    struct Entry
    {
        quint64 id = 0;
        enum class Kind { Rename, BatchRename, Move, Trash } kind = Kind::Rename;
        struct Item
        {
            QString originalPath;
            QString currentPath;
            Snapshot currentSnapshot;
            bool root = false;
            QByteArray recycledItemId;
            SourceSnapshot batchSourceSnapshot;
        };
        QList<Item> items;
        bool requiresQueuedTransfer = false;
    };

    static Snapshot capture(const QString& path);
    static bool matches(const Snapshot& expected, const Snapshot& current);
    static bool renamePath(const Entry::Item& item, bool undo);
    static bool preflight(const Entry& entry);
    QList<Entry> entries_;
    quint64 lastAssignedEntryId_ = 0;
};

} // namespace filemanager::files
