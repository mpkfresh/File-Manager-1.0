#include "ui/navigationtree.h"

#include "platform/knownfolders.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFont>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLocale>
#include <QMimeData>
#include <QMouseEvent>
#include <QSet>
#include <QStyle>
#include <QUrl>
#include <QtConcurrentRun>

#include <algorithm>
#include <queue>
#include <vector>

namespace filemanager::ui {
namespace {

constexpr qsizetype MaximumVisibleChildren = 1024;

struct FolderListing
{
    QString path;
    QStringList children;
    bool truncated = false;
    bool cancelled = false;
    quint64 generation = 0;
};

QString displayNameForPath(const QString& path)
{
    QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QDir::toNativeSeparators(path) : name;
}

} // namespace

NavigationTree::NavigationTree(QWidget* parent)
    : QTreeWidget(parent)
{
    setObjectName(QStringLiteral("QuickAccess"));
    setAccessibleName(tr("Folders and locations"));
    setAccessibleDescription(tr(
        "Expandable folders, bookmarks, storage and explicit Windows locations"));
    setHeaderHidden(true);
    setRootIsDecorated(true);
    setItemsExpandable(true);
    setExpandsOnDoubleClick(true);
    setIndentation(15);
    setIconSize(QSize(17, 17));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    setDragEnabled(true);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    setDropIndicatorShown(true);
    setContextMenuPolicy(Qt::CustomContextMenu);

    connect(this, &QTreeWidget::itemExpanded, this,
        [this](QTreeWidgetItem* item) { requestChildren(item); });
    connect(this, &QTreeWidget::itemActivated, this,
        [this](QTreeWidgetItem* item) {
            if (!item)
                return;
            const QString kind = item->data(0, KindRole).toString();
            if (kind == QStringLiteral("shell-visit")) {
                emit shellLocationActivated(item->data(0, ShellIdentityRole).toByteArray());
                return;
            }
            if (kind == QStringLiteral("home")) {
                emit homeActivated();
                return;
            }
            if (kind == QStringLiteral("this-pc")) {
                emit thisPcActivated();
                return;
            }
            if (kind == QStringLiteral("libraries")) {
                emit librariesActivated();
                return;
            }
            if (kind == QStringLiteral("recycle-bin")) {
                emit recycleBinActivated();
                return;
            }
            if (kind == QStringLiteral("network")) {
                emit networkActivated();
                return;
            }
            const QString path = item->data(0, PathRole).toString();
            if (!path.isEmpty()) {
                emit locationActivated(path);
                return;
            }
            if (item->childCount() > 0)
                item->setExpanded(!item->isExpanded());
        });
}

NavigationTree::~NavigationTree()
{
    listingCancelled_->store(true);
}

void NavigationTree::rebuild(const QStringList& recentPaths,
    const filemanager::settings::FavoriteGroups& favorites,
    const QVector<filemanager::files::StorageVolume>& volumes)
{
    const QString selectedPath = currentItem() ? currentItem()->data(0, PathRole).toString()
                                                : pendingPath_;
    ++generation_;
    listingCancelled_->store(true);
    listingCancelled_ = std::make_shared<std::atomic_bool>(false);
    clear();
    shellNavigationItems_.clear();

    const auto addSection = [this](const QString& label, const QString& kind) {
        auto* item = new QTreeWidgetItem(this, {label});
        item->setData(0, KindRole, kind);
        item->setFlags(Qt::ItemIsEnabled);
        QFont font = item->font(0);
        font.setPointSize(8);
        font.setBold(true);
        item->setFont(0, font);
        item->setForeground(0, QColor(QStringLiteral("#777a84")));
        item->setExpanded(true);
        return item;
    };

    auto* home = new QTreeWidgetItem(this, {tr("Home")});
    home->setData(0, PathRole, QStringLiteral("__home__"));
    home->setData(0, KindRole, QStringLiteral("home"));
    home->setIcon(0, style()->standardIcon(QStyle::SP_DirHomeIcon));
    home->setToolTip(0, tr("Open local favorites, recent folders and explicitly saved searches"));
    home->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

    auto* thisPc = new QTreeWidgetItem(this, {tr("This PC")});
    thisPc->setData(0, PathRole, QStringLiteral("__this_pc__"));
    thisPc->setData(0, KindRole, QStringLiteral("this-pc"));
    thisPc->setIcon(0, style()->standardIcon(QStyle::SP_ComputerIcon));
    thisPc->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled);
    thisPc->setExpanded(true);

