#include "filepane.h"
#include "fileitemdelegate.h"
#include "breadcrumbbar.h"
#include "platform/pathnavigation.h"
#include "homeview.h"
#include "localdirectorymodel.h"
#include "files/nameordering.h"
#include "appearancesettings.h"
#include "applicationtheme.h"
#include "preview/imagemetadataprovider.h"
#include "preview/documentmetadataprovider.h"
#include "preview/mediametadataprovider.h"
#include "preview/shellmetadataprovider.h"
#include "preview/cloudstatusprovider.h"
#include "platform/knownfolders.h"
#include "platform/shellclipboard.h"
#include "settings/shellpermissions.h"

#include <QAbstractItemView>
#include <QAccessible>
#include <QAccessibleEvent>
#include <QApplication>
#include <QDesktopServices>
#include <QDropEvent>
#include <QCompleter>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QPointer>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSizePolicy>
#include <QSortFilterProxyModel>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QTreeView>
#include <QTimer>
#include "ui/presentationtiming.h"
#include "ui/columnprofilepath.h"
#include <QThread>
#include <QUrl>

#include "platform/systemopen.h"
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

QHash<QString, QByteArray>& liveColumnProfiles()
{
    static QHash<QString, QByteArray> profiles;
    return profiles;
}

QString restorePathKey(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}

QHash<QString, QModelIndex> loadedProxyIndexes(filemanager::ui::LocalDirectoryModel* model,
    QSortFilterProxyModel* proxy, const QModelIndex& root, const QStringList& paths)
{
    QSet<QString> wanted;
    for (const QString& path : paths) {
        if (!path.isEmpty())
            wanted.insert(restorePathKey(path));
    }
    QHash<QString, QModelIndex> found;
    std::function<void(const QModelIndex&)> visit = [&](const QModelIndex& parent) {
        const int rows = proxy->rowCount(parent);
        for (int row = 0; row < rows && found.size() < wanted.size(); ++row) {
            const QModelIndex index = proxy->index(row, 0, parent);
            if (!index.isValid())
                continue;
            const QModelIndex source = proxy->mapToSource(index);
            if (!source.isValid())
                continue;
            const QString key = restorePathKey(model->filePath(source));
            if (wanted.contains(key))
                found.insert(key, index);
            // rowCount only exposes rows already published by the current-directory model. It
            // does not resolve a saved absolute path or access an unavailable location.
            if (proxy->rowCount(index) > 0)
                visit(index);
        }
    };
    visit(root);
    return found;
}

QItemSelection selectionFromLoadedIndexes(const QStringList& paths,
    const QHash<QString, QModelIndex>& loaded)
{
    QModelIndexList indexes;
    indexes.reserve(paths.size());
    for (const QString& path : paths) {
        const QModelIndex index = loaded.value(restorePathKey(path));
        if (index.isValid())
            indexes.append(index);
    }
    std::sort(indexes.begin(), indexes.end(), [](const QModelIndex& left, const QModelIndex& right) {
        if (left.parent() != right.parent())
            return left.parent() < right.parent();
        return left.row() < right.row();
    });
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    QItemSelection selection;
    for (qsizetype first = 0; first < indexes.size();) {
        qsizetype last = first;
        while (last + 1 < indexes.size() && indexes[last + 1].parent() == indexes[first].parent()
            && indexes[last + 1].row() == indexes[last].row() + 1)
            ++last;
        selection.select(indexes[first], indexes[last]);
        first = last + 1;
    }
    return selection;
}

class RichDirectoryModel final : public filemanager::ui::LocalDirectoryModel
{
public:
    explicit RichDirectoryModel(QObject* parent = nullptr)
        : LocalDirectoryModel(parent),
          imageMetadata_(new filemanager::preview::ImageMetadataProvider(this)),
          documentMetadata_(new filemanager::preview::DocumentMetadataProvider(this)),
          mediaMetadata_(new filemanager::preview::MediaMetadataProvider(this)),
          shellMetadata_(new filemanager::preview::ShellMetadataProvider(this)),
          cloudStatus_(new filemanager::preview::CloudStatusProvider(this))
    {
        connect(imageMetadata_, &filemanager::preview::ImageMetadataProvider::metadataReady,
            this, [this](const QString& path) {
                const QModelIndex item = indexForPath(path);
                if (item.isValid())
                    emit dataChanged(item.siblingAtColumn(6), item.siblingAtColumn(6),
                        {Qt::DisplayRole});
            });
        connect(documentMetadata_,
            &filemanager::preview::DocumentMetadataProvider::metadataReady,
            this, [this](const QString& path) {
                const QModelIndex item = indexForPath(path);
                if (item.isValid())
                    emit dataChanged(item.siblingAtColumn(8), item.siblingAtColumn(10),
                        {Qt::DisplayRole});
            });
        const auto refreshRichMetadata = [this](const QString& path) {
            const QModelIndex item = indexForPath(path);
            if (item.isValid())
                emit dataChanged(item.siblingAtColumn(6), item.siblingAtColumn(9),
                    {Qt::DisplayRole});
        };
        connect(mediaMetadata_, &filemanager::preview::MediaMetadataProvider::metadataReady,
            this, refreshRichMetadata);
        connect(shellMetadata_, &filemanager::preview::ShellMetadataProvider::metadataReady,
            this, refreshRichMetadata);
        connect(cloudStatus_, &filemanager::preview::CloudStatusProvider::statusReady,
            this, [this](const QString& path) {
                const QModelIndex item = indexForPath(path);
                if (item.isValid())
                    emit dataChanged(item.siblingAtColumn(11), item.siblingAtColumn(11),
                        {Qt::DisplayRole});
            });
    }

    int columnCount(const QModelIndex& parent = {}) const override
    {
        return LocalDirectoryModel::columnCount(parent) + 8;
    }

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    {
        if (index.column() < LocalDirectoryModel::columnCount(index.parent()))
            return LocalDirectoryModel::data(index, role);
        if (role != Qt::DisplayRole && role != Qt::EditRole)
            return {};
        const QFileInfo info = fileInfo(index.siblingAtColumn(0));
        if (index.column() == 4) {
            const QDateTime created = info.birthTime();
            return created.isValid() ? created : QVariant();
        }
        if (index.column() == 5)
            return info.isDir() ? QStringLiteral("Folder") : info.suffix().toLower();
        if (index.column() == 6) {
            const auto dimensions = imageMetadata_->requestDimensions(info.absoluteFilePath());
            if (dimensions)
                return QStringLiteral("%1 × %2").arg(
                    QLocale().toString(dimensions->width()),
                    QLocale().toString(dimensions->height()));
            const auto metadata = combinedMetadata(info.absoluteFilePath());
            return metadata && metadata->resolution.isValid()
                ? QStringLiteral("%1 × %2").arg(
                      QLocale().toString(metadata->resolution.width()),
                      QLocale().toString(metadata->resolution.height()))
                : QString();
        }
        if (index.column() == 11) {
            const auto status = cloudStatus_->request(info.absoluteFilePath());
            return status ? filemanager::platform::cloudFileStatusText(*status) : QString{};
        }
        const auto metadata = combinedMetadata(info.absoluteFilePath());
        const auto document = documentMetadata_->request(info.absoluteFilePath());
        if (index.column() == 7)
            return metadata ? metadata->durationText() : QString();
        if (index.column() == 8)
            return metadata && !metadata->title.isEmpty()
                ? metadata->title : (document ? document->title : QString());
        if (index.column() == 9)
            return metadata && !metadata->artists.isEmpty()
                ? metadata->artists : (document ? document->authors : QString());
        if (index.column() == 10)
            return document && document->pageCount > 0 ? document->pageCount : QVariant();
        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation,
        int role = Qt::DisplayRole) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            if (section == 4)
                return QStringLiteral("Date created");
            if (section == 5)
                return QStringLiteral("Extension");
            if (section == 6)
                return QStringLiteral("Dimensions");
            if (section == 7)
                return QStringLiteral("Duration");
            if (section == 8)
                return QStringLiteral("Title");
            if (section == 9)
                return QStringLiteral("Artists");
            if (section == 10)
                return QStringLiteral("Pages");
            if (section == 11)
                return QStringLiteral("Availability");
        }
        return LocalDirectoryModel::headerData(section, orientation, role);
    }

private:
    std::optional<filemanager::preview::MediaMetadata> combinedMetadata(const QString& path) const
    {
        const auto builtIn = mediaMetadata_->request(path);
        const auto shell = shellMetadata_->request(path);
        if (!builtIn)
            return shell;
        if (!shell)
            return builtIn;
        filemanager::preview::MediaMetadata combined = *builtIn;
        if (combined.durationMs <= 0)
            combined.durationMs = shell->durationMs;
        if (combined.title.isEmpty())
            combined.title = shell->title;
        if (combined.artists.isEmpty())
            combined.artists = shell->artists;
        if (!combined.resolution.isValid())
            combined.resolution = shell->resolution;
        return combined;
    }

    filemanager::preview::ImageMetadataProvider* imageMetadata_ = nullptr;
    filemanager::preview::DocumentMetadataProvider* documentMetadata_ = nullptr;
    filemanager::preview::MediaMetadataProvider* mediaMetadata_ = nullptr;
    filemanager::preview::ShellMetadataProvider* shellMetadata_ = nullptr;
    filemanager::preview::CloudStatusProvider* cloudStatus_ = nullptr;
};

