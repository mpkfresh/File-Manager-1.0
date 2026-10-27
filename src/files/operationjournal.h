#pragma once

#include "files/transferqueue.h"

#include <QDateTime>
#include <QByteArray>
#include <QObject>
#include <QTimer>

#include <optional>

namespace filemanager::files {

enum class JournalState { Queued, Running, Paused, Succeeded, Failed, Cancelled, Interrupted };

struct JournalRecord
{
    QString id;
    TransferRequest request;
    JournalState state = JournalState::Queued;
    QDateTime queuedAt;
    QDateTime startedAt;
    QDateTime finishedAt;
    qint64 completedBytes = 0;
    qint64 totalBytes = 0;
    QString currentPath;
    QStringList errors;
    QList<TransferManifestEntry> manifest;

};

struct RecoveryCleanupResult
{
    QStringList removedPaths;
    QStringList refusedPaths;
    QStringList errors;
};

QByteArray serializeOperationJournal(const QList<JournalRecord>& records, int schema = 2);
bool parseOperationJournal(const QByteArray& plain, QList<JournalRecord>* records,
    int* schema = nullptr, QString* error = nullptr);

class OperationJournal final : public QObject
{
    Q_OBJECT

public:
    static constexpr int StandardRetentionDays = 7;

    explicit OperationJournal(QString storagePath = {}, QObject* parent = nullptr);
    ~OperationJournal() override;

    bool load(QString* error = nullptr);
    void synchronize(const QList<TransferRecord>& transfers);
    bool flush(QString* error = nullptr);
    bool clear(QString* error = nullptr);
    bool dismissInterrupted(const QStringList& recordIds, QString* error = nullptr);
    QStringList verifiedCleanupPaths(const QString& recordId,
        QStringList* refusedPaths = nullptr) const;
    RecoveryCleanupResult cleanupVerifiedCreatedItems(const QString& recordId) const;
    std::optional<TransferRequest> retryRequest(const QString& recordId,
        QStringList* unavailableSources = nullptr) const;
    void prune(const QDateTime& now = QDateTime::currentDateTimeUtc());

    QList<JournalRecord> records() const;
    QList<JournalRecord> interruptedRecords() const;
    QString storagePath() const;

signals:
    void changed();

private:
    void scheduleFlush();
    bool quarantineCorruptStorage(const QString& reason, QString* error);

    QString storagePath_;
    QList<JournalRecord> records_;
    QTimer flushTimer_;
};

} // namespace filemanager::files