    using filemanager::platform::KnownFolder;
    // These rows share the current native folder artwork. Reuse it within this rebuild,
    // without retaining a cache across theme changes or querying any folder's contents.
    const QIcon folderIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const QList<QPair<QString, KnownFolder>> knownFolders = {
        {tr("Desktop"), KnownFolder::Desktop}, {tr("Documents"), KnownFolder::Documents},
        {tr("Downloads"), KnownFolder::Downloads}, {tr("Music"), KnownFolder::Music},
        {tr("Pictures"), KnownFolder::Pictures}, {tr("Videos"), KnownFolder::Videos}};
    for (const auto& [label, folder] : knownFolders) {
        const QString path = filemanager::platform::knownFolderPath(folder);
        if (!path.isEmpty())
            addFolder(thisPc, label, path, folderIcon,
                QStringLiteral("known-folder"));
    }
    for (const auto& volume : volumes) {
        const QString capacity = volume.bytesTotal > 0
            ? tr("%1 free of %2").arg(
                QLocale().formattedDataSize(static_cast<qint64>(volume.bytesAvailable)),
                QLocale().formattedDataSize(static_cast<qint64>(volume.bytesTotal)))
            : QString{};
        const QString nativeRoot = QDir::toNativeSeparators(volume.rootPath);
        const QString volumeLabel = volume.displayName.contains(nativeRoot, Qt::CaseInsensitive)
            ? volume.displayName : tr("%1 (%2)").arg(volume.displayName, nativeRoot);
        addFolder(thisPc, volumeLabel, volume.rootPath,
            style()->standardIcon(volume.kind == filemanager::files::StorageLocationKind::Network
                    ? QStyle::SP_DriveNetIcon : QStyle::SP_DriveHDIcon),
            QStringLiteral("storage"), {},
            QDir::toNativeSeparators(volume.rootPath)
                + (capacity.isEmpty() ? QString{} : QStringLiteral("\n") + capacity));
    }

    auto* libraries = new QTreeWidgetItem(this, {tr("Libraries")});
    libraries->setData(0, PathRole, QStringLiteral("__libraries__"));
    libraries->setData(0, KindRole, QStringLiteral("libraries"));
    libraries->setIcon(0, folderIcon);
    libraries->setToolTip(0, tr("Open the built-in Windows Libraries location"));
    libraries->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

    auto* recycleBin = new QTreeWidgetItem(this, {tr("Recycle Bin")});
    recycleBin->setData(0, PathRole, QStringLiteral("__recycle_bin__"));
    recycleBin->setData(0, KindRole, QStringLiteral("recycle-bin"));
    recycleBin->setIcon(0, style()->standardIcon(QStyle::SP_TrashIcon));
    recycleBin->setToolTip(0, tr("Browse items managed by the Windows Recycle Bin"));
    recycleBin->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

    // A single named group is already a clear label and a useful drop target.
    // Keep the extra heading only when it distinguishes multiple groups.
    QTreeWidgetItem* bookmarks = favorites.groups.size() > 1
        ? addSection(tr("BOOKMARKS"), QStringLiteral("bookmarks-section")) : nullptr;
    for (const auto& group : favorites.groups) {
        auto* groupItem = bookmarks
            ? new QTreeWidgetItem(bookmarks, {group.name})
            : new QTreeWidgetItem(this, {group.name});
        groupItem->setData(0, KindRole, QStringLiteral("favorite-group"));
        groupItem->setData(0, GroupIdRole, group.id);
        groupItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
        groupItem->setToolTip(0, tr("Drop a folder here to pin it"));
        groupItem->setExpanded(true);
        for (const QString& path : group.paths)
            addFolder(groupItem, displayNameForPath(path), path,
                folderIcon, QStringLiteral("favorite"), group.id);
    }

    QTreeWidgetItem* recents = nullptr;
    for (const QString& path : recentPaths) {
        if (path.isEmpty())
            continue;
        if (!recents)
            recents = addSection(tr("RECENT FOLDERS"), QStringLiteral("recents-section"));
        addFolder(recents, displayNameForPath(path), path,
            folderIcon, QStringLiteral("recent"));
    }

