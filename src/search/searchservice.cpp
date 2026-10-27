#include "searchservice.h"

#include "fuzzymatcher.h"
#include "files/nameordering.h"
#include "files/scopeenumerator.h"
#include "search/windowssearch.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QThread>

namespace filemanager::search {

bool searchHitPrecedes(const SearchHit& left, const SearchHit& right)
{
    if (left.score != right.score)
        return left.score > right.score;
    const int nameOrder = filemanager::files::naturalNameCompare(left.name, right.name);
    if (nameOrder != 0)
        return nameOrder < 0;
    return left.path.compare(right.path, Qt::CaseInsensitive) < 0;
}

bool SearchFilters::isDefault() const
{
    return name.trimmed().isEmpty() && extension.trimmed().isEmpty()
        && itemType == SearchItemType::Any && minimumSize < 0 && maximumSize < 0
        && !modifiedAfter.isValid() && !modifiedBefore.isValid();
}

bool matchesSearchFilters(const QFileInfo& info, const SearchFilters& filters, int* score)
{
    const auto candidate = match(filters.name.trimmed(), info.fileName());
    if (!candidate.matched)
        return false;
    const QString normalizedExtension = QString(filters.extension).trimmed().remove('.');
    if (!normalizedExtension.isEmpty()
            && info.suffix().compare(normalizedExtension, Qt::CaseInsensitive) != 0)
        return false;
    if (filters.itemType == SearchItemType::Files && !info.isFile())
        return false;
    if (filters.itemType == SearchItemType::Folders && !info.isDir())
        return false;
    if (info.isFile()) {
        if (filters.minimumSize >= 0 && info.size() < filters.minimumSize)
            return false;
        if (filters.maximumSize >= 0 && info.size() > filters.maximumSize)
            return false;
    } else if (filters.minimumSize >= 0 || filters.maximumSize >= 0) {
        return false;
    }
    const QDateTime modified = info.lastModified();
    if (filters.modifiedAfter.isValid() && modified < filters.modifiedAfter)
        return false;
    if (filters.modifiedBefore.isValid() && modified > filters.modifiedBefore)
        return false;
    if (score)
        *score = candidate.score;
    return true;
}

SearchService::SearchService(QObject* parent)
    : QObject(parent)
{
}

SearchService::~SearchService()
{
    cancel();
}

quint64 SearchService::start(
    const QString& root, const QString& query, const QString& extension, int maximumResults)
{
    SearchFilters filters;
    filters.name = query;
    filters.extension = extension;
    return start(root, std::move(filters), maximumResults);
}

quint64 SearchService::start(
    const QString& root, SearchFilters filters, int maximumResults)
{
    cancel();
    const quint64 generation = ++generation_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    QPointer<SearchService> guard(this);
    QThread* worker = QThread::create([guard, cancellation, generation, root,
                                         filters = std::move(filters), maximumResults] {
        int resultCount = 0;
        const int maximumCandidates = qMin(20000, qMax(maximumResults, maximumResults * 8));
        const WindowsSearchResult indexed = WindowsSearch::query(
            root, filters.name, maximumCandidates, cancellation.get());
        if (indexed.usedIndex) {
            if (guard) {
                QMetaObject::invokeMethod(guard, [guard, generation] {
                    if (guard)
                        emit guard->backendSelected(generation, QStringLiteral("Windows Search index"));
                });
            }
            for (const QString& path : indexed.paths) {
                if (cancellation->load() || resultCount >= maximumResults)
                    break;
                const QFileInfo info(path);
                int score = 0;
                if (!info.exists() || !matchesSearchFilters(info, filters, &score))
                    continue;
                const SearchHit hit{info.absoluteFilePath(), info.fileName(), info.isDir(), score};
                ++resultCount;
                if (guard) {
                    QMetaObject::invokeMethod(guard, [guard, generation, hit] {
                        if (guard)
                            emit guard->resultFound(generation, hit);
                    });
                }
            }
            const bool wasCancelled = cancellation->load() || indexed.cancelled;
            if (guard) {
                QMetaObject::invokeMethod(guard,
                    [guard, generation, wasCancelled, resultCount] {
                        if (guard)
                            emit guard->finished(generation, wasCancelled, resultCount);
                    });
            }
            return;
        }
        if (guard) {
            QMetaObject::invokeMethod(guard, [guard, generation] {
                if (guard)
                    emit guard->backendSelected(generation, QStringLiteral("Direct folder scan"));
            });
        }
        filemanager::files::enumerateRegularTree(root, true, cancellation.get(),
            [&](const QFileInfo& info) {
            if (resultCount >= maximumResults)
                return false;
            int score = 0;
            if (!matchesSearchFilters(info, filters, &score))
                return true;
            SearchHit hit{info.absoluteFilePath(), info.fileName(), info.isDir(), score};
            ++resultCount;
            if (guard) {
                QMetaObject::invokeMethod(guard, [guard, generation, hit] {
                    if (guard)
                        emit guard->resultFound(generation, hit);
                });
            }
            return resultCount < maximumResults;
        });
        const bool wasCancelled = cancellation->load();
        if (guard) {
            QMetaObject::invokeMethod(guard, [guard, generation, wasCancelled, resultCount] {
                if (guard)
                    emit guard->finished(generation, wasCancelled, resultCount);
            });
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return generation;
}

quint64 SearchService::startAqs(
    const QString& root, const QString& advancedQuery, int maximumResults)
{
    cancel();
    const quint64 generation = ++generation_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    QPointer<SearchService> guard(this);
    QThread* worker = QThread::create(
        [guard, cancellation, generation, root, advancedQuery, maximumResults] {
            const int maximumCandidates = qMin(20000, qMax(maximumResults, maximumResults * 8));
            const WindowsSearchResult indexed = WindowsSearch::queryAqs(
                root, advancedQuery, maximumCandidates, cancellation.get());
            if (guard && !indexed.cancelled && !indexed.usedIndex) {
                QMetaObject::invokeMethod(guard, [guard, generation, error = indexed.diagnostic] {
                    if (guard)
                        emit guard->errorOccurred(generation, error);
                });
            }
            if (indexed.usedIndex && guard) {
                QMetaObject::invokeMethod(guard, [guard, generation] {
                    if (guard)
                        emit guard->backendSelected(
                            generation, QStringLiteral("Windows Search AQS"));
                });
            }
            int resultCount = 0;
            for (const QString& path : indexed.paths) {
                if (cancellation->load() || resultCount >= maximumResults)
                    break;
                const QFileInfo info(path);
                if (!info.exists())
                    continue;
                const SearchHit hit{info.absoluteFilePath(), info.fileName(), info.isDir(),
                    maximumResults - resultCount};
                ++resultCount;
                if (guard) {
                    QMetaObject::invokeMethod(guard, [guard, generation, hit] {
                        if (guard)
                            emit guard->resultFound(generation, hit);
                    });
                }
            }
            const bool wasCancelled = cancellation->load() || indexed.cancelled;
            if (guard) {
                QMetaObject::invokeMethod(guard,
                    [guard, generation, wasCancelled, resultCount] {
                        if (guard)
                            emit guard->finished(generation, wasCancelled, resultCount);
                    });
            }
        });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return generation;
}

void SearchService::cancel()
{
    if (cancellation_)
        cancellation_->store(true);
}

} // namespace filemanager::search
