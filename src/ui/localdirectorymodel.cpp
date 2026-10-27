#include "localdirectorymodel.h"
#include "presentationtiming.h"
#include "backgroundfileicons.h"

#include <QFileSystemModel>
#include "platform/directorywatcher.h"
#include <QFutureWatcher>
#include <QLocale>
#include <QMimeData>
#include <QTimeZone>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <functional>
#include <atomic>
#include <utility>
#include <vector>

namespace filemanager::ui {
namespace {

constexpr int FirstPublishBatchSize = 256;
constexpr int LaterPublishBatchSize = 1024;
constexpr int CacheLimit = 512;

struct DirectoryListing
{
    QString path;
    QFileInfoList entries;
    QSet<QString> changedMetadata;
    QString error;
    quint64 rootGeneration = 0;
    quint64 nodeGeneration = 0;
    bool clearExisting = false;
    bool retainTypeIcons = false;
    bool refreshHint = false;
    bool unavailable = false;
    bool unchanged = false;
    bool cancelled = false;
};

QString pathKey(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}

bool metadataChanged(const QFileInfo& before, const QFileInfo& after)
{
    // Compare instants directly. Local-time conversion is unnecessary here and
    // repeats for every unchanged entry during a differential refresh.
    using Stage = diagnostics::PresentationStage;
    const auto differs = [&](Stage stage, const auto& read) {
        const diagnostics::PresentationScope timing(stage);
        return read(before) != read(after);
    };
    return differs(Stage::MetadataSize, [](const QFileInfo& info) { return info.size(); })
        || differs(Stage::MetadataModified,
            [](const QFileInfo& info) { return info.lastModified(QTimeZone::UTC); })
        || differs(Stage::MetadataChange,
            [](const QFileInfo& info) { return info.metadataChangeTime(QTimeZone::UTC); })
        || differs(Stage::MetadataBirth,
            [](const QFileInfo& info) { return info.birthTime(QTimeZone::UTC); })
        || differs(Stage::MetadataPermissions, [](const QFileInfo& info) { return info.permissions(); })
        || differs(Stage::MetadataType, [](const QFileInfo& info) { return info.isDir(); });
}

} // namespace

struct LocalDirectoryModel::Node
{
    ~Node()
    {
        if (enumerationCancelled)
            enumerationCancelled->store(true, std::memory_order_relaxed);
    }

    QFileInfo info;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
    int published = 0;
    quint64 generation = 0;
    bool loading = false;
    bool loaded = false;
    std::shared_ptr<std::atomic_bool> enumerationCancelled;
};

LocalDirectoryModel::LocalDirectoryModel(QObject* parent)
    : QAbstractItemModel(parent), watcher_(new platform::DirectoryWatcher(this))
{
    backgroundIcons_ = new BackgroundFileIcons(this);
    connect(backgroundIcons_, &BackgroundFileIcons::iconsChanged, this, [this] {
        // New QIcon identities prevent style caches from retaining a pending raster.
        iconCache_.clear();
        emit iconsChanged();
    });
    publishTimer_.setInterval(1);
    connect(&publishTimer_, &QTimer::timeout,
        this, &LocalDirectoryModel::publishNextBatch);
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(75);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] {
        const diagnostics::PresentationScope timing(diagnostics::PresentationStage::ListingHints);
        const QSet<QString> paths = std::exchange(changedDirectories_, {});
        for (const QString& path : paths) {
            if (Node* node = findNode(path)) {
                if (node->loading)
                    changedDirectories_.insert(path);
                else
                    startEnumeration(node, !node->loaded, false, true, !monitoringChecks_.remove(path));
            } else {
                monitoringChecks_.remove(path);
            }
        }
        if (!changedDirectories_.isEmpty())
            refreshTimer_.start();
    });
    connect(watcher_, &platform::DirectoryWatcher::directoryChanged, this,
        [this](const QString& path) {
            changedDirectories_.insert(pathKey(path));
            if (!refreshTimer_.isActive())
                refreshTimer_.start();
        });
    connect(watcher_, &platform::DirectoryWatcher::monitoringFailed, this,
        [this](const QString& path) {
            // Registration can fail after enumeration has already published rows
            // for a folder that has since disappeared. Reconcile once, without
            // turning a persistent watcher failure into an automatic retry loop.
            monitoringChecks_.insert(pathKey(path));
            changedDirectories_.insert(pathKey(path));
            if (!refreshTimer_.isActive())
                refreshTimer_.start();
            emit directoryMonitoringChanged(path, false);
        });
    connect(watcher_, &platform::DirectoryWatcher::monitoringReady, this,
        [this](const QString& path) {
            monitoringChecks_.remove(pathKey(path));
            emit directoryMonitoringChanged(path, true);
        });
}

