#pragma once

#include "platform/shellitem.h"

#include <QStringList>
#include <QWidget>

class QAbstractItemView;
class QLineEdit;
class QLabel;
class QListView;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QSortFilterProxyModel;
class QTreeView;
class QTimer;
class QEvent;
class QModelIndex;

namespace filemanager::ui { class BreadcrumbBar; class HomeView; class LocalDirectoryModel; }

struct FilePaneRestoreState
{
    QStringList selectedPaths;
    QString focusedPath;
    QStringList expandedPaths;
    int verticalScroll = 0;
    int horizontalScroll = 0;
};

class FilePane final : public QWidget
{
    Q_OBJECT

public:
    static constexpr auto HomeIdentity = "filemanager:home";
    static constexpr int DetailsColumnCount = 12;
    enum class ViewMode { Details, List, LargeIcons, ExtraLargeIcons, MediumIcons, SmallIcons };
    enum class FolderPlacement { First, Last, Mixed };
    enum class GroupBy { None, Type, DateModified, Size, Name };
    explicit FilePane(QWidget* parent = nullptr);
    explicit FilePane(const QString& initialPath, QWidget* parent = nullptr);
    ~FilePane() override;

    QString currentPath() const;
    QString currentLocation() const;
    QString serializedLocation() const;
    bool isFileSystemLocation() const { return !homeLocation_ && !shellLocation_.valid; }
    bool isHomeLocation() const { return homeLocation_; }
    filemanager::platform::BuiltInShellLocation builtInShellLocation() const {
        return builtInShellLocation_;
    }
    filemanager::platform::ShellItemReference currentShellLocation() const {
        return shellLocation_;
    }
    QString selectedPath() const;
    QStringList selectedPaths() const;
    QStringList selectedPathsInSelectionOrder() const;
    QList<filemanager::platform::ShellItemReference> selectedShellItems() const;
    int selectedItemCount() const;
    int visibleItemCount() const;
    QAbstractItemView* view() const;
    QWidget* contentFocusTarget() const;

