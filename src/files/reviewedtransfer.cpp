#include "reviewedtransfer.h"

#include "filenamevalidator.h"
#include "scopeenumerator.h"
#include "platform/guardedfilerename.h"
#include "platform/nativefilecopy.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace filemanager::files {
namespace {

QString key(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

ReviewedTransferValidation validateSkippableMoveStructure(const TransferRequest& request,
    const std::atomic_bool* cancelled)
{
    ReviewedTransferValidation result;
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return result;
    }
    if (!request.reviewedMove || !request.reviewedSkipStaleItems
            || request.operation != TransferOperation::Move
            || request.sources.isEmpty() || request.sources.size() > MaximumReviewedMoveFiles
            || request.sources.size() != request.reviewedMoves.size()
            || !request.reviewedDestination.valid
            || !QDir::isAbsolutePath(request.destinationDirectory)
            || !isRegularDirectoryChain(request.destinationDirectory)) {
        result.errors.append(QStringLiteral("The skippable reviewed move request is incomplete or unsafe."));
        return result;
    }
    const SourceSnapshot root = captureSourceSnapshot(request.destinationDirectory);
    if (!root.valid || root.volumeSerial != request.reviewedDestination.volumeSerial
            || root.fileIndex != request.reviewedDestination.fileIndex
            || !(root.attributes & 0x10U)) {
        result.errors.append(QStringLiteral("The reviewed destination root changed."));
        return result;
    }
    QSet<QString> sources;
    QSet<QString> destinations;
    for (qsizetype index = 0; index < request.reviewedMoves.size(); ++index) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            return result;
        }
        const auto& move = request.reviewedMoves.at(index);
        const QFileInfo source(move.source);
        const QFileInfo target(move.destination);
        if (move.source.isEmpty() || move.destination.isEmpty()
                || !QDir::isAbsolutePath(move.source)
                || !QDir::isAbsolutePath(move.destination)
                || !QDir::isAbsolutePath(request.sources.at(index))
                || key(request.sources.at(index)) != key(move.source)
                || sources.contains(key(move.source))
                || destinations.contains(key(move.destination))
                || source.fileName() != target.fileName()
                || !move.sourceSnapshot.valid || move.sourceSnapshot.fileIndex == 0
                || !validateWindowsPath(move.destination).valid
                || !isPathInside(move.destination, request.destinationDirectory)
                || isPathInside(move.source, request.destinationDirectory)
                || !isRegularDirectoryChain(target.absolutePath(), true)) {
            result.errors.append(QStringLiteral("A retained tag mapping is invalid: ")
                + move.source);
            return result;
        }
        sources.insert(key(move.source));
        destinations.insert(key(move.destination));
    }
    return result;
}

bool staleSkippableItem(const ReviewedFileMove& move, QString* reason)
{
    const QFileInfo source(move.source);
    if (!isRegularDirectoryChain(source.absolutePath())
            || platform::nativePathKind(move.source) != platform::NativePathKind::Regular
            || !source.isFile()
            || captureSourceSnapshot(move.source) != move.sourceSnapshot) {
        *reason = QStringLiteral("Source changed after confirmation; make a fresh preview.");
        return true;
    }
    if (platform::nativePathKind(move.destination)
            != platform::NativePathKind::Missing) {
        *reason = QStringLiteral("Destination became occupied after confirmation; review it again.");
        return true;
    }
    return false;
}

ReviewedMoveExecutionResult executeSkippableReviewedMove(const TransferRequest& request,
    const std::function<bool(qsizetype, qint64, qint64)>& progress,
    const std::atomic_bool* cancelled, bool allowMetadataDegradation,
    const ReviewedMetadataProbe& metadataProbe,
    const std::function<void(const QString&, const SourceSnapshot&)>& directoryCreated,
    const std::function<void(const ReviewedMoveItemResult&, const SourceSnapshot&)>& itemFinished);

} // namespace