LocalDirectoryModel::~LocalDirectoryModel() = default;

QModelIndex LocalDirectoryModel::index(
    int row, int column, const QModelIndex& parentIndex) const
{
    if (!root_ || row < 0 || column < 0 || column >= columnCount(parentIndex))
        return {};
    Node* parentNode = nodeForIndex(parentIndex);
    if (!parentNode || row >= parentNode->published)
        return {};
    return createIndex(row, column, parentNode->children.at(row).get());
}

QModelIndex LocalDirectoryModel::parent(const QModelIndex& child) const
{
    if (!child.isValid())
        return {};
    const auto* node = static_cast<const Node*>(child.internalPointer());
    return node ? indexForNode(node->parent) : QModelIndex{};
}

int LocalDirectoryModel::rowCount(const QModelIndex& parentIndex) const
{
    if (parentIndex.isValid() && parentIndex.column() != 0)
        return 0;
    const Node* node = nodeForIndex(parentIndex);
    return node ? node->published : 0;
}

int LocalDirectoryModel::columnCount(const QModelIndex&) const
{
    return 4;
}

bool LocalDirectoryModel::hasChildren(const QModelIndex& parentIndex) const
{
    const Node* node = nodeForIndex(parentIndex);
    if (!node)
        return false;
    if (node == root_.get())
        return node->published > 0 || !node->loaded;
    return node->info.isDir() && (!node->loaded || node->published > 0);
}

bool LocalDirectoryModel::canFetchMore(const QModelIndex& parentIndex) const
{
    const Node* node = nodeForIndex(parentIndex);
    return node && node->info.isDir() && !node->loaded && !node->loading;
}

void LocalDirectoryModel::fetchMore(const QModelIndex& parentIndex)
{
    if (Node* node = nodeForIndex(parentIndex); node && canFetchMore(parentIndex))
        startEnumeration(node, true);
}

QVariant LocalDirectoryModel::data(const QModelIndex& item, int role) const
{
    const diagnostics::PresentationScope timing(diagnostics::PresentationStage::ModelData,
        role, item.column());
    if (!item.isValid())
        return {};
    const Node* node = static_cast<const Node*>(item.internalPointer());
    if (!node)
        return {};
    const QFileInfo& info = node->info;
    if (role == QFileSystemModel::FilePathRole)
        return info.absoluteFilePath();
    if (role == QFileSystemModel::FileNameRole)
        return info.fileName();
    if (role == QFileSystemModel::FilePermissions)
        return static_cast<int>(info.permissions());
    if (role == Qt::FileInfoRole)
        return QVariant::fromValue(info);
    if (role == Qt::ToolTipRole && item.column() == 0)
        return QDir::toNativeSeparators(info.absoluteFilePath());
    if (role == Qt::DecorationRole && item.column() == 0) {
        const QString key = cacheKey(info);
        auto found = iconCache_.constFind(key);
        if (found == iconCache_.cend()) {
            if (iconCache_.size() >= CacheLimit)
                iconCache_.clear();
            QIcon icon;
            {
                const diagnostics::PresentationScope lookup(diagnostics::PresentationStage::IconLookup);
                icon = iconProvider_.icon(info);
            }
            found = iconCache_.insert(key,
                backgroundIcons_->icon(info.absoluteFilePath(), icon.availableSizes(),
                    key.startsWith(QStringLiteral("extension:")) ? key : QString()));
        }
        return *found;
    }
    if (role != Qt::DisplayRole && role != Qt::EditRole)
        return {};
    if (item.column() == 0)
        return info.fileName();
    if (item.column() == 1)
        return info.isDir() ? QVariant{} : QLocale().formattedDataSize(info.size());
    if (item.column() == 2) {
        const QString key = cacheKey(info);
        auto found = typeCache_.constFind(key);
        if (found == typeCache_.cend()) {
            if (typeCache_.size() >= CacheLimit)
                typeCache_.clear();
            QString type;
            {
                const diagnostics::PresentationScope lookup(diagnostics::PresentationStage::TypeLookup);
                type = iconProvider_.type(info);
            }
            found = typeCache_.insert(key, type);
        }
        return *found;
    }
    if (item.column() == 3)
        return QLocale().toString(info.lastModified(), QLocale::ShortFormat);
    return {};
}

