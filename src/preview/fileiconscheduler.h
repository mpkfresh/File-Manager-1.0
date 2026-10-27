#pragma once

#include "platform/fileicon.h"
#include <QCache>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <functional>

namespace filemanager::preview {

class FileIconScheduler final : public QObject {
    Q_OBJECT
public:
    enum class State { Pending, Cached, Busy, Invalid };
    struct Lookup {
        State state = State::Invalid;
        platform::FileIconResult result;
    };
    // Loader runs on workers; captures must remain valid and support concurrent calls.
    using Loader = std::function<platform::FileIconResult(
        const QString&, int, const platform::ShellCancellation&)>;
    static constexpr int MaximumActiveRequests = 2;
    static constexpr int MaximumPendingRequests = 64;
    static constexpr int CacheBudget = 8 * 1024 * 1024;
    static constexpr int MinimumCacheEntryCost = 16 * 1024;

    explicit FileIconScheduler(QObject* parent = nullptr);
    explicit FileIconScheduler(Loader loader, QObject* parent = nullptr);
    ~FileIconScheduler() override;
    Lookup request(const QString& absolutePath, int pixelExtent);
    // Cancels accepted pending work and invalidates all completed results. Late
    // completions cannot publish into the new generation, even for the same key.
    void clear();
    int activeRequestCount() const { return static_cast<int>(active_.size()); }
    int pendingRequestCount() const { return static_cast<int>(pending_.size()); }
    int cachedCount() const { return static_cast<int>(cache_.size()); }
    int cacheCost() const { return static_cast<int>(cache_.totalCost()); }

signals:
    void requestFinished(const QString& path, int pixelExtent);
    void capacityAvailable();

private:
    struct Request {
        QString key;
        QString path;
        int extent = 0;
        quint64 generation = 0;
        quint64 id = 0;
        platform::ShellCancellation cancelled;
    };
    void startNext();
    Loader loader_;
    QCache<QString, platform::FileIconResult> cache_{CacheBudget};
    QQueue<Request> queue_;
    QSet<QString> pending_;
    QHash<quint64, platform::ShellCancellation> active_;
    quint64 generation_ = 0;
    quint64 nextId_ = 0;
};

} // namespace filemanager::preview
