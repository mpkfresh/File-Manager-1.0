#include "transfercontroller.h"
#include "filenamevalidator.h"
#include "reviewedtransfer.h"
#include "platform/nativefilecopy.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QMutex>
#include <QPointer>
#include <QStorageInfo>
#include <QThread>
#include <QWaitCondition>

#include <atomic>
#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace filemanager::files {

struct TransferController::SharedState
{
    std::atomic_bool cancelled = false;
    std::atomic_bool paused = false;
    QMutex pauseMutex;
    QWaitCondition resumeRequested;
    QMutex conflictMutex;
    QWaitCondition conflictResolved;
    std::optional<ConflictDecision> decision;
    std::optional<ConflictDecision> applyToAllDecision;
    QMutex metadataMutex;
    QWaitCondition metadataResolved;
    std::optional<bool> metadataDecision;
    bool allowMetadataDegradation = false;
    quint64 generation = 0;
};

namespace {

struct WorkerContext
{
    QPointer<TransferController> controller;
    std::shared_ptr<TransferController::SharedState> state;
    quint64 generation = 0;
    qint64 completedBytes = 0;
    qint64 totalBytes = 0;
    QStringList errors;
    TransferFailureCategory failureCategory = TransferFailureCategory::Unknown;
    quint32 failureNativeCode = 0;
    QElapsedTimer transferTimer;
    TransferController::CopyBackend copyBackend;
};

bool waitIfPaused(WorkerContext& context)
{
    if (!context.state->paused.load())
        return !context.state->cancelled.load();
    QMutexLocker locker(&context.state->pauseMutex);
    while (context.state->paused.load() && !context.state->cancelled.load())
        context.state->resumeRequested.wait(&context.state->pauseMutex);
    return !context.state->cancelled.load();
}

qint64 byteCount(const QString& path, const std::shared_ptr<TransferController::SharedState>& state)
{
    if (state->cancelled.load())
        return 0;
    const QFileInfo info(path);
    const auto kind = filemanager::platform::nativePathKind(path);
    if (kind == filemanager::platform::NativePathKind::SymbolicLink)
        return 0;
    if (info.isFile())
        return info.size();
    if (!info.isDir())
        return 0;
    qint64 total = 0;
    QDirIterator iterator(path, QDir::Files | QDir::Hidden | QDir::System,
        QDirIterator::Subdirectories);
    while (iterator.hasNext() && !state->cancelled.load()) {
        iterator.next();
        const QFileInfo child = iterator.fileInfo();
        if (!child.isSymbolicLink())
            total += child.size();
    }
    return total;
}

bool pathsReferToSameItem(const QString& source, const QString& destination)
{
    if (!QFileInfo::exists(destination))
        return false;
    const QFileInfo sourceInfo(source);
    const QFileInfo destinationInfo(destination);
    const QString sourceCanonical = sourceInfo.canonicalFilePath();
    const QString destinationCanonical = destinationInfo.canonicalFilePath();
    if (!sourceCanonical.isEmpty() && !destinationCanonical.isEmpty()
            && sourceCanonical.compare(destinationCanonical, Qt::CaseInsensitive) == 0)
        return true;
#ifdef Q_OS_WIN
    const auto openIdentityHandle = [](const QString& path) {
        return CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    };
    const HANDLE sourceHandle = openIdentityHandle(sourceInfo.absoluteFilePath());
    const HANDLE destinationHandle = openIdentityHandle(destinationInfo.absoluteFilePath());
    BY_HANDLE_FILE_INFORMATION sourceIdentity{};
    BY_HANDLE_FILE_INFORMATION destinationIdentity{};
    const bool comparable = sourceHandle != INVALID_HANDLE_VALUE
        && destinationHandle != INVALID_HANDLE_VALUE
        && GetFileInformationByHandle(sourceHandle, &sourceIdentity)
        && GetFileInformationByHandle(destinationHandle, &destinationIdentity);
    if (sourceHandle != INVALID_HANDLE_VALUE)
        CloseHandle(sourceHandle);
    if (destinationHandle != INVALID_HANDLE_VALUE)
        CloseHandle(destinationHandle);
    if (comparable) {
        return sourceIdentity.dwVolumeSerialNumber == destinationIdentity.dwVolumeSerialNumber
            && sourceIdentity.nFileIndexHigh == destinationIdentity.nFileIndexHigh
            && sourceIdentity.nFileIndexLow == destinationIdentity.nFileIndexLow;
    }
#endif
    return false;
}

QString preflightError(const QString& source, const QString& destination)
{
    const QFileInfo sourceInfo(source);
    const auto kind = filemanager::platform::nativePathKind(source);
    if (kind == filemanager::platform::NativePathKind::Missing)
        return "Source no longer exists: " + source;
    if (kind == filemanager::platform::NativePathKind::CloudPlaceholder)
        return "Cloud item is not locally available. Make it available offline and retry: " + source;
    if (kind == filemanager::platform::NativePathKind::UnsupportedReparse)
        return "Unsupported reparse point was not followed: " + source;
    const QString normalizedSource = QDir::cleanPath(sourceInfo.absoluteFilePath());
    const QString normalizedDestination = QDir::cleanPath(QFileInfo(destination).absoluteFilePath());
    if (normalizedSource.compare(normalizedDestination, Qt::CaseInsensitive) == 0
            || pathsReferToSameItem(source, destination))
        return "Source and destination are the same path: " + source;
    if (sourceInfo.isDir() && isPathInside(normalizedDestination, normalizedSource))
        return "A folder cannot be transferred inside itself: " + source;
    return {};
}

QString keepBothPath(const QString& destination)
{
    const QFileInfo info(destination);
    const QString stem = info.isDir() ? info.fileName() : info.completeBaseName();
    const QString suffix = info.isDir() || info.suffix().isEmpty() ? QString() : '.' + info.suffix();
    for (int number = 2; number < 10000; ++number) {
        const QString candidate = info.dir().filePath(
            QString("%1 (%2)%3").arg(stem).arg(number).arg(suffix));
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return {};
}

ConflictDecision waitForConflict(
    WorkerContext& context, const QString& source, const QString& destination, bool directory)
{
    {
        QMutexLocker locker(&context.state->conflictMutex);
        if (context.state->applyToAllDecision)
            return *context.state->applyToAllDecision;
        context.state->decision.reset();
    }
    if (context.controller) {
        QMetaObject::invokeMethod(context.controller,
            [guard = context.controller, generation = context.generation, source, destination, directory] {
                if (guard)
                    emit guard->conflict(generation, source, destination, directory);
            });
    }
    QMutexLocker locker(&context.state->conflictMutex);
    while (!context.state->decision && !context.state->cancelled.load())
        context.state->conflictResolved.wait(&context.state->conflictMutex);
    return context.state->cancelled.load()
        ? ConflictDecision::Cancel
        : *context.state->decision;
}

bool waitForMetadataDegradation(WorkerContext& context, const QString& source,
    const QString& destination, const QStringList& details)
{
    {
        QMutexLocker locker(&context.state->metadataMutex);
        if (context.state->allowMetadataDegradation)
            return true;
        context.state->metadataDecision.reset();
    }
    if (context.controller) {
        QMetaObject::invokeMethod(context.controller,
            [guard = context.controller, generation = context.generation,
                source, destination, details] {
                if (guard)
                    emit guard->metadataDegradation(
                        generation, source, destination, details);
            });
    }
    QMutexLocker locker(&context.state->metadataMutex);
    while (!context.state->metadataDecision && !context.state->cancelled.load())
        context.state->metadataResolved.wait(&context.state->metadataMutex);
    if (context.state->cancelled.load() || !context.state->metadataDecision.value_or(false)) {
        context.state->cancelled.store(true);
        return false;
    }
    context.state->allowMetadataDegradation = true;
    return true;
}

void reportProgress(WorkerContext& context, const QString& path)
{
    if (!context.controller)
        return;
    const TransferProgress details = TransferProgress::calculate(context.completedBytes,
        context.totalBytes, context.transferTimer.isValid() ? context.transferTimer.elapsed() : 0,
        path);
    QMetaObject::invokeMethod(context.controller,
        [guard = context.controller, generation = context.generation,
            completed = context.completedBytes, total = context.totalBytes, path, details] {
            if (guard) {
                emit guard->progress(generation, completed, total, path);
                emit guard->progressDetails(generation, details);
            }
        });
}

TransferManifestEntry manifestEntry(const QString& source, const QString& destination,
    bool destinationCreated)
{
    const QFileInfo info(destination);
    TransferManifestEntry entry;
    entry.sourcePath = source;
    entry.destinationPath = info.absoluteFilePath();
    entry.directory = info.isDir();
    entry.destinationCreated = destinationCreated;
    entry.size = info.isFile() ? info.size() : 0;
    entry.lastModifiedUtcMs = info.lastModified().toUTC().toMSecsSinceEpoch();
#ifdef Q_OS_WIN
    const auto captureIdentity = [](const QString& path, quint64& volume, quint64& index) {
        const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
            FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION identity{};
        bool available = false;
        if (handle != INVALID_HANDLE_VALUE) {
            if (GetFileInformationByHandle(handle, &identity)) {
                volume = identity.dwVolumeSerialNumber;
                index = (static_cast<quint64>(identity.nFileIndexHigh) << 32)
                    | identity.nFileIndexLow;
                available = true;
            }
            CloseHandle(handle);
        }
        return available;
    };
    entry.identityAvailable = captureIdentity(
        entry.destinationPath, entry.volumeSerial, entry.fileIndex);
    const QFileInfo sourceInfo(source);
    entry.sourceSize = sourceInfo.isFile() ? sourceInfo.size() : 0;
    entry.sourceLastModifiedUtcMs = sourceInfo.lastModified().toUTC().toMSecsSinceEpoch();
    entry.sourceIdentityAvailable = captureIdentity(
        sourceInfo.absoluteFilePath(), entry.sourceVolumeSerial, entry.sourceFileIndex);
#endif
    return entry;
}

void reportManifestEntry(WorkerContext& context, const QString& source,
    const QString& destination, bool destinationCreated)
{
    if (!context.controller)
        return;
    const TransferManifestEntry entry = manifestEntry(source, destination, destinationCreated);
    QMetaObject::invokeMethod(context.controller,
        [guard = context.controller, generation = context.generation, entry] {
            if (guard)
                emit guard->manifestEntryCommitted(generation, entry);
        });
}

void reportManifestObject(WorkerContext& context, const TransferManifestEntry& entry)
{
    if (!context.controller)
        return;
    QMetaObject::invokeMethod(context.controller,
        [guard = context.controller, generation = context.generation, entry] {
            if (guard)
                emit guard->manifestEntryCommitted(generation, entry);
        });
}

qint64 windowsFileTimeMilliseconds(quint64 value)
{
    constexpr quint64 ticksToUnixEpoch = 116'444'736'000'000'000ULL;
    return value >= ticksToUnixEpoch
        ? static_cast<qint64>((value - ticksToUnixEpoch) / 10'000ULL) : 0;
}

TransferManifestEntry reviewedDirectoryManifest(const QString& destination,
    const SourceSnapshot& snapshot)
{
    TransferManifestEntry entry;
    entry.destinationPath = QFileInfo(destination).absoluteFilePath();
    entry.directory = true;
    entry.destinationCreated = true;
    entry.lastModifiedUtcMs = windowsFileTimeMilliseconds(snapshot.lastWriteTime);
    entry.volumeSerial = snapshot.volumeSerial;
    entry.fileIndex = snapshot.fileIndex;
    entry.identityAvailable = snapshot.valid;
    return entry;
}

TransferManifestEntry reviewedFileManifest(const ReviewedMoveItemResult& item,
    const SourceSnapshot& sourceSnapshot)
{
    TransferManifestEntry entry;
    entry.sourcePath = QFileInfo(item.source).absoluteFilePath();
    entry.destinationPath = QFileInfo(item.destination).absoluteFilePath();
    entry.destinationCreated = item.destinationCommitted;
    entry.sourceRemoved = item.sourceRemoved;
    entry.size = static_cast<qint64>(item.destinationSnapshot.size);
    entry.lastModifiedUtcMs = windowsFileTimeMilliseconds(item.destinationSnapshot.lastWriteTime);
    entry.volumeSerial = item.destinationSnapshot.volumeSerial;
    entry.fileIndex = item.destinationSnapshot.fileIndex;
    entry.identityAvailable = item.destinationSnapshot.valid;
    entry.sourceSize = static_cast<qint64>(sourceSnapshot.size);
    entry.sourceLastModifiedUtcMs = windowsFileTimeMilliseconds(sourceSnapshot.lastWriteTime);
    entry.sourceVolumeSerial = sourceSnapshot.volumeSerial;
    entry.sourceFileIndex = sourceSnapshot.fileIndex;
    entry.sourceIdentityAvailable = sourceSnapshot.valid;
    return entry;
}

void reportSourceRemoved(WorkerContext& context, const QString& source)
{
    if (!context.controller)
        return;
    QMetaObject::invokeMethod(context.controller,
        [guard = context.controller, generation = context.generation, source] {
            if (guard)
                emit guard->manifestSourceRemoved(generation, source);
        });
}

void reportItemFailure(WorkerContext& context, const QString& source,
    const QString& destination, const QString& error,
    TransferFailureCategory category = TransferFailureCategory::Unknown,
    quint32 nativeCode = 0)
{
    if (!context.controller)
        return;
    const TransferFailure failure{source, destination, error,
        isRetryableTransferFailure(category), category, nativeCode};
    QMetaObject::invokeMethod(context.controller,
        [guard = context.controller, generation = context.generation, failure] {
            if (guard)
                emit guard->itemFailed(generation, failure);
        });
}

bool removePath(const QString& path)
{
    const auto kind = filemanager::platform::nativePathKind(path);
    if (kind == filemanager::platform::NativePathKind::SymbolicLink)
        return QFile::remove(path) || QDir().rmdir(path);
    const QFileInfo info(path);
    if (!info.isDir())
        return QFile::remove(path);
    const auto entries = QDir(path).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& child : entries) {
        if (!removePath(child.absoluteFilePath()))
            return false;
    }
    return QDir().rmdir(path);
}

QString copyDirectoryMetadata(const QString& source, const QString& destination)
{
#ifdef Q_OS_WIN
    const HANDLE sourceHandle = CreateFileW(reinterpret_cast<LPCWSTR>(source.utf16()),
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (sourceHandle == INVALID_HANDLE_VALUE)
        return QStringLiteral("Could not read source folder metadata.");
    const HANDLE destinationHandle = CreateFileW(reinterpret_cast<LPCWSTR>(destination.utf16()),
        FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (destinationHandle == INVALID_HANDLE_VALUE) {
        CloseHandle(sourceHandle);
        return QStringLiteral("Could not open the destination folder to preserve metadata.");
    }
    FILETIME created{}, accessed{}, written{};
    const bool timesRead = GetFileTime(sourceHandle, &created, &accessed, &written) != FALSE;
    const bool timesWritten = timesRead
        && SetFileTime(destinationHandle, &created, &accessed, &written) != FALSE;
    CloseHandle(destinationHandle);
    CloseHandle(sourceHandle);
    if (!timesWritten)
        return QStringLiteral("Could not preserve folder timestamps.");

    const DWORD sourceAttributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(source.utf16()));
    const DWORD preserved = sourceAttributes
        & (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED
            | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_TEMPORARY);
    if (sourceAttributes == INVALID_FILE_ATTRIBUTES
            || !SetFileAttributesW(reinterpret_cast<LPCWSTR>(destination.utf16()), preserved)) {
        return QStringLiteral("Could not preserve folder attributes.");
    }
#else
    Q_UNUSED(source);
    Q_UNUSED(destination);
#endif
    return {};
}

enum class CopyOutcome { Copied, Skipped, Failed };

CopyOutcome copyNode(WorkerContext& context, const QString& source, QString destination,
    QStringList& createdPaths)
{
    if (!waitIfPaused(context))
        return CopyOutcome::Failed;
    const QFileInfo sourceInfo(source);
    const auto sourceKind = filemanager::platform::nativePathKind(source);
    if (sourceKind == filemanager::platform::NativePathKind::Missing) {
        context.errors.append("Source no longer exists: " + source);
        context.failureCategory = TransferFailureCategory::SourceUnavailable;
#ifdef Q_OS_WIN
        context.failureNativeCode = ERROR_FILE_NOT_FOUND;
#endif
        return CopyOutcome::Failed;
    }
    if (sourceKind == filemanager::platform::NativePathKind::CloudPlaceholder) {
        context.errors.append(
            "Cloud item is not locally available. Make it available offline and retry: " + source);
        context.failureCategory = TransferFailureCategory::SourceUnavailable;
        return CopyOutcome::Failed;
    }
    if (sourceKind == filemanager::platform::NativePathKind::UnsupportedReparse) {
        context.errors.append("Unsupported reparse point was not followed: " + source);
        context.failureCategory = TransferFailureCategory::InvalidRequest;
        return CopyOutcome::Failed;
    }
    bool destinationWasNew = !QFileInfo::exists(destination);
    if (!destinationWasNew) {
        const ConflictDecision decision = waitForConflict(
            context, source, destination, sourceInfo.isDir());
        if (decision == ConflictDecision::Cancel) {
            context.state->cancelled.store(true);
            return CopyOutcome::Failed;
        }
        if (decision == ConflictDecision::Skip)
            return CopyOutcome::Skipped;
        if (decision == ConflictDecision::KeepBoth) {
            destination = keepBothPath(destination);
            if (destination.isEmpty()) {
                context.errors.append("Could not generate a unique destination name for: " + source);
                context.failureCategory = TransferFailureCategory::InvalidRequest;
                return CopyOutcome::Failed;
            }
            destinationWasNew = true;
        }
    }

    if (sourceInfo.isDir() && sourceKind != filemanager::platform::NativePathKind::SymbolicLink) {
        if (!QFileInfo::exists(destination)) {
            if (!QDir().mkpath(destination)) {
                context.errors.append("Could not create destination folder: " + destination);
                context.failureCategory = TransferFailureCategory::DestinationUnavailable;
                return CopyOutcome::Failed;
            }
            createdPaths.prepend(destination);
            reportManifestEntry(context, source, destination, true);
        }
        const auto entries = QDir(source).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo& child : entries) {
            if (copyNode(context, child.absoluteFilePath(),
                    QDir(destination).filePath(child.fileName()), createdPaths)
                == CopyOutcome::Failed)
                return CopyOutcome::Failed;
        }
        if (destinationWasNew) {
            const QString metadataError = copyDirectoryMetadata(source, destination);
            if (!metadataError.isEmpty()) {
                context.errors.append(metadataError + ": " + source);
                context.failureCategory = TransferFailureCategory::IntegrityFailure;
                return CopyOutcome::Failed;
            }
            reportManifestEntry(context, source, destination, true);
        }
        return CopyOutcome::Copied;
    }

    const qint64 completedBefore = context.completedBytes;
    const auto copyWithPolicy = [&](bool allowMetadataDegradation) {
        return context.copyBackend(source, destination,
            !destinationWasNew, [&](qint64 transferred, qint64) {
            if (!waitIfPaused(context))
                return false;
            context.completedBytes = completedBefore
                + qMin(transferred, sourceKind == filemanager::platform::NativePathKind::SymbolicLink
                    ? qint64{0} : sourceInfo.size());
            reportProgress(context, source);
            return !context.state->cancelled.load();
        }, allowMetadataDegradation);
    };
    auto copyResult = copyWithPolicy(false);
    if (copyResult.metadataConfirmationRequired) {
        if (!waitForMetadataDegradation(context, source, destination,
                copyResult.metadataDegradations))
            return CopyOutcome::Failed;
        copyResult = copyWithPolicy(true);
    }
    if (!copyResult.ok) {
        if (!copyResult.cancelled) {
            context.errors.append(copyResult.error + ": " + source);
            context.failureCategory = classifyNativeTransferFailure(copyResult.nativeCode);
            context.failureNativeCode = copyResult.nativeCode;
        } else {
            context.failureCategory = TransferFailureCategory::Cancelled;
            context.failureNativeCode = copyResult.nativeCode;
        }
        return CopyOutcome::Failed;
    }
    context.completedBytes = completedBefore
        + (sourceKind == filemanager::platform::NativePathKind::SymbolicLink
            ? qint64{0} : sourceInfo.size());
    reportProgress(context, source);
    reportManifestEntry(context, source, destination, destinationWasNew);
    if (destinationWasNew)
        createdPaths.prepend(destination);
    return CopyOutcome::Copied;
}

} // namespace

TransferController::TransferController(QObject* parent)
    : TransferController({}, {}, parent)
{
}

TransferController::TransferController(CopyBackend copyBackend, QObject* parent)
    : TransferController(std::move(copyBackend), {}, parent)
{
}

TransferController::TransferController(CopyBackend copyBackend,
    DestinationProbe destinationProbe, QObject* parent)
    : QObject(parent), copyBackend_(std::move(copyBackend)),
      destinationProbe_(std::move(destinationProbe))
{
    if (!copyBackend_) {
        copyBackend_ = [](const QString& source, const QString& destination,
                           bool replaceExisting,
                           const std::function<bool(qint64, qint64)>& progress,
                           bool allowMetadataDegradation) {
            return filemanager::platform::copyFileAtomically(source, destination,
                replaceExisting, progress, allowMetadataDegradation);
        };
    }
    if (!destinationProbe_) {
        destinationProbe_ = [](const QString& path) {
            DestinationState state;
            const QFileInfo destination(path);
            state.available = destination.isDir();
            if (state.available) {
                const QStorageInfo storage(path);
                if (storage.isValid() && storage.isReady())
                    state.availableBytes = storage.bytesAvailable();
            }
            return state;
        };
    }
}

TransferController::~TransferController()
{
    cancel();
}

quint64 TransferController::start(TransferRequest request)
{
    cancel();
    const quint64 generation = ++generation_;
    state_ = std::make_shared<SharedState>();
    state_->generation = generation;
    const auto state = state_;
    QPointer<TransferController> guard(this);
    QThread* worker = QThread::create([guard, state, generation,
            request = std::move(request), copyBackend = copyBackend_,
            destinationProbe = destinationProbe_] {
        WorkerContext context{guard, state, generation};
        context.copyBackend = copyBackend;
        if (request.hasReviewConstraints()) {
            // Reviewed mappings are handled only by the retained native executor. Malformed or
            // stale requests fail there and can never fall through to the path-only loop below.
            QList<qint64> completedBefore;
            qint64 accumulated = 0;
            for (const auto& move : request.reviewedMoves) {
                completedBefore.append(accumulated);
                accumulated += static_cast<qint64>(move.sourceSnapshot.size);
            }
            context.totalBytes = accumulated;
            qint64 movedBytes = 0;
            context.transferTimer.start();
            reportProgress(context, {});
            const auto execution = executeReviewedMove(request,
                [&](qsizetype index, qint64 transferred, qint64 total) {
                    if (!waitIfPaused(context))
                        return false;
                    const qint64 base = request.reviewedSkipStaleItems ? movedBytes
                        : index >= 0 && index < completedBefore.size()
                            ? completedBefore.at(index) : context.completedBytes;
                    context.completedBytes = base + qMin(transferred, total);
                    const QString path = index >= 0 && index < request.reviewedMoves.size()
                        ? request.reviewedMoves.at(index).source : QString{};
                    reportProgress(context, path);
                    return !state->cancelled.load(std::memory_order_relaxed);
                }, &state->cancelled, request.reviewedMetadataDegradationApproved, {},
                [&](const QString& directory, const SourceSnapshot& snapshot) {
                    reportManifestObject(context, reviewedDirectoryManifest(directory, snapshot));
                },
                [&](const ReviewedMoveItemResult& item, const SourceSnapshot& sourceSnapshot) {
                    if (request.reviewedSkipStaleItems && item.moved) {
                        movedBytes += static_cast<qint64>(sourceSnapshot.size);
                        context.completedBytes = movedBytes;
                        reportProgress(context, item.source);
                    }
                    if (item.destinationCommitted)
                        reportManifestObject(context, reviewedFileManifest(item, sourceSnapshot));
                    if (item.sourceRemoved)
                        reportSourceRemoved(context, item.source);
                    if (!item.moved) {
                        reportItemFailure(context, item.source, item.destination,
                            item.error.isEmpty() ? QStringLiteral("The reviewed item could not be moved.")
                                                 : item.error,
                            classifyNativeTransferFailure(item.nativeCode), item.nativeCode);
                    }
                });
            context.errors = execution.errors;
            if (execution.complete()) {
                context.completedBytes = context.totalBytes;
                reportProgress(context, {});
            } else if (execution.items.isEmpty()) {
                const QString error = execution.errors.isEmpty()
                    ? QStringLiteral("The reviewed move request was refused before execution.")
                    : execution.errors.join('\n');
                for (const QString& source : request.sources)
                    reportItemFailure(context, source, request.destinationDirectory, error,
                        TransferFailureCategory::InvalidRequest);
            }
            if (guard) {
                QMetaObject::invokeMethod(guard, [guard, generation,
                        cancelled = execution.cancelled, errors = execution.errors] {
                    if (guard)
                        emit guard->finished(generation, cancelled, errors);
                });
            }
            return;
        }
        for (const QString& source : request.sources) {
            const QString destination = QDir(request.destinationDirectory)
                                            .filePath(QFileInfo(source).fileName());
            if (preflightError(source, destination).isEmpty())
                context.totalBytes += byteCount(source, state);
        }
        context.transferTimer.start();
        reportProgress(context, {});
        bool globalPreflightFailed = false;
        TransferFailureCategory globalFailureCategory = TransferFailureCategory::Unknown;
        const DestinationState destinationState = destinationProbe(request.destinationDirectory);
        if (!destinationState.available) {
            context.errors.append("Destination folder is unavailable: "
                + request.destinationDirectory);
            globalPreflightFailed = true;
            globalFailureCategory = TransferFailureCategory::DestinationUnavailable;
        } else {
            if (!hasSufficientTransferSpace(
                    context.totalBytes, destinationState.availableBytes)) {
                context.errors.append(QStringLiteral("Insufficient free space: %1 bytes are required "
                    "but only %2 bytes are available in %3.")
                    .arg(context.totalBytes).arg(destinationState.availableBytes)
                    .arg(request.destinationDirectory));
                globalPreflightFailed = true;
                globalFailureCategory = TransferFailureCategory::InsufficientSpace;
            }
        }
        for (const QString& source : request.sources) {
            if (globalPreflightFailed) {
                const QString destination = QDir(request.destinationDirectory)
                                                .filePath(QFileInfo(source).fileName());
                reportItemFailure(context, source, destination, context.errors.constLast(),
                    globalFailureCategory);
                continue;
            }
            if (state->cancelled.load())
                break;
            const QString destination = QDir(request.destinationDirectory)
                                            .filePath(QFileInfo(source).fileName());
            const QString validationError = preflightError(source, destination);
            if (!validationError.isEmpty()) {
                context.errors.append(validationError);
                const auto category = validationError.contains(
                        QStringLiteral("no longer exists"), Qt::CaseInsensitive)
                    ? TransferFailureCategory::SourceUnavailable
                    : TransferFailureCategory::InvalidRequest;
                reportItemFailure(context, source, destination, validationError, category);
                continue;
            }
            const bool destinationExisted = QFileInfo::exists(destination);
            QStringList createdPaths;
            const qsizetype errorCountBefore = context.errors.size();
            context.failureCategory = TransferFailureCategory::Unknown;
            context.failureNativeCode = 0;
            const CopyOutcome outcome = copyNode(context, source, destination, createdPaths);
            if (outcome == CopyOutcome::Failed) {
                for (const QString& created : createdPaths)
                    removePath(created);
                if (!state->cancelled.load()) {
                    const QString error = context.errors.mid(errorCountBefore).join('\n');
                    reportItemFailure(context, source, destination,
                        error.isEmpty() ? QStringLiteral("The item could not be transferred.") : error,
                        context.failureCategory, context.failureNativeCode);
                }
                continue;
            }
            if (outcome == CopyOutcome::Skipped)
                continue;
            if (request.operation == TransferOperation::Move && destinationExisted) {
                const QString error =
                    "Source retained because the move merged with an existing destination: " + source;
                context.errors.append(error);
                reportItemFailure(context, source, destination, error);
            } else if (request.operation == TransferOperation::Move && !removePath(source)) {
                if (!destinationExisted) {
                    for (const QString& created : createdPaths)
                        removePath(created);
                    const QString error =
                        "Move rolled back because the source could not be removed: " + source;
                    context.errors.append(error);
                    reportItemFailure(context, source, destination, error);
                } else {
                    const QString error = "Source could not be removed after copying; the source "
                        "was retained and destination changes remain: " + source;
                    context.errors.append(error);
                    reportItemFailure(context, source, destination, error);
                }
            } else if (request.operation == TransferOperation::Move) {
                reportSourceRemoved(context, source);
            }
        }
        if (guard) {
            QMetaObject::invokeMethod(guard,
                [guard, generation, cancelled = state->cancelled.load(), errors = context.errors] {
                    if (guard)
                        emit guard->finished(generation, cancelled, errors);
                });
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return generation;
}

void TransferController::cancel()
{
    if (!state_)
        return;
    state_->cancelled.store(true);
    state_->conflictResolved.wakeAll();
    state_->metadataResolved.wakeAll();
    state_->resumeRequested.wakeAll();
}

void TransferController::pause()
{
    if (state_)
        state_->paused.store(true);
}

void TransferController::resume()
{
    if (!state_)
        return;
    state_->paused.store(false);
    state_->resumeRequested.wakeAll();
}

void TransferController::resolveConflict(
    quint64 generation, ConflictDecision decision, bool applyToAll)
{
    if (!state_ || state_->generation != generation)
        return;
    QMutexLocker locker(&state_->conflictMutex);
    state_->decision = decision;
    if (applyToAll && decision != ConflictDecision::Cancel)
        state_->applyToAllDecision = decision;
    state_->conflictResolved.wakeAll();
}

void TransferController::resolveMetadataDegradation(quint64 generation, bool proceed)
{
    if (!state_ || state_->generation != generation)
        return;
    QMutexLocker locker(&state_->metadataMutex);
    state_->metadataDecision = proceed;
    state_->metadataResolved.wakeAll();
}

} // namespace filemanager::files