ReviewedTransferRequest createReviewedTvMove(const TvOrganizationPlan& plan,
    const QStringList& includedEpisodeSources)
{
    ReviewedTransferRequest result;
    if (!plan.complete() || !plan.destinationSnapshot.valid || plan.destinationDirectory.isEmpty()) {
        result.errors.append("A complete retained preview with a destination identity is required.");
        return result;
    }
    QSet<QString> included;
    for (const QString& source : includedEpisodeSources) {
        if (!source.isEmpty())
            included.insert(key(source));
    }
    if (included.isEmpty()) {
        result.errors.append("Select at least one complete group.");
        return result;
    }
    QSet<QString> knownGroups;
    for (const auto& candidate : plan.candidates) {
        if (!candidate.companion)
            knownGroups.insert(key(candidate.episodeSource));
    }
    for (const QString& group : included) {
        if (!knownGroups.contains(group)) {
            result.errors.append("The selection contains a group absent from the retained preview.");
            return result;
        }
    }
    result.request.destinationDirectory = plan.destinationDirectory;
    result.request.operation = TransferOperation::Move;
    result.request.reviewedMove = true;
    result.request.reviewedDestination = plan.destinationSnapshot;
    for (const auto& candidate : plan.candidates) {
        if (!included.contains(key(candidate.episodeSource)))
            continue;
        if (!candidate.sourceSnapshot.valid || result.request.sources.size() >= MaximumReviewedMoveFiles) {
            result.request = {};
            result.errors.append("A reviewed batch requires valid identities and at most 4,096 files. "
                "Select fewer complete groups; the selection was not truncated.");
            return result;
        }
        result.request.sources.append(candidate.source);
        result.request.reviewedMoves.append({candidate.source, candidate.destination, candidate.sourceSnapshot});
    }
    return result;
}

ReviewedTransferValidation validateReviewedMove(const TransferRequest& request,
    const std::atomic_bool* cancelled)
{
    ReviewedTransferValidation result;
    const auto stopped = [&] {
        result.cancelled = cancelled && cancelled->load(std::memory_order_relaxed);
        return result.cancelled;
    };
    if (stopped())
        return result;
    if (!request.reviewedMove || request.operation != TransferOperation::Move
            || request.sources.isEmpty() || request.sources.size() > MaximumReviewedMoveFiles
            || request.sources.size() != request.reviewedMoves.size()
            || !request.reviewedDestination.valid || !QDir::isAbsolutePath(request.destinationDirectory)) {
        result.errors.append("The reviewed move request is incomplete or has an unsupported size.");
        return result;
    }
    if (!isRegularDirectoryChain(request.destinationDirectory, false)) {
        result.errors.append("The reviewed destination or one of its ancestors is unavailable or a reparse point.");
        return result;
    }
    const auto destination = captureSourceSnapshot(request.destinationDirectory);
    if (!destination.valid || destination.volumeSerial != request.reviewedDestination.volumeSerial
            || destination.fileIndex != request.reviewedDestination.fileIndex
            || (request.reviewedDestination.attributes & 0x10U) == 0) {
        result.errors.append("The destination folder changed since the preview. Scan and review again.");
        return result;
    }
    QSet<QString> sourceKeys;
    QSet<QString> destinationKeys;
    for (qsizetype index = 0; index < request.reviewedMoves.size(); ++index) {
        if (stopped())
            return result;
        const auto& move = request.reviewedMoves.at(index);
        const QFileInfo source(move.source);
        const QFileInfo target(move.destination);
        const QString sourceKey = key(move.source);
        const QString destinationKey = key(move.destination);
        if (move.source.isEmpty() || move.destination.isEmpty() || !QDir::isAbsolutePath(move.source)
                || !QDir::isAbsolutePath(request.sources.at(index))
                || !QDir::isAbsolutePath(move.destination) || key(request.sources.at(index)) != sourceKey
                || sourceKeys.contains(sourceKey) || destinationKeys.contains(destinationKey)
                || source.fileName() != target.fileName() || !validateWindowsPath(move.destination).valid
                || !isPathInside(move.destination, request.destinationDirectory)
                || isPathInside(move.source, request.destinationDirectory)) {
            result.errors.append("The reviewed mapping is duplicated, changed or outside its destination: " + move.source);
            return result;
        }
        sourceKeys.insert(sourceKey);
        destinationKeys.insert(destinationKey);
        if (!move.sourceSnapshot.valid || !isRegularDirectoryChain(source.absolutePath(), false)
                || platform::nativePathKind(move.source) != platform::NativePathKind::Regular
                || !source.isFile() || move.sourceSnapshot != captureSourceSnapshot(move.source)) {
            result.errors.append("Source identity or metadata changed since preview: " + move.source);
            return result;
        }
        if (!isRegularDirectoryChain(target.absolutePath(), true)
                || platform::nativePathKind(move.destination) != platform::NativePathKind::Missing) {
            result.errors.append("Destination exists or its ancestors are unsafe: " + move.destination);
            return result;
        }
    }
    stopped();
    return result;
}