QVariant LocalDirectoryModel::headerData(
    int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        static const QStringList labels{
            QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Type"),
            QStringLiteral("Date modified")};
        return section >= 0 && section < labels.size() ? labels.at(section) : QVariant{};
    }
    return QAbstractItemModel::headerData(section, orientation, role);
}

Qt::ItemFlags LocalDirectoryModel::flags(const QModelIndex& item) const
{
    if (!item.isValid())
        return Qt::ItemIsDropEnabled;
    Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (fileInfo(item).isDir())
        result |= Qt::ItemIsDropEnabled;
    return result;
}

QStringList LocalDirectoryModel::mimeTypes() const
{
    return {QStringLiteral("text/uri-list")};
}

QMimeData* LocalDirectoryModel::mimeData(const QModelIndexList& indexes) const
{
    QList<QUrl> urls;
    QSet<const Node*> seen;
    for (const QModelIndex& item : indexes) {
        const auto* node = item.isValid() ? static_cast<const Node*>(item.internalPointer()) : nullptr;
        if (!node || seen.contains(node))
            continue;
        seen.insert(node);
        urls.append(QUrl::fromLocalFile(node->info.absoluteFilePath()));
    }
    if (urls.isEmpty())
        return nullptr;
    auto* result = new QMimeData;
    result->setUrls(urls);
    return result;
}

Qt::DropActions LocalDirectoryModel::supportedDropActions() const
{
    return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
}

void LocalDirectoryModel::setDirectory(const QString& path)
{
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (pathKey(absolute) == pathKey(directory_) && root_ && root_->loaded) {
        refresh();
        return;
    }
    ++rootGeneration_;
    publishTimer_.stop();
    refreshTimer_.stop();
    publishQueue_.clear();
    changedDirectories_.clear();
    monitoringChecks_.clear();
    const QStringList watched = watcher_->directories();
    if (!watched.isEmpty())
        watcher_->removePaths(watched);
    beginResetModel();
    iconCache_.clear();
    backgroundIcons_->navigate();
    directory_ = absolute;
    root_ = std::make_unique<Node>();
    root_->info = QFileInfo(directory_);
    endResetModel();
    watcher_->setRoot(directory_);
    startEnumeration(root_.get(), true, true);
}

void LocalDirectoryModel::refresh()
{
    if (root_)
        startEnumeration(root_.get(), !root_->loaded);
}

void LocalDirectoryModel::setFilters(QDir::Filters filters)
{
    if (filters_ == filters)
        return;
    filters_ = filters;
    if (!root_)
        return;
    if (!root_->loaded && root_->published == 0) {
        startEnumeration(root_.get(), true);
        return;
    }

    // Keep the current rows visible while the new filter is evaluated. This avoids
    // flashing an empty folder when Hidden items is toggled, while the differential
    // refresh below still adds and removes the exact affected entries.
    std::vector<Node*> loadedDirectories;
    std::function<void(Node*)> collectLoadedDirectories =
        [&](Node* parentNode) {
            loadedDirectories.push_back(parentNode);
            for (int row = 0; row < parentNode->published; ++row) {
                Node* child = parentNode->children.at(row).get();
                if (child->info.isDir() && (child->loaded || child->loading))
                    collectLoadedDirectories(child);
            }
        };
    collectLoadedDirectories(root_.get());
    for (Node* directory : loadedDirectories)
        startEnumeration(directory, !directory->loaded && directory->published == 0);
}