class NameFilter final : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    using CheckStateHandler = std::function<void(const QModelIndex&, bool)>;

    void setCheckStateHandler(CheckStateHandler handler)
    {
        checkStateHandler_ = std::move(handler);
    }
    void setSelectionCheckBoxesEnabled(bool enabled)
    {
        if (selectionCheckBoxesEnabled_ == enabled)
            return;
        selectionCheckBoxesEnabled_ = enabled;
        refreshCheckRoles({});
    }
    void setSelectedPaths(const QStringList& paths)
    {
        QSet<QString> selected;
        for (const QString& path : paths)
            selected.insert(restorePathKey(path));
        if (selected == selectedPaths_)
            return;
        selectedPaths_ = std::move(selected);
        refreshCheckRoles({});
    }
    void setFolderPlacement(FilePane::FolderPlacement placement)
    {
        placement_ = placement;
        invalidate();
    }
    void setShowFileExtensions(bool enabled)
    {
        showFileExtensions_ = enabled;
        invalidate();
    }
    void setGroupBy(FilePane::GroupBy grouping)
    {
        grouping_ = grouping;
        invalidate();
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (selectionCheckBoxesEnabled_ && role == Qt::CheckStateRole
            && index.column() == 0) {
            auto* files = qobject_cast<filemanager::ui::LocalDirectoryModel*>(sourceModel());
            const QModelIndex source = mapToSource(index);
            const QString path = files && source.isValid() ? files->filePath(source) : QString{};
            return selectedPaths_.contains(restorePathKey(path)) ? Qt::Checked : Qt::Unchecked;
        }
        if (role == filemanager::ui::FileItemDelegate::GroupLabelRole) {
            auto* files = qobject_cast<filemanager::ui::LocalDirectoryModel*>(sourceModel());
            if (!files)
                return {};
            const QString label = groupLabel(
                files->fileInfo(mapToSource(index.siblingAtColumn(0))));
            return label.section(QStringLiteral(" · "), 1);
        }
        if (!showFileExtensions_ && role == Qt::DisplayRole && index.column() == 0) {
            auto* files = qobject_cast<filemanager::ui::LocalDirectoryModel*>(sourceModel());
            const QFileInfo info = files ? files->fileInfo(mapToSource(index)) : QFileInfo();
            if (info.isFile() && !info.suffix().isEmpty())
                return info.completeBaseName();
        }
        return QSortFilterProxyModel::data(index, role);
    }

    bool setData(const QModelIndex& index, const QVariant& value, int role) override
    {
        if (selectionCheckBoxesEnabled_ && role == Qt::CheckStateRole
            && index.isValid() && index.column() == 0 && checkStateHandler_) {
            checkStateHandler_(index, value.toInt() == Qt::Checked);
            return true;
        }
        return QSortFilterProxyModel::setData(index, value, role);
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        Qt::ItemFlags result = QSortFilterProxyModel::flags(index);
        if (selectionCheckBoxesEnabled_ && index.isValid() && index.column() == 0)
            result |= Qt::ItemIsUserCheckable;
        return result;
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        if (filterRegularExpression().pattern().isEmpty())
            return true;
        return sourceModel()->index(sourceRow, 0, sourceParent).data().toString().contains(
            filterRegularExpression());
    }

    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override
    {
        auto* files = qobject_cast<filemanager::ui::LocalDirectoryModel*>(sourceModel());
        if (files) {
            const QFileInfo leftInfo = files->fileInfo(left);
            const QFileInfo rightInfo = files->fileInfo(right);
            if (grouping_ != FilePane::GroupBy::None) {
                const int grouped = QString::localeAwareCompare(
                    groupLabel(leftInfo), groupLabel(rightInfo));
                if (grouped != 0)
                    return grouped < 0;
            }
            const bool leftDirectory = leftInfo.isDir();
            const bool rightDirectory = rightInfo.isDir();
            if (leftDirectory != rightDirectory && placement_ != FilePane::FolderPlacement::Mixed) {
                const bool directoryBefore = placement_ == FilePane::FolderPlacement::First;
                const bool ascendingResult = directoryBefore ? leftDirectory : !leftDirectory;
                return sortOrder() == Qt::AscendingOrder ? ascendingResult : !ascendingResult;
            }
        }
        if (left.column() == 0 && right.column() == 0)
            return filemanager::files::naturalNameCompare(
                left.data().toString(), right.data().toString()) < 0;
        if (files && left.column() == right.column()) {
            const QFileInfo leftInfo = files->fileInfo(left);
            const QFileInfo rightInfo = files->fileInfo(right);
            if (left.column() == 1)
                return leftInfo.size() < rightInfo.size();
            if (left.column() == 2)
                return QString::localeAwareCompare(leftInfo.suffix(), rightInfo.suffix()) < 0;
            if (left.column() == 3)
                return leftInfo.lastModified() < rightInfo.lastModified();
        }
        return QSortFilterProxyModel::lessThan(left, right);
    }

private:
    void refreshCheckRoles(const QModelIndex& parent)
    {
        const int rows = rowCount(parent);
        if (rows <= 0)
            return;
        emit dataChanged(index(0, 0, parent), index(rows - 1, 0, parent),
            {Qt::CheckStateRole});
        for (int row = 0; row < rows; ++row) {
            const QModelIndex child = index(row, 0, parent);
            if (rowCount(child) > 0)
                refreshCheckRoles(child);
        }
    }

    QString groupLabel(const QFileInfo& info) const
    {
        if (grouping_ == FilePane::GroupBy::None)
            return {};
        if (info.isDir())
            return QStringLiteral("0 · Folders");
        if (grouping_ == FilePane::GroupBy::Name) {
            const QString name = info.fileName().trimmed();
            if (name.isEmpty())
                return QStringLiteral("5 · Other");
            const QChar first = name.front().toUpper();
            if (first.isDigit())
                return QStringLiteral("1 · 0–9");
            if (first >= QLatin1Char('A') && first <= QLatin1Char('H'))
                return QStringLiteral("2 · A–H");
            if (first >= QLatin1Char('I') && first <= QLatin1Char('P'))
                return QStringLiteral("3 · I–P");
            if (first >= QLatin1Char('Q') && first <= QLatin1Char('Z'))
                return QStringLiteral("4 · Q–Z");
            return QStringLiteral("5 · Other");
        }
        if (grouping_ == FilePane::GroupBy::Type) {
            const QString suffix = info.suffix().toUpper();
            return suffix.isEmpty() ? QStringLiteral("1 · Files without an extension")
                                    : QStringLiteral("1 · %1 files").arg(suffix);
        }
        if (grouping_ == FilePane::GroupBy::DateModified) {
            const qint64 days = info.lastModified().date().daysTo(QDate::currentDate());
            if (days <= 0)
                return QStringLiteral("1 · Today");
            if (days == 1)
                return QStringLiteral("2 · Yesterday");
            if (days <= 7)
                return QStringLiteral("3 · Earlier this week");
            if (days <= 31)
                return QStringLiteral("4 · Earlier this month");
            return QStringLiteral("5 · Older");
        }
        const qint64 size = info.size();
        if (size < 1024 * 1024)
            return QStringLiteral("1 · Small (under 1 MB)");
        if (size < 100LL * 1024 * 1024)
            return QStringLiteral("2 · Medium (1–100 MB)");
        if (size < 1024LL * 1024 * 1024)
            return QStringLiteral("3 · Large (100 MB–1 GB)");
        return QStringLiteral("4 · Very large (over 1 GB)");
    }

    FilePane::FolderPlacement placement_ = FilePane::FolderPlacement::First;
    bool showFileExtensions_ = true;
    FilePane::GroupBy grouping_ = FilePane::GroupBy::None;
    bool selectionCheckBoxesEnabled_ = false;
    QSet<QString> selectedPaths_;
    CheckStateHandler checkStateHandler_;
};

class ShellPaneListWidget final : public QListWidget
{
public:
    using ItemFactory = std::function<QList<filemanager::platform::ShellItemReference>()>;

    explicit ShellPaneListWidget(QWidget* parent = nullptr) : QListWidget(parent) {}
    void setItemFactory(ItemFactory factory) { itemFactory_ = std::move(factory); }

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>&) const override
    {
        if (!itemFactory_)
            return nullptr;
        const auto items = itemFactory_();
        if (items.isEmpty())
            return nullptr;
        auto* mime = new QMimeData;
        QString error;
        if (!filemanager::platform::ShellClipboard::populateShellItems(mime, items, &error)) {
            delete mime;
            return nullptr;
        }
        return mime;
    }

private:
    ItemFactory itemFactory_;
};

} // namespace

FilePane::FilePane(QWidget* parent)
    : FilePane(QDir::homePath(), parent)
{
}

