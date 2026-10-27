#pragma once

#include "transfercontroller.h"

#include <QObject>
#include <QQueue>
#include <QDateTime>
#include <QHash>

#include <optional>

namespace filemanager::files {

enum class TransferJobState { Queued, Running, Paused, Succeeded, Failed, Cancelled };

struct TransferRecord
{
    quint64 id = 0;
    QString durableId;
    TransferRequest request;
    TransferJobState state = TransferJobState::Queued;
    QDateTime queuedAt;
    QDateTime startedAt;
    QDateTime finishedAt;
    QStringList errors;
    qint64 completedBytes = 0;
    qint64 totalBytes = 0;
    QString currentPath;
    QList<TransferManifestEntry> manifest;
    QList<TransferFailure> failures;
};

class TransferQueue final : public QObject
{
    Q_OBJECT

public:
    explicit TransferQueue(QObject* parent = nullptr);

    quint64 enqueue(TransferRequest request);
    void cancel(quint64 jobId);
    void pause(quint64 jobId);
    void resume(quint64 jobId);
    void resolveConflict(quint64 jobId, ConflictDecision decision, bool applyToAll = false);
    void resolveMetadataDegradation(quint64 jobId, bool proceed);
    std::optional<quint64> retryFailed(quint64 jobId);
    int pendingCount() const;
    std::optional<quint64> activeJobId() const;
    QList<TransferRecord> history() const;

signals:
    void jobQueued(quint64 jobId);
    void jobStarted(quint64 jobId);
    void progress(quint64 jobId, qint64 completedBytes, qint64 totalBytes,
        const QString& currentPath);
    void progressDetails(quint64 jobId, const TransferProgress& progress);
    void recoveryManifestChanged(quint64 jobId);
    void conflict(quint64 jobId, const QString& source, const QString& destination,
        bool directory);
    void metadataDegradation(quint64 jobId, const QString& source,
        const QString& destination, const QStringList& details);
    void jobFinished(quint64 jobId, bool cancelled, const QStringList& errors);
    void historyChanged();

private:
    struct Job { quint64 id; TransferRequest request; };
    void startNext();

    quint64 nextJobId_ = 0;
    QQueue<Job> pending_;
    std::optional<Job> active_;
    TransferController* controller_ = nullptr;
    quint64 controllerGeneration_ = 0;
    QList<TransferRecord> history_;
    QHash<quint64, QHash<QString, qsizetype>> manifestDestinationIndexes_;
    QHash<quint64, QHash<QString, qsizetype>> manifestSourceIndexes_;
};

} // namespace filemanager::files