QString LocalDirectoryModel::filePath(const QModelIndex& item) const
{
    return fileInfo(item).absoluteFilePath();
}

QFileInfo LocalDirectoryModel::fileInfo(const QModelIndex& item) const
{
    if (!item.isValid())
        return {};
    const auto* node = static_cast<const Node*>(item.internalPointer());
    return node ? node->info : QFileInfo{};
}

QModelIndex LocalDirectoryModel::indexForPath(const QString& path, int column) const
{
    return indexForNode(findNode(path), column);
}

bool LocalDirectoryModel::loading() const
{
    return root_ && root_->loading;
}

LocalDirectoryModel::Node* LocalDirectoryModel::nodeForIndex(const QModelIndex& item) const
{
    return item.isValid() ? static_cast<Node*>(item.internalPointer()) : root_.get();
}

LocalDirectoryModel::Node* LocalDirectoryModel::findNode(const QString& path) const
{
    if (!root_)
        return nullptr;
    const QString wanted = pathKey(path);
    if (pathKey(root_->info.absoluteFilePath()) == wanted)
        return root_.get();
    std::function<Node*(Node*)> find = [&](Node* parentNode) -> Node* {
        for (int row = 0; row < parentNode->published; ++row) {
            Node* child = parentNode->children.at(row).get();
            if (pathKey(child->info.absoluteFilePath()) == wanted)
                return child;
            if (Node* nested = find(child))
                return nested;
        }
        return nullptr;
    };
    return find(root_.get());
}

QModelIndex LocalDirectoryModel::indexForNode(const Node* node, int column) const
{
    if (!node || node == root_.get() || !node->parent
            || column < 0 || column >= columnCount())
        return {};
    const Node* parentNode = node->parent;
    for (int row = 0; row < parentNode->published; ++row) {
        if (parentNode->children.at(row).get() == node)
            return createIndex(row, column, const_cast<Node*>(node));
    }
    return {};
}

void LocalDirectoryModel::forgetSubtree(Node* node, bool includeSelf)
{
    QSet<QString> retiredPaths;
    std::function<void(const Node*)> collect = [&](const Node* retired) {
        retiredPaths.insert(pathKey(retired->info.absoluteFilePath()));
        for (const auto& child : retired->children)
            collect(child.get());
    };
    if (includeSelf)
        collect(node);
    else
        for (const auto& child : node->children)
            collect(child.get());
    publishQueue_.removeIf([&](const QString& path) { return retiredPaths.contains(pathKey(path)); });
    changedDirectories_.subtract(retiredPaths);
    monitoringChecks_.subtract(retiredPaths);
    QStringList retiredWatches;
    for (const QString& watched : watcher_->directories()) {
        if (retiredPaths.contains(pathKey(watched)))
            retiredWatches.append(watched);
    }
    if (!retiredWatches.isEmpty())
        watcher_->removePaths(retiredWatches);
}