    auto* network = new QTreeWidgetItem(this, {tr("Network")});
    network->setData(0, PathRole, QStringLiteral("__network__"));
    network->setData(0, KindRole, QStringLiteral("network"));
    network->setIcon(0, style()->standardIcon(QStyle::SP_DriveNetIcon));
    network->setToolTip(0, tr("Open the explicit network-path and one-time scan window"));
    network->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

    synchronizeToPath(selectedPath);
}

void NavigationTree::setShellNavigationItems(
    const QList<filemanager::platform::ShellItemReference>& items)
{
    const auto boundedItems = items.mid(0, MaximumVisibleChildren + 1);
    if (boundedItems.size() == shellNavigationItems_.size()
            && std::equal(boundedItems.cbegin(), boundedItems.cend(), shellNavigationItems_.cbegin(),
                [](const auto& left, const auto& right) {
                    return left.valid == right.valid && left.folder == right.folder
                        && left.idList == right.idList && left.displayName == right.displayName
                        && left.parsingName == right.parsingName;
                }))
        return; // Keep keyboard selection and disclosure state on unrelated count notifications.
    shellNavigationItems_ = boundedItems;
    const bool selectedVisit = currentItem()
        && currentItem()->data(0, KindRole).toString() == QStringLiteral("shell-visit");
    const QByteArray selectedIdentity = selectedVisit
        ? currentItem()->data(0, ShellIdentityRole).toByteArray() : QByteArray{};
    // Never resolve, enumerate, persist or attach filesystem drop targets to these identities.
    for (int index = topLevelItemCount() - 1; index >= 0; --index) {
        if (topLevelItem(index)->data(0, KindRole).toString() == QStringLiteral("shell-visit-section"))
            delete takeTopLevelItem(index);
    }
    if (boundedItems.isEmpty() || !boundedItems.first().valid || !boundedItems.first().folder
            || !filemanager::platform::ShellItem::isValidSerializedIdentity(boundedItems.first().idList)) {
        if (selectedVisit) {
            for (int index = 0; index < topLevelItemCount(); ++index) {
                auto* item = topLevelItem(index);
                if (item->data(0, KindRole).toString() == QStringLiteral("home")) {
                    setCurrentItem(item);
                    break;
                }
            }
        }
        return;
    }
    auto* section = new QTreeWidgetItem({tr("Current Windows visit")});
    section->setData(0, KindRole, QStringLiteral("shell-visit-section"));
    section->setFlags(Qt::ItemIsEnabled);
    section->setToolTip(0, tr("Folders already loaded by the active pane. Cleared when this visit ends."));
    insertTopLevelItem(std::min(1, topLevelItemCount()), section);
    const QIcon folderIcon = style()->standardIcon(QStyle::SP_DirIcon);
    QTreeWidgetItem* root = nullptr;
    QTreeWidgetItem* restoredSelection = nullptr;
    QSet<QByteArray> seen;
    for (const auto& entry : boundedItems) {
        if (!entry.valid || !entry.folder || seen.contains(entry.idList)
                || !filemanager::platform::ShellItem::isValidSerializedIdentity(entry.idList))
            continue;
        if (seen.size() >= MaximumVisibleChildren + 1)
            break;
        seen.insert(entry.idList);
        auto* node = new QTreeWidgetItem(root ? root : section,
            {entry.displayName.isEmpty() ? entry.parsingName : entry.displayName});
        node->setData(0, KindRole, QStringLiteral("shell-visit"));
        node->setData(0, ShellIdentityRole, entry.idList);
        node->setData(0, ChildrenStateRole, static_cast<int>(ChildrenState::Loaded));
        node->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        node->setIcon(0, folderIcon);
        node->setToolTip(0, tr("Open this folder from the current Windows visit.\n%1").arg(entry.parsingName));
        if (!root)
            root = node;
        if (selectedVisit && entry.idList == selectedIdentity)
            restoredSelection = node;
    }
    section->setExpanded(true);
    root->setExpanded(true);
    pendingPath_.clear();
    if (selectedVisit || !currentItem())
        setCurrentItem(restoredSelection ? restoredSelection : root);
}

