#include "metadataindex.h"

#include "fuzzymatcher.h"
#include "files/scopeenumerator.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>

namespace filemanager::search {
namespace {

QString connectionName()
{
    return "file-manager-index-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

class IndexStorageExclusion {
public:
    explicit IndexStorageExclusion(const QString& databasePath)
    {
        const QFileInfo database(databasePath);
        for (const QString& base : {database.absoluteFilePath(), database.canonicalFilePath()}) {
            if (base.isEmpty())
                continue;
            for (const QString& suffix : {QString(), QStringLiteral("-journal"),
                     QStringLiteral("-wal"), QStringLiteral("-shm")}) {
                const QString path = QDir::cleanPath(QDir::fromNativeSeparators(base + suffix));
                paths_.insert(path);
                names_.insert(QFileInfo(path).fileName().toCaseFolded());
            }
        }
    }

    bool contains(const QString& path, bool resolveAliases = true) const
    {
        const QFileInfo candidate(path);
        // Ordinary entries need no canonical-path lookup. This is not a suffix
        // filter: another database or same basename in another folder is retained.
        if (!names_.contains(candidate.fileName().toCaseFolded()))
            return false;
        const QString absolute = QDir::cleanPath(candidate.absoluteFilePath());
        if (paths_.contains(absolute))
            return true;
        // Cached queries must not probe stored result paths (which can be offline).
        // Only an explicit rebuild resolves candidate aliases during its traversal.
        if (!resolveAliases)
            return false;
        const QString canonical = candidate.canonicalFilePath();
        if (!canonical.isEmpty() && paths_.contains(QDir::cleanPath(canonical)))
            return true;
        // A cached journal row can outlive the journal. Resolve only its parent
        // so missing sidecars are still excluded through a directory-path alias.
        const QString parent = QDir(candidate.absolutePath()).canonicalPath();
        return !parent.isEmpty()
            && paths_.contains(QDir(parent).filePath(candidate.fileName()));
    }

private:
    QSet<QString> paths_;
    QSet<QString> names_;
};

bool prepareSchema(QSqlDatabase& database, QString& error)
{
    QSqlQuery query(database);
    if (!query.exec("CREATE TABLE IF NOT EXISTS metadata ("
                    "root TEXT NOT NULL, path TEXT PRIMARY KEY, name TEXT NOT NULL, "
                    "extension TEXT NOT NULL, size INTEGER NOT NULL, modified INTEGER NOT NULL, "
                    "is_directory INTEGER NOT NULL)")) {
        error = query.lastError().text();
        return false;
    }
    if (!query.exec("CREATE INDEX IF NOT EXISTS metadata_root_name "
                    "ON metadata(root, name COLLATE NOCASE)")) {
        error = query.lastError().text();
        return false;
    }
    return true;
}

void emitFinished(const QPointer<MetadataIndex>& guard, quint64 generation,
    bool cancelled, const QString& error, int count)
{
    if (!guard)
        return;
    QMetaObject::invokeMethod(guard, [guard, generation, cancelled, error, count] {
        if (guard)
            emit guard->finished(generation, cancelled, error, count);
    });
}

} // namespace

MetadataIndex::MetadataIndex(QObject* parent)
    : QObject(parent)
{
}

MetadataIndex::~MetadataIndex()
{
    cancel();
}

quint64 MetadataIndex::rebuild(const QString& databasePath, const QString& root, RebuildObserver observer)
{
    cancel();
    const quint64 generation = ++generation_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    QPointer<MetadataIndex> guard(this);
    QElapsedTimer timer;
    if (observer) timer.start();
    QThread* worker = QThread::create([guard, cancellation, generation, databasePath, root,
                                         observer = std::move(observer), timer] {
        const QString connection = connectionName();
        QString error;
        int count = 0;
        const auto trace = [&](RebuildStage stage) {
            if (observer) observer(stage, timer.elapsed(), count);
        };
        trace(RebuildStage::WorkerStarted);
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(databasePath);
            trace(RebuildStage::ConnectionCreated);
            const bool parentReady = QDir().mkpath(QFileInfo(databasePath).absolutePath());
            trace(RebuildStage::ParentDirectoryChecked);
            if (!parentReady) {
                error = QStringLiteral("The metadata index directory could not be created.");
            } else if (!database.open()) {
                error = database.lastError().text();
            } else {
                trace(RebuildStage::DatabaseOpened);
                const IndexStorageExclusion storage(databasePath);
                trace(RebuildStage::StoragePathsResolved);
                // Keep initial schema creation and replacement data in one durable
                // transaction, avoiding separate implicit commits for each DDL.
                if (!database.transaction()) {
                    error = database.lastError().text();
                } else {
                    trace(RebuildStage::TransactionStarted);
                    const bool schemaReady = prepareSchema(database, error);
                    trace(RebuildStage::SchemaPrepared);
                    if (schemaReady) {
                        QSqlQuery remove(database);
                        remove.prepare("DELETE FROM metadata WHERE root = ?");
                        remove.addBindValue(QDir::cleanPath(root));
                        if (!remove.exec())
                            error = remove.lastError().text();

                        QSqlQuery insert(database);
                        insert.prepare("INSERT OR REPLACE INTO metadata "
                            "(root, path, name, extension, size, modified, is_directory) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?)");
                        const auto scopeResult = filemanager::files::enumerateRegularTree(
                            root, true, cancellation.get(), [&](const QFileInfo& info) {
                            if (!error.isEmpty())
                                return false;
                            if (!info.isDir() && storage.contains(info.absoluteFilePath()))
                                return true;
                            insert.bindValue(0, QDir::cleanPath(root));
                            insert.bindValue(1, info.absoluteFilePath());
                            insert.bindValue(2, info.fileName());
                            const QString suffix = info.suffix();
                            insert.bindValue(3, suffix.isNull() ? QStringLiteral("") : suffix);
                            insert.bindValue(4, info.isDir() ? 0 : info.size());
                            insert.bindValue(5, info.lastModified().toMSecsSinceEpoch());
                            insert.bindValue(6, info.isDir());
                            if (!insert.exec()) {
                                error = insert.lastError().text();
                                return false;
                            }
                            ++count;
                            if (guard && count % 128 == 0) {
                                const QString path = info.absoluteFilePath();
                                QMetaObject::invokeMethod(guard, [guard, generation, count, path] {
                                    if (guard)
                                        emit guard->indexingProgress(generation, count, path);
                                });
                            }
                            return true;
                        });
                        if (error.isEmpty() && !scopeResult.error.isEmpty())
                            error = scopeResult.error;
                        trace(RebuildStage::EnumerationFinished);
                    }
                    if (cancellation->load() || !error.isEmpty()) {
                        database.rollback();
                    } else if (!database.commit()) {
                        error = database.lastError().text();
                        database.rollback();
                    }
                    trace(RebuildStage::TransactionFinished);
                }
            }
            database.close();
            trace(RebuildStage::ConnectionClosed);
        }
        QSqlDatabase::removeDatabase(connection);
        trace(RebuildStage::ConnectionRemoved);
        emitFinished(guard, generation, cancellation->load(), error,
            cancellation->load() || !error.isEmpty() ? 0 : count);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return generation;
}

quint64 MetadataIndex::query(const QString& databasePath, const QString& root,
    const QString& text, const QString& extension, int maximumResults)
{
    SearchFilters filters;
    filters.name = text;
    filters.extension = extension;
    return query(databasePath, root, std::move(filters), maximumResults);
}

quint64 MetadataIndex::query(const QString& databasePath, const QString& root,
    SearchFilters filters, int maximumResults)
{
    cancel();
    const quint64 generation = ++generation_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    QPointer<MetadataIndex> guard(this);
    QThread* worker = QThread::create([guard, cancellation, generation, databasePath, root,
                                         filters = std::move(filters), maximumResults] {
        const QString connection = connectionName();
        QString error;
        int count = 0;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(databasePath);
            if (!database.open()) {
                error = database.lastError().text();
            } else if (prepareSchema(database, error)) {
                const IndexStorageExclusion storage(databasePath);
                QSqlQuery query(database);
                query.prepare("SELECT path, name, extension, size, modified, is_directory FROM metadata "
                              "WHERE root = ? AND name LIKE ? COLLATE NOCASE LIMIT 20000");
                query.addBindValue(QDir::cleanPath(root));
                query.addBindValue('%' + filters.name.left(1) + '%');
                if (!query.exec()) {
                    error = query.lastError().text();
                } else {
                    const QString normalizedExtension = QString(filters.extension).trimmed().remove('.');
                    while (query.next() && count < maximumResults && !cancellation->load()) {
                        const bool directory = query.value(5).toBool();
                        // Older indexes may already contain their own storage files.
                        if (!directory && storage.contains(query.value(0).toString(), false))
                            continue;
                        const QString name = query.value(1).toString();
                        const auto candidate = match(filters.name.trimmed(), name);
                        const qint64 size = query.value(3).toLongLong();
                        const QDateTime modified = QDateTime::fromMSecsSinceEpoch(
                            query.value(4).toLongLong());
                        if (!candidate.matched
                            || (!normalizedExtension.isEmpty()
                                && query.value(2).toString().compare(
                                    normalizedExtension, Qt::CaseInsensitive) != 0)
                            || (filters.itemType == SearchItemType::Files && directory)
                            || (filters.itemType == SearchItemType::Folders && !directory)
                            || ((filters.minimumSize >= 0 || filters.maximumSize >= 0) && directory)
                            || (filters.minimumSize >= 0 && size < filters.minimumSize)
                            || (filters.maximumSize >= 0 && size > filters.maximumSize)
                            || (filters.modifiedAfter.isValid() && modified < filters.modifiedAfter)
                            || (filters.modifiedBefore.isValid() && modified > filters.modifiedBefore))
                            continue;
                        SearchHit hit{query.value(0).toString(), name,
                            directory, candidate.score};
                        ++count;
                        if (guard) {
                            QMetaObject::invokeMethod(guard, [guard, generation, hit] {
                                if (guard)
                                    emit guard->resultFound(generation, hit);
                            });
                        }
                    }
                }
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        emitFinished(guard, generation, cancellation->load(), error, count);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return generation;
}

quint64 MetadataIndex::removeRoot(const QString& databasePath, const QString& root)
{
    cancel();
    const quint64 generation = ++generation_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    QPointer<MetadataIndex> guard(this);
    QThread* worker = QThread::create([guard, cancellation, generation, databasePath, root] {
        const QString connection = connectionName();
        QString error;
        int removed = 0;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(databasePath);
            if (!database.open()) {
                error = database.lastError().text();
            } else if (prepareSchema(database, error)) {
                QSqlQuery query(database);
                query.prepare("DELETE FROM metadata WHERE root = ?");
                query.addBindValue(QDir::cleanPath(root));
                if (!query.exec())
                    error = query.lastError().text();
                else
                    removed = query.numRowsAffected();
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        emitFinished(guard, generation, cancellation->load(), error, removed);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return generation;
}

void MetadataIndex::cancel()
{
    if (cancellation_)
        cancellation_->store(true);
}

} // namespace filemanager::search