    void navigate(const QString& path, bool remember = true);
    void navigateHome(bool remember = true);
    bool navigateBuiltInShell(filemanager::platform::BuiltInShellLocation location,
        bool remember = true);
    void navigateShellItem(const filemanager::platform::ShellItemReference& folder,
        bool remember = true);
    void navigateShellParsingName(const QString& parsingName);
    void activateCurrentShellItem();
    bool hasTemporaryShellVisit() const { return temporaryShellVisit_; }
    // A memory-only snapshot of the current authorized, completed listing: root then folders.
    QList<filemanager::platform::ShellItemReference> shellNavigationItems() const;
    bool navigateShellVisitItem(const QByteArray& identity);
    void endShellVisit();
    void openPath(const QString& path);
    void revealPath(const QString& path);
    void refresh();
    void activateCurrentHomeItem();
    void focusLocation();
    void focusFilter();
    void setShowHidden(bool enabled);
    bool showHidden() const { return showHidden_; }
    void setDetailsView(bool enabled);
    void setViewMode(ViewMode mode);
    ViewMode viewMode() const { return viewMode_; }
    void setFolderPlacement(FolderPlacement placement);
    FolderPlacement folderPlacement() const { return folderPlacement_; }
    void setGroupBy(GroupBy grouping);
    GroupBy groupBy() const { return groupBy_; }
    void setSort(int column, Qt::SortOrder order);
    int sortColumn() const;
    Qt::SortOrder sortOrder() const;
    QString columnName(int column) const;
    bool columnVisible(int column) const;
    void setColumnVisible(int column, bool visible);
    void setShowFileExtensions(bool enabled);
    bool showFileExtensions() const { return showFileExtensions_; }
    QStringList navigationHistory() const { return history_; }
    int navigationHistoryIndex() const { return historyIndex_; }
    bool hasPathAutocomplete() const;
    void setSingleClickActivation(bool enabled) { singleClickActivation_ = enabled; }
    bool singleClickActivation() const { return singleClickActivation_; }
    void setSelectionCheckBoxes(bool enabled);
    bool selectionCheckBoxes() const { return selectionCheckBoxes_; }
    FilePaneRestoreState restoreState() const;
    void restoreViewState(const FilePaneRestoreState& state);
    void selectAll();
    void clearSelection();
    void invertSelection();

signals:
    void activated(FilePane* pane);
    void selectionChanged(const QString& path);
    void pathChanged(const QString& path);
    void locationChanged(const QString& displayLocation);
    void contextMenuRequested(FilePane* pane, const QPoint& point);
    void columnHeaderMenuRequested(FilePane* pane, const QPoint& globalPoint);
    void viewStateChanged();
    void itemCountChanged();
    void shellLoadFinished(bool success, bool truncated, const QString& error);
    void shellParentNavigationFinished(bool navigated, const QString& error);
    void shellProviderRequestStarted();
    void filesDropped(const QStringList& paths, const QString& destination, bool move,
        bool confirmationRequired, bool linkRequested);
    void shellItemsDropped(const QList<filemanager::platform::ShellItemReference>& items,
        const QString& destination, bool moveRequested, const QString& error);
    void shellItemOpenRequested(const QString& parsingName);
    void backgroundTabRequested(const QString& path);
    void savedSearchRequested(const QString& id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QString columnProfileForPath(const QString& path) const;
    QString displayedColumnProfileForPath(const QString& path) const;
    void cacheColumnProfile(const QString& path) const;
    void applyColumnProfile(const QString& path, const QString& profile);
    void saveColumnProfile(const QString& path) const;
    void resetDefaultColumns();
    void goBack();
    void goForward();
    void goUp();
    void updateNavigationActions();
    void showHistoryMenu(QPushButton* button, bool backward);
    QString dropDestination(QAbstractItemView* sourceView, const QPoint& position) const;
    void activateIndex(const QModelIndex& index);
    void updateEmptyState();
    void navigateHistoryEntry(int index);
    void navigateShellItemInternal(const filemanager::platform::ShellItemReference& folder,
        bool remember, const QString& identity,
        filemanager::platform::BuiltInShellLocation builtInLocation, bool continueVisit = false);
    void activateShellRow(int row);
    void loadShellLocation();
    void loadSafeThisPc();
    bool hasShellAccess() const;
    bool parentStaysInShellVisit() const;
    void cancelShellWork();
    void updateShellConsent();
    void updateSelectionOrder(QAbstractItemView* sourceView);
    void updateSelectionCheckState(QAbstractItemView* sourceView);
    void updateSelectionAccessibility();
    void selectPathsInOrder(const QStringList& paths);
    void applyPendingRestoreState();
    void applyPendingReveal();

    filemanager::ui::LocalDirectoryModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTreeView* view_ = nullptr;
    QListView* iconView_ = nullptr;
    QListWidget* shellView_ = nullptr;
    QWidget* shellPage_ = nullptr;
    QLabel* shellStatus_ = nullptr;
    QWidget* shellConsent_ = nullptr;
    QPushButton* shellEndVisit_ = nullptr;
    bool temporaryShellVisit_ = false;
    QByteArray shellVisitRoot_;
    bool safeThisPc_ = false;
    QList<QPair<QString, QString>> safeThisPcPaths_;
    filemanager::platform::ShellCancellation shellCancelled_;
    filemanager::ui::HomeView* homeView_ = nullptr;
    QStackedWidget* viewStack_ = nullptr;
    filemanager::ui::BreadcrumbBar* addressBar_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QLabel* emptyState_ = nullptr;
    QPushButton* backButton_ = nullptr;
    QPushButton* forwardButton_ = nullptr;
    QPushButton* upButton_ = nullptr;
    QStringList history_;
    QList<filemanager::platform::ShellItemReference> shellHistory_;
    int historyIndex_ = -1;
    filemanager::platform::ShellItemReference shellLocation_;
    filemanager::platform::BuiltInShellLocation builtInShellLocation_
        = filemanager::platform::BuiltInShellLocation::None;
    bool homeLocation_ = false;
    QList<filemanager::platform::ShellItemReference> shellItems_;
    quint64 shellGeneration_ = 0;
    bool shellParentPending_ = false;
    bool showHidden_ = false;
    ViewMode viewMode_ = ViewMode::Details;
    FolderPlacement folderPlacement_ = FolderPlacement::First;
    GroupBy groupBy_ = GroupBy::None;
    bool showFileExtensions_ = true;
    bool singleClickActivation_ = false;
    bool selectionCheckBoxes_ = false;
    bool updatingShellChecks_ = false;
    bool applyingColumnProfile_ = false;
    QString columnProfilePath_;
    QString columnProfile_;
    QTimer* columnSaveTimer_ = nullptr;
    QTimer* restoreTimer_ = nullptr;
    FilePaneRestoreState pendingRestoreState_;
    int restoreAttempts_ = 0;
    bool restorePending_ = false;
    QString pendingRevealPath_;
    QStringList selectionOrder_;
};
