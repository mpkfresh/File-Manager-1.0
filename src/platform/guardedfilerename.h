#pragma once

#include "files/sourcesnapshot.h"
#include "platform/nativefilecopy.h"

#include <atomic>
#include <memory>
#include <QList>
#include <QStringList>
#include <functional>

namespace filemanager::platform {

struct GuardedRenameResult
{
    bool moved = false;
    bool cancelled = false;
    QString error;
    quint32 nativeCode = 0;
    files::SourceSnapshot destinationSnapshot;
};

struct GuardedDirectoryResult
{
    bool ok = false;
    bool cancelled = false;
    QString error;
    quint32 nativeCode = 0;
    QStringList createdDirectories;
    QList<files::SourceSnapshot> createdSnapshots;
};

struct GuardedBatchRenameItem
{
    QString source;
    QString destination;
    bool directory = false;
    files::SourceSnapshot expectedSource;
};

struct GuardedBatchRenameHooks
{
    // Deterministic integration-test seams. Production callers leave these empty.
    std::function<void()> afterPrepared;
    std::function<void(qsizetype)> beforeCommit;
};

struct GuardedBatchRenameResult
{
    bool ok = false;
    int renamed = 0;
    bool rollbackComplete = true;
    QStringList errors;
    QStringList manualRecoveryPaths;
};

// Retains every source object and parent directory before mutation. All staging, commit and
// rollback renames are non-replacing and directory-relative; no path-only or copy/delete fallback.
GuardedBatchRenameResult guardedBatchRename(
    const QList<GuardedBatchRenameItem>& items, const GuardedBatchRenameHooks& hooks = {});

// Holds a reviewed destination root and every directory opened/created below it. Calls are
// relative to retained directory handles and never follow a reparse point. Created directories
// are intentionally not removed automatically: callers must journal and report them honestly.
class GuardedDirectoryTree
{
public:
    GuardedDirectoryTree(const QString& reviewedRoot, const files::SourceSnapshot& expectedRoot);
    ~GuardedDirectoryTree();
    GuardedDirectoryTree(const GuardedDirectoryTree&) = delete;
    GuardedDirectoryTree& operator=(const GuardedDirectoryTree&) = delete;

    bool ready() const;
    QString error() const;
    GuardedDirectoryResult ensure(const QString& relativeDirectory,
        const std::atomic_bool* cancelled = nullptr);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// One same-volume rename, with existing destination directories only. Preparation is read-only.
// Keep the session alive through recording the result: it retains the source/destination handle
// and directory guards. This is not a batch transaction or a cross-volume copy fallback.
class GuardedFileRename
{
public:
    GuardedFileRename(const QString& source, const files::SourceSnapshot& expectedSource,
        const QString& destination, const QString& reviewedRoot,
        const files::SourceSnapshot& expectedRoot);
    ~GuardedFileRename();
    GuardedFileRename(const GuardedFileRename&) = delete;
    GuardedFileRename& operator=(const GuardedFileRename&) = delete;

    bool ready() const;
    QString error() const;
    GuardedRenameResult commit(const std::atomic_bool* cancelled = nullptr);

private:
    struct State;
    std::unique_ptr<State> state_;
};

struct GuardedCrossVolumeResult
{
    bool moved = false;
    bool destinationCommitted = false;
    bool sourceRemoved = false;
    bool cancelled = false;
    QString error;
    quint32 nativeCode = 0;
    bool metadataConfirmationRequired = false;
    QStringList metadataDegradations;
    files::SourceSnapshot destinationSnapshot;
};

// One regular-file cross-volume move. Copying reuses the metadata-preserving native backend;
// commit and source disposition are bound to retained handles. A committed destination is never
// removed automatically after a later source-removal failure and must be reported as partial.
class GuardedCrossVolumeMove
{
public:
    GuardedCrossVolumeMove(const QString& source, const files::SourceSnapshot& expectedSource,
        const QString& destination, const QString& reviewedRoot,
        const files::SourceSnapshot& expectedRoot);
    ~GuardedCrossVolumeMove();
    GuardedCrossVolumeMove(const GuardedCrossVolumeMove&) = delete;
    GuardedCrossVolumeMove& operator=(const GuardedCrossVolumeMove&) = delete;

    bool ready() const;
    QString error() const;
    // The optional list is checked again by the native copier immediately
    // before staging a cross-volume file. It is not retained after commit.
    GuardedCrossVolumeResult commit(const std::function<bool(qint64, qint64)>& progress = {},
        const std::atomic_bool* cancelled = nullptr,
        bool allowMetadataDegradation = false,
        const QStringList* approvedMetadataDegradations = nullptr,
        const NativeMetadataProbe& additionalMetadataProbe = {});

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace filemanager::platform
