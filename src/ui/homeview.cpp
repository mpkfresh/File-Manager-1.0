#include "ui/homeview.h"

#include "search/savedsearchstore.h"
#include "settings/favoritegroups.h"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QSettings>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace filemanager::ui {
namespace {

enum ItemRole {
    KindRole = Qt::UserRole,
    PathRole,
    SavedSearchIdRole
};

QString displayNameForPath(const QString& rawPath)
{
    QString path = QDir::fromNativeSeparators(rawPath.trimmed());
    while (path.size() > 3 && path.endsWith('/'))
        path.chop(1);
    const qsizetype separator = path.lastIndexOf('/');
    const QString name = separator >= 0 ? path.mid(separator + 1) : path;
    return name.isEmpty() ? QDir::toNativeSeparators(path) : name;
}

QTreeWidgetItem* addSection(QTreeWidget* tree, const QString& label)
{
    auto* section = new QTreeWidgetItem(tree, {label});
    section->setData(0, KindRole, QStringLiteral("section"));
    section->setFlags(Qt::ItemIsEnabled);
    QFont font = section->font(0);
    font.setBold(true);
    section->setFont(0, font);
    section->setExpanded(true);
    return section;
}

QTreeWidgetItem* addPath(QTreeWidgetItem* parent, const QString& label,
    const QString& rawPath, const QIcon& icon)
{
    const QString path = QDir::cleanPath(QDir::fromNativeSeparators(rawPath));
    auto* item = new QTreeWidgetItem(parent, {label, QDir::toNativeSeparators(path)});
    item->setData(0, KindRole, QStringLiteral("path"));
    item->setData(0, PathRole, path);
    item->setIcon(0, icon);
    item->setToolTip(0, QDir::toNativeSeparators(path));
    item->setToolTip(1, QDir::toNativeSeparators(path));
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}

} // namespace