void LocalDirectoryModel::startEnumeration(Node* node, bool clearExisting, bool retainTypeIcons,
    bool refreshHint, bool registerWatch)
{
    const diagnostics::PresentationScope timing(diagnostics::PresentationStage::ListingStart);
    if (!node || !node->info.isDir())
        return;
    const QString nodePath = node->info.absoluteFilePath();
    if (registerWatch)
        watcher_->addPath(nodePath);
    publishQueue_.removeAll(pathKey(nodePath));
    if (clearExisting) {
        forgetSubtree(node, false);
        if (node->published > 0) {
            const QModelIndex parentIndex = indexForNode(node);
            beginRemoveRows(parentIndex, 0, node->published - 1);
            node->children.clear();
            node->published = 0;
            endRemoveRows();
        } else {
            node->children.clear();
        }
    } else if (node->published < static_cast<int>(node->children.size())) {
        // A superseded listing may have rows waiting in the publication queue.
        // They were never announced to views, so discard them before merging the
        // replacement result while preserving every row already on screen.
        node->children.resize(static_cast<size_t>(node->published));
    }
    // A differential refresh keeps a usable listing. Navigation/reveal of the
    // same folder must not reset it merely because a background hint is in flight.
    if (clearExisting)
        node->loaded = false;
    node->loading = true;
    // A replacement node at the same path must never inherit an old worker's
    // generation. The counter belongs to the model, not the node's lifetime.
    const quint64 nodeGeneration = node->generation = ++enumerationGeneration_;
    const quint64 requestedRootGeneration = rootGeneration_;
    const QDir::Filters requestedFilters = filters_;
    if (node->enumerationCancelled)
        node->enumerationCancelled->store(true, std::memory_order_relaxed);
    const auto cancelled = node->enumerationCancelled = std::make_shared<std::atomic_bool>(false);
    QFileInfoList previousEntries;
    if (!clearExisting) {
        const diagnostics::PresentationScope snapshotTiming(diagnostics::PresentationStage::ListingSnapshot);
        previousEntries.reserve(static_cast<qsizetype>(node->children.size()));
        for (const auto& child : node->children) {
            QFileInfo snapshot = child->info;
            // QFileInfo's const getters populate caches. Detach through this
            // non-const setter, retaining the native cached metadata, before
            // handing a snapshot to a worker that may query missing fields.
            snapshot.setCaching(snapshot.caching());
            previousEntries.append(std::move(snapshot));
        }
    }
    auto* resultWatcher = new QFutureWatcher<DirectoryListing>(this);
    connect(resultWatcher, &QFutureWatcher<DirectoryListing>::finished, this,
        [this, resultWatcher] {
            const diagnostics::PresentationScope completion(diagnostics::PresentationStage::ListingCompletion);
            const DirectoryListing result = resultWatcher->result();
            resultWatcher->deleteLater();
            if (result.cancelled || result.rootGeneration != rootGeneration_)
                return;
            Node* node = findNode(result.path);
            if (!node || node->generation != result.nodeGeneration)
                return;
            if (!result.error.isEmpty()) {
                watcher_->removePaths({result.path});
                if (result.unavailable) {
                    forgetSubtree(node, false);
                    const int published = node->published;
                    if (published)
                        beginRemoveRows(indexForNode(node), 0, published - 1);
                    node->children.clear();
                    node->published = 0;
                    if (published)
                        endRemoveRows();
                }
                if (node->parent) {
                    // The parent's removal hint can precede the final release of
                    // the child's native watch. Refresh it again after retirement
                    // so a disappeared child cannot remain as a stale model row.
                    changedDirectories_.insert(pathKey(node->parent->info.absoluteFilePath()));
                    if (!refreshTimer_.isActive())
                        refreshTimer_.start();
                }
                node->loading = false;
                node->loaded = true;
                emit directoryLoadFailed(result.path, result.error);
                emit directoryLoaded(result.path);
                return;
            }
            if (result.refreshHint && !result.clearExisting && result.unchanged
                    && result.entries.size() == static_cast<qsizetype>(node->children.size())) {
                const diagnostics::PresentationScope comparison(diagnostics::PresentationStage::ListingUnchangedCheck);
                // The worker returns unchanged entries in the snapshot's row
                // order. Generation checks above guard that model snapshot.
                for (qsizetype row = 0; row < result.entries.size(); ++row)
                    node->children.at(static_cast<size_t>(row))->info = result.entries.at(row);
                // Retain the worker-populated metadata caches without changing
                // rows, selection, pending icon requests or warm pixels.
                node->loading = false;
                node->loaded = true;
                return;
            }
            iconCache_.clear();
            if (!result.retainTypeIcons)
                backgroundIcons_->clear();
            typeCache_.clear();
            if (result.clearExisting) {
                node->children.reserve(static_cast<size_t>(result.entries.size()));
                for (const QFileInfo& info : result.entries) {
                    auto child = std::make_unique<Node>();
                    child->info = info;
                    child->parent = node;
                    node->children.push_back(std::move(child));
                }
                node->published = 0;
            } else {
                applyRefresh(node, result.entries, result.changedMetadata);
            }
            if (node->published < static_cast<int>(node->children.size()))
                schedulePublication(node);
            else {
                node->loading = false;
                node->loaded = true;
                emit directoryLoaded(result.path);
            }
        });
    resultWatcher->setFuture(QtConcurrent::run(
        [nodePath, requestedFilters, requestedRootGeneration, nodeGeneration, clearExisting,
            retainTypeIcons, refreshHint, cancelled, previousEntries = std::move(previousEntries)] {
            DirectoryListing result;
            result.path = nodePath;
            result.rootGeneration = requestedRootGeneration;
            result.nodeGeneration = nodeGeneration;
            result.clearExisting = clearExisting;
            result.retainTypeIcons = retainTypeIcons;
            result.refreshHint = refreshHint;
            const auto isCancelled = [&] {
                result.cancelled = cancelled->load(std::memory_order_relaxed);
                return result.cancelled;
            };
            if (isCancelled())
                return result;
            // The GUI only detaches the model's metadata snapshots. Path
            // normalization and hash allocation belong to this worker too;
            // repeated hints can otherwise stack that work in one GUI turn.
            QHash<QString, QFileInfo> previous;
            previous.reserve(previousEntries.size());
            for (const QFileInfo& snapshot : previousEntries) {
                if (isCancelled())
                    return result;
                previous.insert(pathKey(snapshot.absoluteFilePath()), snapshot);
            }
            const QFileInfo directoryInfo(nodePath);
            result.unavailable = !directoryInfo.exists() || !directoryInfo.isDir();
            if (!directoryInfo.exists() || !directoryInfo.isDir() || !directoryInfo.isReadable()) {
                result.error = QStringLiteral("The folder is unavailable or cannot be read.");
                return result;
            }
            result.entries = QDir(nodePath).entryInfoList(requestedFilters, QDir::NoSort);
            if (isCancelled())
                return result;
            if (!clearExisting) {
                result.unchanged = previous.size() == result.entries.size();
                for (const auto& info : result.entries) {
                    if (isCancelled())
                        return result;
                    const QString key = pathKey(info.absoluteFilePath());
                    const auto found = previous.constFind(key);
                    if (found == previous.cend() || metadataChanged(*found, info)) {
                        result.changedMetadata.insert(key);
                        result.unchanged = false;
                    }
                }
                if (refreshHint && result.unchanged) {
                    // Prepare cache hydration in the original row order here,
                    // avoiding another full path index in the GUI callback.
                    QHash<QString, QFileInfo> refreshed;
                    refreshed.reserve(result.entries.size());
                    for (const auto& info : result.entries) {
                        if (isCancelled())
                            return result;
                        refreshed.insert(pathKey(info.absoluteFilePath()), info);
                    }
                    QFileInfoList ordered;
                    ordered.reserve(previousEntries.size());
                    for (const auto& snapshot : previousEntries) {
                        if (isCancelled())
                            return result;
                        const auto found = refreshed.constFind(pathKey(snapshot.absoluteFilePath()));
                        if (found == refreshed.cend()) {
                            result.unchanged = false;
                            break;
                        }
                        ordered.append(*found);
                    }
                    if (result.unchanged)
                        result.entries = std::move(ordered);
                }
            }
            return result;
        }));
}

