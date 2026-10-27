#pragma once

#include "platform/nativefilecopy.h"
#include "sourcesnapshot.h"

#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>

namespace filemanager::files {

enum class TransferOperation { Copy, Move };
enum class ConflictDecision { Replace, Skip, KeepBoth, Cancel };
enum class TransferFailureCategory {
    Unknown,
    SourceUnavailable,
    DestinationUnavailable,
    InsufficientSpace,
    PermissionDenied,
    SharingViolation,
    DeviceDisconnected,
    Cancelled,
    InvalidRequest,
    IntegrityFailure
};

inline TransferFailureCategory classifyNativeTransferFailure(quint32 nativeCode)
{
    // Stable Win32 error values are used here so classification remains a pure, portable test seam.
    switch (nativeCode) {
    case 2: return TransferFailureCategory::SourceUnavailable; // ERROR_FILE_NOT_FOUND
    case 3: case 53: case 67: case 1201:
        return TransferFailureCategory::DestinationUnavailable;
    case 39: case 112:
        return TransferFailureCategory::InsufficientSpace;
    case 5: case 19: case 1314:
        return TransferFailureCategory::PermissionDenied;
    case 32: case 33:
        return TransferFailureCategory::SharingViolation;
    case 21: case 55: case 59: case 64: case 1117: case 1167: case 1231:
        return TransferFailureCategory::DeviceDisconnected;
    case 995: case 1235:
        return TransferFailureCategory::Cancelled;
    case 123: case 267:
        return TransferFailureCategory::InvalidRequest;
    case 23: case 323:
        return TransferFailureCategory::IntegrityFailure;
    default:
        return TransferFailureCategory::Unknown;
    }
}

inline bool isRetryableTransferFailure(TransferFailureCategory category)
{
    switch (category) {
    case TransferFailureCategory::SourceUnavailable:
    case TransferFailureCategory::DestinationUnavailable:
    case TransferFailureCategory::InsufficientSpace:
    case TransferFailureCategory::PermissionDenied:
    case TransferFailureCategory::SharingViolation:
    case TransferFailureCategory::DeviceDisconnected:
    case TransferFailureCategory::IntegrityFailure:
    case TransferFailureCategory::Unknown:
        return true;
    case TransferFailureCategory::Cancelled:
    case TransferFailureCategory::InvalidRequest:
        return false;
    }
    return false;
}

inline bool hasSufficientTransferSpace(qint64 requiredBytes, qint64 availableBytes)
{
    return requiredBytes <= 0 || availableBytes < 0 || requiredBytes <= availableBytes;
}

struct ReviewedFileMove
{
    QString source;
    QString destination;
    SourceSnapshot sourceSnapshot;
};

struct TransferRequest
{
    QStringList sources;
    QString destinationDirectory;
    TransferOperation operation = TransferOperation::Copy;
    bool reviewedMove = false;
    // Tag rules may skip a stale/occupied individual file after final consent.
    // Other reviewed workflows retain their existing stop-the-batch behavior.
    bool reviewedSkipStaleItems = false;
    QList<ReviewedFileMove> reviewedMoves;
    SourceSnapshot reviewedDestination;
    // Job-scoped, explicit UI consent. Reviewed mappings and this consent are never replayed from
    // the operation journal; restart always requires a new scan and confirmation.
    bool reviewedMetadataDegradationApproved = false;
    // Exact source/detail warnings displayed during final consent. Execution
    // refuses new or changed metadata loss, even when this request was approved.
    QStringList reviewedApprovedMetadataWarnings;
    bool hasReviewConstraints() const
    {
        return reviewedMove || reviewedSkipStaleItems || !reviewedMoves.isEmpty()
            || reviewedDestination.valid;
    }
};

struct TransferProgress
{
    qint64 completedBytes = 0;
    qint64 totalBytes = 0;
    qint64 bytesPerSecond = 0;
    qint64 remainingSeconds = -1;
    QString currentPath;