FilePane::FilePane(const QString& initialPath, QWidget* parent)
    : QWidget(parent)
{
    singleClickActivation_ = QSettings().value("interaction/singleClickActivation", false).toBool();
    selectionCheckBoxes_ = QSettings().value("interaction/itemSelectionCheckBoxes", false).toBool();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(5, 5, 5, 5);
    root->setSpacing(6);

    auto* navigation = new QHBoxLayout;
    backButton_ = new QPushButton(this);
    forwardButton_ = new QPushButton(this);
    upButton_ = new QPushButton(this);
    auto* homeButton = new QPushButton(this);
    backButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    forwardButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    upButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    homeButton->setIcon(style()->standardIcon(QStyle::SP_DirHomeIcon));
    const QList<QPair<QPushButton*, QString>> navigationButtons = {
        {backButton_, "Back"}, {forwardButton_, "Forward"},
        {upButton_, "Up one folder"}, {homeButton, "Home"}};
    for (const auto& [button, label] : navigationButtons) {
        button->setFixedSize(32, 32);
        button->setToolTip(label);
        button->setAccessibleName(label);
    }

    addressBar_ = new filemanager::ui::BreadcrumbBar(this);
    pathEdit_ = addressBar_->editor();
    auto* completionModel = new QFileSystemModel(pathEdit_);
    completionModel->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);
    completionModel->setRootPath({});
    auto* pathCompleter = new QCompleter(completionModel, pathEdit_);
    pathCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    pathCompleter->setCompletionMode(QCompleter::PopupCompletion);
    pathEdit_->setCompleter(pathCompleter);
    navigation->addWidget(backButton_);
    navigation->addWidget(forwardButton_);
    navigation->addWidget(upButton_);
    navigation->addWidget(homeButton);
    navigation->addWidget(addressBar_, 1);
    root->addLayout(navigation);

    filterEdit_ = new QLineEdit(this);
    filterEdit_->setAccessibleName("Filter current folder");
    filterEdit_->setPlaceholderText("Filter this folder (Ctrl+F)");
    filterEdit_->setClearButtonEnabled(true);
    root->addWidget(filterEdit_);

    model_ = new RichDirectoryModel(this);
    model_->setFilters(QDir::AllEntries | QDir::NoDotAndDotDot);
    connect(model_, &filemanager::ui::LocalDirectoryModel::directoryLoaded, this, [this] {
        applyPendingReveal();
        updateEmptyState();
        if (restorePending_)
            applyPendingRestoreState();
    });
    connect(model_, &filemanager::ui::LocalDirectoryModel::directoryLoadFailed, this,
        [this](const QString& path, const QString& error) {
            if (QFileInfo(path).absoluteFilePath() != QFileInfo(currentPath()).absoluteFilePath())
                return;
            pathEdit_->setStyleSheet("border-color: #e05d6f");
            pathEdit_->setToolTip(error);
            pathEdit_->setAccessibleDescription(QStringLiteral("Error: ") + error);
            emptyState_->setText(QStringLiteral("This folder could not be read.\n\n%1").arg(error));
            emptyState_->show();
        });

    connect(model_, &filemanager::ui::LocalDirectoryModel::directoryMonitoringChanged, this,
        [this](const QString& path, bool available) {
            if (QFileInfo(path).absoluteFilePath() != QFileInfo(currentPath()).absoluteFilePath())
                return;
            const QString warning = tr("Automatic folder updates are unavailable. Refresh to retry.");
            if (!available) {
                pathEdit_->setToolTip(warning);
                pathEdit_->setAccessibleDescription(warning);
            } else if (pathEdit_->toolTip() == warning) {
                pathEdit_->setToolTip(QString());
                pathEdit_->setAccessibleDescription(QString());
            }
        });

    proxy_ = new NameFilter(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    static_cast<NameFilter*>(proxy_)->setCheckStateHandler(
        [this](const QModelIndex& index, bool checked) {
            if (!index.isValid() || index.model() != proxy_)
                return;
            QAbstractItemView* currentView = view();
            if (!currentView || !currentView->selectionModel())
                return;
            currentView->selectionModel()->select(index,
                (checked ? QItemSelectionModel::Select : QItemSelectionModel::Deselect)
                    | QItemSelectionModel::Rows);
            currentView->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        });
    connect(proxy_, &QAbstractItemModel::rowsInserted, this, [this] {
        applyPendingReveal();
        emit itemCountChanged();
        updateEmptyState();
    });
    connect(proxy_, &QAbstractItemModel::rowsRemoved, this,
        [this] { emit itemCountChanged(); updateEmptyState(); });
    connect(proxy_, &QAbstractItemModel::modelReset, this,
        [this] { emit itemCountChanged(); updateEmptyState(); });

    view_ = new QTreeView(this);
    view_->setObjectName("FolderDetailsView");
    view_->setAccessibleName("Items in the current folder");
    auto* delegate = new filemanager::ui::FileItemDelegate(view_);
    view_->setItemDelegate(delegate);
    connect(delegate, &filemanager::ui::FileItemDelegate::thumbnailReady,
        view_->viewport(), [this] { view_->viewport()->update(); });
    connect(model_, &filemanager::ui::LocalDirectoryModel::iconsChanged,
        view_->viewport(), [this] { view_->viewport()->update(); });
    view_->setModel(proxy_);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setAlternatingRowColors(true);
    view_->setSortingEnabled(true);
    view_->sortByColumn(0, Qt::AscendingOrder);
    view_->setAnimated(filemanager::ui::applicationAnimationsEnabled());
    view_->setUniformRowHeights(true);
    view_->setIndentation(0);
    view_->setDragEnabled(true);
    view_->setAcceptDrops(true);
    view_->setDropIndicatorShown(true);
    view_->setDragDropMode(QAbstractItemView::DragDrop);
    view_->setDefaultDropAction(Qt::CopyAction);
    view_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->header()->setObjectName("FolderDetailsHeader");
    view_->header()->setAccessibleName("Folder detail columns");
    view_->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->installEventFilter(this);
    view_->viewport()->installEventFilter(this);
    iconView_ = new QListView(this);
    iconView_->setObjectName("FolderIconView");
    iconView_->setAccessibleName("Items in the current folder");
    iconView_->setModel(proxy_);
    auto* iconDelegate = new filemanager::ui::FileItemDelegate(iconView_);
    iconView_->setItemDelegate(iconDelegate);
    connect(iconDelegate, &filemanager::ui::FileItemDelegate::thumbnailReady,
        iconView_->viewport(), [this] { iconView_->viewport()->update(); });
    connect(model_, &filemanager::ui::LocalDirectoryModel::iconsChanged,
        iconView_->viewport(), [this] { iconView_->viewport()->update(); });
    iconView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    iconView_->setDragEnabled(true);
    iconView_->setAcceptDrops(true);
    iconView_->setDropIndicatorShown(true);
    iconView_->setDragDropMode(QAbstractItemView::DragDrop);
    iconView_->setDefaultDropAction(Qt::CopyAction);
    iconView_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    iconView_->setContextMenuPolicy(Qt::CustomContextMenu);
    iconView_->installEventFilter(this);
    iconView_->viewport()->installEventFilter(this);
    iconView_->setResizeMode(QListView::Adjust);
    iconView_->setUniformItemSizes(true);
    viewStack_ = new QStackedWidget(this);
    viewStack_->addWidget(view_);
    viewStack_->addWidget(iconView_);
    shellPage_ = new QWidget(viewStack_);
    auto* shellLayout = new QVBoxLayout(shellPage_);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    auto* shellList = new ShellPaneListWidget(shellPage_);
    shellView_ = shellList;
    shellView_->setObjectName("ShellNamespacePaneItems");
    shellView_->setAccessibleName("Items in the current Windows Shell location");
    shellView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    shellView_->setDragEnabled(true);
    shellView_->setDragDropMode(QAbstractItemView::DragOnly);
    shellView_->setDefaultDropAction(Qt::CopyAction);
    shellView_->installEventFilter(this);
    shellList->setItemFactory([this] { return selectedShellItems(); });
    shellStatus_ = new QLabel(shellPage_);
    shellStatus_->setObjectName("Muted");
    shellStatus_->setAccessibleName("Windows Shell location status");
    shellStatus_->setWordWrap(true);
    shellConsent_ = new QWidget(shellPage_);
    shellConsent_->setObjectName("ShellVisitConsent");
    auto* consentLayout = new QVBoxLayout(shellConsent_);
    auto* disclosure = new QLabel(
        "Browsing this location and its children may run installed Windows providers with your "
        "Windows access. Allow for this visit only, or open it in Windows. This permission does "
        "not restrict which files a provider can access.", shellConsent_);
    disclosure->setWordWrap(true);
    disclosure->setTextFormat(Qt::PlainText);
    consentLayout->addWidget(disclosure);
    auto* consentActions = new QHBoxLayout;
    auto* allowVisit = new QPushButton("Allow for this visit", shellConsent_);
    allowVisit->setObjectName("AllowShellVisit");
    allowVisit->setAccessibleName(allowVisit->text());
    auto* openWindows = new QPushButton("Open in Windows", shellConsent_);
    openWindows->setObjectName("OpenShellInWindows");
    openWindows->setAccessibleName(openWindows->text());
    auto* cancelVisit = new QPushButton("Cancel", shellConsent_);
    cancelVisit->setObjectName("CancelShellVisit");
    cancelVisit->setAccessibleName(cancelVisit->text());
    consentActions->addWidget(allowVisit);
    consentActions->addWidget(openWindows);
    consentActions->addWidget(cancelVisit);
    consentLayout->addLayout(consentActions);
    shellEndVisit_ = new QPushButton("End this visit", shellPage_);
    shellEndVisit_->setObjectName("EndShellVisit");
    shellEndVisit_->setAccessibleName(shellEndVisit_->text());
    // Keep consent and status above the flexible item list. A short split pane or
    // a 1024x768 desktop must not place the permission choices below the window.
    shellLayout->addWidget(shellConsent_);
    shellLayout->addWidget(shellEndVisit_);
    shellLayout->addWidget(shellStatus_);
    shellView_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    shellView_->setMinimumHeight(80);
    shellLayout->addWidget(shellView_, 1);
    connect(allowVisit, &QPushButton::clicked, this, [this] {
        if (homeLocation_ || !shellLocation_.valid || hasShellAccess())
            return;
        temporaryShellVisit_ = true;
        shellVisitRoot_ = shellLocation_.idList;
        loadShellLocation();
    });
    connect(openWindows, &QPushButton::clicked, this, [this] {
        const QString target = builtInShellLocation_ == filemanager::platform::BuiltInShellLocation::None
            ? shellLocation_.parsingName
            : filemanager::platform::ShellItem::builtInParsingName(builtInShellLocation_);
        endShellVisit();
        if (!target.isEmpty())
            emit shellItemOpenRequested(target);
    });
    connect(cancelVisit, &QPushButton::clicked, this, [this] { navigateHome(); });
    connect(shellEndVisit_, &QPushButton::clicked, this, &FilePane::endShellVisit);
    connect(filemanager::settings::shellPermissionNotifier(),
        &filemanager::settings::ShellPermissionNotifier::changed, this, &FilePane::endShellVisit);
    viewStack_->addWidget(shellPage_);
    homeView_ = new filemanager::ui::HomeView(viewStack_);
    viewStack_->addWidget(homeView_);
    viewStack_->installEventFilter(this);
    emptyState_ = new QLabel(viewStack_);
    emptyState_->setObjectName("EmptyFolderGuidance");
    emptyState_->setAccessibleName("Empty folder guidance");
    emptyState_->setAlignment(Qt::AlignCenter);
    emptyState_->setWordWrap(true);
    emptyState_->setTextFormat(Qt::PlainText);
    emptyState_->setAttribute(Qt::WA_TransparentForMouseEvents);
    emptyState_->hide();
    root->addWidget(viewStack_, 1);

    connect(backButton_, &QPushButton::clicked, this, &FilePane::goBack);
    connect(forwardButton_, &QPushButton::clicked, this, &FilePane::goForward);
    backButton_->setContextMenuPolicy(Qt::CustomContextMenu);
    forwardButton_->setContextMenuPolicy(Qt::CustomContextMenu);
    backButton_->setToolTip("Back (right-click for history)");
    forwardButton_->setToolTip("Forward (right-click for history)");
    connect(backButton_, &QPushButton::customContextMenuRequested, this,
        [this](const QPoint&) { showHistoryMenu(backButton_, true); });
    connect(forwardButton_, &QPushButton::customContextMenuRequested, this,
        [this](const QPoint&) { showHistoryMenu(forwardButton_, false); });
    connect(upButton_, &QPushButton::clicked, this, &FilePane::goUp);
    connect(homeButton, &QPushButton::clicked, this, [this] { navigateHome(); });
    connect(addressBar_, &filemanager::ui::BreadcrumbBar::pathSubmitted,
        this, [this](const QString& path) { navigate(path); });
    connect(addressBar_, &filemanager::ui::BreadcrumbBar::pathActivated,
        this, [this](const QString& path) { navigate(path); });
    connect(addressBar_, &filemanager::ui::BreadcrumbBar::backgroundPathRequested,
        this, &FilePane::backgroundTabRequested);
    connect(homeView_, &filemanager::ui::HomeView::pathActivated,
        this, [this](const QString& path) { navigate(path); });
    connect(homeView_, &filemanager::ui::HomeView::backgroundPathRequested,
        this, &FilePane::backgroundTabRequested);
    connect(homeView_, &filemanager::ui::HomeView::savedSearchActivated,
        this, &FilePane::savedSearchRequested);
    connect(homeView_, &filemanager::ui::HomeView::selectionChanged, this, [this] {
        emit activated(this);
        emit selectionChanged(selectedPath());
    });
    connect(filterEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        proxy_->setFilterRegularExpression(QRegularExpression(
            QRegularExpression::escape(text), QRegularExpression::CaseInsensitiveOption));
        updateEmptyState();
        QTimer::singleShot(0, this, &FilePane::updateEmptyState);
    });
    connect(view_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!singleClickActivation_)
            activateIndex(index);
    });
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        updateSelectionOrder(view_);
        updateSelectionCheckState(view_);
        emit activated(this);
        emit selectionChanged(selectedPath());
    });
    connect(iconView_, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!singleClickActivation_)
            activateIndex(index);
    });
    connect(iconView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        updateSelectionOrder(iconView_);
        updateSelectionCheckState(iconView_);
        emit activated(this);
        emit selectionChanged(selectedPath());
    });
    connect(shellView_, &QListWidget::itemDoubleClicked, this,
        [this](QListWidgetItem* item) {
            if (!singleClickActivation_ && item)
                activateShellRow(item->data(Qt::UserRole).toInt());
        });
    connect(shellView_, &QListWidget::itemSelectionChanged, this, [this] {
        if (selectionCheckBoxes_ && !updatingShellChecks_) {
            updatingShellChecks_ = true;
            for (int row = 0; row < shellView_->count(); ++row) {
                QListWidgetItem* item = shellView_->item(row);
                item->setCheckState(item->isSelected() ? Qt::Checked : Qt::Unchecked);
            }
            updatingShellChecks_ = false;
        }
        emit activated(this);
        emit selectionChanged({});
    });
    connect(shellView_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (!selectionCheckBoxes_ || updatingShellChecks_ || !item)
            return;
        item->setSelected(item->checkState() == Qt::Checked);
        shellView_->setCurrentItem(item, QItemSelectionModel::NoUpdate);
    });
    connect(view_, &QTreeView::customContextMenuRequested, this,
        [this](const QPoint& point) { emit contextMenuRequested(this, point); });
    connect(iconView_, &QListView::customContextMenuRequested, this,
        [this](const QPoint& point) { emit contextMenuRequested(this, point); });
    connect(view_->header(), &QHeaderView::customContextMenuRequested, this,
        [this](const QPoint& point) {
            emit columnHeaderMenuRequested(this, view_->header()->mapToGlobal(point));
        });

    resetDefaultColumns();
    columnSaveTimer_ = new QTimer(this);
    columnSaveTimer_->setSingleShot(true);
    columnSaveTimer_->setInterval(300);
    connect(columnSaveTimer_, &QTimer::timeout, this,
        [this] { saveColumnProfile(currentPath()); });
    restoreTimer_ = new QTimer(this);
    restoreTimer_->setInterval(50);
    connect(restoreTimer_, &QTimer::timeout, this, [this] {
        if (restorePending_)
            ++restoreAttempts_;
        applyPendingRestoreState();
    });
    connect(view_->header(), &QHeaderView::sectionResized, this,
        [this](int, int, int) {
            if (applyingColumnProfile_)
                return;
            cacheColumnProfile(currentPath());
            if (!columnSaveTimer_->isActive())
                columnSaveTimer_->start();
        });
    connect(view_->header(), &QHeaderView::sectionMoved, this,
        [this](int, int, int) {
            if (applyingColumnProfile_)
                return;
            cacheColumnProfile(currentPath());
            if (!columnSaveTimer_->isActive())
                columnSaveTimer_->start();
        });
    connect(view_->header(), &QHeaderView::sortIndicatorChanged, this,
        [this](int, Qt::SortOrder) {
            if (applyingColumnProfile_)
                return;
            cacheColumnProfile(currentPath());
            if (!columnSaveTimer_->isActive())
                columnSaveTimer_->start();
            emit viewStateChanged();
        });
    static_cast<NameFilter*>(proxy_)->setSelectionCheckBoxesEnabled(selectionCheckBoxes_);
    updateSelectionAccessibility();
    if (initialPath == QLatin1String(HomeIdentity))
        navigateHome();
    else if (const auto builtIn
            = filemanager::platform::ShellItem::builtInLocationFromIdentity(initialPath);
        builtIn != filemanager::platform::BuiltInShellLocation::None)
        navigateBuiltInShell(builtIn);
    else
        navigate(QFileInfo(initialPath).isDir() ? initialPath : QDir::homePath());
}