void LocalDirectoryModel::applyRefresh(Node* node, const QFileInfoList& incoming,
    const QSet<QString>& changedMetadata)
{
    const diagnostics::PresentationScope refresh(diagnostics::PresentationStage::ListingRefresh);
    QHash<QString, QFileInfo> byPath;
    byPath.reserve(incoming.size());
    for (const QFileInfo& info : incoming)
        byPath.insert(pathKey(info.absoluteFilePath()), info);

    const QModelIndex parentIndex = indexForNode(node);
    for (int row = node->published - 1; row >= 0; --row) {
        const QString key = pathKey(node->children.at(row)->info.absoluteFilePath());
        const auto replacement = byPath.constFind(key);
        if (replacement != byPath.cend()
                && node->children.at(row)->info.isDir() == replacement->isDir())
            continue;
        // A file/folder type change is a new tree item, not a metadata update.
        // Retire its children and pending publication before reusing the path.
        forgetSubtree(node->children.at(row).get(), true);
        beginRemoveRows(parentIndex, row, row);
        node->children.erase(node->children.begin() + row);
        --node->published;
        endRemoveRows();
    }
    for (int row = 0; row < node->published; ++row) {
        Node* child = node->children.at(row).get();
        const QString key = pathKey(child->info.absoluteFilePath());
        const auto found = byPath.constFind(key);
        if (found == byPath.cend())
            continue;
        child->info = *found;
        if (changedMetadata.contains(key)) {
            emit dataChanged(index(row, 0, parentIndex),
                index(row, columnCount() - 1, parentIndex));
        }
        byPath.remove(key);
    }
    for (const QFileInfo& info : incoming) {
        if (!byPath.contains(pathKey(info.absoluteFilePath())))
            continue;
        auto child = std::make_unique<Node>();
        child->info = info;
        child->parent = node;
        node->children.push_back(std::move(child));
    }
}

