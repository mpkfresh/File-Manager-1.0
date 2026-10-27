#include "files/analysisservice.h"

#include <QFileInfo>
#include <QDir>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <optional>

namespace filemanager::files {
namespace {

struct CachedFolderAnalysis
{
    PathStatistics statistics;
    QDateTime measuredAt;
    QDateTime rootModified;
    quint64 accessSequence = 0;
};

QMutex cacheMutex;
QHash<QString, CachedFolderAnalysis> folderCache;
quint64 cacheSequence = 0;
constexpr int MaximumFolderCacheEntries = 128;
constexpr qint64 MaximumFolderCacheAgeSeconds = 5 * 60;

QString cacheKey(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

std::optional<CachedFolderAnalysis> cachedFolder(const QString& path)
{
    const QFileInfo root(path);
    if (!root.isDir())
        return std::nullopt;
    const QString key = cacheKey(path);
    QMutexLocker lock(&cacheMutex);
    auto found = folderCache.find(key);
    if (found == folderCache.end())
        return std::nullopt;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (found->measuredAt.secsTo(now) > MaximumFolderCacheAgeSeconds
        || found->rootModified != root.lastModified()) {
        folderCache.erase(found);
        return std::nullopt;
    }
    found->accessSequence = ++cacheSequence;
    return *found;
}

void storeFolder(const QString& path, const PathStatistics& statistics)
{
    const QFileInfo root(path);
    if (!root.isDir() || statistics.cancelled || !statistics.error.isEmpty())
        return;
    QMutexLocker lock(&cacheMutex);
    folderCache.insert(cacheKey(path), {statistics, QDateTime::currentDateTimeUtc(),
        root.lastModified(), ++cacheSequence});
    while (folderCache.size() > MaximumFolderCacheEntries) {
        auto oldest = std::min_element(folderCache.begin(), folderCache.end(),
            [](const CachedFolderAnalysis& left, const CachedFolderAnalysis& right) {
                return left.accessSequence < right.accessSequence;
            });
        if (oldest == folderCache.end())
            break;
        folderCache.erase(oldest);
    }
}

} // namespace

AnalysisService::AnalysisService(QObject* parent)
    : QObject(parent)
{
}

AnalysisService::~AnalysisService()
{
    cancel();
}

quint64 AnalysisService::start(const QString& path, bool refresh)
{
    cancel();
    const quint64 generation = ++generation_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    if (!refresh) {
        if (const auto cached = cachedFolder(path)) {
            AnalysisResult result{path, cached->statistics, {}, true, cached->measuredAt};
            QTimer::singleShot(0, this, [this, generation, result] {
                emit finished(generation, result);
            });
            return generation;
        }
    }
    const auto cancellation = cancellation_;
    QPointer<AnalysisService> guard(this);
    QThread* worker = QThread::create([guard, cancellation, generation, path] {
        AnalysisResult result;
        result.path = path;
        result.statistics = measurePath(path, cancellation.get(), [guard, generation, path](
                const PathStatistics& partial) {
            if (guard) {
                QMetaObject::invokeMethod(guard, [guard, generation, path, partial] {
                    if (guard)
                        emit guard->progress(generation, path, partial);
                });
            }
        });
        result.measuredAt = QDateTime::currentDateTimeUtc();
        if (!result.statistics.cancelled && result.statistics.error.isEmpty()
            && QFileInfo(path).isFile()) {
            result.sha256 = sha256File(path, &result.statistics.error, cancellation.get());
            result.statistics.cancelled = cancellation->load();
        }
        if (QFileInfo(path).isDir())
            storeFolder(path, result.statistics);
        if (guard) {
            QMetaObject::invokeMethod(guard, [guard, generation, result] {
                if (guard)
                    emit guard->finished(generation, result);
            });
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return generation;
}

void AnalysisService::cancel()
{
    if (cancellation_)
        cancellation_->store(true);
}

void AnalysisService::clearFolderCache(const QString& path)
{
    QMutexLocker lock(&cacheMutex);
    if (path.isEmpty())
        folderCache.clear();
    else
        folderCache.remove(cacheKey(path));
}

int AnalysisService::folderCacheSize()
{
    QMutexLocker lock(&cacheMutex);
    return folderCache.size();
}

} // namespace filemanager::files