ReviewedMoveExecutionResult executeReviewedMove(const TransferRequest& request,
    const std::function<bool(qsizetype, qint64, qint64)>& progress,
    const std::atomic_bool* cancelled, bool allowMetadataDegradation,
    const ReviewedMetadataProbe& metadataProbe,
    const std::function<void(const QString&, const SourceSnapshot&)>& directoryCreated,
    const std::function<void(const ReviewedMoveItemResult&, const SourceSnapshot&)>& itemFinished)
{
    if (request.reviewedSkipStaleItems)
        return executeSkippableReviewedMove(request, progress, cancelled,
            allowMetadataDegradation, metadataProbe, directoryCreated, itemFinished);
    ReviewedMoveExecutionResult result;
    const ReviewedMetadataReview metadata = reviewReviewedMoveMetadata(
        request, cancelled, metadataProbe);
    if (!metadata.valid()) {
        result.cancelled = metadata.cancelled;
        result.errors = metadata.errors;
        if (result.cancelled && result.errors.isEmpty())
            result.errors.append("Reviewed move cancelled before execution.");
        return result;
    }
    if (metadata.confirmationRequired()) {
        bool newWarning = !allowMetadataDegradation;
        for (const auto& item : metadata.items) {
            for (const QString& detail : item.degradations) {
                const QString message = QDir::toNativeSeparators(item.source)
                    + QStringLiteral(": ") + detail;
                if (!request.reviewedApprovedMetadataWarnings.contains(message))
                    newWarning = true;
                if (!result.metadataDegradations.contains(message))
                    result.metadataDegradations.append(message);
            }
        }
        if (newWarning) {
            result.metadataConfirmationRequired = true;
            result.errors.append("New destination metadata loss needs a fresh review before this batch can start.");
            return result;
        }
        result.metadataDegradations.clear();
    }

    platform::GuardedDirectoryTree destinationTree(
        request.destinationDirectory, request.reviewedDestination);
    if (!destinationTree.ready()) {
        result.errors.append(destinationTree.error());
        return result;
    }

    for (qsizetype index = 0; index < request.reviewedMoves.size(); ++index) {
        const auto& move = request.reviewedMoves.at(index);
        ReviewedMoveItemResult item;
        item.source = move.source;
        item.destination = move.destination;
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            item.cancelled = true;
            item.error = "Reviewed move cancelled before the next item.";
            result.cancelled = true;
            result.errors.append(item.error);
            if (itemFinished)
                itemFinished(item, move.sourceSnapshot);
            result.items.append(std::move(item));
            break;
        }

        const QString parent = QFileInfo(move.destination).absolutePath();
        const QString relativeParent = QDir(request.destinationDirectory).relativeFilePath(parent);
        // A direct child is already under the retained destination root; dot is not
        // a directory component to create. Keep the tree traversal rejection intact.
        platform::GuardedDirectoryResult created;
        if (relativeParent == QStringLiteral("."))
            created.ok = true;
        else
            created = destinationTree.ensure(relativeParent, cancelled);
        result.createdDirectories.append(created.createdDirectories);
        for (qsizetype createdIndex = 0; createdIndex < created.createdDirectories.size(); ++createdIndex) {
            if (directoryCreated)
                directoryCreated(created.createdDirectories.at(createdIndex),
                    created.createdSnapshots.at(createdIndex));
        }
        if (!created.ok) {
            item.cancelled = created.cancelled;
            item.error = created.error;
            item.nativeCode = created.nativeCode;
            result.cancelled = created.cancelled;
            result.errors.append(item.error);
            if (itemFinished)
                itemFinished(item, move.sourceSnapshot);
            result.items.append(std::move(item));
            break;
        }

        const SourceSnapshot parentSnapshot = captureSourceSnapshot(parent);
        if (!parentSnapshot.valid) {
            item.error = "The retained destination folder could not be verified before moving.";
            result.errors.append(item.error);
            if (itemFinished)
                itemFinished(item, move.sourceSnapshot);
            result.items.append(std::move(item));
            break;
        }
        if (parentSnapshot.volumeSerial == move.sourceSnapshot.volumeSerial) {
            platform::GuardedFileRename guarded(move.source, move.sourceSnapshot,
                move.destination, request.destinationDirectory, request.reviewedDestination);
            const auto moved = guarded.commit(cancelled);
            item.moved = moved.moved;
            item.destinationCommitted = moved.moved;
            item.sourceRemoved = moved.moved;
            item.cancelled = moved.cancelled;
            item.error = moved.error;
            item.nativeCode = moved.nativeCode;
            item.destinationSnapshot = moved.destinationSnapshot;
        } else {
            platform::GuardedCrossVolumeMove guarded(move.source, move.sourceSnapshot,
                move.destination, request.destinationDirectory, request.reviewedDestination);
            QStringList approvedDetails;
            const QString warningPrefix = QDir::toNativeSeparators(move.source)
                + QStringLiteral(": ");
            for (const QString& warning : request.reviewedApprovedMetadataWarnings) {
                if (warning.startsWith(warningPrefix))
                    approvedDetails.append(warning.mid(warningPrefix.size()));
            }
            const auto moved = guarded.commit([&](qint64 transferred, qint64 total) {
                return !progress || progress(index, transferred, total);
            }, cancelled, allowMetadataDegradation, &approvedDetails);
            item.moved = moved.moved;
            item.destinationCommitted = moved.destinationCommitted;
            item.sourceRemoved = moved.sourceRemoved;
            item.cancelled = moved.cancelled;
            item.error = moved.error;
            item.nativeCode = moved.nativeCode;
            item.metadataConfirmationRequired = moved.metadataConfirmationRequired;
            item.metadataDegradations = moved.metadataDegradations;
            item.destinationSnapshot = moved.destinationSnapshot;
            if (item.metadataConfirmationRequired) {
                result.metadataConfirmationRequired = true;
                for (const QString& detail : item.metadataDegradations) {
                    result.metadataDegradations.append(
                        QDir::toNativeSeparators(move.source) + QStringLiteral(": ") + detail);
                }
            }
        }
        result.cancelled = item.cancelled;
        if (!item.moved) {
            result.errors.append(item.error.isEmpty()
                ? QStringLiteral("Reviewed move failed: ") + item.source : item.error);
            if (itemFinished)
                itemFinished(item, move.sourceSnapshot);
            result.items.append(std::move(item));
            break;
        }
        if (itemFinished)
            itemFinished(item, move.sourceSnapshot);
        result.items.append(std::move(item));
    }
    return result;
}