HomeView::HomeView(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HomePage"));
    setAccessibleName(tr("Home"));
    setAccessibleDescription(tr(
        "Local File Manager favorites, recent folders, and explicitly saved searches"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto* heading = new QLabel(tr("Home"), this);
    heading->setObjectName(QStringLiteral("Title"));
    heading->setAccessibleName(tr("Home"));
    layout->addWidget(heading);

    auto* disclosure = new QLabel(tr(
        "Home shows only places and searches you use. "
        "It does not scan folders or contact providers on its own."),
        this);
    disclosure->setObjectName(QStringLiteral("HomePrivacyDisclosure"));
    disclosure->setAccessibleName(tr("Home privacy information"));
    disclosure->setWordWrap(true);
    disclosure->setTextFormat(Qt::PlainText);
    layout->addWidget(disclosure);

    emptyState_ = new QLabel(tr(
        "No places here yet.\nOpen a folder, pin one, or save a search to add it to Home."),
        this);
    emptyState_->setObjectName(QStringLiteral("HomeEmptyState"));
    emptyState_->setAccessibleName(tr("Home is empty"));
    emptyState_->setFocusPolicy(Qt::StrongFocus);
    emptyState_->setWordWrap(true);
    emptyState_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(emptyState_, 1);

    locations_ = new QTreeWidget(this);
    locations_->setObjectName(QStringLiteral("HomeLocations"));
    locations_->setAccessibleName(tr("Favorite locations, recent folders, and saved searches"));
    locations_->setColumnCount(2);
    locations_->setHeaderLabels({tr("Name"), tr("Location or scope")});
    locations_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    locations_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    locations_->setRootIsDecorated(true);
    locations_->setItemsExpandable(true);
    locations_->setSelectionMode(QAbstractItemView::SingleSelection);
    locations_->viewport()->installEventFilter(this);
    layout->addWidget(locations_, 1);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("Muted"));
    status_->setAccessibleName(tr("Home status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);

    connect(locations_, &QTreeWidget::itemActivated,
        this, [this](QTreeWidgetItem* item) { activateItem(item); });
    connect(locations_, &QTreeWidget::itemSelectionChanged,
        this, &HomeView::selectionChanged);
    refresh();
}

void HomeView::refresh()
{
    const QString selectedPathBefore = selectedPath();
    const QString selectedSearchBefore = selectedSavedSearchId();
    locations_->clear();
    itemCount_ = 0;

    QSettings localSettings;
    const auto favorites = settings::loadFavoriteGroups(localSettings);
    QTreeWidgetItem* favoriteSection = nullptr;
    for (const auto& group : favorites.groups) {
        if (group.paths.isEmpty())
            continue;
        if (!favoriteSection)
            favoriteSection = addSection(locations_, tr("FAVORITES"));
        auto* groupItem = new QTreeWidgetItem(favoriteSection, {group.name});
        groupItem->setData(0, KindRole, QStringLiteral("section"));
        groupItem->setFlags(Qt::ItemIsEnabled);
        groupItem->setExpanded(true);
        for (const QString& path : group.paths) {
            addPath(groupItem, displayNameForPath(path), path,
                style()->standardIcon(QStyle::SP_DirIcon));
            ++itemCount_;
        }
    }

    QTreeWidgetItem* recentSection = nullptr;
    const QStringList recents =
        localSettings.value(QStringLiteral("navigation/recents")).toStringList();
    for (const QString& path : recents.mid(0, 15)) {
        if (path.trimmed().isEmpty())
            continue;
        if (!recentSection)
            recentSection = addSection(locations_, tr("RECENT FOLDERS"));
        addPath(recentSection, displayNameForPath(path), path,
            style()->standardIcon(QStyle::SP_DirOpenIcon));
        ++itemCount_;
    }

    QTreeWidgetItem* searchSection = nullptr;
    search::SavedSearchStore searches;
    QString savedSearchError;
    const bool savedSearchesAvailable = searches.load(&savedSearchError);
    if (savedSearchesAvailable) {
        for (const auto& search : searches.entries()) {
            if (!searchSection)
                searchSection = addSection(locations_, tr("SAVED SEARCHES"));
            auto* item = new QTreeWidgetItem(searchSection,
                {search.name, QDir::toNativeSeparators(search.root)});
            item->setData(0, KindRole, QStringLiteral("saved-search"));
            item->setData(0, SavedSearchIdRole, search.id);
            item->setIcon(0, style()->standardIcon(QStyle::SP_FileDialogContentsView));
            item->setToolTip(0, tr("Open this explicitly saved filename search"));
            item->setToolTip(1, QDir::toNativeSeparators(search.root));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            ++itemCount_;
        }
    }

    QTreeWidgetItem* restored = nullptr;
    const auto allItems = locations_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : allItems) {
        if ((!selectedPathBefore.isEmpty()
                && item->data(0, PathRole).toString().compare(
                    selectedPathBefore, Qt::CaseInsensitive) == 0)
            || (!selectedSearchBefore.isEmpty()
                && item->data(0, SavedSearchIdRole).toString() == selectedSearchBefore)) {
            restored = item;
            break;
        }
    }
    if (restored)
        locations_->setCurrentItem(restored);
    else {
        for (QTreeWidgetItem* item : allItems) {
            if (item->flags().testFlag(Qt::ItemIsSelectable)) {
                locations_->setCurrentItem(item);
                break;
            }
        }
    }

    if (!savedSearchesAvailable) {
        status_->setText(tr("Saved searches are unavailable: %1").arg(savedSearchError));
    } else {
        status_->clear();
    }
    locations_->setVisible(itemCount_ > 0);
    emptyState_->setVisible(itemCount_ == 0);
    status_->setVisible(!status_->text().isEmpty());
}

QString HomeView::selectedPath() const
{
    return locations_->currentItem()
        ? locations_->currentItem()->data(0, PathRole).toString() : QString{};
}

QString HomeView::selectedSavedSearchId() const
{
    return locations_->currentItem()
        ? locations_->currentItem()->data(0, SavedSearchIdRole).toString() : QString{};
}

int HomeView::itemCount() const
{
    return itemCount_;
}

QWidget* HomeView::focusTarget() const
{
    return itemCount_ > 0 ? static_cast<QWidget*>(locations_) : static_cast<QWidget*>(emptyState_);
}

void HomeView::activateCurrent()
{
    activateItem(locations_->currentItem());
}

void HomeView::activateItem(QTreeWidgetItem* item)
{
    if (!item)
        return;
    const QString path = item->data(0, PathRole).toString();
    if (!path.isEmpty()) {
        emit pathActivated(path);
        return;
    }
    const QString searchId = item->data(0, SavedSearchIdRole).toString();
    if (!searchId.isEmpty())
        emit savedSearchActivated(searchId);
}

bool HomeView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == locations_->viewport() && event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::MiddleButton) {
            QTreeWidgetItem* item = locations_->itemAt(mouse->position().toPoint());
            const QString path = item ? item->data(0, PathRole).toString() : QString{};
            if (!path.isEmpty()) {
                emit backgroundPathRequested(path);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace filemanager::ui