QTreeWidgetItem* NavigationTree::addFolder(QTreeWidgetItem* parent, const QString& label,
    const QString& rawPath, const QIcon& icon, const QString& kind,
    const QString& groupId, const QString& tooltip)
{
    const QString path = normalizedPath(rawPath);
    auto* item = parent ? new QTreeWidgetItem(parent, {label})
                        : new QTreeWidgetItem(this, {label});
    item->setData(0, PathRole, path);
    item->setData(0, KindRole, kind);
    item->setData(0, GroupIdRole, groupId);
    item->setData(0, ChildrenStateRole, static_cast<int>(ChildrenState::NotRequested));
    item->setIcon(0, icon);
    item->setToolTip(0, tooltip.isEmpty() ? QDir::toNativeSeparators(path) : tooltip);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled);
    if (kind == QStringLiteral("favorite"))
        item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
    item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    return item;
}

void NavigationTree::requestChildren(QTreeWidgetItem* item)
{
    if (!item || item->data(0, PathRole).toString().startsWith(QStringLiteral("__")))
        return;
    if (static_cast<ChildrenState>(item->data(0, ChildrenStateRole).toInt())
        != ChildrenState::NotRequested)
        return;

    const QString path = item->data(0, PathRole).toString();
    if (path.isEmpty())
        return;
    item->setData(0, ChildrenStateRole, static_cast<int>(ChildrenState::Loading));
    auto* loading = new QTreeWidgetItem(item, {tr("Loading…")});
    loading->setData(0, KindRole, QStringLiteral("loading"));
    loading->setFlags(Qt::NoItemFlags);

    const quint64 requestedGeneration = generation_;
    const auto cancelled = listingCancelled_;
    auto* watcher = new QFutureWatcher<FolderListing>(this);
    connect(watcher, &QFutureWatcher<FolderListing>::finished, this,
        [this, watcher] {
            const FolderListing result = watcher->result();
            watcher->deleteLater();
            if (!result.cancelled && result.generation == generation_)
                applyChildren(result.path, result.children, result.truncated);
        });
    watcher->setFuture(QtConcurrent::run([path, requestedGeneration, cancelled] {
        FolderListing result;
        result.path = path;
        result.generation = requestedGeneration;
        if (cancelled->load()) {
            result.cancelled = true;
            return result;
        }
        const auto nameLess = [](const QString& left, const QString& right) {
            const int comparison = QString::compare(left, right, Qt::CaseInsensitive);
            return comparison == 0 ? left < right : comparison < 0;
        };
        // Keep the alphabetically first 1,024 immediate folders without buffering the full directory.
        // The iterator is non-recursive; cancellation is cooperative between native iterator calls.
        std::priority_queue<QString, std::vector<QString>, decltype(nameLess)> kept(nameLess);
        QDirIterator entries(path, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDirIterator::NoIteratorFlags);
        while (!cancelled->load() && entries.hasNext()) {
            const QString child = entries.nextFileInfo().absoluteFilePath();
            if (kept.size() < static_cast<size_t>(MaximumVisibleChildren)) {
                kept.push(child);
            } else {
                result.truncated = true;
                if (nameLess(child, kept.top())) {
                    kept.pop();
                    kept.push(child);
                }
            }
        }
        if (cancelled->load()) {
            result.cancelled = true;
            return result;
        }
        result.children.reserve(static_cast<qsizetype>(kept.size()));
        while (!kept.empty()) {
            result.children.append(kept.top());
            kept.pop();
        }
        std::reverse(result.children.begin(), result.children.end());
        return result;
    }));
}

void NavigationTree::applyChildren(const QString& path, const QStringList& childPaths, bool truncated)
{
    const QIcon folderIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const QList<QTreeWidgetItem*> matches = findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    QList<QTreeWidgetItem*> targets;
    for (QTreeWidgetItem* item : matches) {
        if (samePath(item->data(0, PathRole).toString(), path)
            && static_cast<ChildrenState>(item->data(0, ChildrenStateRole).toInt())
                == ChildrenState::Loading)
            targets.append(item);
    }
    // The recursive match list also includes loading children; do not revisit it after deleting them.
    for (QTreeWidgetItem* item : targets) {
        qDeleteAll(item->takeChildren());
        for (const QString& childPath : childPaths)
            addFolder(item, displayNameForPath(childPath), childPath,
                folderIcon, QStringLiteral("folder"));
        if (truncated) {
            auto* more = new QTreeWidgetItem(item,
                {tr("More than %1 folders; refine from the main pane").arg(MaximumVisibleChildren)});
            more->setData(0, KindRole, QStringLiteral("limit"));
            more->setFlags(Qt::NoItemFlags);
        }
        item->setData(0, ChildrenStateRole, static_cast<int>(ChildrenState::Loaded));
        if (childPaths.isEmpty() && !truncated)
            item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicatorWhenChildless);
    }
    if (!pendingPath_.isEmpty())
        synchronizeToPath(pendingPath_);
}