FilePane::~FilePane()
{
    cancelShellWork();
    if (columnSaveTimer_ && columnSaveTimer_->isActive())
        saveColumnProfile(currentPath());
}

QString FilePane::currentPath() const
{
    return isFileSystemLocation() && addressBar_ ? addressBar_->path() : QString{};
}

QString FilePane::currentLocation() const
{
    if (homeLocation_)
        return QStringLiteral("Home");
    return addressBar_ ? addressBar_->path() : QString{};
}

QString FilePane::serializedLocation() const
{
    if (homeLocation_)
        return QLatin1String(HomeIdentity);
    if (builtInShellLocation_ != filemanager::platform::BuiltInShellLocation::None)
        return filemanager::platform::ShellItem::builtInIdentity(builtInShellLocation_);
    return isFileSystemLocation() ? currentPath() : QString{};
}

QAbstractItemView* FilePane::view() const
{
    if (!isFileSystemLocation())
        return shellView_;
    return viewMode_ == ViewMode::Details
        ? static_cast<QAbstractItemView*>(view_)
        : static_cast<QAbstractItemView*>(iconView_);
}

QWidget* FilePane::contentFocusTarget() const
{
    if (homeLocation_)
        return homeView_ ? homeView_->focusTarget() : nullptr;
    if (builtInShellLocation_ != filemanager::platform::BuiltInShellLocation::None
            || shellLocation_.valid) {
        if (!shellView_->isEnabled() && shellConsent_->isVisible())
            return shellConsent_->findChild<QPushButton*>("AllowShellVisit");
        return shellView_;
    }
    return view();
}

bool FilePane::hasPathAutocomplete() const
{
    return pathEdit_->completer() != nullptr;
}

FilePaneRestoreState FilePane::restoreState() const
{
    FilePaneRestoreState state;
    if (!isFileSystemLocation())
        return state;
    state.selectedPaths = selectedPaths();
    if (state.selectedPaths.size() > 256)
        state.selectedPaths.resize(256);
    const QModelIndex current = view()->currentIndex();
    if (current.isValid() && current.model() == proxy_) {
        const QModelIndex source = proxy_->mapToSource(current);
        if (source.isValid())
            state.focusedPath = model_->filePath(source);
    }
    state.verticalScroll = qMax(0, view()->verticalScrollBar()->value());
    state.horizontalScroll = qMax(0, view()->horizontalScrollBar()->value());
    std::function<void(const QModelIndex&)> collectExpanded = [&](const QModelIndex& parent) {
        if (state.expandedPaths.size() >= 128)
            return;
        for (int row = 0; row < proxy_->rowCount(parent); ++row) {
            const QModelIndex index = proxy_->index(row, 0, parent);
            if (!index.isValid() || !view_->isExpanded(index))
                continue;
            state.expandedPaths.append(model_->filePath(proxy_->mapToSource(index)));
            collectExpanded(index);
            if (state.expandedPaths.size() >= 128)
                return;
        }
    };
    if (viewMode_ == ViewMode::Details)
        collectExpanded(view_->rootIndex());
    return state;
}

void FilePane::restoreViewState(const FilePaneRestoreState& state)
{
    if (!isFileSystemLocation())
        return;
    FilePaneRestoreState bounded = state;
    if (bounded.selectedPaths.size() > 256)
        bounded.selectedPaths.resize(256);
    if (bounded.expandedPaths.size() > 128)
        bounded.expandedPaths.resize(128);
    bounded.verticalScroll = qMax(0, bounded.verticalScroll);
    bounded.horizontalScroll = qMax(0, bounded.horizontalScroll);
    pendingRestoreState_ = bounded;
    // Count the immediate attempt and timed retries only. Directory-loaded
    // notifications must not exhaust the retry window before pending rows arrive.
    restoreAttempts_ = 1;
    restorePending_ = true;
    restoreTimer_->start();
    applyPendingRestoreState();
}