namespace {

ReviewedMoveExecutionResult executeSkippableReviewedMove(const TransferRequest& request,
    const std::function<bool(qsizetype, qint64, qint64)>& progress,
    const std::atomic_bool* cancelled, bool allowMetadataDegradation,
    const ReviewedMetadataProbe& metadataProbe,
    const std::function<void(const QString&, const SourceSnapshot&)>& directoryCreated,
    const std::function<void(const ReviewedMoveItemResult&, const SourceSnapshot&)>& itemFinished)
{
    ReviewedMoveExecutionResult result;
    const auto structure = validateSkippableMoveStructure(request, cancelled);
    if (!structure.valid()) {
        result.cancelled = structure.cancelled;
        result.errors = structure.errors;
        return result;
    }
    for (qsizetype index = 0; index < request.reviewedMoves.size(); ++index) {
        const auto& move = request.reviewedMoves.at(index);
        auto reportOmitted = [&](const QString& reason, bool wasCancelled) {
            ReviewedMoveItemResult item;
            item.source = move.source;
            item.destination = move.destination;
            item.cancelled = wasCancelled;
            item.error = reason;
            if (itemFinished)
                itemFinished(item, move.sourceSnapshot);
            result.items.append(std::move(item));
            result.errors.append(reason);
            result.cancelled = wasCancelled;
        };
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            reportOmitted(QStringLiteral("Reviewed tag move cancelled before the next file."), true);
            break;
        }
        QString staleReason;
        if (staleSkippableItem(move, &staleReason)) {
            reportOmitted(staleReason, false);
            continue;
        }
        TransferRequest single = request;
        single.reviewedSkipStaleItems = false;
        single.sources = {move.source};
        single.reviewedMoves = {move};
        const auto one = executeReviewedMove(single,
            [&](qsizetype, qint64 transferred, qint64 total) {
                return !progress || progress(index, transferred, total);
            }, cancelled, allowMetadataDegradation, metadataProbe,
            directoryCreated, itemFinished);
        result.createdDirectories.append(one.createdDirectories);
        result.metadataConfirmationRequired |= one.metadataConfirmationRequired;
        result.metadataDegradations.append(one.metadataDegradations);
        if (one.items.isEmpty()) {
            if (!one.cancelled && staleSkippableItem(move, &staleReason)) {
                reportOmitted(staleReason, false);
                continue;
            }
            reportOmitted(one.errors.isEmpty()
                ? QStringLiteral("The reviewed tag move stopped before this file.")
                : one.errors.join('\n'), one.cancelled);
            break;
        }
        result.items.append(one.items);
        result.errors.append(one.errors);
        result.cancelled = one.cancelled;
        if (one.complete())
            continue;
        if (!one.cancelled && one.items.size() == 1
                && !one.items.first().destinationCommitted
                && move.sourceSnapshot.volumeSerial
                    == request.reviewedDestination.volumeSerial
                && staleSkippableItem(move, &staleReason))
            continue;
        break;
    }
    return result;
}

} // namespace

