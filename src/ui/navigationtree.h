#pragma once

#include "files/storagevolumes.h"
#include "settings/favoritegroups.h"
#include "platform/shellitem.h"

#include <QHash>
#include <QStringList>
#include <QTreeWidget>
#include <atomic>
#include <memory>

class QDragMoveEvent;
class QDragEnterEvent;
class QDropEvent;
class QMouseEvent;
class QTreeWidgetItem;

namespace filemanager::ui {

class NavigationTree final : public QTreeWidget
{
    Q_OBJECT

public:
    enum ItemRole {
        PathRole = Qt::UserRole,
        KindRole,
        GroupIdRole,
        ChildrenStateRole,
        ShellIdentityRole
    };

    explicit NavigationTree(QWidget* parent = nullptr);
    ~NavigationTree() override;

    void rebuild(const QStringList& recentPaths,
        const filemanager::settings::FavoriteGroups& favorites,
        const QVector<filemanager::files::StorageVolume>& volumes);
    void synchronizeToPath(const QString& path);
    void setShellNavigationItems(const QList<filemanager::platform::ShellItemReference>& items);
    void selectFavorite(const QString& path, const QString& groupId);
    QString selectedFavoritePath() const;
    QString selectedFavoriteGroupId() const;
    QHash<QString, QStringList> favoriteOrder() const;

signals:
    void locationActivated(const QString& path);
    void backgroundLocationRequested(const QString& path);
    void shellLocationActivated(const QByteArray& identity);
    void shellBackgroundRequested(const QByteArray& identity);
    void homeActivated();
    void homeBackgroundRequested();
    void thisPcActivated();
    void thisPcBackgroundRequested();
    void librariesActivated();
    void librariesBackgroundRequested();
    void recycleBinActivated();
    void recycleBinBackgroundRequested();
    void networkActivated();
    void favoriteOrderChanged();
    void filesDropped(const QStringList& paths, const QString& destination,
        bool move, bool confirmationRequired, bool linkRequested);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    enum class ChildrenState { NotRequested, Loading, Loaded };

    QTreeWidgetItem* addFolder(QTreeWidgetItem* parent, const QString& label,
        const QString& path, const QIcon& icon, const QString& kind = {},
        const QString& groupId = {}, const QString& tooltip = {});
    void requestChildren(QTreeWidgetItem* item);
    void applyChildren(const QString& path, const QStringList& childPaths, bool truncated);
    QTreeWidgetItem* findExactPath(const QString& path) const;
    QTreeWidgetItem* deepestVisibleAncestor(const QString& path) const;
    static bool samePath(const QString& left, const QString& right);
    static bool isPathAncestor(const QString& parent, const QString& child);
    static QString normalizedPath(const QString& path);
    QString dropDestination(const QPoint& position) const;
    bool moveFavoriteAt(const QPoint& position);

    QString pendingPath_;
    QList<filemanager::platform::ShellItemReference> shellNavigationItems_;
    quint64 generation_ = 0;
    std::shared_ptr<std::atomic_bool> listingCancelled_ = std::make_shared<std::atomic_bool>(false);
};

} // namespace filemanager::ui