void NavigationTree::synchronizeToPath(const QString& rawPath)
{
    const QString path = normalizedPath(rawPath);
    if (path.isEmpty() || path.startsWith(QStringLiteral("__")))
        return;
    pendingPath_ = path;
    if (QTreeWidgetItem* exact = findExactPath(path)) {
        for (QTreeWidgetItem* parent = exact->parent(); parent; parent = parent->parent())
            parent->setExpanded(true);
        setCurrentItem(exact);
        scrollToItem(exact, QAbstractItemView::PositionAtCenter);
        pendingPath_.clear();
        return;
    }
    if (QTreeWidgetItem* ancestor = deepestVisibleAncestor(path)) {
        ancestor->setExpanded(true);
        requestChildren(ancestor);
    }
}

void NavigationTree::selectFavorite(const QString& path, const QString& groupId)
{
    const QList<QTreeWidgetItem*> items = findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
        if (item->data(0, KindRole).toString() == QStringLiteral("favorite")
            && item->data(0, GroupIdRole).toString() == groupId
            && samePath(item->data(0, PathRole).toString(), path)) {
            setCurrentItem(item);
            scrollToItem(item);
            return;
        }
    }
}

QString NavigationTree::selectedFavoritePath() const
{
    return currentItem() && currentItem()->data(0, KindRole).toString() == QStringLiteral("favorite")
        ? currentItem()->data(0, PathRole).toString() : QString{};
}

QString NavigationTree::selectedFavoriteGroupId() const
{
    return currentItem() && currentItem()->data(0, KindRole).toString() == QStringLiteral("favorite")
        ? currentItem()->data(0, GroupIdRole).toString() : QString{};
}

QHash<QString, QStringList> NavigationTree::favoriteOrder() const
{
    QHash<QString, QStringList> result;
    const QList<QTreeWidgetItem*> items = findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
        if (item->data(0, KindRole).toString() != QStringLiteral("favorite-group"))
            continue;
        QStringList paths;
        for (int index = 0; index < item->childCount(); ++index) {
            QTreeWidgetItem* child = item->child(index);
            if (child->data(0, KindRole).toString() == QStringLiteral("favorite"))
                paths.append(child->data(0, PathRole).toString());
        }
        result.insert(item->data(0, GroupIdRole).toString(), paths);
    }
    return result;
}

void NavigationTree::startDrag(Qt::DropActions supportedActions)
{
    if (currentItem()
        && currentItem()->data(0, KindRole).toString() == QStringLiteral("favorite"))
        QTreeWidget::startDrag(supportedActions);
}

void NavigationTree::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->source() == this) {
        QTreeWidget::dragEnterEvent(event);
        return;
    }
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        event->ignore();
}

void NavigationTree::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->source() == this) {
        QTreeWidgetItem* dragged = currentItem();
        QTreeWidgetItem* target = itemAt(event->position().toPoint());
        QTreeWidgetItem* group = dragged ? dragged->parent() : nullptr;
        const bool valid = dragged && group && target
            && dragged->data(0, KindRole).toString() == QStringLiteral("favorite")
            && (target == group || target->parent() == group)
            && (target == group
                || target->data(0, KindRole).toString() == QStringLiteral("favorite"));
        valid ? event->acceptProposedAction() : event->ignore();
        return;
    }
    if (!event->mimeData()->hasUrls() || dropDestination(event->position().toPoint()).isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}

void NavigationTree::dropEvent(QDropEvent* event)
{
    if (event->source() == this) {
        if (moveFavoriteAt(event->position().toPoint()))
            event->acceptProposedAction();
        else
            event->ignore();
        return;
    }
    const QString destination = dropDestination(event->position().toPoint());
    if (destination.isEmpty() || !event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths.append(QDir::cleanPath(url.toLocalFile()));
    }
    if (paths.isEmpty()) {
        event->ignore();
        return;
    }
    Qt::DropAction action = event->dropAction();
    if (action == Qt::IgnoreAction)
        action = event->proposedAction();
    const bool knownEffect = action == Qt::CopyAction || action == Qt::MoveAction
        || action == Qt::LinkAction;
    emit filesDropped(paths, destination, action == Qt::MoveAction,
        !knownEffect, action == Qt::LinkAction);
    event->acceptProposedAction();
}