ReviewedMetadataReview reviewReviewedMoveMetadata(const TransferRequest& request,
    const std::atomic_bool* cancelled, const ReviewedMetadataProbe& metadataProbe)
{
    ReviewedMetadataReview result;
    const ReviewedTransferValidation validation = validateReviewedMove(request, cancelled);
    if (!validation.valid()) {
        result.cancelled = validation.cancelled;
        result.errors = validation.errors;
        return result;
    }
    const auto probe = metadataProbe ? metadataProbe
        : ReviewedMetadataProbe([](const QString& source, const QString& destinationLocation) {
            return platform::reviewNativeCopyMetadata(source, destinationLocation);
        });
    for (const auto& move : request.reviewedMoves) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            return result;
        }
        if (move.sourceSnapshot.volumeSerial == request.reviewedDestination.volumeSerial)
            continue;
        const auto reviewed = probe(move.source, request.destinationDirectory);
        if (!reviewed.inspected || !reviewed.error.isEmpty()) {
            result.errors.append(reviewed.error.isEmpty()
                ? QStringLiteral("Could not verify destination metadata compatibility: ") + move.source
                : reviewed.error + QStringLiteral(": ") + move.source);
            return result;
        }
        result.items.append({move.source, move.destination, reviewed.degradations});
    }
    return result;
}

} // namespace filemanager::files
