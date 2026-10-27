#include "preview/fileiconscheduler.h"

#include <QCoreApplication>
#include <QDir>
#include <QFutureWatcher>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <utility>

namespace filemanager::preview {
namespace {
QThreadPool* iconWorkerPool()
{
    // Shell icon requests can block independently of CPU count. Keep two
    // dedicated worker slots even when Qt's global pool has one thread, and
    // prevent unrelated QtConcurrent work from starving icon extraction.
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(FileIconScheduler::MaximumActiveRequests);
        pool.setExpiryTimeout(30000);
        return true;
    }();
    (void)configured;
    return &pool;
}

FileIconScheduler::Loader nativeLoader()
{
    // Resolve this while the owning application is alive, not from a late worker.
    const QString helper = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("FileManagerShellHost.exe"));
    return [helper](const QString& path, int extent, const platform::ShellCancellation& cancel) {
        return platform::readFileIconIsolated(path, extent, 5000, cancel, helper);
    };
}
}

FileIconScheduler::FileIconScheduler(QObject* parent)
    : FileIconScheduler(nativeLoader(), parent)
{
}

FileIconScheduler::FileIconScheduler(Loader loader, QObject* parent)
    : QObject(parent), loader_(std::move(loader))
{
}

FileIconScheduler::~FileIconScheduler()
{
    for (const auto& cancel : std::as_const(active_))
        cancel->store(true);
    // Watchers disconnect on destruction. Worker closures own only their loader,
    // request strings and cancellation token; they never dereference this object.
}

FileIconScheduler::Lookup FileIconScheduler::request(const QString& path, int pixelExtent)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (!loader_ || !platform::validFileIconRequest(path, pixelExtent))
        return {};
    // Preserve case and exact path identity, including case-sensitive directories.
    // Cross-path/type equivalence belongs to the model, not this raw-pixel cache.
    const QString key = path + QLatin1Char('|') + QString::number(pixelExtent);
    if (const auto* cached = cache_.object(key))
        return {State::Cached, *cached};
    if (pending_.contains(key))
        return {State::Pending};
    if (pending_.size() >= MaximumPendingRequests)
        return {State::Busy};
    Request request{key, path, pixelExtent, generation_, ++nextId_,
        std::make_shared<std::atomic_bool>(false)};
    pending_.insert(key);
    queue_.enqueue(std::move(request));
    startNext();
    return {State::Pending};
}

void FileIconScheduler::clear()
{
    Q_ASSERT(thread() == QThread::currentThread());
    ++generation_;
    cache_.clear();
    queue_.clear();
    pending_.clear();
    for (const auto& cancel : std::as_const(active_))
        cancel->store(true);
    // Running old-generation jobs still occupy their slots until they finish.
    emit capacityAvailable();
}

void FileIconScheduler::startNext()
{
    while (active_.size() < MaximumActiveRequests && !queue_.isEmpty()) {
        const Request request = queue_.dequeue();
        active_.insert(request.id, request.cancelled);
        auto* watcher = new QFutureWatcher<platform::FileIconResult>(this);
        connect(watcher, &QFutureWatcher<platform::FileIconResult>::finished, this,
            [this, watcher, request] {
                auto result = watcher->result();
                watcher->deleteLater();
                active_.remove(request.id);
                const bool current = request.generation == generation_ && !request.cancelled->load();
                if (current) {
                    pending_.remove(request.key);
                    if (result.status == platform::FileIconStatus::Ready
                            && (result.image.isNull()
                                || result.image.width() > platform::MaximumFileIconExtent
                                || result.image.height() > platform::MaximumFileIconExtent
                                || result.image.sizeInBytes() > 4 * platform::MaximumFileIconExtent
                                    * platform::MaximumFileIconExtent)) {
                        result.status = platform::FileIconStatus::InvalidReply;
                    }
                    if (result.status != platform::FileIconStatus::Ready)
                        result.image = {};
                    const qsizetype bytes = result.image.sizeInBytes()
                        + request.key.size() * qsizetype(sizeof(QChar))
                        + qsizetype(sizeof(platform::FileIconResult));
                    const int cost = static_cast<int>(qMax(qsizetype(MinimumCacheEntryCost), bytes));
                    cache_.insert(request.key, new platform::FileIconResult(std::move(result)), cost);
                }
                startNext();
                // A receiver may clear or destroy the scheduler during either signal.
                QPointer<FileIconScheduler> guard(this);
                if (current)
                    emit requestFinished(request.path, request.extent);
                if (guard && pending_.size() < MaximumPendingRequests)
                    emit capacityAvailable();
            });
        const Loader loader = loader_;
        watcher->setFuture(QtConcurrent::run(iconWorkerPool(), [loader, request] {
            if (request.cancelled->load())
                return platform::FileIconResult{platform::FileIconStatus::Cancelled};
            try {
                return loader(request.path, request.extent, request.cancelled);
            } catch (...) {
                return platform::FileIconResult{platform::FileIconStatus::HelperFailed};
            }
        }));
    }
}

} // namespace filemanager::preview