    static TransferProgress calculate(qint64 completed, qint64 total,
        qint64 elapsedMilliseconds, const QString& path = {})
    {
        TransferProgress progress;
        progress.completedBytes = completed > 0 ? completed : 0;
        progress.totalBytes = total > 0 ? total : 0;
        progress.currentPath = path;
        if (progress.completedBytes > 0 && elapsedMilliseconds > 0)
            progress.bytesPerSecond = progress.completedBytes * 1000 / elapsedMilliseconds;
        if (progress.bytesPerSecond > 0 && progress.totalBytes > progress.completedBytes) {
            const qint64 remainingBytes = progress.totalBytes - progress.completedBytes;
            progress.remainingSeconds = (remainingBytes + progress.bytesPerSecond - 1)
                / progress.bytesPerSecond;
        } else if (progress.totalBytes > 0 && progress.completedBytes >= progress.totalBytes) {
            progress.remainingSeconds = 0;
        }
        return progress;
    }
};

struct TransferManifestEntry
{
    QString sourcePath;
    QString destinationPath;
    bool directory = false;
    bool destinationCreated = false;
    bool sourceRemoved = false;
    qint64 size = 0;
    qint64 lastModifiedUtcMs = 0;
    quint64 volumeSerial = 0;
    quint64 fileIndex = 0;
    bool identityAvailable = false;
    qint64 sourceSize = 0;
    qint64 sourceLastModifiedUtcMs = 0;
    quint64 sourceVolumeSerial = 0;
    quint64 sourceFileIndex = 0;
    bool sourceIdentityAvailable = false;
};

struct TransferFailure
{
    QString source;
    QString destination;
    QString error;
    bool retryable = false;
    TransferFailureCategory category = TransferFailureCategory::Unknown;
    quint32 nativeCode = 0;
};

inline bool isElevationEligibleCopy(const TransferRequest& request,
    const QList<TransferFailure>& failures)
{
    if (request.hasReviewConstraints() || request.operation != TransferOperation::Copy || failures.isEmpty()
            || failures.size() > 32) {
        return false;
    }
    for (const auto& failure : failures) {
        if (failure.category != TransferFailureCategory::PermissionDenied
                || (failure.nativeCode != 5 && failure.nativeCode != 1314)) {
            return false;
        }
    }
    return true;
}

class TransferController final : public QObject
{
    Q_OBJECT

public:
    struct DestinationState {
        bool available = false;
        qint64 availableBytes = -1;
    };
    using CopyBackend = std::function<filemanager::platform::NativeCopyResult(
        const QString&, const QString&, bool,
        const std::function<bool(qint64, qint64)>&, bool)>;
    using DestinationProbe = std::function<DestinationState(const QString&)>;
    struct SharedState;
    explicit TransferController(QObject* parent = nullptr);
    explicit TransferController(CopyBackend copyBackend, QObject* parent = nullptr);
    TransferController(CopyBackend copyBackend, DestinationProbe destinationProbe,
        QObject* parent = nullptr);
    ~TransferController() override;

    quint64 start(TransferRequest request);
    void cancel();
    void pause();
    void resume();
    void resolveConflict(quint64 generation, ConflictDecision decision, bool applyToAll = false);
    void resolveMetadataDegradation(quint64 generation, bool proceed);

signals:
    void progress(quint64 generation, qint64 completedBytes, qint64 totalBytes,
        const QString& currentPath);
    void progressDetails(quint64 generation, const TransferProgress& progress);
    void manifestEntryCommitted(quint64 generation, const TransferManifestEntry& entry);
    void manifestSourceRemoved(quint64 generation, const QString& sourceRoot);
    void itemFailed(quint64 generation, const TransferFailure& failure);
    void conflict(quint64 generation, const QString& source, const QString& destination,
        bool directory);
    void metadataDegradation(quint64 generation, const QString& source,
        const QString& destination, const QStringList& details);
    void finished(quint64 generation, bool cancelled, const QStringList& errors);

private:
    quint64 generation_ = 0;
    std::shared_ptr<SharedState> state_;
    CopyBackend copyBackend_;
    DestinationProbe destinationProbe_;
};

} // namespace filemanager::files