void LocalDirectoryModel::schedulePublication(Node* node)
{
    const QString key = pathKey(node->info.absoluteFilePath());
    if (!publishQueue_.contains(key))
        publishQueue_.enqueue(key);
    if (!publishTimer_.isActive())
        publishTimer_.start();
}

void LocalDirectoryModel::publishNextBatch()
{
    const diagnostics::PresentationScope timing(diagnostics::PresentationStage::ListingPublish);
    if (publishQueue_.isEmpty()) {
        publishTimer_.stop();
        return;
    }
    const QString path = publishQueue_.dequeue();
    Node* node = findNode(path);
    if (!node)
        return;
    const int available = static_cast<int>(node->children.size()) - node->published;
    // Show an initial slice promptly, then reduce repeated view layouts/repaints
    // while continuing to yield and rotate through the pending directories.
    const int batchSize = node->published == 0 ? FirstPublishBatchSize : LaterPublishBatchSize;
    const int count = qMin(batchSize, available);
    if (count > 0) {
        const QModelIndex parentIndex = indexForNode(node);
        beginInsertRows(parentIndex, node->published, node->published + count - 1);
        node->published += count;
        endInsertRows();
    }
    if (node->published < static_cast<int>(node->children.size())) {
        publishQueue_.enqueue(path);
    } else {
        node->loading = false;
        node->loaded = true;
        watcher_->addPath(node->info.absoluteFilePath());
        emit directoryLoaded(node->info.absoluteFilePath());
    }
    if (publishQueue_.isEmpty())
        publishTimer_.stop();
}

QString LocalDirectoryModel::cacheKey(const QFileInfo& info) const
{
    if (info.isDir())
        return QStringLiteral("directory:") + info.absoluteFilePath();
    const QString suffix = info.suffix().toCaseFolded();
    if (suffix == QStringLiteral("exe") || suffix == QStringLiteral("lnk")
            || suffix == QStringLiteral("ico") || info.isSymLink() || info.isExecutable())
        return QStringLiteral("path:") + info.absoluteFilePath();
    if (suffix.isEmpty())
        return QStringLiteral("filename:") + info.fileName();
    return QStringLiteral("extension:") + suffix;
}

} // namespace filemanager::ui
