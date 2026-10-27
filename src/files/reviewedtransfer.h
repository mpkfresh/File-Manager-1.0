#pragma once

#include "transfercontroller.h"
#include "tvorganizer.h"

#include <algorithm>
#include <functional>

namespace filemanager::files {

inline constexpr qsizetype MaximumReviewedMoveFiles = 4096;

struct ReviewedTransferValidation
{
    QStringList errors;
    bool cancelled = false;
    bool valid() const { return !cancelled && errors.isEmpty(); }
};

struct ReviewedTransferRequest
{
    TransferRequest request;
    QStringList errors;
    bool valid() const { return errors.isEmpty() && request.reviewedMove && !request.sources.isEmpty(); }
};

struct ReviewedMoveItemResult
{
    QString source;
    QString destination;
    bool destinationCommitted = false;
    bool sourceRemoved = false;
    bool moved = false;
    bool cancelled = false;
    QString error;
    quint32 nativeCode = 0;
    bool metadataConfirmationRequired = false;
    QStringList metadataDegradations;
    SourceSnapshot destinationSnapshot;
};

struct ReviewedMoveExecutionResult
{
    QList<ReviewedMoveItemResult> items;
    QStringList createdDirectories;
    QStringList errors;
    bool cancelled = false;
    bool metadataConfirmationRequired = false;
    QStringList metadataDegradations;
    bool complete() const
    {
        return !cancelled && errors.isEmpty() && !items.isEmpty()
            && std::all_of(items.cbegin(), items.cend(),
                [](const ReviewedMoveItemResult& item) { return item.moved; });
    }
};

struct ReviewedMetadataItem
{
    QString source;
    QString destination;
    QStringList degradations;
};

struct ReviewedMetadataReview
{
    QList<ReviewedMetadataItem> items;
    QStringList errors;
    bool cancelled = false;
    bool valid() const { return !cancelled && errors.isEmpty(); }
    bool confirmationRequired() const
    {
        return std::any_of(items.cbegin(), items.cend(),
            [](const ReviewedMetadataItem& item) { return !item.degradations.isEmpty(); });
    }
};

using ReviewedMetadataProbe = std::function<platform::NativeMetadataReview(
    const QString&, const QString&)>;

// Selects whole episode groups from the retained preview; never recaptures identities.
ReviewedTransferRequest createReviewedTvMove(const TvOrganizationPlan& plan,
    const QStringList& includedEpisodeSources);

// Read-only whole-batch checks. This is not a lock or authority to use path-only deletion.
ReviewedTransferValidation validateReviewedMove(const TransferRequest& request,
    const std::atomic_bool* cancelled = nullptr);

ReviewedMetadataReview reviewReviewedMoveMetadata(const TransferRequest& request,
    const std::atomic_bool* cancelled = nullptr,
    const ReviewedMetadataProbe& metadataProbe = {});

// Executes an already reviewed regular-file batch in order. The entire request is preflighted
// before any mutation, then each item uses the retained native destination tree and the appropriate
// same/cross-volume primitive. It stops on the first failure and reports every committed partial.
ReviewedMoveExecutionResult executeReviewedMove(const TransferRequest& request,
    const std::function<bool(qsizetype, qint64, qint64)>& progress = {},
    const std::atomic_bool* cancelled = nullptr,
    bool allowMetadataDegradation = false,
    const ReviewedMetadataProbe& metadataProbe = {},
    const std::function<void(const QString&, const SourceSnapshot&)>& directoryCreated = {},
    const std::function<void(const ReviewedMoveItemResult&, const SourceSnapshot&)>& itemFinished = {});

} // namespace filemanager::files