void FilePane::applyPendingRestoreState()
{
    const filemanager::ui::diagnostics::PresentationScope timing(
        filemanager::ui::diagnostics::PresentationStage::PaneRestore);
    if (!restorePending_ || !isFileSystemLocation()) {
        restoreTimer_->stop();
        return;
    }
    QStringList targets = pendingRestoreState_.selectedPaths;
    targets.append(pendingRestoreState_.expandedPaths);
    targets.append(pendingRestoreState_.focusedPath);
    auto loaded = loadedProxyIndexes(model_, proxy_, view_->rootIndex(), targets);
    bool expandedReady = true;
    if (viewMode_ == ViewMode::Details) {
        QStringList expanded = pendingRestoreState_.expandedPaths;
        std::sort(expanded.begin(), expanded.end(), [](const QString& left, const QString& right) {
            const auto depth = [](const QString& path) {
                return path.count('/') + path.count('\\');
            };
            return depth(left) < depth(right);
        });
        for (const QString& path : expanded) {
            const QModelIndex index = loaded.value(restorePathKey(path));
            if (!index.isValid()) {
                expandedReady = false;
                continue;
            }
            view_->expand(index);
        }
        // Expansion may expose already-cached children immediately. Rebuild from loaded rows only.
        loaded = loadedProxyIndexes(model_, proxy_, view_->rootIndex(), targets);
        for (const QString& path : expanded)
            expandedReady = expandedReady && loaded.contains(restorePathKey(path));
    }
    selectionOrder_ = pendingRestoreState_.selectedPaths;
    view()->selectionModel()->select(
        selectionFromLoadedIndexes(pendingRestoreState_.selectedPaths, loaded),
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const QModelIndex focused = loaded.value(restorePathKey(pendingRestoreState_.focusedPath));
    if (focused.isValid())
        view()->selectionModel()->setCurrentIndex(focused, QItemSelectionModel::NoUpdate);
    view()->verticalScrollBar()->setValue(pendingRestoreState_.verticalScroll);
    view()->horizontalScrollBar()->setValue(pendingRestoreState_.horizontalScroll);

    QSet<QString> selectedTargets;
    for (const QString& path : pendingRestoreState_.selectedPaths)
        selectedTargets.insert(restorePathKey(path));
    bool selectionReady = true;
    for (const QString& key : selectedTargets)
        selectionReady = selectionReady && loaded.contains(key);
    const bool focusReady = pendingRestoreState_.focusedPath.isEmpty() || focused.isValid();
    if ((selectionReady && expandedReady && focusReady) || restoreAttempts_ >= 40) {
        restorePending_ = false;
        restoreTimer_->stop();
    }
}

QString FilePane::selectedPath() const
{
    if (homeLocation_)
        return homeView_ ? homeView_->selectedPath() : QString{};
    const QModelIndex index = view()->currentIndex();
    if (!index.isValid() || index.model() != proxy_)
        return {};
    const QModelIndex source = proxy_->mapToSource(index);
    return source.isValid() ? model_->filePath(source) : QString();
}

QStringList FilePane::selectedPaths() const
{
    if (homeLocation_) {
        const QString path = homeView_ ? homeView_->selectedPath() : QString{};
        return path.isEmpty() ? QStringList{} : QStringList{path};
    }
    QStringList result;
    const QItemSelectionModel* selection = view()->selectionModel();
    if (!selection)
        return result;
    const QModelIndexList selectedRows = selection->selectedRows(0);
    for (const QModelIndex& index : selectedRows) {
        if (!index.isValid() || index.model() != proxy_)
            continue;
        const QModelIndex source = proxy_->mapToSource(index);
        if (source.isValid())
            result.append(model_->filePath(source));
    }
    return result;
}

QStringList FilePane::selectedPathsInSelectionOrder() const
{
    const QStringList selected = selectedPaths();
    QSet<QString> remaining(selected.cbegin(), selected.cend());
    QStringList result;
    result.reserve(selected.size());
    for (const auto& path : selectionOrder_) {
        if (remaining.remove(path))
            result.append(path);
    }
    for (const auto& path : selected) {
        if (remaining.remove(path))
            result.append(path);
    }
    return result;
}

void FilePane::updateSelectionOrder(QAbstractItemView* sourceView)
{
    if (!isFileSystemLocation() || sourceView != view())
        return;
    const QStringList selected = selectedPaths();
    const QSet<QString> selectedSet(selected.cbegin(), selected.cend());
    selectionOrder_.removeIf([&](const QString& path) { return !selectedSet.contains(path); });
    QSet<QString> tracked(selectionOrder_.cbegin(), selectionOrder_.cend());
    auto rows = sourceView->selectionModel()->selectedRows(0);
    std::sort(rows.begin(), rows.end(), [](const QModelIndex& left, const QModelIndex& right) {
        return left.row() < right.row();
    });
    for (const auto& index : rows) {
        const QString path = model_->filePath(proxy_->mapToSource(index));
        if (!tracked.contains(path)) {
            selectionOrder_.append(path);
            tracked.insert(path);
        }
    }
}

void FilePane::selectPathsInOrder(const QStringList& paths)
{
    const auto loaded = loadedProxyIndexes(model_, proxy_, view_->rootIndex(), paths);
    // Keep click chronology while Qt restores ranges in one notification. Expanded
    // tree children have different parents and must never share a selection range.
    selectionOrder_ = paths;
    view()->selectionModel()->select(selectionFromLoadedIndexes(paths, loaded),
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

QList<filemanager::platform::ShellItemReference> FilePane::selectedShellItems() const
{
    QList<filemanager::platform::ShellItemReference> result;
    if (isFileSystemLocation() || homeLocation_ || !shellView_ || safeThisPc_ || !hasShellAccess())
        return result;
    const auto selected = shellView_->selectedItems();
    for (const QListWidgetItem* row : selected) {
        const int index = row->data(Qt::UserRole).toInt();
        if (index >= 0 && index < shellItems_.size())
            result.append(shellItems_.at(index));
        if (result.size() >= 256)
            break;
    }
    return result;
}

int FilePane::selectedItemCount() const
{
    if (safeThisPc_ && !isFileSystemLocation() && !homeLocation_)
        return shellView_->selectedItems().size();
    if (homeLocation_)
        return homeView_ && (!homeView_->selectedPath().isEmpty()
            || !homeView_->selectedSavedSearchId().isEmpty()) ? 1 : 0;
    return isFileSystemLocation() ? selectedPaths().size() : selectedShellItems().size();
}

int FilePane::visibleItemCount() const
{
    if (homeLocation_)
        return homeView_ ? homeView_->itemCount() : 0;
    if (!isFileSystemLocation())
        return safeThisPc_ ? safeThisPcPaths_.size() : shellItems_.size();
    return proxy_->rowCount(view()->rootIndex());
}

void FilePane::navigate(const QString& rawPath, bool remember)
{
    pendingRevealPath_.clear();
    const QString normalized = filemanager::ui::BreadcrumbBar::normalizeAddressInput(rawPath);
    const QString path = normalized.isEmpty()
        ? QString{} : QDir::cleanPath(QDir::fromNativeSeparators(normalized));
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        addressBar_->beginEditing();
        pathEdit_->setStyleSheet("border-color: #e05d6f");
        pathEdit_->setToolTip("Folder path is unavailable or is not a folder.");
        pathEdit_->setAccessibleDescription(
            "Error: folder path is unavailable or is not a folder.");
        return;
    }

    restorePending_ = false;
    if (restoreTimer_)
        restoreTimer_->stop();

    pathEdit_->setStyleSheet({});
    pathEdit_->setToolTip({});
    pathEdit_->setAccessibleDescription({});
    const QString absolutePath = info.absoluteFilePath();
    cancelShellWork();
    temporaryShellVisit_ = false;
    shellVisitRoot_.clear();
    safeThisPc_ = false;
    shellItems_.clear();
    safeThisPcPaths_.clear();
    shellView_->clear();
    const QString previousPath = currentPath();
    const QString nextProfile = columnProfileForPath(absolutePath);
    if (!previousPath.isEmpty()
        && displayedColumnProfileForPath(previousPath) != nextProfile)
        saveColumnProfile(previousPath);
    if (remember && history_.value(historyIndex_) != absolutePath) {
        while (history_.size() > historyIndex_ + 1) {
            history_.removeLast();
            if (!shellHistory_.isEmpty())
                shellHistory_.removeLast();
        }
        history_.append(absolutePath);
        shellHistory_.append(filemanager::platform::ShellItemReference{});
        historyIndex_ = history_.size() - 1;
    }

    homeLocation_ = false;
    shellLocation_ = {};
    builtInShellLocation_ = filemanager::platform::BuiltInShellLocation::None;
    ++shellGeneration_;
    addressBar_->setPath(absolutePath);
    model_->setDirectory(absolutePath);
    view_->setRootIndex({});
    iconView_->setRootIndex({});
    filterEdit_->clear();
    filterEdit_->setEnabled(true);
    viewStack_->setCurrentWidget(viewMode_ == ViewMode::Details
        ? static_cast<QWidget*>(view_) : static_cast<QWidget*>(iconView_));
    emptyState_->show();
    applyColumnProfile(absolutePath, nextProfile);
    updateNavigationActions();
    emit pathChanged(absolutePath);
    emit locationChanged(absolutePath);
    QTimer::singleShot(0, this, &FilePane::updateEmptyState);
    QTimer::singleShot(150, this, &FilePane::updateEmptyState);
}

void FilePane::navigateHome(bool remember)
{
    cancelShellWork();
    temporaryShellVisit_ = false;
    shellVisitRoot_.clear();
    safeThisPc_ = false;
    shellItems_.clear();
    safeThisPcPaths_.clear();
    shellView_->clear();
    pendingRevealPath_.clear();
    restorePending_ = false;
    if (restoreTimer_)
        restoreTimer_->stop();
    const QString identity = QLatin1String(HomeIdentity);
    if (remember && history_.value(historyIndex_) != identity) {
        while (history_.size() > historyIndex_ + 1) {
            history_.removeLast();
            if (!shellHistory_.isEmpty())
                shellHistory_.removeLast();
        }
        history_.append(identity);
        shellHistory_.append(filemanager::platform::ShellItemReference{});
        historyIndex_ = history_.size() - 1;
    }
    ++shellGeneration_;
    shellLocation_ = {};
    builtInShellLocation_ = filemanager::platform::BuiltInShellLocation::None;
    homeLocation_ = true;
    pathEdit_->setStyleSheet({});
    pathEdit_->setToolTip(
        "Local File Manager Home; filesystem-only commands are unavailable until a folder opens.");
    pathEdit_->setAccessibleDescription(pathEdit_->toolTip());
    addressBar_->setShellLocation(QStringLiteral("Home"), identity);
    filterEdit_->clear();
    filterEdit_->setEnabled(false);
    homeView_->refresh();
    viewStack_->setCurrentWidget(homeView_);
    emptyState_->hide();
    updateNavigationActions();
    emit selectionChanged(selectedPath());
    emit locationChanged(QStringLiteral("Home"));
    emit itemCountChanged();
}

void FilePane::navigateShellItem(
    const filemanager::platform::ShellItemReference& folder, bool remember)
{
    navigateShellItemInternal(folder, remember, folder.parsingName.trimmed(),
        filemanager::platform::BuiltInShellLocation::None);
}

QList<filemanager::platform::ShellItemReference> FilePane::shellNavigationItems() const
{
    using filemanager::platform::ShellItem;
    if (homeLocation_ || isFileSystemLocation() || safeThisPc_ || !hasShellAccess()
            || !shellView_->isEnabled() || !ShellItem::isValidSerializedIdentity(shellLocation_.idList))
        return {};
    QList<filemanager::platform::ShellItemReference> result{shellLocation_};
    for (const auto& item : shellItems_) {
        if (item.valid && item.folder && ShellItem::isValidSerializedIdentity(item.idList))
            result.append(item);
    }
    return result;
}

bool FilePane::navigateShellVisitItem(const QByteArray& identity)
{
    const auto snapshot = shellNavigationItems();
    for (const auto& item : snapshot) {
        if (item.idList != identity)
            continue;
        if (identity == shellLocation_.idList)
            return true;
        navigateShellItemInternal(item, true, item.parsingName,
            filemanager::platform::BuiltInShellLocation::None, true);
        return true;
    }
    return false;
}

bool FilePane::navigateBuiltInShell(
    filemanager::platform::BuiltInShellLocation location, bool remember)
{
    if (location == filemanager::platform::BuiltInShellLocation::None)
        return false;
    // A pending destination is not a resolved PIDL. Do not bind a provider to display consent.
    filemanager::platform::ShellItemReference folder;
    folder.valid = true;
    folder.folder = true;
    folder.parsingName = filemanager::platform::ShellItem::builtInParsingName(location);
    folder.displayName = filemanager::platform::ShellItem::builtInDisplayName(location);
    navigateShellItemInternal(folder, remember,
        filemanager::platform::ShellItem::builtInIdentity(location), location);
    return builtInShellLocation_ == location;
}

void FilePane::navigateShellParsingName(const QString& parsingName)
{
    if (parsingName.trimmed().isEmpty() || parsingName.size() > 8192 || parsingName.contains(QChar::Null))
        return;
    filemanager::platform::ShellItemReference pending;
    pending.valid = true;
    pending.folder = true;
    pending.parsingName = parsingName.trimmed();
    pending.displayName = pending.parsingName;
    navigateShellItem(pending);
}

void FilePane::navigateShellItemInternal(
    const filemanager::platform::ShellItemReference& folder, bool remember,
    const QString& requestedIdentity,
    filemanager::platform::BuiltInShellLocation builtInLocation, bool continueVisit)
{
    pendingRevealPath_.clear();
    if (!folder.valid || !folder.folder)
        return;
    if (builtInLocation == filemanager::platform::BuiltInShellLocation::None
        && folder.fileSystem && QFileInfo(folder.fileSystemPath).isDir()) {
        navigate(folder.fileSystemPath, remember);
        return;
    }
    const QString identity = requestedIdentity.trimmed();
    if (identity.isEmpty())
        return;
    cancelShellWork();
    if (!continueVisit || shellVisitRoot_.isEmpty()
            || !filemanager::platform::ShellItem::isValidSerializedIdentity(folder.idList)
            || !folder.idList.startsWith(shellVisitRoot_.left(shellVisitRoot_.size() - 2))) {
        temporaryShellVisit_ = false;
        shellVisitRoot_.clear();
    }
    safeThisPc_ = false;
    restorePending_ = false;
    if (restoreTimer_)
        restoreTimer_->stop();
    if (remember && history_.value(historyIndex_) != identity) {
        while (history_.size() > historyIndex_ + 1) {
            history_.removeLast();
            if (!shellHistory_.isEmpty())
                shellHistory_.removeLast();
        }
        history_.append(identity);
        shellHistory_.append(folder);
        historyIndex_ = history_.size() - 1;
    }
    homeLocation_ = false;
    shellLocation_ = folder;
    builtInShellLocation_ = builtInLocation;
    pathEdit_->setStyleSheet({});
    pathEdit_->setToolTip("Windows Shell location; filesystem-only commands are unavailable here.");
    pathEdit_->setAccessibleDescription(pathEdit_->toolTip());
    addressBar_->setShellLocation(folder.displayName, identity);
    filterEdit_->clear();
    filterEdit_->setEnabled(false);
    shellItems_.clear();
    shellView_->clear();
    shellView_->setEnabled(false);
    shellStatus_->setText("Loading this Windows location in the isolated Shell host…");
    viewStack_->setCurrentWidget(shellPage_);
    emptyState_->hide();
    updateNavigationActions();
    emit selectionChanged({});
    emit locationChanged(folder.displayName.isEmpty() ? identity : folder.displayName);
    emit itemCountChanged();
    loadShellLocation();
}

void FilePane::loadShellLocation()
{
    if (!shellLocation_.valid)
        return;
    cancelShellWork();
    safeThisPc_ = false;
    updateShellConsent();
    if (!hasShellAccess()) {
        if (builtInShellLocation_ == filemanager::platform::BuiltInShellLocation::ThisPc) {
            loadSafeThisPc();
            return;
        }
        shellItems_.clear();
        shellView_->clear();
        shellView_->setEnabled(false);
        shellStatus_->setText("Choose how to open this Windows location.");
        updateNavigationActions();
        emit selectionChanged({});
        emit itemCountChanged();
        emit shellLoadFinished(false, false, "Permission is required for this virtual view.");
        return;
    }
    shellItems_.clear();
    shellView_->clear();
    shellView_->setEnabled(false);
    shellView_->setDragEnabled(true);
    shellStatus_->setText("Loading this Windows location…");
    emit itemCountChanged();
    const auto folder = shellLocation_;
    const auto builtIn = builtInShellLocation_;
    const quint64 generation = shellGeneration_;
    shellCancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = shellCancelled_;
    QPointer<FilePane> guard(this);
    emit shellProviderRequestStarted();
    QThread* worker = QThread::create([guard, folder, builtIn, generation, cancelled] {
        auto resolved = folder;
        if (!filemanager::platform::ShellItem::isValidSerializedIdentity(resolved.idList))
            resolved = filemanager::platform::ShellItem::resolveIsolated(
                builtIn == filemanager::platform::BuiltInShellLocation::None ? folder.parsingName
                    : filemanager::platform::ShellItem::builtInIdentity(builtIn), 5000, {}, cancelled);
        const auto result = resolved.valid && resolved.folder
            ? filemanager::platform::ShellItem::enumerateChildrenIsolated(resolved, 1024, 5000, {}, cancelled)
            : filemanager::platform::ShellItemEnumeration{false, false, resolved.nativeCode, {},
                resolved.error.isEmpty() ? QStringLiteral("The Windows location is not a folder.") : resolved.error};
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, resolved, result, generation] {
            if (!guard || generation != guard->shellGeneration_ || guard->isFileSystemLocation()
                    || !guard->hasShellAccess())
                return;
            if (resolved.valid && resolved.folder) {
                guard->shellLocation_ = resolved;
                if (guard->temporaryShellVisit_ && guard->shellVisitRoot_.isEmpty())
                    guard->shellVisitRoot_ = resolved.idList;
                if (guard->historyIndex_ >= 0 && guard->historyIndex_ < guard->shellHistory_.size())
                    guard->shellHistory_[guard->historyIndex_] = resolved;
            }
            guard->updateNavigationActions();
            guard->shellItems_ = result.items;
            guard->shellView_->clear();
            const QIcon folderIcon = guard->style()->standardIcon(QStyle::SP_DirIcon);
            const QIcon fileIcon = guard->style()->standardIcon(QStyle::SP_FileIcon);
            for (int index = 0; index < result.items.size(); ++index) {
                const auto& item = result.items.at(index);
                auto* row = new QListWidgetItem(item.folder ? folderIcon : fileIcon,
                    item.displayName.isEmpty() ? item.parsingName : item.displayName,
                    guard->shellView_);
                row->setData(Qt::UserRole, index);
                row->setToolTip(item.parsingName);
                row->setFlags(row->flags() | Qt::ItemIsDragEnabled);
                if (guard->selectionCheckBoxes_) {
                    row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
                    row->setCheckState(Qt::Unchecked);
                }
            }
            guard->shellView_->setEnabled(result.success);
            guard->shellStatus_->setText(result.success
                ? QStringLiteral("%1%2 · isolated Windows Shell provider")
                    .arg(result.items.size())
                    .arg(result.truncated ? QStringLiteral("; first 1024 shown") : QString{})
                : QStringLiteral("Windows could not enumerate this location: %1")
                    .arg(result.error));
            if (!result.items.isEmpty())
                guard->shellView_->setCurrentRow(0);
            emit guard->itemCountChanged();
            emit guard->shellLoadFinished(result.success, result.truncated, result.error);
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

bool FilePane::hasShellAccess() const
{
    return temporaryShellVisit_ || filemanager::settings::loadShellPermissions().allowThirdPartyNamespaces;
}

bool FilePane::parentStaysInShellVisit() const
{
    return !temporaryShellVisit_ || filemanager::settings::loadShellPermissions().allowThirdPartyNamespaces
        || shellVisitRoot_.isEmpty() || shellLocation_.idList != shellVisitRoot_;
}

void FilePane::cancelShellWork()
{
    if (shellCancelled_)
        shellCancelled_->store(true);
    ++shellGeneration_;
    shellParentPending_ = false;
}

void FilePane::updateShellConsent()
{
    shellConsent_->setVisible(!hasShellAccess());
    shellEndVisit_->setVisible(temporaryShellVisit_);
}

void FilePane::endShellVisit()
{
    cancelShellWork();
    temporaryShellVisit_ = false;
    shellVisitRoot_.clear();
    if (!isFileSystemLocation() && !homeLocation_)
        loadShellLocation();
}

void FilePane::loadSafeThisPc()
{
    safeThisPc_ = true;
    shellItems_.clear();
    safeThisPcPaths_.clear();
    shellView_->clear();
    shellView_->setEnabled(false);
    shellView_->setDragEnabled(false);
    shellStatus_->setText("Loading known folders and drive roots…");
    emit itemCountChanged();
    updateNavigationActions();
    const quint64 generation = shellGeneration_;
    QPointer<FilePane> guard(this);
    QThread* worker = QThread::create([guard, generation] {
        QList<QPair<QString, QString>> paths;
        using filemanager::platform::KnownFolder;
        for (const auto& entry : {qMakePair(KnownFolder::Desktop, "Desktop"),
                 qMakePair(KnownFolder::Documents, "Documents"), qMakePair(KnownFolder::Downloads, "Downloads"),
                 qMakePair(KnownFolder::Music, "Music"), qMakePair(KnownFolder::Pictures, "Pictures"),
                 qMakePair(KnownFolder::Videos, "Videos")}) {
            const QString path = filemanager::platform::knownFolderPath(entry.first);
            if (!path.isEmpty())
                paths.append({QString::fromLatin1(entry.second), path});
        }
        for (const auto& drive : QDir::drives())
            paths.append({QDir::toNativeSeparators(drive.absoluteFilePath()), drive.absoluteFilePath()});
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, generation, paths] {
            if (!guard || generation != guard->shellGeneration_ || !guard->safeThisPc_)
                return;
            guard->safeThisPcPaths_ = paths;
            const QIcon icon = guard->style()->standardIcon(QStyle::SP_DirIcon);
            for (int index = 0; index < paths.size(); ++index) {
                auto* row = new QListWidgetItem(icon, paths.at(index).first, guard->shellView_);
                row->setData(Qt::UserRole, index);
                row->setToolTip(paths.at(index).second);
            }
            guard->shellView_->setEnabled(true);
            guard->shellStatus_->setText("Known folders and drive roots · other Windows locations need permission");
            emit guard->itemCountChanged();
            emit guard->shellLoadFinished(true, false, {});
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void FilePane::updateEmptyState()
{
    const filemanager::ui::diagnostics::PresentationScope timing(
        filemanager::ui::diagnostics::PresentationStage::PaneEmptyState);
    if (!emptyState_ || !viewStack_ || !proxy_)
        return;
    if (!isFileSystemLocation()) {
        emptyState_->hide();
        return;
    }

    const bool empty = proxy_->rowCount(view()->rootIndex()) == 0;
    if (filterEdit_->text().trimmed().isEmpty()) {
        emptyState_->setText(
            "This folder is empty.\n\nDrop files here, or use File > New folder or New text file.");
    } else {
        emptyState_->setText(
            "No items match this filter.\n\nChange or clear the filter to see this folder again.");
    }
    emptyState_->setGeometry(viewStack_->rect().adjusted(24, 24, -24, -24));
    emptyState_->setVisible(empty);
    if (empty)
        emptyState_->raise();
}

QString FilePane::columnProfileForPath(const QString& path) const
{
    const filemanager::ui::diagnostics::PresentationScope timing(
        filemanager::ui::diagnostics::PresentationStage::ColumnProfileLookup);
    const QString absolutePath = QDir(path).absolutePath();
    using filemanager::platform::KnownFolder;
    const auto containsPath = [&absolutePath](KnownFolder folder) {
        const QString root = filemanager::platform::knownFolderPath(folder);
        return filemanager::ui::columnProfileContainsPath(root, absolutePath);
    };
    if (containsPath(KnownFolder::Documents))
        return "documents";
    if (containsPath(KnownFolder::Pictures))
        return "pictures";
    if (containsPath(KnownFolder::Music))
        return "music";
    if (containsPath(KnownFolder::Videos))
        return "videos";
    return "general";
}

void FilePane::resetDefaultColumns()
{
    const QList<int> widths = {330, 110, 130, 130, 130, 80, 105, 85, 160, 160, 80, 210};
    for (int column = 0; column < widths.size(); ++column) {
        view_->setColumnHidden(column, false);
        view_->header()->resizeSection(column, widths.at(column));
    }
    view_->setColumnHidden(11, true);
}

QString FilePane::displayedColumnProfileForPath(const QString& path) const
{
    // Keep saves tied to the profile actually displayed by this pane. Navigation
    // resolves the destination afresh, including after known-folder redirection.
    if (!columnProfile_.isEmpty() && restorePathKey(path) == columnProfilePath_)
        return columnProfile_;
    return columnProfileForPath(path);
}

void FilePane::applyColumnProfile(const QString& path, const QString& profile)
{
    applyingColumnProfile_ = true;
    columnProfilePath_ = restorePathKey(path);
    columnProfile_ = profile;
    QSettings settings;
    QByteArray state = liveColumnProfiles().value(profile);
    if (state.isEmpty()) {
        state = settings.value("view/columnProfiles/" + profile).toByteArray();
        if (!state.isEmpty())
            liveColumnProfiles().insert(profile, state);
    }
    if (state.isEmpty() || !view_->header()->restoreState(state))
        resetDefaultColumns();
    else if (settings.value("view/columnProfileVersions/" + profile, 1).toInt() < 2)
        view_->setColumnHidden(11, true);
    view_->setColumnHidden(0, false);
    applyingColumnProfile_ = false;
}

void FilePane::cacheColumnProfile(const QString& path) const
{
    if (!path.isEmpty())
        liveColumnProfiles().insert(displayedColumnProfileForPath(path), view_->header()->saveState());
}

void FilePane::saveColumnProfile(const QString& path) const
{
    const filemanager::ui::diagnostics::PresentationScope timing(
        filemanager::ui::diagnostics::PresentationStage::PaneColumnsSave);
    if (path.isEmpty())
        return;
    const QString profile = displayedColumnProfileForPath(path);
    QByteArray state = liveColumnProfiles().value(profile);
    if (state.isEmpty()) {
        state = view_->header()->saveState();
        liveColumnProfiles().insert(profile, state);
    }
    QSettings settings;
    if (settings.value("view/columnProfiles/" + profile).toByteArray() == state
            && settings.value("view/columnProfileVersions/" + profile, 1).toInt() == 2)
        return;
    settings.setValue("view/columnProfiles/" + profile, state);
    settings.setValue("view/columnProfileVersions/" + profile, 2);
}

void FilePane::openPath(const QString& path)
{
    if (QFileInfo(path).isDir())
        navigate(path);
    else
        filemanager::platform::openWithSystemShell(path);
}

void FilePane::revealPath(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return;

    navigate(info.absolutePath());
    pendingRevealPath_ = info.absoluteFilePath();
    applyPendingReveal();
}

void FilePane::applyPendingReveal()
{
    if (pendingRevealPath_.isEmpty() || !isFileSystemLocation())
        return;
    const QModelIndex sourceIndex = model_->indexForPath(pendingRevealPath_);
    const QModelIndex proxyIndex = proxy_->mapFromSource(sourceIndex);
    if (!proxyIndex.isValid())
        return;
    view()->setCurrentIndex(proxyIndex);
    view()->selectionModel()->select(
        proxyIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    view()->scrollTo(proxyIndex, QAbstractItemView::PositionAtCenter);
    view()->setFocus();
    pendingRevealPath_.clear();
}

void FilePane::goBack()
{
    if (historyIndex_ > 0)
        navigateHistoryEntry(--historyIndex_);
}

void FilePane::goForward()
{
    if (historyIndex_ + 1 < history_.size())
        navigateHistoryEntry(++historyIndex_);
}

void FilePane::goUp()
{
    if (homeLocation_)
        return;
    if (!isFileSystemLocation()) {
        if (!hasShellAccess() || !parentStaysInShellVisit() || safeThisPc_ || shellParentPending_
                || !filemanager::platform::ShellItem::hasParentIdentity(shellLocation_))
            return;
        shellParentPending_ = true;
        updateNavigationActions();
        const auto folder = shellLocation_;
        const quint64 generation = shellGeneration_;
        const auto cancelled = shellCancelled_;
        QPointer<FilePane> guard(this);
        QThread* worker = QThread::create([guard, folder, generation, cancelled] {
            const auto parent = filemanager::platform::ShellItem::parentIsolated(folder, 5000, {}, cancelled);
            if (!guard)
                return;
            QMetaObject::invokeMethod(guard, [guard, parent, generation] {
                if (!guard)
                    return;
                if (generation != guard->shellGeneration_) {
                    emit guard->shellParentNavigationFinished(false, {});
                    return;
                }
                guard->shellParentPending_ = false;
                guard->updateNavigationActions();
                if (parent.valid && parent.folder && guard->hasShellAccess())
                    guard->navigateShellItemInternal(parent, true, parent.parsingName,
                        filemanager::platform::BuiltInShellLocation::None, true);
                else
                    guard->shellStatus_->setText(
                        QStringLiteral("Windows could not open the parent location: %1")
                            .arg(parent.error));
                emit guard->shellParentNavigationFinished(parent.valid && parent.folder, parent.error);
            });
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
        return;
    }
    const QString parent = filemanager::platform::fileSystemParentPath(currentPath());
    if (!parent.isEmpty())
        navigate(parent);
}

void FilePane::updateNavigationActions()
{
    backButton_->setEnabled(historyIndex_ > 0);
    forwardButton_->setEnabled(historyIndex_ + 1 < history_.size());
    if (upButton_) {
        if (homeLocation_) {
            upButton_->setEnabled(false);
        } else if (isFileSystemLocation()) {
            upButton_->setEnabled(!filemanager::platform::fileSystemParentPath(currentPath()).isEmpty());
        } else {
            upButton_->setEnabled(hasShellAccess() && parentStaysInShellVisit()
                && !safeThisPc_ && !shellParentPending_
                && filemanager::platform::ShellItem::hasParentIdentity(shellLocation_));
        }
    }
}

void FilePane::navigateHistoryEntry(int index)
{
    if (index < 0 || index >= history_.size())
        return;
    const auto shell = shellHistory_.value(index);
    if (history_.at(index) == QLatin1String(HomeIdentity))
        navigateHome(false);
    else if (shell.valid) {
        const auto builtIn = filemanager::platform::ShellItem::builtInLocationFromIdentity(
            history_.at(index));
        navigateShellItemInternal(shell, false, history_.at(index), builtIn);
    }
    else
        navigate(history_.at(index), false);
}

void FilePane::showHistoryMenu(QPushButton* button, bool backward)
{
    QMenu menu(this);
    const int begin = backward ? historyIndex_ - 1 : historyIndex_ + 1;
    const int end = backward ? -1 : history_.size();
    const int step = backward ? -1 : 1;
    for (int index = begin; index != end; index += step) {
        const auto builtIn = filemanager::platform::ShellItem::builtInLocationFromIdentity(
            history_.at(index));
        const QString label = history_.at(index) == QLatin1String(HomeIdentity)
            ? QStringLiteral("Home")
            : builtIn != filemanager::platform::BuiltInShellLocation::None
                ? filemanager::platform::ShellItem::builtInDisplayName(builtIn)
                : QDir::toNativeSeparators(history_.at(index));
        QAction* action = menu.addAction(label);
        connect(action, &QAction::triggered, this, [this, index] {
            historyIndex_ = index;
            navigateHistoryEntry(index);
        });
    }
    if (!menu.isEmpty())
        menu.exec(button->mapToGlobal(QPoint(0, button->height())));
}

void FilePane::refresh()
{
    if (homeLocation_) {
        homeView_->refresh();
        emit selectionChanged(selectedPath());
        emit itemCountChanged();
        return;
    }
    if (!isFileSystemLocation()) {
        shellItems_.clear();
        shellView_->clear();
        shellView_->setEnabled(false);
        shellStatus_->setText("Refreshing this Windows location…");
        emit itemCountChanged();
        loadShellLocation();
        return;
    }
    const QString path = currentPath();
    model_->refresh();
    view_->setRootIndex({});
    iconView_->setRootIndex({});
    QTimer::singleShot(0, this, &FilePane::updateEmptyState);
}

void FilePane::activateCurrentHomeItem()
{
    if (homeLocation_ && homeView_)
        homeView_->activateCurrent();
}

void FilePane::focusLocation()
{
    if (addressBar_)
        addressBar_->beginEditing();
}

void FilePane::focusFilter()
{
    filterEdit_->setFocus();
    filterEdit_->selectAll();
}

void FilePane::setShowHidden(bool enabled)
{
    if (showHidden_ == enabled)
        return;
    showHidden_ = enabled;
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
    if (enabled)
        filters |= QDir::Hidden | QDir::System;
    model_->setFilters(filters);
    emit viewStateChanged();
    updateEmptyState();
}

void FilePane::setDetailsView(bool enabled)
{
    setViewMode(enabled ? ViewMode::Details : ViewMode::List);
}

void FilePane::setViewMode(ViewMode mode)
{
    if (viewMode_ == mode)
        return;
    if (homeLocation_) {
        viewMode_ = mode;
        emit viewStateChanged();
        return;
    }
    const QStringList selected = selectedPathsInSelectionOrder();
    const QItemSelection selectedIndexes = isFileSystemLocation()
        ? view()->selectionModel()->selection() : QItemSelection{};
    viewMode_ = mode;
    if (!isFileSystemLocation()) {
        const bool grid = mode == ViewMode::MediumIcons || mode == ViewMode::LargeIcons
            || mode == ViewMode::ExtraLargeIcons;
        int iconPixels = mode == ViewMode::SmallIcons ? 16 : 20;
        QSize gridSize;
        if (mode == ViewMode::MediumIcons) {
            iconPixels = 32;
            gridSize = QSize(110, 72);
        } else if (mode == ViewMode::LargeIcons) {
            iconPixels = 64;
            gridSize = QSize(150, 100);
        } else if (mode == ViewMode::ExtraLargeIcons) {
            iconPixels = 96;
            gridSize = QSize(190, 138);
        }
        shellView_->setViewMode(grid ? QListView::IconMode : QListView::ListMode);
        shellView_->setIconSize(QSize(iconPixels, iconPixels));
        shellView_->setGridSize(gridSize);
        shellView_->setWordWrap(grid);
        emit viewStateChanged();
        return;
    }
    if (mode == ViewMode::Details) {
        viewStack_->setCurrentWidget(view_);
    } else {
        const bool grid = mode == ViewMode::MediumIcons || mode == ViewMode::LargeIcons
            || mode == ViewMode::ExtraLargeIcons;
        int iconPixels = 20;
        QSize gridSize;
        if (mode == ViewMode::SmallIcons)
            iconPixels = 16;
        else if (mode == ViewMode::MediumIcons) {
            iconPixels = 32;
            gridSize = QSize(110, 72);
        } else if (mode == ViewMode::LargeIcons) {
            iconPixels = 64;
            gridSize = QSize(150, 100);
        } else if (mode == ViewMode::ExtraLargeIcons) {
            iconPixels = 96;
            gridSize = QSize(190, 138);
        }
        iconView_->setViewMode(grid ? QListView::IconMode : QListView::ListMode);
        iconView_->setIconSize(QSize(iconPixels, iconPixels));
        iconView_->setGridSize(gridSize);
        iconView_->setWordWrap(grid);
        viewStack_->setCurrentWidget(iconView_);
    }
    // Both filesystem views share proxy_. Transfer its existing indexes rather than
    // resolve every selected path through the filesystem again for a layout change.
    selectionOrder_ = selected;
    view()->selectionModel()->select(selectedIndexes,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    emit viewStateChanged();
}

void FilePane::setFolderPlacement(FolderPlacement placement)
{
    if (folderPlacement_ == placement)
        return;
    folderPlacement_ = placement;
    static_cast<NameFilter*>(proxy_)->setFolderPlacement(placement);
    emit viewStateChanged();
}

void FilePane::setGroupBy(GroupBy grouping)
{
    if (groupBy_ == grouping)
        return;
    groupBy_ = grouping;
    static_cast<NameFilter*>(proxy_)->setGroupBy(grouping);
    emit viewStateChanged();
}

void FilePane::setSort(int column, Qt::SortOrder order)
{
    if (column < 0 || column >= DetailsColumnCount || !isFileSystemLocation())
        return;
    view_->sortByColumn(column, order);
}

int FilePane::sortColumn() const
{
    return view_ ? view_->header()->sortIndicatorSection() : 0;
}

Qt::SortOrder FilePane::sortOrder() const
{
    return view_ ? view_->header()->sortIndicatorOrder() : Qt::AscendingOrder;
}

QString FilePane::columnName(int column) const
{
    if (!model_ || column < 0 || column >= DetailsColumnCount)
        return {};
    return model_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
}

bool FilePane::columnVisible(int column) const
{
    return view_ && column >= 0 && column < DetailsColumnCount
        && !view_->isColumnHidden(column);
}

void FilePane::setColumnVisible(int column, bool visible)
{
    if (!view_ || !isFileSystemLocation() || column < 0 || column >= DetailsColumnCount)
        return;
    if (column == 0)
        visible = true;
    if (columnVisible(column) == visible)
        return;
    view_->setColumnHidden(column, !visible);
    cacheColumnProfile(currentPath());
    if (!columnSaveTimer_->isActive())
        columnSaveTimer_->start();
    emit viewStateChanged();
}

void FilePane::setShowFileExtensions(bool enabled)
{
    if (showFileExtensions_ == enabled)
        return;
    showFileExtensions_ = enabled;
    static_cast<NameFilter*>(proxy_)->setShowFileExtensions(enabled);
    emit viewStateChanged();
}

void FilePane::setSelectionCheckBoxes(bool enabled)
{
    if (selectionCheckBoxes_ == enabled)
        return;
    selectionCheckBoxes_ = enabled;
    static_cast<NameFilter*>(proxy_)->setSelectionCheckBoxesEnabled(enabled);
    updateSelectionCheckState(view());
    updatingShellChecks_ = true;
    for (int row = 0; row < shellView_->count(); ++row) {
        QListWidgetItem* item = shellView_->item(row);
        if (enabled) {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(item->isSelected() ? Qt::Checked : Qt::Unchecked);
        } else {
            item->setData(Qt::CheckStateRole, {});
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        }
    }
    updatingShellChecks_ = false;
    updateSelectionAccessibility();
    view_->viewport()->update();
    iconView_->viewport()->update();
    shellView_->viewport()->update();
}

void FilePane::updateSelectionCheckState(QAbstractItemView* sourceView)
{
    if (!isFileSystemLocation() || sourceView != view())
        return;
    static_cast<NameFilter*>(proxy_)->setSelectedPaths(selectedPaths());
}

void FilePane::updateSelectionAccessibility()
{
    const QString description = selectionCheckBoxes_
        ? QStringLiteral("Item-selection check boxes are enabled. Use a check box or press Space "
                         "on the focused item to add or remove it from the selection.")
        : QStringLiteral("Use Ctrl+Space on the focused item to add or remove it from the selection.");
    view_->setAccessibleDescription(description);
    iconView_->setAccessibleDescription(description);
    shellView_->setAccessibleDescription(description);
}

void FilePane::selectAll() { view()->selectAll(); }
void FilePane::clearSelection() { view()->clearSelection(); }

void FilePane::invertSelection()
{
    if (!isFileSystemLocation()) {
        for (int row = 0; row < shellView_->count(); ++row)
            shellView_->item(row)->setSelected(!shellView_->item(row)->isSelected());
        return;
    }
    const QModelIndex root = view()->rootIndex();
    const int rows = proxy_->rowCount(root);
    if (rows == 0)
        return;
    const QItemSelection selection(proxy_->index(0, 0, root), proxy_->index(rows - 1, 0, root));
    view()->selectionModel()->select(selection, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
}

QString FilePane::dropDestination(QAbstractItemView* sourceView, const QPoint& position) const
{
    const QModelIndex proxyIndex = sourceView ? sourceView->indexAt(position) : QModelIndex{};
    if (proxyIndex.isValid()) {
        const QString candidate = model_->filePath(proxy_->mapToSource(proxyIndex));
        if (QFileInfo(candidate).isDir())
            return candidate;
    }
    return currentPath();
}

bool FilePane::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == shellView_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            activateCurrentShellItem();
            return true;
        }
    }
    if (watched == viewStack_) {
        if (event->type() == QEvent::Resize)
            updateEmptyState();
        return QWidget::eventFilter(watched, event);
    }

    if (selectionCheckBoxes_ && shellView_ && watched == shellView_
        && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        QListWidgetItem* item = shellView_->currentItem();
        if (item && key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier) {
            item->setSelected(!item->isSelected());
            return true;
        }
    }

    QAbstractItemView* sourceView = nullptr;
    bool viewportEvent = false;
    if (view_ && (watched == view_ || watched == view_->viewport())) {
        sourceView = view_;
        viewportEvent = watched == view_->viewport();
    } else if (iconView_ && (watched == iconView_ || watched == iconView_->viewport())) {
        sourceView = iconView_;
        viewportEvent = watched == iconView_->viewport();
    }

    if (!sourceView)
        return QWidget::eventFilter(watched, event);

    if (selectionCheckBoxes_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const QModelIndex index = sourceView->currentIndex().siblingAtColumn(0);
        if (index.isValid() && key->key() == Qt::Key_Space
            && key->modifiers() == Qt::NoModifier) {
            const bool selected = sourceView->selectionModel()->isSelected(index);
            sourceView->selectionModel()->select(index,
                (selected ? QItemSelectionModel::Deselect : QItemSelectionModel::Select)
                    | QItemSelectionModel::Rows);
            sourceView->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
            QAccessibleAnnouncementEvent announcement(sourceView,
                selected ? QStringLiteral("Item unchecked") : QStringLiteral("Item checked"));
            announcement.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
            QAccessible::updateAccessibility(&announcement);
            return true;
        }
    }

    if (viewportEvent && event->type() == QEvent::MouseButtonRelease) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const QModelIndex index = sourceView->indexAt(mouse->position().toPoint());
        bool checkIndicatorClicked = false;
        if (selectionCheckBoxes_ && index.isValid() && index.column() == 0) {
            QStyleOptionViewItem option;
            option.initFrom(sourceView);
            option.rect = sourceView->visualRect(index);
            option.features |= QStyleOptionViewItem::HasCheckIndicator;
            option.checkState = sourceView->selectionModel()->isSelected(index)
                ? Qt::Checked : Qt::Unchecked;
            checkIndicatorClicked = sourceView->style()->subElementRect(
                QStyle::SE_ItemViewItemCheckIndicator, &option, sourceView)
                                        .contains(mouse->position().toPoint());
        }
        if (singleClickActivation_ && mouse->button() == Qt::LeftButton
            && !checkIndicatorClicked
            && !(mouse->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier
                | Qt::AltModifier | Qt::MetaModifier))) {
            if (index.isValid())
                activateIndex(index);
        }
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto* drag = static_cast<QDropEvent*>(event);
        if (!drag->mimeData()->hasUrls()
                && drag->mimeData()->hasFormat(
                    filemanager::platform::ShellClipboard::shellIdListMimeType())) {
            drag->acceptProposedAction();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() != QEvent::Drop)
        return QWidget::eventFilter(watched, event);

    auto* drop = static_cast<QDropEvent*>(event);
    if (!drop->mimeData()->hasUrls()) {
        const auto shellItems =
            filemanager::platform::ShellClipboard::shellItems(drop->mimeData());
        if (!shellItems.available)
            return QWidget::eventFilter(watched, event);
        bool moveRequested = drop->modifiers().testFlag(Qt::ShiftModifier)
            || filemanager::platform::ShellClipboard::preferredEffect(drop->mimeData())
                == filemanager::platform::ShellTransferEffect::Move;
        QString error = shellItems.error;
        if (drop->modifiers().testFlag(Qt::ControlModifier)
                && drop->modifiers().testFlag(Qt::ShiftModifier)) {
            error = QStringLiteral(
                "Link drops for virtual Windows items are not supported or reinterpreted.");
        }
        const QString destination = dropDestination(sourceView, drop->position().toPoint());
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        emit shellItemsDropped(shellItems.items, destination, moveRequested, error);
        return true;
    }

    QStringList paths;
    for (const QUrl& url : drop->mimeData()->urls()) {
        if (!url.isLocalFile())
            return QWidget::eventFilter(watched, event);
        paths.append(url.toLocalFile());
    }
    if (paths.isEmpty())
        return true;

    const QString destination = dropDestination(sourceView, drop->position().toPoint());
    const Qt::KeyboardModifiers modifiers = drop->modifiers();
    if (modifiers.testFlag(Qt::ControlModifier) && modifiers.testFlag(Qt::ShiftModifier)) {
        emit filesDropped(paths, destination, false, true, true);
        drop->ignore();
        return true;
    }

    bool move = false;
    bool explicitEffect = false;
    if (modifiers.testFlag(Qt::ControlModifier)) {
        move = false;
        explicitEffect = true;
    } else if (modifiers.testFlag(Qt::ShiftModifier)) {
        move = true;
        explicitEffect = true;
    } else if (const auto preferred =
            filemanager::platform::ShellClipboard::preferredEffect(drop->mimeData())) {
        move = *preferred == filemanager::platform::ShellTransferEffect::Move;
        explicitEffect = true;
    } else if (drop->proposedAction() == Qt::MoveAction) {
        move = true;
    } else {
        const QByteArray destinationDevice = QStorageInfo(destination).device();
        const QByteArray sourceDevice = QStorageInfo(paths.constFirst()).device();
        move = !destinationDevice.isEmpty() && destinationDevice == sourceDevice;
    }

    drop->setDropAction(move ? Qt::MoveAction : Qt::CopyAction);
    drop->accept();
    emit filesDropped(paths, destination, move, !explicitEffect, false);
    return true;
}

void FilePane::activateIndex(const QModelIndex& index)
{
    if (!index.isValid() || index.model() != proxy_)
        return;
    const QModelIndex source = proxy_->mapToSource(index);
    if (!source.isValid())
        return;
    emit activated(this);
    openPath(model_->filePath(source));
}

void FilePane::activateShellRow(int row)
{
    if (safeThisPc_) {
        if (row >= 0 && row < safeThisPcPaths_.size())
            navigate(safeThisPcPaths_.at(row).second);
        return;
    }
    if (!hasShellAccess())
        return;
    if (row < 0 || row >= shellItems_.size())
        return;
    emit activated(this);
    const auto item = shellItems_.at(row);
    if (item.folder) {
        if (item.fileSystem)
            navigate(item.fileSystemPath);
        else
            navigateShellItemInternal(item, true, item.parsingName,
                filemanager::platform::BuiltInShellLocation::None, true);
        return;
    }
    emit shellItemOpenRequested(item.parsingName);
}

void FilePane::activateCurrentShellItem()
{
    if (shellView_ && !isFileSystemLocation() && !homeLocation_)
        activateShellRow(shellView_->currentRow());
}
