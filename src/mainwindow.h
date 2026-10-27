#pragma once

#include <QMainWindow>
#include <QHash>
#include <QMap>
#include <QPointer>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include "diagnostics/asyncdiagnosticlog.h"
#include "files/sessionundo.h"
#include "files/storagevolumes.h"
#include "search/sessionsearchhistory.h"
#include "platform/shellitem.h"

class QLabel;
class QPushButton;
class QSplitter;
class QTreeView;
class QTabWidget;
class QCloseEvent;
class QShowEvent;
class FilePane;
class TabbedPanel;
namespace filemanager::commands { class CommandRegistry; }
namespace filemanager::files { class OperationJournal; class TransferQueue; }
namespace filemanager::preview { class PreviewController; }
namespace filemanager::platform { class WindowsShareController; }
namespace filemanager::settings { struct Session; }
namespace filemanager::ui { class NavigationTree; }
namespace filemanager::preview {
class DocumentMetadataProvider;
class MediaMetadataProvider;
class ShellMetadataProvider;
}
namespace filemanager::workspace { class WorkspaceView; }

class MainWindow final : public QMainWindow
{
    Q_OBJECT
public:
    // Optional benchmark-only, memory-only durations; never retained in diagnostics/settings.
    using StartupTimings = QMap<QString, qint64>;
    explicit MainWindow(const QString& initialLocation = {}, StartupTimings* timings = nullptr);
    void openInitialPaths(const QStringList& paths);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildUi(StartupTimings* timings);
    void createCommands();
    void cycleFocusRegion(bool backwards);
    QList<QWidget*> focusRegions() const;
    void setActivePane(FilePane* pane);
    void syncViewCommands();
    void syncLocationCommands();
    void connectPane(FilePane* pane);
    void newFolderTab();
    void closeFolderTab();
    void duplicateFolderTab();
    void reopenClosedTab();
    void openSelectedFolderInTab();
    void openSelectedFolderInWindow();
    void addCurrentFolderFavorite();
    void addFavoritePath(const QString& path);
    void removeSelectedFavorite();
    void moveSelectedFavorite(int offset);
    void manageFavoriteGroups();
    void rebuildQuickAccess();
    void syncShellNavigation();
    enum class ShellTreeTarget { CurrentPane, BackgroundTab, NewTab, NewWindow };
    void openShellTreeLocation(const QByteArray& identity, ShellTreeTarget target);
    void recordRecentLocation(const QString& path);
    void refreshStorageLocations();
    void handleFilesystemDrop(const QStringList& paths, const QString& destination,
        bool move, bool confirmationRequired, bool linkRequested);
    void showThisPc();
    void showBuiltInShell(filemanager::platform::BuiltInShellLocation location);
    void showStorageDetails();
    void mapNetworkDrive();
    void disconnectNetworkDrive();
    void restoreSelectedRecycleBinItems();
    void emptyRecycleBin();
    void showShellNamespace(const QString& parsingName);
    void showNetworkBrowser();
    void showNavigationContextMenu(const QPoint& point);
    void updatePreview(const QString& path);
    void refreshRichPreviewMetadata();
    void openSelected();
    void createFolder();
    void createTextFile();
    void createShortcuts();
    void renameSelected();
    void undoLastSessionOperation();
    void showBatchRenameUndoPrompt();
    void clearBatchRenameUndoPrompt();
    void deleteSelected();
    void permanentlyDeleteSelected();
    void recyclePaths(const QStringList& paths, QWidget* dialogParent,
        std::function<void(const QStringList&)> completion = {});
    void copyToOther(bool move);
    void startTransfer(const QStringList& sources, const QString& destination, bool move,
        bool recordUndo = true, std::function<void(bool)> completion = {});
    void offerElevatedCopyRetry(quint64 failedJobId);
    void showGoTo();
    void showSearch();
    void showAsyncSearch(const QString& savedSearchId = {});
    void showContentSearch();
    void showCommandPalette();
    void batchRename();
    void editSelectedTags();
    void createZipArchive();
    void extractZipArchive();
    void openNativeArchive();
    void shareSelected();
    void showPreferences();
    void showShortcutEditor();
    void showHelpBrowser();
    void showGettingStarted();
    void showRecoveryGuidance();
    void showAboutSupport();
    void showTransfers();
    void showInterruptedOperations();
    void addWorkspaceTab();
    filemanager::settings::Session captureSession() const;
    bool applySession(const filemanager::settings::Session& session, QString* error = nullptr);
    void saveLayout();
    void loadLayout();
    void saveNamedLayout();
    void manageNamedLayouts();
    void showColumnHeaderMenu(FilePane* pane, const QPoint& globalPoint);
    void showContextMenu(FilePane* pane, const QPoint& point);
    void copySelection(bool cut);
    void copyPathsToClipboard(const QStringList& paths, bool cut);
    void pasteClipboard();
    bool copyShellItemsToFolder(
        const QList<filemanager::platform::ShellItemReference>& items,
        const QString& destinationPath, bool moveRequested, QWidget* dialogParent);
    void showProperties();
    void showDetails();
    void showWindowsProperties(const QString& path);
    void analyzeSelected();
    void findDuplicateFiles();
    void openTerminal();
    void updateStatus();
    FilePane* otherPane() const;

    FilePane* active_ = nullptr;
    QList<QPointer<FilePane>> connectedPanes_;
    filemanager::workspace::WorkspaceView* workspace_ = nullptr;
    QWidget* preview_ = nullptr;
    QWidget* navigationPane_ = nullptr;
    QLabel* previewTitle_ = nullptr;
    QLabel* previewMeta_ = nullptr;
    QLabel* previewImage_ = nullptr;
    QLabel* previewText_ = nullptr;
    QTabWidget* workspaces_ = nullptr;
    filemanager::ui::NavigationTree* quickAccess_ = nullptr;
    QSplitter* workspaceSplitter_ = nullptr;
    filemanager::commands::CommandRegistry* commands_ = nullptr;
    filemanager::files::TransferQueue* transferQueue_ = nullptr;
    filemanager::files::OperationJournal* operationJournal_ = nullptr;
    QHash<quint64, QString> recoveryRetries_;
    filemanager::files::SessionUndoStack sessionUndo_;
    QPointer<QPushButton> batchRenameUndoPrompt_;
    filemanager::preview::PreviewController* previewController_ = nullptr;
    filemanager::platform::WindowsShareController* shareController_ = nullptr;
    filemanager::preview::DocumentMetadataProvider* documentMetadata_ = nullptr;
    filemanager::preview::MediaMetadataProvider* mediaMetadata_ = nullptr;
    filemanager::preview::ShellMetadataProvider* shellMetadata_ = nullptr;
    quint64 previewGeneration_ = 0;
    QString previewPath_;
    QString previewBaseMetadata_;
    QStringList storageRoots_;
    QVector<filemanager::files::StorageVolume> storageVolumes_;
    filemanager::search::SessionSearchHistory filenameSearchHistory_;
    filemanager::diagnostics::AsyncDiagnosticLog diagnosticLog_;
    bool diagnosticShutdownStarted_ = false;
    bool diagnosticShutdownFinished_ = false;
    std::shared_ptr<std::atomic_bool> archiveCancel_;
    bool closeAfterArchive_ = false;
    QString initialLocation_;
};
