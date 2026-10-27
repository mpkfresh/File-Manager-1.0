#pragma once

#include "searchservice.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <functional>

namespace filemanager::search {

class MetadataIndex final : public QObject
{
    Q_OBJECT

public:
    explicit MetadataIndex(QObject* parent = nullptr);
    ~MetadataIndex() override;

    enum class RebuildStage { WorkerStarted, ConnectionCreated, ParentDirectoryChecked,
        DatabaseOpened, StoragePathsResolved, TransactionStarted, SchemaPrepared,
        EnumerationFinished, TransactionFinished, ConnectionClosed, ConnectionRemoved, Count };
    // Optional diagnostic callback, invoked on the worker. It must be nonblocking,
    // thread-safe and own its captures. Contains no paths or query contents.
    using RebuildObserver = std::function<void(RebuildStage, qint64 elapsedMs, int indexedItems)>;
    quint64 rebuild(const QString& databasePath, const QString& root, RebuildObserver observer = {});
    quint64 query(const QString& databasePath, const QString& root, const QString& text,
        const QString& extension = {}, int maximumResults = 2000);
    quint64 query(const QString& databasePath, const QString& root, SearchFilters filters,
        int maximumResults = 2000);
    quint64 removeRoot(const QString& databasePath, const QString& root);
    void cancel();

signals:
    void indexingProgress(quint64 generation, int indexedItems, const QString& currentPath);
    void resultFound(quint64 generation, const filemanager::search::SearchHit& hit);
    void finished(quint64 generation, bool cancelled, const QString& error, int itemCount);

private:
    quint64 generation_ = 0;
    std::shared_ptr<std::atomic_bool> cancellation_;
};

} // namespace filemanager::search