void NavigationTree::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        QTreeWidgetItem* item = itemAt(event->position().toPoint());
        const QString kind = item ? item->data(0, KindRole).toString() : QString{};
        if (kind == QStringLiteral("shell-visit")) {
            emit shellBackgroundRequested(item->data(0, ShellIdentityRole).toByteArray());
            event->accept();
            return;
        }
        if (kind == QStringLiteral("home")) {
            emit homeBackgroundRequested();
            event->accept();
            return;
        }
        if (kind == QStringLiteral("this-pc")) {
            emit thisPcBackgroundRequested();
            event->accept();
            return;
        }
        if (kind == QStringLiteral("libraries")) {
            emit librariesBackgroundRequested();
            event->accept();
            return;
        }
        if (kind == QStringLiteral("recycle-bin")) {
            emit recycleBinBackgroundRequested();
            event->accept();
            return;
        }
        const QString path = item ? item->data(0, PathRole).toString() : QString{};
        if (!path.isEmpty() && !path.startsWith(QStringLiteral("__"))) {
            emit backgroundLocationRequested(path);
            event->accept();
            return;
        }
    }
    QTreeWidget::mouseReleaseEvent(event);
}

QString NavigationTree::dropDestination(const QPoint& position) const
{
    QTreeWidgetItem* target = itemAt(position);
    if (!target)
        return {};
    const QString path = target->data(0, PathRole).toString();
    return path.startsWith(QStringLiteral("__")) ? QString{} : path;
}

bool NavigationTree::moveFavoriteAt(const QPoint& position)
{
    QTreeWidgetItem* dragged = currentItem();
    QTreeWidgetItem* target = itemAt(position);
    QTreeWidgetItem* group = dragged ? dragged->parent() : nullptr;
    if (!dragged || !target || !group
        || dragged->data(0, KindRole).toString() != QStringLiteral("favorite")
        || (target != group && target->parent() != group)
        || (target != group
            && target->data(0, KindRole).toString() != QStringLiteral("favorite")))
        return false;

    const int from = group->indexOfChild(dragged);
    int destination = target == group ? group->childCount() : group->indexOfChild(target);
    if (target != group && dropIndicatorPosition() == QAbstractItemView::BelowItem)
        ++destination;
    QTreeWidgetItem* moved = group->takeChild(from);
    if (from < destination)
        --destination;
    destination = qBound(0, destination, group->childCount());
    group->insertChild(destination, moved);
    setCurrentItem(moved);
    emit favoriteOrderChanged();
    return true;
}

QTreeWidgetItem* NavigationTree::findExactPath(const QString& path) const
{
    const QList<QTreeWidgetItem*> items = findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items)
        if (samePath(item->data(0, PathRole).toString(), path))
            return item;
    return nullptr;
}

QTreeWidgetItem* NavigationTree::deepestVisibleAncestor(const QString& path) const
{
    QTreeWidgetItem* result = nullptr;
    qsizetype length = -1;
    const QList<QTreeWidgetItem*> items = findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
        const QString candidate = item->data(0, PathRole).toString();
        if (candidate.size() > length && isPathAncestor(candidate, path)) {
            result = item;
            length = candidate.size();
        }
    }
    return result;
}

bool NavigationTree::samePath(const QString& left, const QString& right)
{
    return normalizedPath(left).compare(normalizedPath(right), Qt::CaseInsensitive) == 0;
}

bool NavigationTree::isPathAncestor(const QString& rawParent, const QString& rawChild)
{
    QString parent = normalizedPath(rawParent);
    const QString child = normalizedPath(rawChild);
    if (parent.isEmpty() || parent.startsWith(QStringLiteral("__")) || child.isEmpty())
        return false;
    if (parent.compare(child, Qt::CaseInsensitive) == 0)
        return true;
    if (!parent.endsWith('/'))
        parent += '/';
    return child.startsWith(parent, Qt::CaseInsensitive);
}

QString NavigationTree::normalizedPath(const QString& path)
{
    if (path.isEmpty() || path.startsWith(QStringLiteral("__")))
        return path;
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

} // namespace filemanager::ui
