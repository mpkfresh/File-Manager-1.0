#include "transferqueue.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QUuid>

#include <algorithm>


namespace filemanager::files {
namespace {

QString manifestPathKey(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

} // namespace

TransferQueue::TransferQueue(QObject* parent)
    : QObject(parent)
{
}

quint64 TransferQueue::enqueue(TransferRequest request)
{
    const quint64 id = ++nextJobId_;
    TransferRecord record;
    record.id = id;
    record.durableId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.request = request;
    record.state = TransferJobState::Queued;
    record.queuedAt = QDateTime::currentDateTimeUtc();
    history_.append(std::move(record));
    pending_.enqueue({id, std::move(request)});
    emit jobQueued(id);
    emit historyChanged();
    startNext();
    return id;
}

void TransferQueue::cancel(quint64 jobId)
{
    if (active_ && active_->id == jobId) {
        controller_->cancel();
        return;
    }
    for (int index = 0; index < pending_.size(); ++index) {
        if (pending_.at(index).id != jobId)
            continue;
        pending_.removeAt(index);
        for (TransferRecord& record : history_) {
            if (record.id == jobId) {
                record.state = TransferJobState::Cancelled;
                record.finishedAt = QDateTime::currentDateTimeUtc();
                break;
            }
        }
        emit jobFinished(jobId, true, {});
        emit historyChanged();
        return;
    }
}

void TransferQueue::pause(quint64 jobId)
{
    if (!active_ || active_->id != jobId)
        return;
    controller_->pause();
    for (TransferRecord& record : history_) {
        if (record.id == jobId) {
            record.state = TransferJobState::Paused;
            break;
        }
    }
    emit historyChanged();
}

void TransferQueue::resume(quint64 jobId)
{
    if (!active_ || active_->id != jobId)
        return;
    controller_->resume();
    for (TransferRecord& record : history_) {
        if (record.id == jobId) {
            record.state = TransferJobState::Running;
            break;
        }
    }
    emit historyChanged();
}

void TransferQueue::resolveConflict(quint64 jobId, ConflictDecision decision, bool applyToAll)
{
    if (active_ && active_->id == jobId)
        controller_->resolveConflict(controllerGeneration_, decision, applyToAll);
}

void TransferQueue::resolveMetadataDegradation(quint64 jobId, bool proceed)
{
    if (active_ && active_->id == jobId)
        controller_->resolveMetadataDegradation(controllerGeneration_, proceed);
}

std::optional<quint64> TransferQueue::retryFailed(quint64 jobId)
{
    const auto record = std::find_if(history_.cbegin(), history_.cend(),
        [jobId](const TransferRecord& candidate) { return candidate.id == jobId; });
    if (record == history_.cend())
        return std::nullopt;
    if (record->request.hasReviewConstraints())
        return std::nullopt; // Requires a fresh explicit preview, never path-only retry.
    QStringList sources;
    for (const TransferFailure& failure : record->failures) {
        if (failure.retryable && QFileInfo::exists(failure.source)
                && !sources.contains(failure.source, Qt::CaseInsensitive)) {
            sources.append(failure.source);
        }
    }
    if (sources.isEmpty())
        return std::nullopt;
    return enqueue({sources, record->request.destinationDirectory, record->request.operation});
}

int TransferQueue::pendingCount() const
{
    return pending_.size();
}

std::optional<quint64> TransferQueue::activeJobId() const
{
    return active_ ? std::optional<quint64>(active_->id) : std::nullopt;
}

QList<TransferRecord> TransferQueue::history() const
{
    return history_;
}

void TransferQueue::startNext()
{
    if (active_ || pending_.isEmpty())
        return;
    active_ = pending_.dequeue();
    for (TransferRecord& record : history_) {
        if (record.id == active_->id) {
            record.state = TransferJobState::Running;
            record.startedAt = QDateTime::currentDateTimeUtc();
            break;
        }
    }
    emit historyChanged();
    controller_ = new TransferController(this);
    const quint64 jobId = active_->id;
    connect(controller_, &TransferController::progress, this,
        [this, jobId](quint64, qint64 completed, qint64 total, const QString& path) {
            emit progress(jobId, completed, total, path);
        });
    connect(controller_, &TransferController::progressDetails, this,
        [this, jobId](quint64, const TransferProgress& progress) {
            for (TransferRecord& record : history_) {
                if (record.id == jobId) {
                    record.completedBytes = progress.completedBytes;
                    record.totalBytes = progress.totalBytes;
                    record.currentPath = progress.currentPath;
                    break;
                }
            }
            emit progressDetails(jobId, progress);
        });
    connect(controller_, &TransferController::manifestEntryCommitted, this,
        [this, jobId](quint64, const TransferManifestEntry& entry) {
            for (TransferRecord& record : history_) {
                if (record.id != jobId)
                    continue;
                auto& destinations = manifestDestinationIndexes_[jobId];
                const QString destinationKey = manifestPathKey(entry.destinationPath);
                const auto destinationIndex = destinations.constFind(destinationKey);
                if (destinationIndex == destinations.cend()) {
                    const qsizetype index = record.manifest.size();
                    record.manifest.append(entry);
                    destinations.insert(destinationKey, index);
                    if (!entry.sourcePath.isEmpty())
                        manifestSourceIndexes_[jobId].insert(manifestPathKey(entry.sourcePath), index);
                } else {
                    const QString previousSource = record.manifest.at(*destinationIndex).sourcePath;
                    if (!previousSource.isEmpty()
                            && previousSource.compare(entry.sourcePath, Qt::CaseInsensitive) != 0) {
                        manifestSourceIndexes_[jobId].remove(manifestPathKey(previousSource));
                    }
                    record.manifest[*destinationIndex] = entry;
                    if (!entry.sourcePath.isEmpty())
                        manifestSourceIndexes_[jobId].insert(
                            manifestPathKey(entry.sourcePath), *destinationIndex);
                }
                break;
            }
            emit recoveryManifestChanged(jobId);
        });
    connect(controller_, &TransferController::manifestSourceRemoved, this,
        [this, jobId](quint64, const QString& sourceRoot) {
            const QString root = QDir::cleanPath(sourceRoot);
            for (TransferRecord& record : history_) {
                if (record.id != jobId)
                    continue;
                const QString sourceKey = manifestPathKey(root);
                const auto exact = manifestSourceIndexes_[jobId].constFind(sourceKey);
                if (exact != manifestSourceIndexes_[jobId].cend()
                        && !record.manifest.at(*exact).directory) {
                    record.manifest[*exact].sourceRemoved = true;
                    break;
                }
                for (TransferManifestEntry& entry : record.manifest) {
                    const QString source = QDir::cleanPath(entry.sourcePath);
                    if (source.compare(root, Qt::CaseInsensitive) == 0
                            || source.startsWith(root + QDir::separator(), Qt::CaseInsensitive))
                        entry.sourceRemoved = true;
                }
                break;
            }
            emit recoveryManifestChanged(jobId);
        });
    connect(controller_, &TransferController::itemFailed, this,
        [this, jobId](quint64, const TransferFailure& failure) {
            for (TransferRecord& record : history_) {
                if (record.id == jobId) {
                    record.failures.append(failure);
                    break;
                }
            }
            emit historyChanged();
        });
    connect(controller_, &TransferController::conflict, this,
        [this, jobId](quint64, const QString& source, const QString& destination, bool directory) {
            emit conflict(jobId, source, destination, directory);
        });
    connect(controller_, &TransferController::metadataDegradation, this,
        [this, jobId](quint64, const QString& source, const QString& destination,
            const QStringList& details) {
            emit metadataDegradation(jobId, source, destination, details);
        });
    connect(controller_, &TransferController::finished, this,
        [this, jobId](quint64, bool cancelled, const QStringList& errors) {
            for (TransferRecord& record : history_) {
                if (record.id == jobId) {
                    record.state = cancelled ? TransferJobState::Cancelled
                        : errors.isEmpty() ? TransferJobState::Succeeded : TransferJobState::Failed;
                    record.errors = errors;
                    record.finishedAt = QDateTime::currentDateTimeUtc();
                    break;
                }
            }
            emit jobFinished(jobId, cancelled, errors);
            emit historyChanged();
            manifestDestinationIndexes_.remove(jobId);
            manifestSourceIndexes_.remove(jobId);
            controller_->deleteLater();
            controller_ = nullptr;
            active_.reset();
            QTimer::singleShot(0, this, &TransferQueue::startNext);
        });
    emit jobStarted(jobId);
    controllerGeneration_ = controller_->start(active_->request);
}

} // namespace filemanager::files
