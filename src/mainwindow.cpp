#include "mainwindow.h"
#include "commands/commandregistry.h"
#include "diagnostics/diagnosticbundle.h"
#include "files/filenamevalidator.h"
#include "files/batchrename.h"
#include "files/fileoperations.h"
#include "files/analysisservice.h"
#include "files/duplicateservice.h"
#include "files/storagevolumes.h"
#include "files/networkdiscoveryservice.h"
#include "files/operationjournal.h"
#include "files/transfercontroller.h"
#include "files/transferqueue.h"
#include "files/ziparchive.h"
#include "tags/tagcatalog.h"
#include "search/fuzzymatcher.h"
#include "search/savedsearchstore.h"
#include "settings/sessionstore.h"
#include "settings/startuppreferences.h"
#include "settings/metadatapermissions.h"
#include "settings/favoritegroups.h"
#include "settings/shellpermissions.h"
#include "settings/contextmenupermissions.h"
#include "settings/portablepreferences.h"
#include "ui/filepane.h"
#include "ui/applicationtheme.h"
#include "ui/appearancesettings.h"
#include "ui/batchrenamedialog.h"
#include "ui/tvorganizerdialog.h"
#include "ui/tageditordialog.h"
#include "ui/brandidentity.h"
#include "ui/conflictdialog.h"
#include "ui/contentsearchdialog.h"
#include "ui/filedetailsdialog.h"
#include "ui/filesearchdialog.h"
#include "ui/iconfactory.h"
#include "ui/navigationtree.h"
#include "ui/shellnamespacebrowser.h"
#include "ui/shortcuteditordialog.h"
#include "ui/tabbedpanel.h"
#include "ui/transferannouncements.h"
#include "ui/localizedformat.h"
#include "ui/operationcenterdialog.h"
#include "ui/preferencesdialog.h"
#include "workspace/layoutmodel.h"
#include "workspace/workspaceview.h"
#include "preview/previewservice.h"
#include "preview/previewcontroller.h"
#include "preview/documentmetadataprovider.h"
#include "preview/mediametadataprovider.h"
#include "preview/shellmetadataprovider.h"
#include "platform/windowswindowchrome.h"
#include "platform/knownfolders.h"
#include "platform/recyclebinoperation.h"
#include "platform/shellclipboard.h"
#include "platform/shellfileoperation.h"
#include "platform/shellitem.h"
#include "platform/networkdriveui.h"
#include "platform/systemopen.h"
#include "platform/shelllink.h"
#include "platform/windowscontextmenu.h"
#include "platform/windowsshare.h"
#include "platform/archivehandoff.h"
#include "platform/windowsfolderintegration.h"

#include <algorithm>
#include <atomic>
#include <optional>

#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
#include <QAccessible>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFocusFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QPainter>
#include <QProcess>
#include <QProgressDialog>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSettings>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QStandardPaths>
#include <QStyle>
#include <QSysInfo>
#include <QTabWidget>
#include <QTabBar>
#include <QTimer>
#include <QThread>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QUuid>
#include <QWidgetAction>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::atomic_bool archiveWorkActive{false};

struct ViewColumnCommand
{
    int column;
    const char* sortId;
    const char* visibilityId;
    const char* label;
};

constexpr ViewColumnCommand ViewColumns[] = {
    {0, "view.sort_name", "view.column_name", "Name"},
    {1, "view.sort_size", "view.column_size", "Size"},
    {2, "view.sort_type", "view.column_type", "Type"},
    {3, "view.sort_modified", "view.column_modified", "Date modified"},
    {4, "view.sort_created", "view.column_created", "Date created"},
    {5, "view.sort_extension", "view.column_extension", "Extension"},
    {6, "view.sort_dimensions", "view.column_dimensions", "Dimensions"},
    {7, "view.sort_duration", "view.column_duration", "Duration"},
    {8, "view.sort_title", "view.column_title", "Title"},
    {9, "view.sort_artists", "view.column_artists", "Artists"},
    {10, "view.sort_pages", "view.column_pages", "Pages"},
    {11, nullptr, "view.column_availability", "Availability"},
};

void announceForAccessibility(QObject* target, const QString& message, bool assertive = false)
{
    if (!target || message.isEmpty())
        return;
    QAccessibleAnnouncementEvent event(target, message);
    event.setPoliteness(assertive ? QAccessible::AnnouncementPoliteness::Assertive
                                  : QAccessible::AnnouncementPoliteness::Polite);
    QAccessible::updateAccessibility(&event);
}

void followCommittedFileLabels(QWidget* parent,
    const QList<filemanager::tags::TagPathChange>& changes)
{
    if (changes.isEmpty())
        return;
    filemanager::tags::TagCatalog catalog;
    QString error;
    if (catalog.load(&error) && catalog.relocate(changes, &error))
        return;
    QMessageBox::warning(parent, QObject::tr("File labels need review"),
        QObject::tr("The file operation finished, but some labels could not follow "
                    "their files. Review labels in Details before continuing.\n\n%1")
            .arg(error));
}

} // namespace

namespace {

class StartupProfiler
{
public:
    explicit StartupProfiler(MainWindow::StartupTimings* timings) : timings_(timings)
    {
        if (timings_)
            timer_.start();
    }

    void finish(const char* stage)
    {
        if (!timings_)
            return;
        timings_->insert(QString::fromLatin1(stage), timer_.nsecsElapsed() / 1000);
        timer_.restart();
    }

private:
    MainWindow::StartupTimings* timings_;
    QElapsedTimer timer_;
};

} // namespace

MainWindow::MainWindow(const QString& initialLocation, StartupTimings* timings)
    : initialLocation_(initialLocation)
{
    if (timings)
        timings->clear();
    StartupProfiler profiler(timings);
    diagnosticLog_.record(QStringLiteral("app.started"));
    connect(&diagnosticLog_, &filemanager::diagnostics::AsyncDiagnosticLog::stateChanged,
        this, [this] {
            const QString error = diagnosticLog_.errorString();
            if (!error.isEmpty() && property("lastDiagnosticStorageError").toString() != error)
                statusBar()->showMessage(error, 8000);
            setProperty("lastDiagnosticStorageError", error);
        });
    connect(&diagnosticLog_, &filemanager::diagnostics::AsyncDiagnosticLog::shutdownFinished,
        this, [this] {
            diagnosticShutdownFinished_ = true;
            QTimer::singleShot(0, this, &QWidget::close);
        });
    if (qApp->property("settingsRecovered").toBool())
        diagnosticLog_.record(QStringLiteral("settings.recovered"));
    resize(1024, 792);
    setMinimumSize(900, 580);
    setWindowTitle(QString("File Manager %1")
                       .arg(QStringLiteral(FILE_MANAGER_DISPLAY_VERSION)));
    setWindowIcon(filemanager::ui::BrandIdentity::applicationIcon());
    transferQueue_ = new filemanager::files::TransferQueue(this);
    operationJournal_ = new filemanager::files::OperationJournal({}, this);
    shareController_ = new filemanager::platform::WindowsShareController(this);
    connect(shareController_, &filemanager::platform::WindowsShareController::shareFailed,
        this, [this](const QString& error) {
            diagnosticLog_.record(QStringLiteral("share.failed"));
            statusBar()->clearMessage();
            QMessageBox::warning(this, "Share", error);
        });
    connect(shareController_, &filemanager::platform::WindowsShareController::shareUiShown,
        this, [this](int itemCount, int providerBackedItems) {
            const QString detail = providerBackedItems > 0
                ? tr(" (%1; Windows or the provider may download content)")
                      .arg(filemanager::ui::localizedItems(providerBackedItems))
                : QString{};
            statusBar()->showMessage(tr("Opened Windows Share for %1%2").arg(
                filemanager::ui::localizedItems(itemCount), detail), 7000);
        });
    QString journalError;
    const bool journalLoaded = operationJournal_->load(&journalError);
    connect(transferQueue_, &filemanager::files::TransferQueue::historyChanged,
        operationJournal_, [this] { operationJournal_->synchronize(transferQueue_->history()); });
    connect(transferQueue_, &filemanager::files::TransferQueue::progressDetails,
        operationJournal_, [this](quint64, const auto&) {
            operationJournal_->synchronize(transferQueue_->history());
        });
    connect(transferQueue_, &filemanager::files::TransferQueue::recoveryManifestChanged,
        operationJournal_, [this](quint64) {
            operationJournal_->synchronize(transferQueue_->history());
            QString error;
            if (!operationJournal_->flush(&error))
                statusBar()->showMessage("Recovery manifest could not be saved: " + error, 10000);
        });
    connect(transferQueue_, &filemanager::files::TransferQueue::jobFinished, this,
        [this](quint64, bool cancelled, const QStringList& errors) {
            diagnosticLog_.record(cancelled ? QStringLiteral("transfer.cancelled")
                : errors.isEmpty() ? QStringLiteral("transfer.completed")
                                   : QStringLiteral("transfer.failed"));
        });
    connect(transferQueue_, &filemanager::files::TransferQueue::jobFinished, this,
        [this](quint64 jobId, bool, const QStringList&) {
            const auto history = transferQueue_->history();
            const auto completed = std::find_if(history.cbegin(), history.cend(),
                [jobId](const auto& record) { return record.id == jobId; });
            if (completed == history.cend()
                    || completed->request.operation != filemanager::files::TransferOperation::Move)
                return;
            QList<filemanager::tags::TagPathChange> changes;
            for (const auto& entry : completed->manifest) {
                if (entry.directory || !entry.sourceRemoved || !entry.identityAvailable
                        || !entry.sourceIdentityAvailable || entry.sourcePath.isEmpty()
                        || entry.destinationPath.isEmpty())
                    continue;
                filemanager::files::SourceSnapshot before;
                before.valid = true;
                before.volumeSerial = entry.sourceVolumeSerial;
                before.fileIndex = entry.sourceFileIndex;
                filemanager::files::SourceSnapshot after;
                after.valid = true;
                after.volumeSerial = entry.volumeSerial;
                after.fileIndex = entry.fileIndex;
                changes.append({entry.sourcePath, entry.destinationPath,
                    before, after, false});
            }
            followCommittedFileLabels(this, changes);
        });
    connect(transferQueue_, &filemanager::files::TransferQueue::jobFinished, this,
        [this](quint64 jobId, bool cancelled, const QStringList& errors) {
            const auto retry = recoveryRetries_.find(jobId);
            if (retry == recoveryRetries_.end())
                return;
            const QString interruptedId = retry.value();
            recoveryRetries_.erase(retry);
            if (cancelled || !errors.isEmpty()) {
                statusBar()->showMessage(
                    "Recovery retry did not complete; the interrupted reminder was retained.",
                    10000);
                return;
            }
            QString error;
            if (!operationJournal_->dismissInterrupted({interruptedId}, &error))
                statusBar()->showMessage("Retry completed, but the recovery reminder could not be "
                    "dismissed: " + error, 10000);
            else
                statusBar()->showMessage("Recovery retry completed successfully.", 7000);
        });
    profiler.finish("window_services");
    createCommands();
    profiler.finish("window_commands");
    buildUi(timings);
    profiler.finish("window_ui_total");
    QSettings guidanceSettings;
    if (!guidanceSettings.value("guidance/firstRunHintShown", false).toBool()) {
        guidanceSettings.setValue("guidance/firstRunHintShown", true);
        statusBar()->showMessage(
            "New to File Manager? Open Help > Getting Started for a quick, private tour.", 15000);
    }
    if (filemanager::settings::SessionStore{}.restorationEnabled())
        loadLayout();
    if (!journalLoaded) {
        QTimer::singleShot(0, this, [this, journalError] {
            QMessageBox::warning(this, "Operation history unavailable",
                "The encrypted operation journal could not be opened. New operations will still "
                "run, but crash recovery is unavailable until this is resolved.\n\n" + journalError);
        });
    } else if (!operationJournal_->interruptedRecords().isEmpty()) {
        QTimer::singleShot(0, this, &MainWindow::showInterruptedOperations);
    }
    profiler.finish("window_restoration_guidance");
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    filemanager::platform::applyWindowChrome(this);
}

void MainWindow::createCommands()
{
    using filemanager::commands::Descriptor;
    QSettings shortcutSettings;
    if (shortcutSettings.value("migrations/explorerFunctionKeys", 0).toInt() < 1) {
        const auto migrateOldDefault = [&shortcutSettings](const QString& id,
                                           const QKeySequence& oldDefault,
                                           const QKeySequence& newDefault) {
            const QString key = "shortcuts/" + id;
            if (!shortcutSettings.contains(key))
                return;
            const QKeySequence saved = QKeySequence::fromString(
                shortcutSettings.value(key).toString(), QKeySequence::PortableText);
            if (saved == oldDefault)
                shortcutSettings.setValue(key, newDefault.toString(QKeySequence::PortableText));
        };
        migrateOldDefault("panel.copy_to_other", QKeySequence("F5"),
            QKeySequence("Ctrl+Alt+F5"));
        migrateOldDefault("panel.move_to_other", QKeySequence("F6"),
            QKeySequence("Ctrl+Alt+F6"));
        shortcutSettings.setValue("migrations/explorerFunctionKeys", 1);
    }
    commands_ = new filemanager::commands::CommandRegistry(this);
    auto add = [this](Descriptor descriptor, const QKeySequence& shortcut, auto callback)
    {
        const QString id = descriptor.id;
        QAction* action = commands_->add(std::move(descriptor), shortcut);
        action->setProperty("defaultShortcut", shortcut.toString(QKeySequence::PortableText));
        const QString saved = QSettings().value("shortcuts/" + id).toString();
        if (!saved.isNull())
            action->setShortcut(QKeySequence::fromString(saved, QKeySequence::PortableText));
        connect(action, &QAction::triggered, this, callback);
    };

    add({"navigation.focus_next_region", "Focus next window region", "Navigation",
            {"cycle", "tabs", "navigation pane", "content", "details", "status"}},
        QKeySequence("F6"), [this] { cycleFocusRegion(false); });
    add({"navigation.focus_previous_region", "Focus previous window region", "Navigation",
            {"cycle", "tabs", "navigation pane", "content", "details", "status"}},
        QKeySequence("Shift+F6"), [this] { cycleFocusRegion(true); });
    add({"navigation.address", "Edit folder address", "Navigation", {"path", "location"}},
        QKeySequence("Ctrl+L"), [this] { if (active_) active_->focusLocation(); });
    add({"navigation.goto", "Go to folder", "GoTo", {"path", "location", "fuzzy"}}, {},
        [this] { showGoTo(); });
    add({"navigation.home", "Open Home", "Navigation",
            {"favorites", "recent folders", "saved searches"}},
        QKeySequence("Alt+Home"), [this] { if (active_) active_->navigateHome(); });
    add({"navigation.this_pc", "Open This PC", "Navigation", {"drives", "storage", "computer"}}, {},
        [this] { showThisPc(); });
    add({"navigation.libraries", "Open Libraries", "Navigation",
            {"documents", "music", "pictures", "videos"}}, {},
        [this] { showBuiltInShell(filemanager::platform::BuiltInShellLocation::Libraries); });
    add({"navigation.recycle_bin", "Open Recycle Bin", "Navigation",
            {"deleted", "restore", "trash"}}, {},
        [this] { showBuiltInShell(filemanager::platform::BuiltInShellLocation::RecycleBin); });
    add({"recycle.restore_selected", "Restore selected Recycle Bin items…", "Folder",
            {"undelete", "original location", "trash"}}, {},
        [this] { restoreSelectedRecycleBinItems(); });
    add({"recycle.empty", "Empty Recycle Bin…", "Folder",
            {"delete all", "permanent", "trash"}}, {},
        [this] { emptyRecycleBin(); });
    add({"navigation.storage_details", "Show storage details", "Navigation",
            {"drives", "capacity", "free space"}}, {},
        [this] { showStorageDetails(); });
    add({"navigation.network", "Open network locations", "Navigation", {"lan", "unc", "shares"}}, {},
        [this] { showNetworkBrowser(); });
    add({"navigation.map_network_drive", "Map network drive…", "Navigation",
            {"windows", "share", "drive letter", "connect"}}, {},
        [this] { mapNetworkDrive(); });
    add({"navigation.disconnect_network_drive", "Disconnect network drive…", "Navigation",
            {"windows", "share", "drive letter", "remove"}}, {},
        [this] { disconnectNetworkDrive(); });
    add({"search.files", "Search files", "Folder", {"find", "fuzzy"}}, QKeySequence("Ctrl+Shift+F"),
        [this] { showSearch(); });
    add({"search.contents", "Search file contents", "Search", {"text", "inside files"}},
        QKeySequence("Ctrl+Shift+G"), [this] { showContentSearch(); });
    add({"folder.open", "Open selected", "Folder", {}}, QKeySequence::Open,
        [this] { openSelected(); });
    add({"folder.new", "New folder", "Folder", {"mkdir"}}, QKeySequence("Ctrl+Shift+N"),
        [this] { createFolder(); });
    add({"file.new_text", "New text file", "Folder", {}}, QKeySequence("Ctrl+Alt+N"),
        [this] { createTextFile(); });
    add({"file.create_shortcut", "Create shortcut", "Folder", {"link", "lnk"}}, {},
        [this] { createShortcuts(); });
    add({"file.rename", "Rename selected", "Rename", {}}, QKeySequence("F2"),
        [this] { renameSelected(); });
    add({"edit.undo", "Undo", "Edit", {"restore", "rename"}}, QKeySequence::Undo,
        [this] { undoLastSessionOperation(); });
    commands_->action("edit.undo")->setEnabled(false);
    add({"file.batch_rename", "Batch rename", "Rename", {}}, QKeySequence("Ctrl+F2"),
        [this] { batchRename(); });
    add({"file.edit_tags", "Tag selected files…", "Tools", {"tags", "labels", "identify"}}, {},
        [this] { editSelectedTags(); });
    add({"file.delete", "Move to Recycle Bin", "Folder", {"delete", "trash"}}, QKeySequence::Delete,
        [this] { deleteSelected(); });
    add({"file.delete_permanently", "Delete permanently…", "Folder",
            {"shift delete", "bypass recycle bin", "irreversible"}},
        QKeySequence("Shift+Delete"), [this] { permanentlyDeleteSelected(); });
    add({"clipboard.cut", "Cut", "Folder", {}}, QKeySequence::Cut, [this] { copySelection(true); });
    add({"clipboard.copy", "Copy", "Folder", {}}, QKeySequence::Copy,
        [this] { copySelection(false); });
    add({"clipboard.copy_filename", "Copy filename", "Folder", {"name"}},
        QKeySequence("Ctrl+Alt+C"), [this] {
            const QString path = active_->selectedPath();
            if (!path.isEmpty())
                QApplication::clipboard()->setText(QFileInfo(path).fileName());
        });
    add({"clipboard.paste", "Paste", "Folder", {}}, QKeySequence::Paste,
        [this] { pasteClipboard(); });
    add({"selection.all", "Select all", "Selection", {"everything"}}, QKeySequence::SelectAll,
        [this] { active_->selectAll(); });
    add({"selection.none", "Select none", "Selection", {"clear", "deselect"}},
        QKeySequence("Ctrl+Shift+A"), [this] { active_->clearSelection(); });
    add({"selection.invert", "Invert selection", "Selection", {"toggle"}}, {},
        [this] { active_->invertSelection(); });
    add({"view.refresh", "Refresh", "View", {"reload"}}, QKeySequence::Refresh,
        [this]
        {
            active_->refresh();
            updateStatus();
        });
    add({"view.details", "Details view", "View", {"columns"}}, QKeySequence("Ctrl+1"),
        [this] { active_->setViewMode(FilePane::ViewMode::Details); syncViewCommands(); });
    add({"view.list", "List view", "View", {}}, QKeySequence("Ctrl+2"),
        [this] { active_->setViewMode(FilePane::ViewMode::List); syncViewCommands(); });
    add({"view.large_icons", "Large icons", "View", {"grid", "thumbnails"}},
        QKeySequence("Ctrl+3"), [this] { active_->setViewMode(FilePane::ViewMode::LargeIcons); syncViewCommands(); });
    add({"view.extra_large_icons", "Extra large icons", "View", {"grid", "thumbnails"}},
        QKeySequence("Ctrl+4"),
        [this] { active_->setViewMode(FilePane::ViewMode::ExtraLargeIcons); syncViewCommands(); });
    add({"view.medium_icons", "Medium icons", "View", {"grid"}}, {},
        [this] { active_->setViewMode(FilePane::ViewMode::MediumIcons); syncViewCommands(); });
    add({"view.small_icons", "Small icons", "View", {}}, {},
        [this] { active_->setViewMode(FilePane::ViewMode::SmallIcons); syncViewCommands(); });
    add({"view.folders_first", "Folders first", "View", {"sort folders"}}, {},
        [this] { active_->setFolderPlacement(FilePane::FolderPlacement::First); syncViewCommands(); });
    add({"view.folders_last", "Folders last", "View", {"sort folders"}}, {},
        [this] { active_->setFolderPlacement(FilePane::FolderPlacement::Last); syncViewCommands(); });
    add({"view.folders_mixed", "Mix folders and files", "View", {"sort folders"}}, {},
        [this] { active_->setFolderPlacement(FilePane::FolderPlacement::Mixed); syncViewCommands(); });
    add({"view.group_none", "Do not group", "View", {"group by", "ungroup"}}, {},
        [this] { active_->setGroupBy(FilePane::GroupBy::None); syncViewCommands(); });
    add({"view.group_name", "Group by name", "View", {"alphabetical", "letters"}}, {},
        [this] { active_->setGroupBy(FilePane::GroupBy::Name); syncViewCommands(); });
    add({"view.group_type", "Group by type", "View", {"extension", "kind"}}, {},
        [this] { active_->setGroupBy(FilePane::GroupBy::Type); syncViewCommands(); });
    add({"view.group_modified", "Group by date modified", "View", {"date", "time"}}, {},
        [this] { active_->setGroupBy(FilePane::GroupBy::DateModified); syncViewCommands(); });
    add({"view.group_size", "Group by size", "View", {"large", "small"}}, {},
        [this] { active_->setGroupBy(FilePane::GroupBy::Size); syncViewCommands(); });
    for (const auto& column : ViewColumns) {
        if (column.sortId) {
            add({QString::fromLatin1(column.sortId),
                    tr("Sort by %1").arg(QString::fromLatin1(column.label)), "View",
                    {"sort", "order", QString::fromLatin1(column.label)}}, {},
                [this, index = column.column] {
                    active_->setSort(index, active_->sortOrder());
                    syncViewCommands();
                });
        }
        add({QString::fromLatin1(column.visibilityId),
                tr("Show %1 column").arg(QString::fromLatin1(column.label)), "View",
                {"column", "details", QString::fromLatin1(column.label)}}, {},
            [this, index = column.column, id = QString::fromLatin1(column.visibilityId)] {
                active_->setColumnVisible(index, commands_->action(id)->isChecked());
                syncViewCommands();
            });
        commands_->action(QString::fromLatin1(column.visibilityId))->setCheckable(true);
    }
    add({"view.sort_ascending", "Sort ascending", "View", {"a to z", "smallest", "oldest"}}, {},
        [this] { active_->setSort(active_->sortColumn(), Qt::AscendingOrder); syncViewCommands(); });
    add({"view.sort_descending", "Sort descending", "View", {"z to a", "largest", "newest"}}, {},
        [this] { active_->setSort(active_->sortColumn(), Qt::DescendingOrder); syncViewCommands(); });
    add({"view.file_extensions", "Show file extensions", "View", {"suffixes"}}, {},
        [this] {
            active_->setShowFileExtensions(commands_->action("view.file_extensions")->isChecked());
        });
    commands_->action("view.file_extensions")->setCheckable(true);
    commands_->action("view.file_extensions")->setChecked(true);
    add({"view.hidden", "Hidden and system items", "View", {"show hidden files"}}, {},
        [this] {
            active_->setShowHidden(commands_->action("view.hidden")->isChecked());
        });
    commands_->action("view.hidden")->setCheckable(true);
    add({"view.navigation_pane", "Navigation pane", "View", {"sidebar", "folders"}}, {},
        [this] {
            const bool visible = commands_->action("view.navigation_pane")->isChecked();
            if (navigationPane_)
                navigationPane_->setVisible(visible);
            QSettings().setValue("view/navigationPaneVisible", visible);
        });
    commands_->action("view.navigation_pane")->setCheckable(true);
    commands_->action("view.navigation_pane")->setChecked(
        QSettings().value("view/navigationPaneVisible", true).toBool());

    const QStringList viewModes = {"view.details", "view.list", "view.large_icons",
        "view.extra_large_icons", "view.medium_icons", "view.small_icons"};
    auto* viewModeGroup = new QActionGroup(this);
    viewModeGroup->setExclusive(true);
    for (const QString& id : viewModes) {
        commands_->action(id)->setCheckable(true);
        viewModeGroup->addAction(commands_->action(id));
    }
    const QStringList folderPlacements = {
        "view.folders_first", "view.folders_last", "view.folders_mixed"};
    auto* folderPlacementGroup = new QActionGroup(this);
    folderPlacementGroup->setExclusive(true);
    for (const QString& id : folderPlacements) {
        commands_->action(id)->setCheckable(true);
        folderPlacementGroup->addAction(commands_->action(id));
    }
    const QStringList groupingModes = {"view.group_none", "view.group_name", "view.group_type",
        "view.group_modified", "view.group_size"};
    auto* groupingGroup = new QActionGroup(this);
    groupingGroup->setExclusive(true);
    for (const QString& id : groupingModes) {
        commands_->action(id)->setCheckable(true);
        groupingGroup->addAction(commands_->action(id));
    }
    auto* sortPropertyGroup = new QActionGroup(this);
    sortPropertyGroup->setExclusive(true);
    for (const auto& column : ViewColumns) {
        if (!column.sortId)
            continue;
        QAction* action = commands_->action(QString::fromLatin1(column.sortId));
        action->setCheckable(true);
        sortPropertyGroup->addAction(action);
    }
    auto* sortDirectionGroup = new QActionGroup(this);
    sortDirectionGroup->setExclusive(true);
    for (const QString& id : {QStringLiteral("view.sort_ascending"),
             QStringLiteral("view.sort_descending")}) {
        QAction* action = commands_->action(id);
        action->setCheckable(true);
        sortDirectionGroup->addAction(action);
    }
    add({"file.properties", "Properties", "Folder", {"details"}}, QKeySequence("Alt+Return"),
        [this] { showProperties(); });
    add({"file.details", "Details inspector", "Inspector", {"metadata", "attributes"}},
        QKeySequence("Ctrl+Alt+Return"), [this] { showDetails(); });
    add({"file.analyze", "Analyze size and checksum", "Tools", {"sha256", "hash", "folder size"}},
        QKeySequence("Ctrl+Shift+H"), [this] { analyzeSelected(); });
    add({"tools.duplicates", "Find duplicate files", "Tools", {"deduplicate", "same files"}},
        QKeySequence("Ctrl+Shift+D"), [this] { findDuplicateFiles(); });
    add({"tools.tv_organizer", "Media Organizer", "Tools", {"media", "tv", "series", "season", "episodes"}}, {},
        [this] {
            filemanager::ui::TvOrganizerDialog dialog(
                active_ ? active_->currentPath() : QString{}, this, transferQueue_);
            if (dialog.exec() == QDialog::Accepted && dialog.queuedJobId() != 0)
                showTransfers();
        });
    add({"archive.create_zip", "Create ZIP archive", "Archive", {"compress", "zip"}}, {},
        [this] { createZipArchive(); });
    add({"archive.extract_zip", "Extract ZIP archive", "Archive", {"unzip", "decompress"}}, {},
        [this] { extractZipArchive(); });
    add({"archive.open_native", "Open RAR/7z/TAR using Windows", "Archive",
            {"rar", "7z", "tar", "provider", "extract"}}, {},
        [this] { openNativeArchive(); });
    commands_->action("archive.open_native")->setToolTip(
        "Hand the selected archive to the Windows-selected handler. File Manager does not extract it.");
    add({"file.share", "Share…", "Folder", {"windows share", "send"}}, {},
        [this] { shareSelected(); });
    add({"folder.terminal", "Open terminal here", "Folder", {"console", "shell"}}, {},
        [this] { openTerminal(); });
    add({"layout.save", "Save named layout…", "Panels", {"workspace", "snapshot"}},
        QKeySequence("Ctrl+Shift+S"), [this] { saveNamedLayout(); });
    add({"layout.load", "Named layouts…", "Panels", {"workspace", "restore", "manage"}},
        QKeySequence("Ctrl+Shift+L"), [this] { manageNamedLayouts(); });
    add({"options.open", "Preferences", "Options", {"settings"}}, QKeySequence("Ctrl+,"),
        [this] { showPreferences(); });
    add({"options.shortcuts", "Keyboard shortcuts", "Options", {"keys", "bindings"}}, {},
        [this] { showShortcutEditor(); });
    add({"help.browse", "Browse help", "Help", {"guide", "instructions", "support"}}, {},
        [this] { showHelpBrowser(); });
    add({"transfers.open", "Operation center", "Transfers", {"queue", "operations", "recovery"}},
        QKeySequence("Ctrl+Shift+J"), [this] { showTransfers(); });
    add({"tab.new", "New folder tab", "Tabs", {"new tab"}}, QKeySequence("Ctrl+T"),
        [this] { newFolderTab(); });
    add({"tab.close", "Close folder tab", "Tabs", {"close tab"}}, QKeySequence("Ctrl+W"),
        [this] { closeFolderTab(); });
    add({"tab.duplicate", "Duplicate folder tab", "Tabs", {"clone tab"}},
        QKeySequence("Ctrl+Alt+T"), [this] { duplicateFolderTab(); });
    add({"tab.reopen", "Reopen closed tab", "Tabs", {"restore tab"}},
        QKeySequence("Ctrl+Shift+T"), [this] { reopenClosedTab(); });
    add({"folder.open_new_tab", "Open folder in new tab", "Tabs", {}},
        QKeySequence("Ctrl+Enter"), [this] { openSelectedFolderInTab(); });
    add({"folder.open_new_window", "Open folder in new window", "Tabs", {}},
        QKeySequence("Ctrl+Shift+Enter"), [this] { openSelectedFolderInWindow(); });
    add({"favorites.add", "Add current folder to favorites", "Navigation", {"bookmark"}},
        QKeySequence("Ctrl+D"), [this] { addCurrentFolderFavorite(); });
    add({"favorites.remove", "Remove selected favorite", "Navigation", {"unbookmark"}}, {},
        [this] { removeSelectedFavorite(); });
    add({"favorites.move_up", "Move favorite up", "Navigation", {"reorder bookmark"}},
        QKeySequence("Alt+Shift+Up"), [this] { moveSelectedFavorite(-1); });
    add({"favorites.move_down", "Move favorite down", "Navigation", {"reorder bookmark"}},
        QKeySequence("Alt+Shift+Down"), [this] { moveSelectedFavorite(1); });
    add({"favorites.groups", "Manage favorite groups", "Navigation", {"bookmarks", "organize"}}, {},
        [this] { manageFavoriteGroups(); });
    add({"commands.open", "Command palette", "Misc", {"actions"}}, QKeySequence("Ctrl+Shift+P"),
        [this] { showCommandPalette(); });
    add({"panel.copy_to_other", "Copy to other panel", "Panels", {}},
        QKeySequence("Ctrl+Alt+F5"),
        [this] { copyToOther(false); });
    add({"panel.move_to_other", "Move to other panel", "Panels", {}},
        QKeySequence("Ctrl+Alt+F6"),
        [this] { copyToOther(true); });
    add({"panel.split_right", "Split panel right", "Panels", {"horizontal"}},
        QKeySequence("Ctrl+\\"), [this] { workspace_->splitActive(Qt::Horizontal); });
    add({"panel.split_bottom", "Split panel below", "Panels", {"vertical"}},
        QKeySequence("Ctrl+Shift+\\"), [this] { workspace_->splitActive(Qt::Vertical); });
    add({"panel.close", "Close panel", "Panels", {}}, QKeySequence("Ctrl+Shift+W"),
        [this] { workspace_->closeActivePanel(); });
    add({"tab.move_to_other", "Move tab to next panel", "Tabs", {"transfer tab"}},
        QKeySequence("Ctrl+Shift+Right"),
        [this]
        {
            if (!workspace_->moveActiveTabToAdjacent())
                QMessageBox::information(this, "Move tab",
                                         "Create another panel before moving a tab.");
        });
    add({"tab.move_left", "Move tab left", "Tabs", {"reorder tab"}},
        QKeySequence("Ctrl+Shift+PgUp"), [this] { workspace_->activePanel()->moveCurrentTab(-1); });
    add({"tab.move_right", "Move tab right", "Tabs", {"reorder tab"}},
        QKeySequence("Ctrl+Shift+PgDown"), [this] { workspace_->activePanel()->moveCurrentTab(1); });
    add({"panel.inspector", "Toggle inspector", "Inspector", {"preview"}}, QKeySequence("Ctrl+I"),
        [this] { preview_->setVisible(!preview_->isVisible()); });

    using filemanager::ui::Symbol;
    const QList<QPair<QString, Symbol>> icons = {
        {"navigation.goto", Symbol::Folder}, {"search.files", Symbol::Search},
        {"search.contents", Symbol::ContentSearch},
        {"commands.open", Symbol::Command}, {"folder.new", Symbol::NewFolder},
        {"file.new_text", Symbol::File}, {"file.rename", Symbol::Rename},
        {"file.batch_rename", Symbol::Batch}, {"edit.undo", Symbol::Undo},
        {"panel.copy_to_other", Symbol::Copy},
        {"panel.move_to_other", Symbol::Move}, {"file.delete", Symbol::Trash},
        {"file.delete_permanently", Symbol::Trash},
        {"folder.open", Symbol::Open}, {"panel.inspector", Symbol::Inspector},
        {"panel.split_right", Symbol::Split}, {"transfers.open", Symbol::Transfers}};
    for (const auto& [id, symbol] : icons)
        commands_->action(id)->setIcon(filemanager::ui::symbolIcon(symbol));
}

void MainWindow::buildUi(StartupTimings* timings)
{
    StartupProfiler profiler(timings);
    auto* toolbar = addToolBar("Actions");
    toolbar->setObjectName("MainActionsToolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(20, 20));
    toolbar->addAction(commands_->action("navigation.goto"));
    toolbar->addAction(commands_->action("search.files"));
    toolbar->addAction(commands_->action("search.contents"));
    toolbar->addAction(commands_->action("commands.open"));
    toolbar->addSeparator();
    toolbar->addAction(commands_->action("folder.new"));
    toolbar->addAction(commands_->action("file.rename"));
    toolbar->addAction(commands_->action("file.batch_rename"));
    toolbar->addAction(commands_->action("edit.undo"));
    toolbar->addAction(commands_->action("file.delete"));
    toolbar->addSeparator();
    auto* inspectorAction = commands_->action("panel.inspector");
    toolbar->addAction(inspectorAction);
    inspectorAction->setCheckable(true);
    toolbar->addAction(commands_->action("panel.split_right"));
    commands_->action("search.contents")->setToolTip("Search inside files in this folder");
    commands_->action("commands.open")->setToolTip("Find any command (Ctrl+Shift+P)");
    commands_->action("file.batch_rename")->setToolTip("Rename multiple selected files (Ctrl+F2)");

    auto* fileMenu = menuBar()->addMenu("File");
    fileMenu->addAction(commands_->action("folder.open"));
    fileMenu->addAction(commands_->action("folder.new"));
    fileMenu->addAction(commands_->action("file.new_text"));
    fileMenu->addAction(commands_->action("file.create_shortcut"));
    fileMenu->addSeparator();
    fileMenu->addAction(commands_->action("file.properties"));
    fileMenu->addAction(commands_->action("file.details"));
    fileMenu->addAction(commands_->action("file.analyze"));
    fileMenu->addAction(commands_->action("tools.duplicates"));
    fileMenu->addAction(commands_->action("tools.tv_organizer"));
    fileMenu->addAction(commands_->action("file.edit_tags"));
    fileMenu->addAction(commands_->action("file.share"));
    auto* networkDriveMenu = fileMenu->addMenu("Network drives");
    networkDriveMenu->setObjectName("file.network_drives");
    auto* mapNetworkDriveAction = commands_->action("navigation.map_network_drive");
    auto* disconnectNetworkDriveAction = commands_->action("navigation.disconnect_network_drive");
    mapNetworkDriveAction->setToolTip(
        "Open the Windows Map Network Drive dialog. File Manager does not store credentials.");
    disconnectNetworkDriveAction->setToolTip(
        "Open the Windows Disconnect Network Drive dialog.");
    networkDriveMenu->setToolTipsVisible(true);
    networkDriveMenu->addAction(mapNetworkDriveAction);
    networkDriveMenu->addAction(disconnectNetworkDriveAction);
    auto* recycleBinMenu = fileMenu->addMenu("Recycle Bin");
    recycleBinMenu->setObjectName("file.recycle_bin");
    recycleBinMenu->addAction(commands_->action("recycle.restore_selected"));
    recycleBinMenu->addAction(commands_->action("recycle.empty"));
    auto* archiveMenu = fileMenu->addMenu("Archive");
    archiveMenu->addAction(commands_->action("archive.create_zip"));
    archiveMenu->addAction(commands_->action("archive.extract_zip"));
    archiveMenu->addSeparator();
    archiveMenu->addAction(commands_->action("archive.open_native"));
    fileMenu->addAction(commands_->action("folder.terminal"));

    auto* editMenu = menuBar()->addMenu("Edit");
    editMenu->addAction(commands_->action("edit.undo"));
    editMenu->addSeparator();
    editMenu->addAction(commands_->action("clipboard.cut"));
    editMenu->addAction(commands_->action("clipboard.copy"));
    editMenu->addAction(commands_->action("clipboard.paste"));
    editMenu->addAction(commands_->action("clipboard.copy_filename"));
    editMenu->addSeparator();
    editMenu->addAction(commands_->action("file.delete"));
    editMenu->addAction(commands_->action("file.delete_permanently"));
    editMenu->addSeparator();
    editMenu->addAction(commands_->action("selection.all"));
    editMenu->addAction(commands_->action("selection.none"));
    editMenu->addAction(commands_->action("selection.invert"));

    auto* displayMenu = menuBar()->addMenu("View");
    displayMenu->addAction(commands_->action("view.refresh"));
    displayMenu->addAction(commands_->action("view.details"));
    displayMenu->addAction(commands_->action("view.list"));
    displayMenu->addAction(commands_->action("view.large_icons"));
    displayMenu->addAction(commands_->action("view.extra_large_icons"));
    displayMenu->addAction(commands_->action("view.medium_icons"));
    displayMenu->addAction(commands_->action("view.small_icons"));
    auto* folderOrder = displayMenu->addMenu("Folder placement");
    folderOrder->addAction(commands_->action("view.folders_first"));
    folderOrder->addAction(commands_->action("view.folders_last"));
    folderOrder->addAction(commands_->action("view.folders_mixed"));
    auto* sortBy = displayMenu->addMenu("Sort by");
    for (const auto& column : ViewColumns) {
        if (column.sortId)
            sortBy->addAction(commands_->action(QString::fromLatin1(column.sortId)));
    }
    sortBy->addSeparator();
    sortBy->addAction(commands_->action("view.sort_ascending"));
    sortBy->addAction(commands_->action("view.sort_descending"));
    auto* groupBy = displayMenu->addMenu("Group by");
    groupBy->addAction(commands_->action("view.group_none"));
    groupBy->addAction(commands_->action("view.group_name"));
    groupBy->addAction(commands_->action("view.group_type"));
    groupBy->addAction(commands_->action("view.group_modified"));
    groupBy->addAction(commands_->action("view.group_size"));
    auto* columns = displayMenu->addMenu("Columns");
    columns->setToolTipsVisible(true);
    for (const auto& column : ViewColumns) {
        QAction* action = commands_->action(QString::fromLatin1(column.visibilityId));
        if (column.column == 0) {
            action->setToolTip("Name is required so every item remains identifiable.");
            action->setEnabled(false);
        }
        columns->addAction(action);
    }
    displayMenu->addAction(commands_->action("view.file_extensions"));
    displayMenu->addAction(commands_->action("view.hidden"));
    displayMenu->addSeparator();
    displayMenu->addAction(commands_->action("view.navigation_pane"));
    displayMenu->addSeparator();
    displayMenu->addAction(commands_->action("navigation.focus_next_region"));
    displayMenu->addAction(commands_->action("navigation.focus_previous_region"));

    auto* viewMenu = menuBar()->addMenu("Workspace");
    viewMenu->addAction(commands_->action("tab.new"));
    viewMenu->addAction(commands_->action("tab.close"));
    viewMenu->addAction(commands_->action("tab.duplicate"));
    viewMenu->addAction(commands_->action("tab.reopen"));
    viewMenu->addAction(commands_->action("favorites.add"));
    viewMenu->addAction(commands_->action("favorites.remove"));
    viewMenu->addAction(commands_->action("favorites.groups"));
    viewMenu->addSeparator();
    viewMenu->addAction(commands_->action("panel.split_right"));
    viewMenu->addAction(commands_->action("panel.split_bottom"));
    viewMenu->addAction(commands_->action("panel.close"));
    viewMenu->addAction(commands_->action("tab.move_to_other"));
    viewMenu->addAction(commands_->action("layout.save"));
    viewMenu->addAction(commands_->action("layout.load"));
    viewMenu->addSeparator();
    viewMenu->addAction(commands_->action("options.open"));
    viewMenu->addAction(commands_->action("options.shortcuts"));
    viewMenu->addAction(commands_->action("transfers.open"));

    auto* helpMenu = menuBar()->addMenu("Help");
    helpMenu->addAction(commands_->action("help.browse"));
    helpMenu->addSeparator();
    QAction* gettingStarted = new QAction("Getting Started…", this);
    gettingStarted->setObjectName("help.getting_started");
    connect(gettingStarted, &QAction::triggered, this, &MainWindow::showGettingStarted);
    QAction* batchRenameHelp = new QAction("Batch Rename…", this);
    batchRenameHelp->setObjectName("help.batch_rename");
    connect(batchRenameHelp, &QAction::triggered, this, [this] {
        filemanager::ui::showBatchRenameHelp(this, commands_->action("file.batch_rename")->shortcut());
    });
    QAction* recoveryGuidance = new QAction("Recovery & Safety…", this);
    QAction* organizerHelp = new QAction("Media Organizer…", this);
    organizerHelp->setObjectName("help.tv_organizer");
    connect(organizerHelp, &QAction::triggered, this, [this] {
        filemanager::ui::showTvOrganizerHelp(this);
    });
    recoveryGuidance->setObjectName("help.recovery_safety");
    connect(recoveryGuidance, &QAction::triggered, this, &MainWindow::showRecoveryGuidance);
    QAction* aboutSupport = helpMenu->addAction("About & Support…");
    aboutSupport->setObjectName("help.about_support");
    connect(aboutSupport, &QAction::triggered, this, &MainWindow::showAboutSupport);

    profiler.finish("ui_menus");
    auto* shell = new QSplitter(Qt::Horizontal, this);
    shell->setChildrenCollapsible(false);
    auto* sidebar = new QWidget(shell);
    navigationPane_ = sidebar;
    sidebar->setMinimumWidth(170);
    sidebar->setMaximumWidth(360);
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(5, 7, 5, 5);
    sideLayout->setSpacing(0);
    auto* brand = new QLabel(sidebar);
    brand->setObjectName("BrandLogo");
    brand->setAccessibleName(tr("%1 logo").arg(filemanager::ui::BrandIdentity::productName()));
    brand->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    brand->setMinimumHeight(52);
    filemanager::ui::BrandIdentity::setLogo(brand, QSize(148, 44));
    if (brand->pixmap().isNull()) {
        brand->setText(filemanager::ui::BrandIdentity::productName().toUpper());
        brand->setObjectName("Section");
    }
    sideLayout->addWidget(brand);
    quickAccess_ = new filemanager::ui::NavigationTree(sidebar);
    profiler.finish("ui_sidebar_widgets");
    storageVolumes_ = filemanager::files::mountedStorageVolumes();
    storageRoots_.reserve(storageVolumes_.size());
    for (const auto& volume : storageVolumes_)
        storageRoots_.append(volume.rootPath.toCaseFolded());
    profiler.finish("ui_storage_snapshot");
    rebuildQuickAccess();
    profiler.finish("ui_navigation_nodes");
    auto* storageRefresh = new QTimer(this);
    storageRefresh->setInterval(3000);
    connect(storageRefresh, &QTimer::timeout, this, &MainWindow::refreshStorageLocations);
    storageRefresh->start();
    connect(quickAccess_, &filemanager::ui::NavigationTree::favoriteOrderChanged, this, [this] {
        QSettings settings;
        auto favorites = filemanager::settings::loadFavoriteGroups(settings);
        const auto order = quickAccess_->favoriteOrder();
        for (auto& group : favorites.groups) {
            if (order.contains(group.id))
                group.paths = order.value(group.id);
        }
        filemanager::settings::saveFavoriteGroups(settings, favorites);
    });
    sideLayout->addWidget(quickAccess_, 1);
    sidebar->setVisible(commands_->action("view.navigation_pane")->isChecked());

    auto* horizontal = new QSplitter(Qt::Horizontal, shell);
    QSettings startupSettings;
    const QString startupLocation = initialLocation_.isEmpty()
        ? filemanager::settings::startupLocation(startupSettings) : initialLocation_;
    profiler.finish("ui_startup_settings");
    workspace_ = new filemanager::workspace::WorkspaceView(startupLocation, horizontal);
    profiler.finish("ui_workspace");
    active_ = workspace_->currentPane();
    preview_ = new QWidget(horizontal);
    preview_->setObjectName("DetailsPane");
    preview_->setAccessibleName("Details pane");
    preview_->setAccessibleDescription(
        "Information and preview for the selected file or folder");
    preview_->setFocusPolicy(Qt::StrongFocus);
    preview_->setMinimumWidth(250);
    auto* pv = new QVBoxLayout(preview_);
    pv->setContentsMargins(18, 18, 18, 18);
    auto* heading = new QLabel("PREVIEW", preview_);
    heading->setObjectName("Muted");
    pv->addWidget(heading);
    previewTitle_ = new QLabel("Select a file", preview_);
    previewTitle_->setObjectName("Title");
    previewTitle_->setAccessibleName("Preview title");
    previewTitle_->setTextFormat(Qt::PlainText);
    previewTitle_->setWordWrap(true);
    pv->addWidget(previewTitle_);
    previewMeta_ = new QLabel("", preview_);
    previewMeta_->setObjectName("Muted");
    previewMeta_->setAccessibleName("Preview metadata");
    previewMeta_->setTextFormat(Qt::PlainText);
    previewMeta_->setWordWrap(true);
    pv->addWidget(previewMeta_);
    previewImage_ = new QLabel(preview_);
    previewImage_->setAlignment(Qt::AlignCenter);
    previewImage_->setMinimumHeight(200);
    previewImage_->setScaledContents(false);
    filemanager::ui::BrandIdentity::setLogo(previewImage_, QSize(220, 220));
    pv->addWidget(previewImage_, 1);
    previewText_ = new QLabel(preview_);
    previewText_->setAccessibleName("Preview contents");
    previewText_->setTextFormat(Qt::PlainText);
    previewText_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    previewText_->setWordWrap(true);
    previewText_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pv->addWidget(previewText_, 1);
    horizontal->addWidget(workspace_);
    horizontal->addWidget(preview_);
    horizontal->setStretchFactor(0, 4);
    horizontal->setStretchFactor(1, 1);
    preview_->hide();
    previewController_ = new filemanager::preview::PreviewController(this);
    documentMetadata_ = new filemanager::preview::DocumentMetadataProvider(this);
    mediaMetadata_ = new filemanager::preview::MediaMetadataProvider(this);
    shellMetadata_ = new filemanager::preview::ShellMetadataProvider(this);
    connect(previewController_, &filemanager::preview::PreviewController::ready, this,
        [this](quint64 generation, const QString&, const filemanager::preview::PreviewData& preview) {
            if (generation != previewGeneration_)
                return;
            previewTitle_->setText(preview.title);
            previewBaseMetadata_ = preview.metadata;
            previewText_->setText(preview.text);
            if (!preview.image.isNull())
                previewImage_->setPixmap(QPixmap::fromImage(preview.image).scaled(
                    340, 420, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            refreshRichPreviewMetadata();
        });
    const auto metadataReady = [this](const QString& path) {
        if (QFileInfo(path).absoluteFilePath().compare(
                QFileInfo(previewPath_).absoluteFilePath(), Qt::CaseInsensitive) == 0)
            refreshRichPreviewMetadata();
    };
    connect(documentMetadata_, &filemanager::preview::DocumentMetadataProvider::metadataReady,
        this, metadataReady);
    connect(mediaMetadata_, &filemanager::preview::MediaMetadataProvider::metadataReady,
        this, metadataReady);
    connect(shellMetadata_, &filemanager::preview::ShellMetadataProvider::metadataReady,
        this, metadataReady);
    shell->addWidget(sidebar);
    shell->addWidget(horizontal);
    shell->setSizes({226, 798});
    workspaceSplitter_ = shell;
    setCentralWidget(shell);
    statusBar()->setObjectName("ApplicationStatusBar");
    statusBar()->setAccessibleName("Status bar");
    statusBar()->setAccessibleDescription(
        "Item count, selection count, and current location or operation status");
    statusBar()->setFocusPolicy(Qt::StrongFocus);
    auto* detailsFocusFrame = new QFocusFrame(this);
    detailsFocusFrame->setWidget(preview_);
    auto* statusFocusFrame = new QFocusFrame(this);
    statusFocusFrame->setWidget(statusBar());
    profiler.finish("ui_details_layout");
    setActivePane(active_);
    connectPane(active_);
    connect(workspace_, &filemanager::workspace::WorkspaceView::paneCreated, this,
            &MainWindow::connectPane);
    connect(workspace_, &filemanager::workspace::WorkspaceView::activePaneChanged, this,
            &MainWindow::setActivePane);
    connect(workspace_, &filemanager::workspace::WorkspaceView::finalTabCloseRequested,
        this, &QWidget::close);
    connect(quickAccess_, &filemanager::ui::NavigationTree::locationActivated,
        this, [this](const QString& path) { if (active_) active_->navigate(path); });
    connect(quickAccess_, &filemanager::ui::NavigationTree::shellLocationActivated,
        this, [this](const QByteArray& identity) {
            openShellTreeLocation(identity, ShellTreeTarget::CurrentPane);
        });
    connect(quickAccess_, &filemanager::ui::NavigationTree::shellBackgroundRequested,
        this, [this](const QByteArray& identity) {
            openShellTreeLocation(identity, ShellTreeTarget::BackgroundTab);
        });
    connect(quickAccess_, &filemanager::ui::NavigationTree::homeActivated,
        this, [this] { if (active_) active_->navigateHome(); });
    connect(quickAccess_, &filemanager::ui::NavigationTree::homeBackgroundRequested,
        this, [this] {
            if (workspace_->activePanel())
                workspace_->activePanel()->addTab(QLatin1String(FilePane::HomeIdentity), false);
        });
    connect(quickAccess_, &filemanager::ui::NavigationTree::backgroundLocationRequested,
        this, [this](const QString& path) {
            if (workspace_->activePanel())
                workspace_->activePanel()->addTab(path, false);
        });
    connect(quickAccess_, &filemanager::ui::NavigationTree::thisPcActivated,
        this, &MainWindow::showThisPc);
    connect(quickAccess_, &filemanager::ui::NavigationTree::thisPcBackgroundRequested,
        this, [this] {
            if (workspace_->activePanel())
                workspace_->activePanel()->addTab(filemanager::platform::ShellItem::builtInIdentity(
                    filemanager::platform::BuiltInShellLocation::ThisPc), false);
        });
    connect(quickAccess_, &filemanager::ui::NavigationTree::librariesActivated,
        this, [this] {
            showBuiltInShell(filemanager::platform::BuiltInShellLocation::Libraries);
        });
    connect(quickAccess_, &filemanager::ui::NavigationTree::librariesBackgroundRequested,
        this, [this] {
            if (workspace_->activePanel())
                workspace_->activePanel()->addTab(filemanager::platform::ShellItem::builtInIdentity(
                    filemanager::platform::BuiltInShellLocation::Libraries), false);
        });
    connect(quickAccess_, &filemanager::ui::NavigationTree::recycleBinActivated,
        this, [this] {
            showBuiltInShell(filemanager::platform::BuiltInShellLocation::RecycleBin);
        });
    connect(quickAccess_, &filemanager::ui::NavigationTree::recycleBinBackgroundRequested,
        this, [this] {
            if (workspace_->activePanel())
                workspace_->activePanel()->addTab(filemanager::platform::ShellItem::builtInIdentity(
                    filemanager::platform::BuiltInShellLocation::RecycleBin), false);
        });
    connect(quickAccess_, &filemanager::ui::NavigationTree::networkActivated,
        this, &MainWindow::showNetworkBrowser);
    connect(quickAccess_, &filemanager::ui::NavigationTree::filesDropped,
        this, &MainWindow::handleFilesystemDrop);
    connect(quickAccess_, &QWidget::customContextMenuRequested,
        this, &MainWindow::showNavigationContextMenu);
    auto* filterAction = new QAction(this);
    filterAction->setShortcut(QKeySequence::Find);
    addAction(filterAction);
    connect(filterAction, &QAction::triggered, this, [this] { active_->focusFilter(); });
    updateStatus();
    const QString shortcutHint =
        "F5 refresh  •  F6 focus regions  •  Ctrl+Alt+F5 copy to panel  •  "
        "Ctrl+Alt+F6 move to panel  •  Ctrl+Shift+P commands";
    statusBar()->showMessage(shortcutHint);
    statusBar()->setToolTip(shortcutHint);
    profiler.finish("ui_connections_status");
}

void MainWindow::showHelpBrowser()
{
    QDialog dialog(this);
    dialog.setObjectName("HelpBrowserDialog");
    dialog.setWindowTitle("Help");
    dialog.resize(620, 450);
    auto* layout = new QVBoxLayout(&dialog);
    auto* search = new QLineEdit(&dialog);
    search->setObjectName("HelpSearch");
    search->setAccessibleName("Find a help topic");
    search->setPlaceholderText("Find a help topic…");
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto* topics = new QListWidget(&dialog);
    topics->setObjectName("HelpTopics");
    topics->setAccessibleName("Help topics");
    topics->setSpacing(4);
    layout->addWidget(topics, 1);
    auto* hint = new QLabel("Enter  Open selected guide     Esc  Close", &dialog);
    hint->setObjectName("Muted");
    layout->addWidget(hint);
    const QList<QPair<QString, QString>> entries = {
        {"Getting Started", "Navigation, tabs, searching and safe everyday actions."},
        {"Batch Rename", "Ordered chapters, pasted names, preview and Undo limits."},
        {"Media Organizer", "Review proposed TV or book moves before applying them."},
        {"Recovery & Safety", "Transfers, conflicts, recovery and permanent deletion."},
        {"About & Support", "Version, dependencies, local diagnostics and support details."}
    };
    const auto populate = [topics, entries](const QString& query) {
        topics->clear();
        for (int index = 0; index < entries.size(); ++index) {
            const auto& entry = entries.at(index);
            if (!query.isEmpty() && !(entry.first + ' ' + entry.second)
                    .contains(query, Qt::CaseInsensitive))
                continue;
            auto* item = new QListWidgetItem(entry.first + "\n" + entry.second, topics);
            item->setData(Qt::UserRole, index);
            item->setSizeHint(QSize(0, 48));
        }
        if (topics->count())
            topics->setCurrentRow(0);
    };
    populate({});
    connect(search, &QLineEdit::textChanged, &dialog, populate);
    const auto openSelected = [this, &dialog, topics] {
        const auto* item = topics->currentItem();
        if (!item)
            return;
        const int index = item->data(Qt::UserRole).toInt();
        dialog.accept();
        QTimer::singleShot(0, this, [this, index] {
            switch (index) {
            case 0: showGettingStarted(); break;
            case 1: filemanager::ui::showBatchRenameHelp(this,
                        commands_->action("file.batch_rename")->shortcut()); break;
            case 2: filemanager::ui::showTvOrganizerHelp(this); break;
            case 3: showRecoveryGuidance(); break;
            case 4: showAboutSupport(); break;
            default: break;
            }
        });
    };
    connect(search, &QLineEdit::returnPressed, &dialog, openSelected);
    connect(topics, &QListWidget::itemActivated, &dialog,
        [openSelected](QListWidgetItem*) { openSelected(); });
    search->setFocus();
    dialog.exec();
}

void MainWindow::showGettingStarted()
{
    QDialog dialog(this);
    dialog.setObjectName("GettingStartedDialog");
    dialog.setWindowTitle("Getting Started");
    dialog.setMinimumWidth(560);
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel("Choose a topic to get started. More detail appears where you need it.", &dialog);
    title->setObjectName("GettingStartedSummary");
    title->setAccessibleName("Getting Started summary");
    title->setWordWrap(true);
    title->setTextFormat(Qt::PlainText);
    layout->addWidget(title);
    auto* topics = new QTabWidget(&dialog);
    topics->setObjectName("GettingStartedTopics");
    topics->setAccessibleName("Getting Started topics");
    const auto addTopic = [topics](const QString& name, const QString& objectName,
                                   const QString& text) {
        auto* page = new QWidget(topics);
        auto* pageLayout = new QVBoxLayout(page);
        auto* guidance = new QLabel(text, page);
        guidance->setObjectName(objectName);
        guidance->setAccessibleName(name + " instructions");
        guidance->setWordWrap(true);
        guidance->setTextFormat(Qt::PlainText);
        guidance->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
        pageLayout->addWidget(guidance);
        pageLayout->addStretch(1);
        topics->addTab(page, name);
    };
    addTopic("Browse", "GettingStartedBrowse",
        "• Open folders in tabs or split panels. Use the address bar for an exact path, "
        "the filter box for this folder, or Search for a broader search.\n\n"
        "• F6 and Shift+F6 move between visible regions; F5 refreshes. "
        "Ctrl+Alt+F5 copies to the other panel and Ctrl+Alt+F6 moves there.\n\n"
        "• In Details view, choose View > Columns or right-click a column header. "
        "Optional Availability shows Windows provider status; opening an online-only file may download it.\n\n"
        "• Preferences can show selection check boxes and choose where new windows open. "
        "Press Space to check or uncheck a focused item when check boxes are on.");
    addTopic("File actions", "GettingStartedContent",
        "• Copy and move use a visible queue. Existing destinations are never silently overwritten.\n\n"
        "• File > Create shortcut makes an ordinary .lnk in this folder without replacing an existing name.\n\n"
        "• ZIP extraction uses a guarded, no-overwrite path. The Open RAR/7z/TAR using Windows "
        "command opens those files with your Windows-selected app; File Manager does not extract them.\n\n"
        "• Delete uses the Recycle Bin. Shift+Delete requires typed confirmation and cannot be undone. "
        "Edit > Undo works only when a supported session operation can be safely reversed.");
    addTopic("Privacy", "GettingStartedPrivacy",
        "• Indexing, Windows extensions and workspace restoration stay off until you enable them in Preferences.\n\n"
        "• Scan network runs only when you choose it. Content search runs only on request.\n\n"
        "• You can review Windows folder-opening and Win+E choices separately in Preferences. "
        "Both start off and can be restored later.");
    layout->addWidget(topics, 1);
    auto* renameGuide = new QPushButton("Batch Rename guide — ordered chapters and pasted name lists…", &dialog);
    renameGuide->setObjectName("GettingStartedRenameGuide");
    connect(renameGuide, &QPushButton::clicked, &dialog, [this, &dialog] {
        filemanager::ui::showBatchRenameHelp(&dialog, commands_->action("file.batch_rename")->shortcut());
    });
    layout->addWidget(renameGuide);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::showRecoveryGuidance()
{
    QDialog dialog(this);
    dialog.setObjectName("RecoverySafetyDialog");
    dialog.setWindowTitle("Recovery & Safety");
    dialog.setMinimumWidth(560);
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel("Review what happened before retrying or removing anything.", &dialog);
    title->setObjectName("RecoverySafetySummary");
    title->setAccessibleName("Recovery and safety summary");
    title->setWordWrap(true);
    title->setTextFormat(Qt::PlainText);
    layout->addWidget(title);
    auto* topics = new QTabWidget(&dialog);
    topics->setObjectName("RecoverySafetyTopics");
    topics->setAccessibleName("Recovery and safety topics");
    const auto addTopic = [topics](const QString& name, const QString& objectName,
                                   const QString& text) {
        auto* page = new QWidget(topics);
        auto* pageLayout = new QVBoxLayout(page);
        auto* guidance = new QLabel(text, page);
        guidance->setObjectName(objectName);
        guidance->setAccessibleName(name + " guidance");
        guidance->setWordWrap(true);
        guidance->setTextFormat(Qt::PlainText);
        guidance->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
        pageLayout->addWidget(guidance);
        pageLayout->addStretch(1);
        topics->addTab(page, name);
    };
    addTopic("Transfers", "RecoverySafetyContent",
        "• File Manager records transfer state in a per-user encrypted journal. "
        "After an interruption, open Workspace > Operation center to inspect the source and destination.\n\n"
        "• Cleanup is never automatic: only destinations verified as newly created by that operation "
        "can be offered for removal.\n\n"
        "• Retry starts a fresh, fully checked operation rather than trusting partial bytes. "
        "You can also dismiss the reminder.");
    addTopic("Delete & Undo", "RecoverySafetyUndo",
        "• Name conflicts never overwrite silently. Delete uses the Windows Recycle Bin by default.\n\n"
        "• Undo for supported moves, renames and recycled items is limited to this session "
        "and is refused when identity checks fail.\n\n"
        "• Permanent deletion bypasses the Recycle Bin, requires typed confirmation "
        "and never creates an Undo entry.");
    addTopic("Local data", "RecoverySafetyData",
        "• Preferences lets you revoke optional permissions and clear retained workspace, "
        "search and other local data independently.\n\n"
        "• Review a recovery choice before clearing its related operation history.");
    layout->addWidget(topics, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::showAboutSupport()
{
#ifdef QT_NO_DEBUG
    const QString buildConfiguration = QStringLiteral("Release");
#else
    const QString buildConfiguration = QStringLiteral("Debug");
#endif
    const QString overview = QString(
        "File Manager %1\n"
        "Build: %2, %3\n"
        "Windows: %4\n"
        "Qt runtime: %5")
        .arg(QStringLiteral(FILE_MANAGER_DISPLAY_VERSION), buildConfiguration,
            QSysInfo::buildCpuArchitecture(), QSysInfo::prettyProductName(),
            QString::fromLatin1(qVersion()));
    const QString dependencies = QStringLiteral(
        "FFmpeg via Qt Multimedia (LGPL 2.1 or later)\n"
        "minizip-ng 4.2.2 (decompression-only, pinned revision 7b238716)\n"
        "zlib-ng 2.3.3 (pinned revision 12731092)\n"
        "License details: THIRD_PARTY_NOTICES.md and licenses/ beside the app.");
    const QString diagnostics = QString(
        "Local event history: %1; path-free codes only; seven-day retention; "
        "512-event maximum.\n"
        "Exports exclude paths and filenames by default. Crash dumps are not collected by the "
        "application in this development build.\n"
        "Support data remains in the current user's local application-data area. Use Preferences "
        "to export diagnostics or clear retained data.")
        .arg(diagnosticLog_.isLoading() ? QStringLiteral("loading")
            : !diagnosticLog_.isAvailable() ? QStringLiteral("unavailable; see Preferences")
            : QStringLiteral("%1 saved, %2 pending")
                .arg(filemanager::ui::localizedItems(diagnosticLog_.eventCount()),
                    filemanager::ui::localizedItems(diagnosticLog_.pendingCount())));
    const QString summary = overview + "\n\nBundled dependencies\n" + dependencies
        + "\n\nDiagnostics\n" + diagnostics;
    QDialog dialog(this);
    dialog.setObjectName("AboutSupportDialog");
    dialog.setWindowTitle("About & Support");
    dialog.setMinimumWidth(560);
    auto* layout = new QVBoxLayout(&dialog);
    auto* logo = new QLabel(&dialog);
    filemanager::ui::BrandIdentity::setLogo(logo, QSize(220, 72));
    logo->setAccessibleName(tr("%1 logo").arg(filemanager::ui::BrandIdentity::productName()));
    layout->addWidget(logo, 0, Qt::AlignLeft);
    auto* topics = new QTabWidget(&dialog);
    topics->setObjectName("AboutSupportTopics");
    topics->setAccessibleName("About and support topics");
    const auto addTopic = [topics](const QString& name, const QString& objectName,
                                   const QString& text) {
        auto* page = new QWidget(topics);
        auto* pageLayout = new QVBoxLayout(page);
        auto* details = new QLabel(text, page);
        details->setObjectName(objectName);
        details->setAccessibleName(name + " information");
        details->setTextFormat(Qt::PlainText);
        details->setWordWrap(true);
        details->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
        pageLayout->addWidget(details);
        pageLayout->addStretch(1);
        topics->addTab(page, name);
    };
    addTopic("Overview", "AboutSupportDetails", overview);
    addTopic("Dependencies", "AboutSupportDependencies", dependencies);
    addTopic("Diagnostics", "AboutSupportDiagnostics", diagnostics);
    layout->addWidget(topics);
    auto* actions = new QHBoxLayout;
    auto* copy = new QPushButton("Copy support summary", &dialog);
    copy->setObjectName("CopySupportSummary");
    copy->setAccessibleName("Copy path-free support summary");
    actions->addWidget(copy);
    actions->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    actions->addWidget(buttons);
    layout->addLayout(actions);
    connect(copy, &QPushButton::clicked, &dialog, [this, summary] {
        QApplication::clipboard()->setText(summary);
        statusBar()->showMessage("Path-free support summary copied", 4000);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::setActivePane(FilePane* pane)
{
    if (!pane)
        return;
    active_ = pane;
    syncShellNavigation();
    syncViewCommands();
    syncLocationCommands();
    if (quickAccess_ && active_->isFileSystemLocation())
        quickAccess_->synchronizeToPath(active_->currentPath());
}

QList<QWidget*> MainWindow::focusRegions() const
{
    QList<QWidget*> regions;
    const auto appendVisible = [this, &regions](QWidget* widget) {
        if (widget && widget->isEnabled() && widget->isVisibleTo(this))
            regions.append(widget);
    };

    QTabBar* tabs = workspace_ && workspace_->activePanel()
        ? workspace_->activePanel()->tabBar() : nullptr;
    QWidget* content = active_ ? active_->contentFocusTarget() : nullptr;
    appendVisible(tabs);

    // Current File Explorer cycles Home through tabs, content, and navigation.
    // Folder views use its full main-region order and omit any hidden optional pane.
    if (active_ && active_->isHomeLocation()) {
        appendVisible(content);
        appendVisible(quickAccess_);
        return regions;
    }

    appendVisible(quickAccess_);
    appendVisible(content);
    appendVisible(preview_);
    appendVisible(statusBar());
    return regions;
}

void MainWindow::cycleFocusRegion(bool backwards)
{
    const QList<QWidget*> regions = focusRegions();
    if (regions.isEmpty())
        return;

    QWidget* focused = QApplication::focusWidget();
    if ((!focused || focused->window() != this) && focusWidget())
        focused = focusWidget();
    int current = -1;
    for (int index = 0; index < regions.size(); ++index) {
        QWidget* region = regions.at(index);
        if (focused && (focused == region || region->isAncestorOf(focused))) {
            current = index;
            break;
        }
    }
    const int next = current < 0
        ? (backwards ? regions.size() - 1 : 0)
        : (current + (backwards ? regions.size() - 1 : 1)) % regions.size();
    regions.at(next)->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::openInitialPaths(const QStringList& paths)
{
    if (!workspace_ || paths.isEmpty())
        return;

    QStringList rejected;
    QStringList opened;
    bool useCurrentTab = true;
    for (const QString& rawPath : paths) {
        const QFileInfo info(QDir::cleanPath(QDir::fromNativeSeparators(rawPath)));
        if (!info.exists()) {
            rejected.append(rawPath);
            continue;
        }

        const QString absolutePath = info.absoluteFilePath();
        const QString comparisonPath = QDir::toNativeSeparators(absolutePath).toCaseFolded();
        if (opened.contains(comparisonPath))
            continue;
        opened.append(comparisonPath);

        FilePane* pane = useCurrentTab ? workspace_->currentPane()
                                       : workspace_->addTab(info.isDir()
                                               ? absolutePath : info.absolutePath());
        useCurrentTab = false;
        if (!pane)
            continue;
        const QString destination = info.isDir() ? absolutePath : info.absolutePath();
        const bool alreadyAtDestination = pane->isFileSystemLocation()
            && QFileInfo(pane->currentPath()).absoluteFilePath().compare(
                QFileInfo(destination).absoluteFilePath(), Qt::CaseInsensitive) == 0;
        if (!alreadyAtDestination)
            pane->navigate(destination);
        if (!info.isDir()) {
            QTimer::singleShot(250, pane, [pane, absolutePath] { pane->revealPath(absolutePath); });
        }
        setActivePane(pane);
    }

    if (!rejected.isEmpty()) {
        statusBar()->showMessage(
            tr("Could not open %1 path(s): %2")
                .arg(rejected.size()).arg(rejected.first()),
            10000);
    }
}

void MainWindow::syncViewCommands()
{
    if (!active_ || !commands_)
        return;

    const auto check = [this](const QString& id, bool checked) {
        QAction* action = commands_->action(id);
        const QSignalBlocker blocker(action);
        action->setChecked(checked);
    };
    check("view.hidden", active_->showHidden());
    check("view.file_extensions", active_->showFileExtensions());
    check("view.details", active_->viewMode() == FilePane::ViewMode::Details);
    check("view.list", active_->viewMode() == FilePane::ViewMode::List);
    check("view.large_icons", active_->viewMode() == FilePane::ViewMode::LargeIcons);
    check("view.extra_large_icons", active_->viewMode() == FilePane::ViewMode::ExtraLargeIcons);
    check("view.medium_icons", active_->viewMode() == FilePane::ViewMode::MediumIcons);
    check("view.small_icons", active_->viewMode() == FilePane::ViewMode::SmallIcons);
    check("view.folders_first", active_->folderPlacement() == FilePane::FolderPlacement::First);
    check("view.folders_last", active_->folderPlacement() == FilePane::FolderPlacement::Last);
    check("view.folders_mixed", active_->folderPlacement() == FilePane::FolderPlacement::Mixed);
    check("view.group_none", active_->groupBy() == FilePane::GroupBy::None);
    check("view.group_name", active_->groupBy() == FilePane::GroupBy::Name);
    check("view.group_type", active_->groupBy() == FilePane::GroupBy::Type);
    check("view.group_modified", active_->groupBy() == FilePane::GroupBy::DateModified);
    check("view.group_size", active_->groupBy() == FilePane::GroupBy::Size);
    const bool filesystem = active_->isFileSystemLocation();
    for (const auto& column : ViewColumns) {
        const QString visibilityId = QString::fromLatin1(column.visibilityId);
        if (column.sortId) {
            const QString sortId = QString::fromLatin1(column.sortId);
            check(sortId, active_->sortColumn() == column.column);
            commands_->action(sortId)->setEnabled(filesystem);
        }
        check(visibilityId, active_->columnVisible(column.column));
        commands_->action(visibilityId)->setEnabled(filesystem && column.column != 0);
    }
    check("view.sort_ascending", active_->sortOrder() == Qt::AscendingOrder);
    check("view.sort_descending", active_->sortOrder() == Qt::DescendingOrder);
    commands_->action("view.sort_ascending")->setEnabled(filesystem);
    commands_->action("view.sort_descending")->setEnabled(filesystem);
}

void MainWindow::syncLocationCommands()
{
    if (!active_ || !commands_)
        return;
    const bool filesystem = active_->isFileSystemLocation();
    const QStringList filesystemOnly = {
        "search.files", "search.contents", "folder.new", "file.new_text",
        "file.create_shortcut", "file.rename",
        "file.batch_rename", "file.edit_tags", "file.delete", "file.delete_permanently", "clipboard.cut",
        "clipboard.copy_filename", "clipboard.paste", "file.properties", "file.details",
        "file.analyze", "tools.duplicates", "archive.create_zip", "archive.extract_zip",
        "archive.open_native",
        "file.share", "folder.terminal", "folder.open_new_tab", "folder.open_new_window",
        "favorites.add"
    };
    for (const QString& id : filesystemOnly) {
        if (QAction* action = commands_->action(id))
            action->setEnabled(filesystem);
    }
    if (QAction* archive = commands_->action("archive.open_native")) {
        archive->setEnabled(filesystem && filemanager::platform::validateNativeArchiveHandoff(
            active_->selectedPath()).valid);
    }
    if (QAction* copy = commands_->action("clipboard.copy"))
        copy->setEnabled(filesystem || !active_->selectedShellItems().isEmpty());
    if (QAction* open = commands_->action("folder.open"))
        open->setEnabled(filesystem
            || (active_->isHomeLocation() && !active_->selectedPath().isEmpty())
            || (!active_->isHomeLocation() && active_->selectedItemCount() > 0));
    const bool recycleBin = active_->builtInShellLocation()
        == filemanager::platform::BuiltInShellLocation::RecycleBin;
    if (QAction* restore = commands_->action("recycle.restore_selected"))
        restore->setEnabled(recycleBin && !active_->selectedShellItems().isEmpty());
    if (QAction* empty = commands_->action("recycle.empty"))
        empty->setEnabled(recycleBin);
}
void MainWindow::connectPane(FilePane* pane)
{
    connectedPanes_.removeIf([](const auto& existing) { return existing.isNull(); });
    if (!pane || connectedPanes_.contains(pane))
        return;
    connectedPanes_.append(pane);
    connect(pane, &FilePane::activated, this, &MainWindow::setActivePane);
    connect(pane, &FilePane::selectionChanged, this,
            [this](const QString& path)
            {
                updatePreview(path);
                updateStatus();
                syncLocationCommands();
            });
    connect(pane, &FilePane::pathChanged, this, [this, pane](const QString& path) {
        updateStatus();
        recordRecentLocation(path);
        syncLocationCommands();
        if (pane == active_ && quickAccess_)
            quickAccess_->synchronizeToPath(path);
    });
    connect(pane, &FilePane::locationChanged, this, [this, pane](const QString&) {
        if (pane == active_) {
            syncShellNavigation();
            updateStatus();
            syncLocationCommands();
        }
    });
    connect(pane, &FilePane::shellItemOpenRequested, this,
        [this](const QString& itemParsingName) {
#ifdef Q_OS_WIN
            const std::wstring nativeParsingName = itemParsingName.toStdWString();
            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof(info);
            info.hwnd = reinterpret_cast<HWND>(winId());
            info.lpVerb = L"open";
            info.lpFile = nativeParsingName.c_str();
            info.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&info))
                QMessageBox::warning(this, "Windows item", "Windows could not open this item.");
#else
            Q_UNUSED(itemParsingName);
#endif
        });
    connect(pane, &FilePane::backgroundTabRequested, this,
        [this, pane](const QString& path) {
            for (TabbedPanel* panel : workspace_->panels()) {
                if (panel->panes().contains(pane)) {
                    panel->addTab(path, false);
                    statusBar()->showMessage(
                        QStringLiteral("Opened %1 in a background tab")
                            .arg(QFileInfo(path).fileName()), 3000);
                    return;
                }
            }
        });
    connect(pane, &FilePane::savedSearchRequested, this,
        [this](const QString& id) { showAsyncSearch(id); });
    connect(pane, &FilePane::viewStateChanged, this, [this, pane] {
        if (pane == active_)
            syncViewCommands();
    });
    connect(pane, &FilePane::itemCountChanged, this, [this, pane] {
        if (pane == active_) {
            syncShellNavigation();
            updateStatus();
        }
    });
    connect(pane, &FilePane::filesDropped, this, &MainWindow::handleFilesystemDrop);
    connect(pane, &FilePane::shellItemsDropped, this,
        [this, pane](const QList<filemanager::platform::ShellItemReference>& items,
            const QString& destination, bool moveRequested, const QString& error) {
            if (!error.isEmpty()) {
                QMessageBox::warning(this, "Drop Windows items", error);
                return;
            }
            copyShellItemsToFolder(items, destination, moveRequested, pane);
        });
    connect(pane, &FilePane::contextMenuRequested, this,
        [this](FilePane* source, const QPoint& point) { showContextMenu(source, point); });
    connect(pane, &FilePane::columnHeaderMenuRequested, this,
        [this](FilePane* source, const QPoint& globalPoint) {
            showColumnHeaderMenu(source, globalPoint);
        });
}

void MainWindow::handleFilesystemDrop(const QStringList& paths, const QString& destination,
    bool move, bool confirmationRequired, bool linkRequested)
{
    if (linkRequested) {
        QMessageBox::information(this, "Link drop is not supported",
            "File Manager will not reinterpret a requested link as a copy or move. "
            "Create the link through a Windows command or cancel the drag.");
        return;
    }
    constexpr qsizetype largeSelectionThreshold = 32;
    if (confirmationRequired || paths.size() > largeSelectionThreshold) {
        QStringList names;
        const qsizetype displayed = qMin<qsizetype>(paths.size(), 5);
        for (qsizetype index = 0; index < displayed; ++index)
            names.append(QFileInfo(paths.at(index)).fileName());
        if (paths.size() > displayed)
            names.append(QStringLiteral("…and %1 more").arg(paths.size() - displayed));
        QMessageBox confirmation(QMessageBox::Question, "Confirm file drop",
            QStringLiteral("%1 %2 to:\n%3\n\n%4\n\n%5")
                .arg(move ? QStringLiteral("Move") : QStringLiteral("Copy"),
                    filemanager::ui::localizedItems(paths.size()),
                    QDir::toNativeSeparators(destination), names.join('\n'),
                    confirmationRequired
                        ? QStringLiteral("No explicit copy/move effect was supplied; the "
                                         "action was inferred from the source and destination.")
                        : QStringLiteral("Large drag selections require confirmation.")),
            QMessageBox::NoButton, this);
        confirmation.setObjectName("FileDropConfirmationDialog");
        confirmation.setTextFormat(Qt::PlainText);
        QPushButton* proceed = confirmation.addButton(
            move ? "Move items" : "Copy items", QMessageBox::AcceptRole);
        QPushButton* cancel = confirmation.addButton(QMessageBox::Cancel);
        confirmation.setDefaultButton(cancel);
        confirmation.exec();
        if (confirmation.clickedButton() != proceed)
            return;
    }
    startTransfer(paths, destination, move);
}

void MainWindow::newFolderTab()
{
    FilePane* pane = workspace_->addTab(active_ ? active_->currentPath() : QDir::homePath());
    setActivePane(pane);
}

void MainWindow::closeFolderTab()
{
    workspace_->closeCurrentTab();
    setActivePane(workspace_->currentPane());
}

void MainWindow::duplicateFolderTab()
{
    if (FilePane* pane = workspace_->activePanel()->duplicateCurrentTab())
        setActivePane(pane);
}

void MainWindow::reopenClosedTab()
{
    if (FilePane* pane = workspace_->activePanel()->reopenLastClosedTab())
        setActivePane(pane);
    else
        statusBar()->showMessage("No recently closed tab in this panel.", 2500);
}

void MainWindow::openSelectedFolderInTab()
{
    const QString path = active_->selectedPath();
    if (!QFileInfo(path).isDir())
        return;
    setActivePane(workspace_->addTab(path));
}

void MainWindow::openSelectedFolderInWindow()
{
    const QString path = active_->selectedPath();
    if (!QFileInfo(path).isDir())
        return;
    auto* window = new MainWindow(path);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
}

void MainWindow::addCurrentFolderFavorite()
{
    if (!active_)
        return;
    addFavoritePath(active_->currentPath());
}

void MainWindow::addFavoritePath(const QString& rawPath)
{
    const QString path = QDir::cleanPath(rawPath);
    if (path.isEmpty())
        return;
    QSettings settings;
    auto favorites = filemanager::settings::loadFavoriteGroups(settings);
    if (favorites.containsPath(path)) {
        statusBar()->showMessage("This folder is already a favorite.", 2500);
        return;
    }
    int groupIndex = 0;
    if (favorites.groups.size() > 1) {
        QStringList names;
        for (const auto& group : favorites.groups)
            names.append(group.name);
        bool ok = false;
        const QString selected = QInputDialog::getItem(
            this, "Add favorite", "Favorite group:", names, 0, false, &ok);
        if (!ok)
            return;
        groupIndex = names.indexOf(selected);
    }
    favorites.groups[qMax(0, groupIndex)].paths.append(path);
    filemanager::settings::saveFavoriteGroups(settings, favorites);
    rebuildQuickAccess();
}

void MainWindow::removeSelectedFavorite()
{
    const QString path = quickAccess_ ? quickAccess_->selectedFavoritePath() : QString{};
    const QString groupId = quickAccess_ ? quickAccess_->selectedFavoriteGroupId() : QString{};
    if (path.isEmpty() || groupId.isEmpty()) {
        statusBar()->showMessage("Select a user favorite in the sidebar first.", 2500);
        return;
    }
    QSettings settings;
    auto favorites = filemanager::settings::loadFavoriteGroups(settings);
    for (auto& group : favorites.groups) {
        if (group.id != groupId)
            continue;
        group.paths.removeIf([&](const QString& value) {
            return value.compare(path, Qt::CaseInsensitive) == 0;
        });
    }
    filemanager::settings::saveFavoriteGroups(settings, favorites);
    rebuildQuickAccess();
}

void MainWindow::moveSelectedFavorite(int offset)
{
    const QString path = quickAccess_ ? quickAccess_->selectedFavoritePath() : QString{};
    const QString groupId = quickAccess_ ? quickAccess_->selectedFavoriteGroupId() : QString{};
    if (path.isEmpty() || groupId.isEmpty()) {
        statusBar()->showMessage("Select a user favorite in the sidebar first.", 2500);
        return;
    }
    QSettings settings;
    auto favorites = filemanager::settings::loadFavoriteGroups(settings);
    if (!favorites.movePath(groupId, path, offset)) {
        statusBar()->showMessage("The favorite is already at the edge of its group.", 2500);
        return;
    }
    filemanager::settings::saveFavoriteGroups(settings, favorites);
    rebuildQuickAccess();
    quickAccess_->selectFavorite(path, groupId);
}

void MainWindow::manageFavoriteGroups()
{
    QSettings settings;
    auto favorites = filemanager::settings::loadFavoriteGroups(settings);
    QDialog dialog(this);
    dialog.setWindowTitle("Favorite groups");
    dialog.resize(460, 380);
    auto* layout = new QVBoxLayout(&dialog);
    auto* groups = new QListWidget(&dialog);
    groups->setAccessibleName("Favorite groups");
    for (const auto& group : favorites.groups) {
        auto* item = new QListWidgetItem(group.name, groups);
        item->setData(Qt::UserRole, group.id);
    }
    layout->addWidget(groups, 1);
    auto* controls = new QHBoxLayout;
    auto* add = new QPushButton("Add", &dialog);
    auto* rename = new QPushButton("Rename", &dialog);
    auto* remove = new QPushButton("Remove", &dialog);
    auto* up = new QPushButton("Move up", &dialog);
    auto* down = new QPushButton("Move down", &dialog);
    for (QPushButton* button : {add, rename, remove, up, down})
        controls->addWidget(button);
    layout->addLayout(controls);
    const auto uniqueName = [groups](const QString& name, QListWidgetItem* except = nullptr) {
        for (int row = 0; row < groups->count(); ++row) {
            QListWidgetItem* item = groups->item(row);
            if (item != except && item->text().compare(name, Qt::CaseInsensitive) == 0)
                return false;
        }
        return true;
    };
    connect(add, &QPushButton::clicked, &dialog, [&] {
        bool ok = false;
        const QString name = QInputDialog::getText(
            &dialog, "New favorite group", "Name:", QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || name.isEmpty() || !uniqueName(name))
            return;
        auto* item = new QListWidgetItem(name, groups);
        item->setData(Qt::UserRole, QUuid::createUuid().toString(QUuid::WithoutBraces));
        groups->setCurrentItem(item);
    });
    connect(rename, &QPushButton::clicked, &dialog, [&] {
        QListWidgetItem* item = groups->currentItem();
        if (!item)
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(
            &dialog, "Rename favorite group", "Name:", QLineEdit::Normal, item->text(), &ok)
                                 .trimmed();
        if (ok && !name.isEmpty() && uniqueName(name, item))
            item->setText(name);
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        if (groups->count() <= 1 || !groups->currentItem())
            return;
        delete groups->takeItem(groups->currentRow());
    });
    const auto moveGroup = [groups](int offset) {
        const int row = groups->currentRow();
        const int target = row + offset;
        if (row < 0 || target < 0 || target >= groups->count())
            return;
        QListWidgetItem* item = groups->takeItem(row);
        groups->insertItem(target, item);
        groups->setCurrentRow(target);
    };
    connect(up, &QPushButton::clicked, &dialog, [moveGroup] { moveGroup(-1); });
    connect(down, &QPushButton::clicked, &dialog, [moveGroup] { moveGroup(1); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QList<filemanager::settings::FavoriteGroup> reordered;
    QStringList orphanedPaths;
    for (const auto& oldGroup : favorites.groups) {
        bool retained = false;
        for (int row = 0; row < groups->count(); ++row)
            retained |= groups->item(row)->data(Qt::UserRole).toString() == oldGroup.id;
        if (!retained)
            orphanedPaths.append(oldGroup.paths);
    }
    for (int row = 0; row < groups->count(); ++row) {
        QListWidgetItem* item = groups->item(row);
        const QString id = item->data(Qt::UserRole).toString();
        QStringList paths;
        for (const auto& oldGroup : favorites.groups) {
            if (oldGroup.id == id) {
                paths = oldGroup.paths;
                break;
            }
        }
        reordered.append({id, item->text(), paths});
    }
    reordered.first().paths.append(orphanedPaths);
    favorites.groups = reordered;
    filemanager::settings::saveFavoriteGroups(settings, favorites);
    rebuildQuickAccess();
}

void MainWindow::recordRecentLocation(const QString& rawPath)
{
    const QString path = QDir::cleanPath(rawPath);
    if (!QFileInfo(path).isDir())
        return;
    QSettings settings;
    QStringList recents = settings.value("navigation/recents").toStringList();
    recents.removeIf([&](const QString& value) {
        return value.compare(path, Qt::CaseInsensitive) == 0;
    });
    recents.prepend(path);
    while (recents.size() > 15)
        recents.removeLast();
    settings.setValue("navigation/recents", recents);
    rebuildQuickAccess();
}

void MainWindow::rebuildQuickAccess()
{
    if (!quickAccess_)
        return;
    QSettings settings;
    quickAccess_->rebuild(settings.value("navigation/recents").toStringList(),
        filemanager::settings::loadFavoriteGroups(settings),
        storageVolumes_);
    syncShellNavigation();
    if (workspace_) {
        for (FilePane* pane : workspace_->panes()) {
            if (pane->isHomeLocation())
                pane->refresh();
        }
    }
    if (active_ && active_->isFileSystemLocation())
        quickAccess_->synchronizeToPath(active_->currentPath());
}

void MainWindow::syncShellNavigation()
{
    if (quickAccess_)
        quickAccess_->setShellNavigationItems(active_ ? active_->shellNavigationItems()
            : QList<filemanager::platform::ShellItemReference>{});
}

void MainWindow::openShellTreeLocation(const QByteArray& identity, ShellTreeTarget target)
{
    if (!active_ || !workspace_)
        return;
    if (target == ShellTreeTarget::CurrentPane) {
        active_->navigateShellVisitItem(identity);
        return;
    }
    // Recheck current permission and membership when the action is used, including after a menu wait.
    const auto snapshot = active_->shellNavigationItems();
    for (const auto& item : snapshot) {
        if (item.idList != identity)
            continue;
        if (target == ShellTreeTarget::NewWindow) {
            auto* window = new MainWindow(QLatin1String(FilePane::HomeIdentity));
            window->setAttribute(Qt::WA_DeleteOnClose);
            window->active_->navigateShellItem(item);
            window->show();
        } else if (auto* panel = workspace_->activePanel()) {
            auto* pane = panel->addTab(QLatin1String(FilePane::HomeIdentity),
                target == ShellTreeTarget::NewTab);
            pane->navigateShellItem(item);
        }
        return;
    }
}

void MainWindow::showThisPc()
{
    showBuiltInShell(filemanager::platform::BuiltInShellLocation::ThisPc);
}

void MainWindow::showBuiltInShell(filemanager::platform::BuiltInShellLocation location)
{
    if (!active_ || active_->navigateBuiltInShell(location))
        return;
    QMessageBox::warning(this, "Windows location",
        QStringLiteral("Windows could not open %1.")
            .arg(filemanager::platform::ShellItem::builtInDisplayName(location)));
}

void MainWindow::showStorageDetails()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Storage details");
    dialog.resize(760, 500);
    auto* layout = new QVBoxLayout(&dialog);
    auto* locations = new QTreeWidget(&dialog);
    locations->setObjectName("ThisPcLocations");
    locations->setAccessibleName("This PC locations and storage volumes");
    locations->setColumnCount(4);
    locations->setHeaderLabels({"Location", "Type", "Free space", "Total size"});
    locations->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    locations->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    locations->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    locations->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    auto* folders = new QTreeWidgetItem(locations, {"Folders"});
    folders->setFlags(Qt::ItemIsEnabled);
    using filemanager::platform::KnownFolder;
    const QList<QPair<QString, KnownFolder>> knownFolders = {{"Desktop", KnownFolder::Desktop},
        {"Documents", KnownFolder::Documents}, {"Downloads", KnownFolder::Downloads},
        {"Music", KnownFolder::Music}, {"Pictures", KnownFolder::Pictures},
        {"Videos", KnownFolder::Videos}};
    for (const auto& [name, folder] : knownFolders) {
        const QString path = filemanager::platform::knownFolderPath(folder);
        if (path.isEmpty() || !QFileInfo(path).isDir())
            continue;
        auto* item = new QTreeWidgetItem(folders, {name, "Folder"});
        item->setData(0, Qt::UserRole, path);
        item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        item->setToolTip(0, QDir::toNativeSeparators(path));
    }
    auto* devices = new QTreeWidgetItem(locations, {"Devices and drives"});
    devices->setFlags(Qt::ItemIsEnabled);
    for (const auto& volume : filemanager::files::mountedStorageVolumes()) {
        const QString free = volume.bytesTotal > 0
            ? QLocale().formattedDataSize(static_cast<qint64>(volume.bytesAvailable)) : QString();
        const QString total = volume.bytesTotal > 0
            ? QLocale().formattedDataSize(static_cast<qint64>(volume.bytesTotal)) : QString();
        QString driveType = volume.readOnly ? QStringLiteral("Read-only drive")
            : QStringLiteral("Drive");
        if (volume.kind == filemanager::files::StorageLocationKind::Removable)
            driveType = volume.readOnly ? QStringLiteral("Read-only removable drive")
                                        : QStringLiteral("Removable drive");
        else if (volume.kind == filemanager::files::StorageLocationKind::Network)
            driveType = QStringLiteral("Network drive");
        else if (volume.kind == filemanager::files::StorageLocationKind::Optical)
            driveType = QStringLiteral("Optical drive");
        auto* item = new QTreeWidgetItem(devices,
            {volume.displayName, driveType, free, total});
        item->setData(0, Qt::UserRole, volume.rootPath);
        item->setIcon(0, style()->standardIcon(QStyle::SP_DriveHDIcon));
        item->setToolTip(0, QDir::toNativeSeparators(volume.rootPath));
    }
    auto* windowsLocations = new QTreeWidgetItem(locations, {"Windows locations"});
    windowsLocations->setFlags(Qt::ItemIsEnabled);
    const QList<QPair<QString, QString>> builtInNamespaces = {
        {"Recycle Bin", "shell:RecycleBinFolder"},
        {"Network", "shell:NetworkPlacesFolder"},
        {"Control Panel", "shell:ControlPanelFolder"}};
    for (const auto& [label, parsingName] : builtInNamespaces) {
        auto* item = new QTreeWidgetItem(windowsLocations, {label, "Windows namespace"});
        item->setData(0, Qt::UserRole + 1, parsingName);
        item->setData(0, Qt::UserRole + 2, true);
        item->setIcon(0, style()->standardIcon(QStyle::SP_ComputerIcon));
        item->setToolTip(0, "Opens the built-in Windows location through the system Shell.");
    }
    windowsLocations->setExpanded(true);

    const auto shellPermissions = filemanager::settings::loadShellPermissions();
    if (shellPermissions.allowThirdPartyNamespaces) {
        auto* extensions = new QTreeWidgetItem(locations,
            {"Shell namespace extensions", "Loading in isolated helper..."});
        extensions->setFlags(Qt::ItemIsEnabled);
        const auto cancelled = std::make_shared<std::atomic_bool>(false);
        connect(&dialog, &QObject::destroyed, this, [cancelled] { cancelled->store(true); });
        QPointer<QTreeWidget> locationGuard(locations);
        connect(filemanager::settings::shellPermissionNotifier(),
            &filemanager::settings::ShellPermissionNotifier::changed, locations,
            [locationGuard, extensions, cancelled] {
                cancelled->store(true);
                if (locationGuard) {
                    qDeleteAll(extensions->takeChildren());
                    extensions->setText(1, "Permission changed; reopen this dialog to browse");
                }
            });
        QThread* loader = QThread::create([locationGuard, extensions, cancelled] {
            const auto thisPc = filemanager::platform::ShellItem::resolveIsolated(
                filemanager::platform::ShellItem::builtInIdentity(
                    filemanager::platform::BuiltInShellLocation::ThisPc), 3000, {}, cancelled);
            const auto result = filemanager::platform::ShellItem::enumerateChildrenIsolated(
                thisPc, 256, 3000, {}, cancelled);
            if (!locationGuard)
                return;
            QMetaObject::invokeMethod(locationGuard, [locationGuard, extensions, result, cancelled] {
                if (!locationGuard || cancelled->load()
                        || !filemanager::settings::loadShellPermissions().allowThirdPartyNamespaces)
                    return;
                extensions->setText(1, result.success
                    ? (result.truncated ? QStringLiteral("First 256 items") : QString{})
                    : QStringLiteral("Unavailable"));
                if (!result.success) {
                    extensions->setToolTip(0, result.error);
                    return;
                }
                for (const auto& shellItem : result.items) {
                    if (shellItem.fileSystem)
                        continue;
                    auto* child = new QTreeWidgetItem(extensions,
                        {shellItem.displayName, shellItem.folder
                            ? QStringLiteral("Virtual folder") : QStringLiteral("Virtual item")});
                    child->setData(0, Qt::UserRole + 1, shellItem.parsingName);
                    child->setToolTip(0,
                        "Enumerated outside the main app. Opening delegates to Windows Shell.");
                }
                extensions->setExpanded(true);
            });
        });
        connect(loader, &QThread::finished, loader, &QObject::deleteLater);
        loader->start();
    }
    folders->setExpanded(true);
    devices->setExpanded(true);
    layout->addWidget(locations, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    const auto openCurrent = [&] {
        QTreeWidgetItem* item = locations->currentItem();
        const QString path = item ? item->data(0, Qt::UserRole).toString() : QString();
        if (!path.isEmpty()) {
            active_->navigate(path);
            dialog.accept();
            return;
        }
        const QString parsingName = item
            ? item->data(0, Qt::UserRole + 1).toString() : QString();
        if (!parsingName.isEmpty()) {
            if (item->data(0, Qt::UserRole + 2).toBool()) {
                dialog.accept();
                QTimer::singleShot(0, this,
                    [this, parsingName] { showShellNamespace(parsingName); });
                return;
            }
#ifdef Q_OS_WIN
            const std::wstring nativeParsingName = parsingName.toStdWString();
            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof(info);
            info.hwnd = reinterpret_cast<HWND>(winId());
            info.lpVerb = L"open";
            info.lpFile = nativeParsingName.c_str();
            info.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&info))
                QMessageBox::warning(this, "Windows location",
                    "Windows could not open this Shell namespace item.");
#endif
            dialog.accept();
        }
    };
    connect(locations, &QTreeWidget::itemDoubleClicked, &dialog,
        [&](QTreeWidgetItem*, int) { openCurrent(); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, openCurrent);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::mapNetworkDrive()
{
    QString error;
    const auto result = filemanager::platform::showMapNetworkDriveDialog(winId(), &error);
    if (result == filemanager::platform::NetworkDriveDialogResult::Cancelled)
        return;
    if (result != filemanager::platform::NetworkDriveDialogResult::Completed) {
        QMessageBox::warning(this, "Map network drive",
            error.isEmpty() ? "The Windows network-drive dialog is unavailable." : error);
        return;
    }
    refreshStorageLocations();
    if (active_)
        active_->refresh();
    statusBar()->showMessage("Windows updated the mapped network drives.", 5000);
}

void MainWindow::disconnectNetworkDrive()
{
    QString error;
    const auto result = filemanager::platform::showDisconnectNetworkDriveDialog(winId(), &error);
    if (result == filemanager::platform::NetworkDriveDialogResult::Cancelled)
        return;
    if (result != filemanager::platform::NetworkDriveDialogResult::Completed) {
        QMessageBox::warning(this, "Disconnect network drive",
            error.isEmpty() ? "The Windows network-drive dialog is unavailable." : error);
        return;
    }
    refreshStorageLocations();
    if (active_)
        active_->refresh();
    statusBar()->showMessage("Windows updated the mapped network drives.", 5000);
}

void MainWindow::restoreSelectedRecycleBinItems()
{
    if (!active_ || active_->builtInShellLocation()
            != filemanager::platform::BuiltInShellLocation::RecycleBin)
        return;
    const auto items = active_->selectedShellItems();
    if (items.isEmpty())
        return;
    const QString prompt = tr("Restore %1 to %2 original %3?\n\n"
                              "Windows will use the selected Recycle Bin identities. File Manager "
                              "will not guess a destination or overwrite an occupied path.")
        .arg(filemanager::ui::localizedItems(items.size()))
        .arg(items.size() == 1 ? QStringLiteral("its") : QStringLiteral("their"))
        .arg(items.size() == 1 ? QStringLiteral("location") : QStringLiteral("locations"));
    QMessageBox confirmation(QMessageBox::Question, "Restore selected", prompt,
        QMessageBox::Cancel, this);
    QPushButton* restoreButton = confirmation.addButton(
        "Restore selected", QMessageBox::AcceptRole);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != restoreButton)
        return;

    auto* progress = new QProgressDialog(
        "Windows is restoring the selected items…", {}, 0, 0, this);
    progress->setWindowTitle("Restore selected");
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setCancelButton(nullptr);
    progress->show();
    statusBar()->showMessage("Restoring selected Recycle Bin items…");
    const quintptr owner = winId();
    QPointer<MainWindow> guard(this);
    QPointer<QProgressDialog> progressGuard(progress);
    QThread* worker = QThread::create([guard, progressGuard, items, owner] {
        const auto result = filemanager::platform::RecycleBinOperation::
            restoreSelectedShellItemsIsolated(items, owner);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, progressGuard, items, result] {
            if (!guard)
                return;
            if (progressGuard)
                progressGuard->close();
            guard->statusBar()->clearMessage();
            if (!result.success) {
                QMessageBox::warning(guard, "Restore selected", result.error);
                return;
            }
            QList<QByteArray> restoredIdentities;
            restoredIdentities.reserve(items.size());
            for (const auto& item : items)
                restoredIdentities.append(item.idList);
            guard->sessionUndo_.discardTrashEntriesContaining(restoredIdentities);
            if (guard->active_)
                guard->active_->refresh();
            guard->updateStatus();
            guard->statusBar()->showMessage(
                tr("Restored %1 to the original location.")
                    .arg(filemanager::ui::localizedItems(result.restoredItems)), 7000);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::emptyRecycleBin()
{
    const auto contents = filemanager::platform::RecycleBinOperation::summary();
    if (!contents.success) {
        QMessageBox::warning(this, "Empty Recycle Bin", contents.error);
        return;
    }
    if (contents.itemCount == 0) {
        QMessageBox::information(this, "Empty Recycle Bin", "The Recycle Bin is already empty.");
        return;
    }
    const QString prompt = tr("Permanently delete all %1 (%2) from every Recycle Bin?\n\n"
                              "This cannot be undone. File Manager's session undo will no longer "
                              "offer recycled items, but rename and move undo entries are preserved.")
        .arg(filemanager::ui::localizedItems(static_cast<qint64>(contents.itemCount)))
        .arg(QLocale().formattedDataSize(static_cast<qint64>(contents.totalBytes)));
    if (QMessageBox::warning(this, "Empty Recycle Bin", prompt,
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    const auto result = filemanager::platform::RecycleBinOperation::empty(winId());
    if (!result.success) {
        if (!result.aborted)
            QMessageBox::warning(this, "Empty Recycle Bin", result.error);
        return;
    }
    sessionUndo_.discardAllTrashEntries();
    if (active_)
        active_->refresh();
    updateStatus();
    statusBar()->showMessage("Recycle Bin emptied permanently.", 7000);
}

void MainWindow::showShellNamespace(const QString& parsingName)
{
    if (active_)
        active_->navigateShellParsingName(parsingName);
}

void MainWindow::showNetworkBrowser()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Network locations");
    dialog.resize(760, 500);
    auto* layout = new QVBoxLayout(&dialog);
    auto* disclosure = new QLabel(
        "Network discovery runs only when you press Scan network. A scan may generate "
        "network traffic and Windows authentication attempts.", &dialog);
    disclosure->setObjectName("Muted");
    disclosure->setWordWrap(true);
    layout->addWidget(disclosure);
    auto* direct = new QLineEdit("\\\\", &dialog);
    direct->setAccessibleName("Direct UNC or mapped network path");
    direct->setPlaceholderText("Enter a UNC path, for example \\\\server\\share");
    layout->addWidget(direct);
    auto* scanControls = new QHBoxLayout;
    auto* scan = new QPushButton("Scan network", &dialog);
    scan->setAccessibleName("Scan network once");
    auto* cancel = new QPushButton("Cancel scan", &dialog);
    cancel->setEnabled(false);
    auto* status = new QLabel("No scan has run.", &dialog);
    status->setObjectName("Muted");
    scanControls->addWidget(scan);
    scanControls->addWidget(cancel);
    scanControls->addWidget(status, 1);
    layout->addLayout(scanControls);
    auto* resources = new QTreeWidget(&dialog);
    resources->setObjectName("NetworkDiscoveryResults");
    resources->setAccessibleName("Discovered network resources");
    resources->setColumnCount(3);
    resources->setHeaderLabels({"Resource", "Path", "Provider"});
    resources->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    resources->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    resources->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(resources, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    auto* discovery = new filemanager::files::NetworkDiscoveryService(&dialog);
    connect(scan, &QPushButton::clicked, &dialog, [=] {
        resources->clear();
        status->setText("Scanning…");
        scan->setEnabled(false);
        cancel->setEnabled(true);
        discovery->start();
    });
    connect(cancel, &QPushButton::clicked, discovery,
        &filemanager::files::NetworkDiscoveryService::cancel);
    connect(cancel, &QPushButton::clicked, &dialog, [=] {
        status->setText("Scan cancelled.");
        scan->setEnabled(true);
        cancel->setEnabled(false);
    });
    connect(discovery, &filemanager::files::NetworkDiscoveryService::finished,
        &dialog, [=](const QVector<filemanager::files::NetworkResource>& found,
                     const QString& error) {
            for (const auto& resource : found) {
                auto* item = new QTreeWidgetItem(resources,
                    {resource.displayName, resource.remotePath, resource.provider});
                item->setData(0, Qt::UserRole, resource.remotePath);
                item->setIcon(0, style()->standardIcon(resource.container
                    ? QStyle::SP_DirIcon : QStyle::SP_DriveNetIcon));
            }
            status->setText(error.isEmpty()
                ? QString("Found %1 network resource(s).").arg(found.size()) : error);
            scan->setEnabled(true);
            cancel->setEnabled(false);
        });
    const auto openPath = [&] {
        QString path = direct->text().trimmed();
        if (QTreeWidgetItem* item = resources->currentItem()) {
            const QString discovered = item->data(0, Qt::UserRole).toString();
            if (!discovered.isEmpty())
                path = discovered;
        }
        if (!path.isEmpty()) {
            active_->navigate(path);
            dialog.accept();
        }
    };
    connect(resources, &QTreeWidget::itemDoubleClicked, &dialog,
        [&](QTreeWidgetItem*, int) { openPath(); });
    connect(direct, &QLineEdit::returnPressed, &dialog, openPath);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, openPath);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::showNavigationContextMenu(const QPoint& point)
{
    if (!quickAccess_ || !active_)
        return;
    QTreeWidgetItem* item = quickAccess_->itemAt(point);
    if (!item)
        return;
    quickAccess_->setCurrentItem(item);
    const QString kind = item->data(0, filemanager::ui::NavigationTree::KindRole).toString();
    const QString path = item->data(0, filemanager::ui::NavigationTree::PathRole).toString();

    QMenu menu(this);
    if (kind == QStringLiteral("shell-visit")) {
        const QByteArray identity = item->data(0, filemanager::ui::NavigationTree::ShellIdentityRole).toByteArray();
        const QPointer<FilePane> source(active_);
        const auto open = [this, source, identity](ShellTreeTarget target) {
            if (source && active_ == source)
                openShellTreeLocation(identity, target);
        };
        menu.addAction("Open", this, [open] { open(ShellTreeTarget::CurrentPane); });
        menu.addAction("Open in new tab", this, [open] { open(ShellTreeTarget::NewTab); });
        menu.addAction("Open in new window", this, [open] { open(ShellTreeTarget::NewWindow); });
    } else if (kind == QStringLiteral("home")) {
        menu.addAction("Open Home", this, [this] { active_->navigateHome(); });
        menu.addAction("Open Home in new tab", this, [this] {
            setActivePane(workspace_->addTab(QLatin1String(FilePane::HomeIdentity)));
        });
    } else if (kind == QStringLiteral("this-pc")) {
        menu.addAction("Open This PC", this, &MainWindow::showThisPc);
        menu.addAction("Open This PC in new tab", this, [this] {
            setActivePane(workspace_->addTab(filemanager::platform::ShellItem::builtInIdentity(
                filemanager::platform::BuiltInShellLocation::ThisPc)));
        });
        menu.addSeparator();
        menu.addAction("Storage details", this, &MainWindow::showStorageDetails);
        menu.addAction(commands_->action("navigation.map_network_drive"));
        menu.addAction(commands_->action("navigation.disconnect_network_drive"));
    } else if (kind == QStringLiteral("libraries")
            || kind == QStringLiteral("recycle-bin")) {
        const auto location = kind == QStringLiteral("libraries")
            ? filemanager::platform::BuiltInShellLocation::Libraries
            : filemanager::platform::BuiltInShellLocation::RecycleBin;
        const QString label = filemanager::platform::ShellItem::builtInDisplayName(location);
        menu.addAction(tr("Open %1").arg(label), this,
            [this, location] { showBuiltInShell(location); });
        menu.addAction(tr("Open %1 in new tab").arg(label), this, [this, location] {
            setActivePane(workspace_->addTab(
                filemanager::platform::ShellItem::builtInIdentity(location)));
        });
        if (location == filemanager::platform::BuiltInShellLocation::RecycleBin) {
            menu.addSeparator();
            menu.addAction(commands_->action("recycle.empty"));
        }
    } else if (kind == QStringLiteral("network")) {
        menu.addAction("Open Network locations", this, &MainWindow::showNetworkBrowser);
        menu.addSeparator();
        menu.addAction(commands_->action("navigation.map_network_drive"));
        menu.addAction(commands_->action("navigation.disconnect_network_drive"));
    } else if (!path.isEmpty() && !path.startsWith(QStringLiteral("__"))) {
        menu.addAction("Open", this, [this, path] { active_->navigate(path); });
        menu.addAction("Open in new tab", this, [this, path] {
            setActivePane(workspace_->addTab(path));
        });
        menu.addAction("Open in new window", this, [path] {
            auto* window = new MainWindow(path);
            window->setAttribute(Qt::WA_DeleteOnClose);
            window->show();
        });
        menu.addSeparator();
        if (kind == QStringLiteral("favorite"))
            menu.addAction("Unpin from favorites", this, &MainWindow::removeSelectedFavorite);
        else {
            QSettings settings;
            const auto favorites = filemanager::settings::loadFavoriteGroups(settings);
            if (!favorites.containsPath(path))
                menu.addAction("Pin to favorites", this,
                    [this, path] { addFavoritePath(path); });
        }
        menu.addSeparator();
        menu.addAction("Properties", this, [this, path] { showWindowsProperties(path); });
    }
    if (!menu.isEmpty())
        menu.exec(quickAccess_->viewport()->mapToGlobal(point));
}

void MainWindow::refreshStorageLocations()
{
    const auto volumes = filemanager::files::mountedStorageVolumes();
    QStringList roots;
    roots.reserve(volumes.size());
    for (const auto& volume : volumes)
        roots.append(volume.rootPath.toCaseFolded());
    if (roots == storageRoots_)
        return;
    storageRoots_ = roots;
    storageVolumes_ = volumes;
    rebuildQuickAccess();
}

FilePane* MainWindow::otherPane() const { return workspace_->adjacentPane(); }
void MainWindow::updatePreview(const QString& path)
{
    previewImage_->clear();
    previewText_->clear();
    previewTitle_->setText(path.isEmpty() ? "Select a file" : "Loading preview...");
    previewMeta_->clear();
    previewPath_ = path;
    previewBaseMetadata_.clear();
    previewGeneration_ = previewController_->start(path);
    documentMetadata_->request(path);
    mediaMetadata_->request(path);
    shellMetadata_->request(path);
}

void MainWindow::refreshRichPreviewMetadata()
{
    QStringList details;
    if (!previewBaseMetadata_.isEmpty())
        details.append(previewBaseMetadata_);
    if (const auto document = documentMetadata_->request(previewPath_)) {
        QStringList line;
        if (document->pageCount > 0) line.append(tr("%1 pages").arg(document->pageCount));
        if (!document->title.isEmpty()) line.append(tr("Title: %1").arg(document->title));
        if (!document->authors.isEmpty()) line.append(tr("Author: %1").arg(document->authors));
        if (!line.isEmpty()) details.append(line.join('\n'));
    }
    const auto appendMedia = [&details](const filemanager::preview::MediaMetadata& media) {
        QStringList line;
        if (!media.durationText().isEmpty()) line.append(QObject::tr("Duration: %1").arg(media.durationText()));
        if (!media.resolution.isEmpty()) line.append(QObject::tr("Resolution: %1 × %2")
            .arg(media.resolution.width()).arg(media.resolution.height()));
        if (!media.title.isEmpty()) line.append(QObject::tr("Title: %1").arg(media.title));
        if (!media.artists.isEmpty()) line.append(QObject::tr("Artist: %1").arg(media.artists));
        if (!line.isEmpty()) details.append(line.join('\n'));
    };
    if (const auto media = mediaMetadata_->request(previewPath_))
        appendMedia(*media);
    else if (const auto shell = shellMetadata_->request(previewPath_))
        appendMedia(*shell);
    previewMeta_->setText(details.join("\n\n"));
}

void MainWindow::openSelected()
{
    if (active_->isHomeLocation()) {
        active_->activateCurrentHomeItem();
        return;
    }
    if (!active_->isFileSystemLocation()) {
        active_->activateCurrentShellItem();
        return;
    }
    const QString path = active_->selectedPath();
    if (!path.isEmpty())
        active_->openPath(path);
}
void MainWindow::createFolder()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, "New folder",
                                               "Folder name:", QLineEdit::Normal, "New folder", &ok)
                             .trimmed();
    if (!ok)
        return;
    const auto validation = filemanager::files::validateWindowsName(name);
    if (!validation.valid)
    {
        QMessageBox::warning(this, "Invalid folder name", validation.error);
        return;
    }
    if (!QDir(active_->currentPath()).mkdir(validation.normalizedName))
        QMessageBox::warning(this, "Could not create folder",
                             "A folder with that name may already exist.");
    active_->refresh();
}
void MainWindow::createTextFile()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, "New text file", "File name:", QLineEdit::Normal,
                                         "New Text Document.txt", &ok)
                       .trimmed();
    if (!ok || name.isEmpty())
        return;
    if (!name.contains('.'))
        name += ".txt";
    const auto validation = filemanager::files::validateWindowsName(name);
    if (!validation.valid)
    {
        QMessageBox::warning(this, "Invalid file name", validation.error);
        return;
    }
    QFile file(QDir(active_->currentPath()).filePath(validation.normalizedName));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        QMessageBox::warning(this, "Could not create file", file.errorString());
    active_->refresh();
}
void MainWindow::createShortcuts()
{
    const QStringList targets = active_ ? active_->selectedPaths() : QStringList{};
    if (targets.isEmpty()) {
        QMessageBox::information(this, "Create shortcut", "Select one or more items first.");
        return;
    }
    const QString destination = active_->currentPath();
    statusBar()->showMessage(QString("Creating shortcuts for %1…")
                                 .arg(filemanager::ui::localizedItems(targets.size())));
    QPointer<MainWindow> guard(this);
    QThread* worker = QThread::create([guard, targets, destination] {
        const auto result = filemanager::platform::createShellLinks(targets, destination);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, result] {
            if (!guard)
                return;
            if (guard->active_)
                guard->active_->refresh();
            if (result.success()) {
                guard->statusBar()->showMessage(
                    QString("Created %1").arg(filemanager::ui::localizedCount(
                        result.createdPaths.size(), "shortcut", "shortcuts")), 5000);
                return;
            }
            guard->statusBar()->clearMessage();
            QMessageBox::warning(guard, "Shortcut creation incomplete",
                QString("Created: %1\nFailed: %2\n\n%3")
                    .arg(result.createdPaths.size()).arg(result.errors.size())
                    .arg(result.errors.join('\n')));
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}
void MainWindow::renameSelected()
{
    const QString source = active_->selectedPath();
    if (source.isEmpty())
        return;
    QFileInfo info(source);
    const bool wasDirectory = info.isDir();
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, "Rename", "New name:", QLineEdit::Normal, info.fileName(), &ok)
            .trimmed();
    if (!ok)
        return;
    const auto validation = filemanager::files::validateWindowsName(name);
    if (!validation.valid)
    {
        QMessageBox::warning(this, "Invalid name", validation.error);
        return;
    }
    if (validation.normalizedName != info.fileName()) {
        const QString destination = info.dir().filePath(validation.normalizedName);
        const auto before = filemanager::files::captureSourceSnapshot(source);
        const auto renameResult = filemanager::files::renameItem(source, destination);
        if (!renameResult.ok) {
            QMessageBox::warning(this, "Rename failed", renameResult.error);
        } else {
            const auto after = filemanager::files::captureSourceSnapshot(destination);
            followCommittedFileLabels(this, {{source, destination, before, after, wasDirectory}});
            if (sessionUndo_.recordRename(source, destination)) {
                QAction* undo = commands_->action("edit.undo");
                undo->setText(sessionUndo_.nextDescription());
                undo->setEnabled(true);
            }
        }
    }
    active_->refresh();
}
void MainWindow::undoLastSessionOperation()
{
    clearBatchRenameUndoPrompt();
    const auto result = sessionUndo_.undoLast();
    if (!result.ok)
        QMessageBox::warning(this, "Undo refused", result.error);
    else if (result.queuedRequest) {
        const auto& request = *result.queuedRequest;
        startTransfer(request.sources, request.destinationDirectory, true, false);
        statusBar()->showMessage("Cross-volume undo queued as a checked move", 5000);
    }
    else {
        QList<filemanager::tags::TagPathChange> changes;
        for (const auto& [source, destination] : result.restoredPathChanges) {
            const auto current = filemanager::files::captureSourceSnapshot(destination);
            changes.append({source, destination, current, current,
                QFileInfo(destination).isDir()});
        }
        followCommittedFileLabels(this, changes);
        for (FilePane* pane : workspace_->panes())
            pane->refresh();
        active_->revealPath(result.restoredPath);
        statusBar()->showMessage("Operation undone", 3000);
    }
    QAction* undo = commands_->action("edit.undo");
    undo->setEnabled(sessionUndo_.canUndo());
    undo->setText(sessionUndo_.size() > 0 ? sessionUndo_.nextDescription() : QStringLiteral("Undo"));
}

void MainWindow::clearBatchRenameUndoPrompt()
{
    if (!batchRenameUndoPrompt_)
        return;
    auto* button = batchRenameUndoPrompt_.data();
    batchRenameUndoPrompt_ = nullptr;
    statusBar()->removeWidget(button);
    button->deleteLater();
}

void MainWindow::showBatchRenameUndoPrompt()
{
    clearBatchRenameUndoPrompt();
    const quint64 batchEntryId = sessionUndo_.topEntryId();
    if (batchEntryId == 0)
        return;
    auto* button = new QPushButton("Undo batch rename", statusBar());
    button->setObjectName("BatchRenameUndoPrompt");
    button->setAccessibleName("Undo this batch rename");
    button->setAccessibleDescription("Restore this batch only while its renamed files are unchanged.");
    button->setToolTip("Restore the names while the renamed files are unchanged (Ctrl+Z)");
    button->setAutoDefault(false);
    button->setFocusPolicy(Qt::StrongFocus);
    batchRenameUndoPrompt_ = button;
    statusBar()->addPermanentWidget(button);
    connect(button, &QPushButton::clicked, this, [this, button, batchEntryId] {
        if (batchRenameUndoPrompt_ != button)
            return;
        clearBatchRenameUndoPrompt();
        if (sessionUndo_.topEntryId() != batchEntryId)
            return;
        undoLastSessionOperation();
    });
    connect(commands_->action("edit.undo"), &QAction::changed, button, [this, button] {
        if (batchRenameUndoPrompt_ == button)
            clearBatchRenameUndoPrompt();
    });
    QTimer::singleShot(15000, button, [this, button] {
        if (batchRenameUndoPrompt_ == button)
            clearBatchRenameUndoPrompt();
    });
}

void MainWindow::copyToOther(bool move)
{
    FilePane* destinationPane = otherPane();
    if (!destinationPane)
    {
        QMessageBox::information(this, move ? "Move to other panel" : "Copy to other panel",
                                 "Create another panel before copying or moving between panels.");
        return;
    }
    const auto paths = active_->selectedPaths();
    if (paths.isEmpty())
        return;
    startTransfer(paths, destinationPane->currentPath(), move);
}

void MainWindow::startTransfer(const QStringList& sources, const QString& destination, bool move,
    bool recordUndo, std::function<void(bool)> completion)
{
    if (sources.isEmpty())
        return;
    bool removableStorage = filemanager::files::storageLocationTraits(destination)
        .safeEjectRecommended;
    for (const QString& source : sources) {
        removableStorage = removableStorage
            || filemanager::files::storageLocationTraits(source).safeEjectRecommended;
    }
    auto* progress = new QProgressDialog(
        removableStorage
            ? QStringLiteral("Preparing transfer with removable storage…\n"
                             "Do not eject it until this operation finishes.")
            : (move ? QStringLiteral("Preparing move...")
                    : QStringLiteral("Preparing copy...")),
        "Cancel", 0, 0, this);
    progress->setWindowTitle(move ? "Move items" : "Copy items");
    progress->setWindowModality(Qt::NonModal);
    progress->setObjectName("TransferProgressDialog");
    progress->setAccessibleName(move ? "Move transfer progress" : "Copy transfer progress");
    progress->setAccessibleDescription(
        "Reports transfer progress, remaining time, cancellation, conflicts and failures.");
    progress->setAutoClose(false);
    progress->setMinimumDuration(0);
    auto jobId = std::make_shared<quint64>(0);
    auto announcedProgress = std::make_shared<int>(-1);
    connect(progress, &QProgressDialog::canceled, progress,
            [this, jobId] { transferQueue_->cancel(*jobId); });
    connect(transferQueue_, &filemanager::files::TransferQueue::progressDetails, progress,
            [progress, jobId, announcedProgress, move](quint64 id,
                const filemanager::files::TransferProgress& details)
            {
                if (id != *jobId)
                    return;
                const qint64 completed = details.completedBytes;
                const qint64 total = details.totalBytes;
                if (total <= 0)
                {
                    progress->setRange(0, 0);
                }
                else
                {
                    progress->setRange(0, 1000);
                    progress->setValue(static_cast<int>((static_cast<double>(completed) / total) * 1000));
                }
                QStringList status;
                if (!details.currentPath.isEmpty())
                    status.append(QDir::toNativeSeparators(details.currentPath));
                if (total > 0) {
                    status.append(QStringLiteral("%1 of %2")
                        .arg(QLocale().formattedDataSize(completed),
                            QLocale().formattedDataSize(total)));
                }
                if (details.bytesPerSecond > 0)
                    status.append(QLocale().formattedDataSize(details.bytesPerSecond) + "/s");
                if (details.remainingSeconds >= 0) {
                    const qint64 minutes = details.remainingSeconds / 60;
                    const qint64 seconds = details.remainingSeconds % 60;
                    status.append(minutes > 0
                        ? QStringLiteral("%1m %2s remaining").arg(
                            QLocale().toString(minutes), QLocale().toString(seconds))
                        : QStringLiteral("%1s remaining").arg(QLocale().toString(seconds)));
                }
                if (!status.isEmpty())
                    progress->setLabelText(status.join(QStringLiteral("\n")));
                const int percent = filemanager::ui::transferProgressAnnouncementPercent(
                    completed, total);
                if (percent >= 0 && percent != *announcedProgress) {
                    *announcedProgress = percent;
                    announceForAccessibility(progress,
                        QStringLiteral("%1 %2 percent complete.")
                            .arg(move ? QStringLiteral("Move") : QStringLiteral("Copy"))
                            .arg(QLocale().toString(percent)));
                }
            });
    connect(transferQueue_, &filemanager::files::TransferQueue::conflict, progress,
            [this, progress, jobId](quint64 id, const QString& source,
                const QString& target, bool directory)
            {
                if (id != *jobId)
                    return;
                announceForAccessibility(progress,
                    QStringLiteral("Destination conflict for %1. Choose replace, keep both, skip, "
                                   "or cancel.").arg(QFileInfo(source).fileName()), true);
                const auto result = filemanager::ui::ConflictDialog::run(
                    this, source, target, directory);
                transferQueue_->resolveConflict(id, result.decision, result.applyToAll);
            });
    connect(transferQueue_, &filemanager::files::TransferQueue::metadataDegradation, progress,
        [this, progress, jobId](quint64 id, const QString& source, const QString& destination,
            const QStringList& details) {
            if (id != *jobId)
                return;
            announceForAccessibility(progress,
                QStringLiteral("Some file properties cannot be preserved. Review the warning "
                               "before continuing."), true);
            QMessageBox decision(QMessageBox::Warning, "File properties cannot all be preserved",
                QString("Windows reports that this destination cannot preserve every property of "
                        "the selected items.\n\nSource: %1\nDestination: %2\n\n%3\n\n"
                        "Continue for this transfer without those properties?")
                    .arg(QFileInfo(source).fileName(), QDir::toNativeSeparators(destination),
                        details.join('\n')),
                QMessageBox::NoButton, this);
            decision.setObjectName("MetadataDegradationDialog");
            decision.setTextFormat(Qt::PlainText);
            QPushButton* proceed = decision.addButton(
                "Copy without these properties", QMessageBox::AcceptRole);
            QPushButton* cancel = decision.addButton(QMessageBox::Cancel);
            decision.setDefaultButton(cancel);
            decision.exec();
            transferQueue_->resolveMetadataDegradation(id, decision.clickedButton() == proceed);
        });
    connect(transferQueue_, &filemanager::files::TransferQueue::jobFinished, progress,
            [this, progress, jobId, move, recordUndo, removableStorage,
                completion = std::move(completion)](
                quint64 id, bool cancelled, const QStringList& errors)
            {
                if (id != *jobId)
                    return;
                announceForAccessibility(progress,
                    filemanager::ui::transferFinishedAnnouncement(
                        move, cancelled, errors.size()), !errors.isEmpty());
                progress->close();
                progress->deleteLater();
                for (FilePane* pane : workspace_->panes())
                    pane->refresh();
                if (!errors.isEmpty()) {
                    QMessageBox::warning(this, "Transfer incomplete", errors.join('\n'));
                    offerElevatedCopyRetry(id);
                } else if (cancelled)
                    statusBar()->showMessage("Transfer cancelled", 3000);
                else {
                    statusBar()->showMessage(removableStorage
                        ? QStringLiteral("Transfer completed — use Windows Safely Remove Hardware "
                                         "before ejecting removable storage")
                        : QStringLiteral("Transfer completed"), removableStorage ? 8000 : 3000);
                    if (move && recordUndo) {
                        const auto history = transferQueue_->history();
                        const auto completed = std::find_if(history.cbegin(), history.cend(),
                            [id](const auto& record) { return record.id == id; });
                        if (completed != history.cend()
                                && sessionUndo_.recordMove(
                                    completed->request.sources, completed->manifest)) {
                            QAction* undo = commands_->action("edit.undo");
                            undo->setText(sessionUndo_.nextDescription());
                            undo->setEnabled(true);
                        }
                    }
                }
                if (completion)
                    completion(!cancelled && errors.isEmpty());
            });
    progress->show();
    announceForAccessibility(progress,
        QStringLiteral("Preparing %1 of %2.")
            .arg(move ? QStringLiteral("move") : QStringLiteral("copy"))
            .arg(filemanager::ui::localizedItems(sources.size())));
    *jobId = transferQueue_->enqueue({sources, destination,
        move ? filemanager::files::TransferOperation::Move
             : filemanager::files::TransferOperation::Copy});
}

void MainWindow::offerElevatedCopyRetry(quint64 failedJobId)
{
#ifndef Q_OS_WIN
    Q_UNUSED(failedJobId);
    return;
#else
    const auto history = transferQueue_->history();
    const auto record = std::find_if(history.cbegin(), history.cend(),
        [failedJobId](const auto& candidate) { return candidate.id == failedJobId; });
    if (record == history.cend()
            || !filemanager::files::isElevationEligibleCopy(
                record->request, record->failures)) {
        return;
    }
    QStringList sources;
    for (const auto& failure : record->failures) {
        if (QFileInfo::exists(failure.source)
                && !sources.contains(failure.source, Qt::CaseInsensitive)) {
            sources.append(failure.source);
        }
    }
    if (sources.isEmpty() || sources.size() > 32)
        return;

    QMessageBox decision(QMessageBox::Warning, "Administrator copy available",
        QStringLiteral("Windows denied this copy. Retry %1 through Windows with administrator "
            "approval?\n\nWindows may show a UAC prompt. The File Manager window itself will "
            "not become administrator. This fallback is copy-only, cannot unlock an in-use file, "
            "cannot be paused, never overwrites an existing item, and may provide less detailed "
            "progress than the normal recoverable queue.")
            .arg(filemanager::ui::localizedItems(sources.size())),
        QMessageBox::NoButton, this);
    decision.setObjectName("ElevatedCopyRetryDialog");
    decision.setTextFormat(Qt::PlainText);
    QPushButton* retry = decision.addButton(
        "Retry copy through Windows", QMessageBox::AcceptRole);
    QPushButton* cancel = decision.addButton(QMessageBox::Cancel);
    decision.setDefaultButton(cancel);
    decision.exec();
    if (decision.clickedButton() != retry)
        return;

    auto* progress = new QProgressDialog(
        "Waiting for Windows administrator approval…", QString{}, 0, 0, this);
    progress->setObjectName("ElevatedCopyProgressDialog");
    progress->setWindowTitle("Administrator copy");
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->show();
    const QString destination = record->request.destinationDirectory;
    const quintptr ownerWindow = static_cast<quintptr>(winId());
    QPointer<MainWindow> guard(this);
    QPointer<QProgressDialog> progressGuard(progress);
    QThread* worker = QThread::create(
        [guard, progressGuard, sources, destination, ownerWindow] {
            QList<filemanager::platform::ShellItemReference> sourceItems;
            QString resolutionError;
            for (const QString& source : sources) {
                const auto item = filemanager::platform::ShellItem::resolve(source);
                if (!item.valid) {
                    resolutionError = item.error;
                    break;
                }
                sourceItems.append(item);
            }
            const auto destinationItem =
                filemanager::platform::ShellItem::resolve(destination);
            filemanager::platform::ShellFileOperationResult result;
            if (!resolutionError.isEmpty() || !destinationItem.valid
                    || !destinationItem.folder) {
                result.error = resolutionError.isEmpty()
                    ? QStringLiteral("The destination is no longer available.")
                    : resolutionError;
            } else {
                result = filemanager::platform::ShellFileOperation::perform(
                    filemanager::platform::ShellFileOperationKind::Copy,
                    sourceItems, destinationItem, {true, ownerWindow});
            }
            if (!guard)
                return;
            QMetaObject::invokeMethod(guard, [guard, progressGuard, result] {
                if (progressGuard) {
                    progressGuard->close();
                    progressGuard->deleteLater();
                }
                if (!guard)
                    return;
                for (FilePane* pane : guard->workspace_->panes())
                    pane->refresh();
                if (result.success) {
                    guard->statusBar()->showMessage(
                        "Administrator copy completed through Windows", 7000);
                } else if (result.aborted) {
                    guard->statusBar()->showMessage("Administrator copy cancelled", 5000);
                } else {
                    QMessageBox::warning(guard, "Administrator copy failed", result.error);
                }
            });
        });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
#endif
}
void MainWindow::deleteSelected()
{
    const auto paths = active_->selectedPaths();
    if (paths.isEmpty())
        return;
    recyclePaths(paths, this, [this](const QStringList&) { active_->refresh(); });
}

void MainWindow::permanentlyDeleteSelected()
{
    const QStringList paths = active_ ? active_->selectedPaths() : QStringList{};
    if (paths.isEmpty())
        return;
    if (paths.size() > 256) {
        QMessageBox::warning(this, "Delete permanently",
            "Permanent deletion is limited to 256 selected items at a time.");
        return;
    }

    QDialog confirmation(this);
    confirmation.setObjectName("PermanentDeleteDialog");
    confirmation.setWindowTitle("Permanently delete items");
    confirmation.setMinimumWidth(560);
    auto* layout = new QVBoxLayout(&confirmation);
    auto* warning = new QLabel(QString(
        "Permanently delete %1?\n\n"
        "This bypasses the Windows Recycle Bin. File Manager cannot undo or recover this action.")
        .arg(filemanager::ui::localizedItems(paths.size())), &confirmation);
    warning->setObjectName("PermanentDeleteWarning");
    warning->setAccessibleName("Permanent deletion warning");
    warning->setTextFormat(Qt::PlainText);
    warning->setWordWrap(true);
    layout->addWidget(warning);
    QStringList names;
    for (int index = 0; index < qMin(paths.size(), 5); ++index)
        names.append(QStringLiteral("• %1").arg(QFileInfo(paths.at(index)).fileName()));
    if (paths.size() > names.size())
        names.append(QStringLiteral("• …and %1 more").arg(paths.size() - names.size()));
    auto* selection = new QLabel(names.join('\n'), &confirmation);
    selection->setObjectName("PermanentDeleteSelection");
    selection->setAccessibleName("Items selected for permanent deletion");
    selection->setTextFormat(Qt::PlainText);
    selection->setWordWrap(true);
    layout->addWidget(selection);
    auto* instruction = new QLabel("Type DELETE to enable the irreversible action:", &confirmation);
    instruction->setTextFormat(Qt::PlainText);
    layout->addWidget(instruction);
    auto* typed = new QLineEdit(&confirmation);
    typed->setObjectName("PermanentDeleteConfirmation");
    typed->setAccessibleName("Type DELETE to confirm permanent deletion");
    typed->setPlaceholderText("DELETE");
    layout->addWidget(typed);
    auto* buttons = new QDialogButtonBox(&confirmation);
    auto* destroy = buttons->addButton("Delete permanently", QDialogButtonBox::DestructiveRole);
    destroy->setObjectName("ConfirmPermanentDelete");
    destroy->setEnabled(false);
    auto* cancel = buttons->addButton(QDialogButtonBox::Cancel);
    cancel->setDefault(true);
    layout->addWidget(buttons);
    connect(typed, &QLineEdit::textChanged, &confirmation,
        [destroy](const QString& text) { destroy->setEnabled(text == "DELETE"); });
    connect(destroy, &QPushButton::clicked, &confirmation, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &confirmation, &QDialog::reject);
    typed->setFocus();
    if (confirmation.exec() != QDialog::Accepted)
        return;

    auto* progress = new QProgressDialog(
        "Deleting permanently through Windows…", QString(), 0, 0, this);
    progress->setObjectName("PermanentDeleteProgress");
    progress->setWindowTitle("Delete permanently");
    progress->setWindowModality(Qt::WindowModal);
    progress->setCancelButton(nullptr);
    progress->setMinimumDuration(0);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->show();
    statusBar()->showMessage("Permanent deletion in progress…");
    QPointer<MainWindow> guard(this);
    QPointer<QProgressDialog> progressGuard(progress);
    QThread* worker = QThread::create([guard, progressGuard, paths] {
        const auto result = filemanager::platform::RecycleBinOperation::permanentlyDelete(paths);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, progressGuard, result] {
            if (!guard)
                return;
            if (progressGuard)
                progressGuard->close();
            if (guard->active_)
                guard->active_->refresh();
            if (result.success) {
                guard->statusBar()->showMessage(
                    QString("Permanently deleted %1; no undo is available")
                        .arg(filemanager::ui::localizedItems(result.deletedPaths.size())), 7000);
                return;
            }
            guard->statusBar()->clearMessage();
            QMessageBox::warning(guard, "Permanent deletion incomplete",
                QString("Permanently deleted: %1\nRemaining: %2\n\n%3\n\n"
                        "Deleted items cannot be restored by File Manager.")
                    .arg(result.deletedPaths.size()).arg(result.remainingPaths.size())
                    .arg(result.error));
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::recyclePaths(const QStringList& paths, QWidget* dialogParent,
    std::function<void(const QStringList&)> completion)
{
    if (paths.isEmpty())
        return;
    if (QMessageBox::question(
            dialogParent, "Move to Recycle Bin",
            QString("Move %1 to the Recycle Bin?").arg(
                filemanager::ui::localizedItems(paths.size())),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    QStringList failures;
    QStringList removedPaths;
    QList<filemanager::files::TrashResult> recycled;
    int withoutUndoIdentity = 0;
    for (const auto& path : paths)
    {
        const auto result = filemanager::files::moveToTrashDetailed(path);
        if (!result.ok)
            failures << result.error;
        else {
            removedPaths.append(path);
            if (!result.recycledItemId.isEmpty())
                recycled.append(result);
            else
                ++withoutUndoIdentity;
        }
    }
    if (!recycled.isEmpty() && sessionUndo_.recordTrash(recycled)) {
        QAction* undo = commands_->action("edit.undo");
        undo->setText(sessionUndo_.nextDescription());
        undo->setEnabled(true);
    }
    if (!failures.isEmpty())
        QMessageBox::warning(dialogParent, "Some items were not deleted", failures.join("\n"));
    else if (withoutUndoIdentity > 0)
        statusBar()->showMessage(QStringLiteral(
            "%1 were recycled, but Windows did not provide an identity for session undo.")
            .arg(filemanager::ui::localizedItems(withoutUndoIdentity)), 7000);
    if (completion)
        completion(removedPaths);
}
void MainWindow::showGoTo()
{
    struct Destination
    {
        int groupOrder = 0;
        QString group;
        QString label;
        QString path;
    };

    QList<Destination> destinations;
    QSet<QString> seen;
    const auto addDestination = [&](int order, const QString& group, const QString& label,
                                    const QString& rawPath) {
        if (destinations.size() >= 256 || rawPath.trimmed().isEmpty())
            return;
        const QString path = QDir::cleanPath(QFileInfo(rawPath).absoluteFilePath());
        const QString key = path.toCaseFolded();
        if (seen.contains(key))
            return;
        seen.insert(key);
        const QString display = label.trimmed().isEmpty()
            ? QDir::toNativeSeparators(path) : label.trimmed();
        destinations.append({order, group, display, path});
    };

    int tabNumber = 1;
    for (FilePane* pane : workspace_->panes()) {
        const QString path = pane->currentPath();
        const QString name = QFileInfo(path).fileName();
        addDestination(10, "Open tabs",
            name.isEmpty() ? QString("Tab %1").arg(tabNumber)
                           : QString("%1 — Tab %2").arg(name).arg(tabNumber),
            path);
        ++tabNumber;
    }

    QSettings settings;
    const auto favorites = filemanager::settings::loadFavoriteGroups(settings);
    for (int groupIndex = 0; groupIndex < favorites.groups.size(); ++groupIndex) {
        const auto& group = favorites.groups.at(groupIndex);
        for (const QString& path : group.paths) {
            const QString name = QFileInfo(path).fileName();
            addDestination(20 + groupIndex, "Favorites — " + group.name,
                name.isEmpty() ? QDir::toNativeSeparators(path) : name, path);
        }
    }
    for (const QString& path : settings.value("navigation/recents").toStringList()) {
        const QString name = QFileInfo(path).fileName();
        addDestination(100, "Recent locations",
            name.isEmpty() ? QDir::toNativeSeparators(path) : name, path);
    }

    using filemanager::platform::KnownFolder;
    const QList<QPair<QString, KnownFolder>> knownFolders = {
        {"Desktop", KnownFolder::Desktop}, {"Documents", KnownFolder::Documents},
        {"Downloads", KnownFolder::Downloads}, {"Music", KnownFolder::Music},
        {"Pictures", KnownFolder::Pictures}, {"Videos", KnownFolder::Videos}};
    for (const auto& [label, folder] : knownFolders)
        addDestination(110, "Windows folders", label,
            filemanager::platform::knownFolderPath(folder));
    for (const auto& volume : filemanager::files::mountedStorageVolumes())
        addDestination(120, "Drives", volume.displayName, volume.rootPath);

    QDialog dialog(this);
    dialog.setObjectName("GoToOverlay");
    dialog.setWindowTitle("Go To");
    dialog.resize(680, 560);
    auto* layout = new QVBoxLayout(&dialog);
    auto* search = new QLineEdit(&dialog);
    search->setObjectName("GoToSearch");
    search->setAccessibleName("Search destinations or enter an absolute folder path");
    search->setPlaceholderText("Search tabs, favorites, recent locations, folders and drives…");
    search->setClearButtonEnabled(true);
    auto* results = new QListWidget(&dialog);
    results->setObjectName("GoToResults");
    results->setAccessibleName("Grouped navigation destinations");
    results->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(search);
    layout->addWidget(results, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel,
        &dialog);
    QPushButton* open = buttons->button(QDialogButtonBox::Open);
    open->setText("Go");
    layout->addWidget(buttons);

    const auto fill = [=](const QString& rawQuery) {
        struct RankedDestination { Destination destination; int score = 0; };
        QList<RankedDestination> ranked;
        const QString query = rawQuery.trimmed();
        for (const Destination& destination : destinations) {
            const auto match = filemanager::search::match(
                query, destination.label + ' ' + QDir::toNativeSeparators(destination.path));
            if (match.matched)
                ranked.append({destination, match.score});
        }
        if (!query.isEmpty() && QFileInfo(query).isAbsolute()) {
            const QString typed = QDir::cleanPath(QFileInfo(query).absoluteFilePath());
            const bool duplicate = std::any_of(ranked.cbegin(), ranked.cend(),
                [&](const RankedDestination& candidate) {
                    return candidate.destination.path.compare(typed, Qt::CaseInsensitive) == 0;
                });
            if (!duplicate)
                ranked.prepend({{0, "Typed path", "Open typed folder", typed}, 1000});
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
            if (left.destination.groupOrder != right.destination.groupOrder)
                return left.destination.groupOrder < right.destination.groupOrder;
            if (left.score != right.score)
                return left.score > right.score;
            const int labels = QString::localeAwareCompare(
                left.destination.label, right.destination.label);
            if (labels != 0)
                return labels < 0;
            return left.destination.path.compare(
                right.destination.path, Qt::CaseInsensitive) < 0;
        });

        results->clear();
        QString currentGroup;
        int displayed = 0;
        QListWidgetItem* firstDestination = nullptr;
        for (const RankedDestination& candidate : ranked) {
            if (displayed >= 100)
                break;
            if (candidate.destination.group != currentGroup) {
                currentGroup = candidate.destination.group;
                auto* heading = new QListWidgetItem(currentGroup, results);
                heading->setData(Qt::UserRole + 1, "heading");
                heading->setFlags(Qt::NoItemFlags);
                QFont font = heading->font();
                font.setBold(true);
                heading->setFont(font);
            }
            auto* item = new QListWidgetItem(
                candidate.destination.label + "\n" + QDir::toNativeSeparators(
                    candidate.destination.path), results);
            item->setData(Qt::UserRole, candidate.destination.path);
            item->setData(Qt::UserRole + 1, "destination");
            item->setToolTip(QDir::toNativeSeparators(candidate.destination.path));
            if (!firstDestination)
                firstDestination = item;
            ++displayed;
        }
        if (firstDestination)
            results->setCurrentItem(firstDestination);
        open->setEnabled(firstDestination != nullptr);
    };
    connect(search, &QLineEdit::textChanged, &dialog, fill);
    connect(results, &QListWidget::currentItemChanged, &dialog,
        [open](QListWidgetItem* current) {
            open->setEnabled(current
                && current->data(Qt::UserRole + 1).toString() == "destination");
        });
    connect(results, &QListWidget::itemDoubleClicked, &dialog,
        [&](QListWidgetItem* item) {
            if (item && item->data(Qt::UserRole + 1).toString() == "destination")
                dialog.accept();
        });
    connect(search, &QLineEdit::returnPressed, &dialog, [&] {
        if (open->isEnabled())
            dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    fill({});
    search->setFocus();

    if (dialog.exec() != QDialog::Accepted)
        return;
    QListWidgetItem* selected = results->currentItem();
    const QString path = selected ? selected->data(Qt::UserRole).toString() : QString();
    if (!QFileInfo(path).isDir()) {
        QMessageBox::warning(this, "Go To", "The selected folder is unavailable.");
        return;
    }
    active_->navigate(path);
}
void MainWindow::copySelection(bool cut)
{
    if (!active_->isFileSystemLocation()) {
        if (cut)
            return;
        const auto items = active_->selectedShellItems();
        if (items.isEmpty())
            return;
        auto* mime = new QMimeData;
        QString error;
        if (!filemanager::platform::ShellClipboard::populateShellItems(mime, items, &error)) {
            delete mime;
            QMessageBox::warning(this, "Copy Windows items", error);
            return;
        }
        QApplication::clipboard()->setMimeData(mime);
        statusBar()->showMessage(QStringLiteral("Copied %1 from the Windows namespace")
            .arg(filemanager::ui::localizedItems(items.size())), 2500);
        return;
    }
    const QStringList paths = active_->selectedPaths();
    copyPathsToClipboard(paths, cut);
}

void MainWindow::copyPathsToClipboard(const QStringList& paths, bool cut)
{
    if (paths.isEmpty())
        return;
    auto* mime = new QMimeData;
    filemanager::platform::ShellClipboard::populateFileTransfer(mime, paths,
        cut ? filemanager::platform::ShellTransferEffect::Move
            : filemanager::platform::ShellTransferEffect::Copy);
    QApplication::clipboard()->setMimeData(mime);
    statusBar()->showMessage(QString("%1 %2").arg(cut ? "Cut" : "Copied",
                                 filemanager::ui::localizedItems(paths.size())), 2500);
}
void MainWindow::pasteClipboard()
{
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    const bool cut = filemanager::platform::ShellClipboard::preferredEffect(mime)
        == filemanager::platform::ShellTransferEffect::Move;
    QStringList sources;
    if (mime->hasUrls()) {
        for (const QUrl& url : mime->urls()) {
            if (url.isLocalFile())
                sources.append(url.toLocalFile());
        }
    }
    if (!sources.isEmpty()) {
        const quint64 clipboardSequence = filemanager::platform::ShellClipboard::sequenceNumber();
        startTransfer(sources, active_->currentPath(), cut, true,
            [this, cut, clipboardSequence](bool success) {
                if (!success)
                    return;
                QString completionError;
                const bool published = filemanager::platform::ShellClipboard::publishPerformedEffect(
                    cut ? filemanager::platform::ShellTransferEffect::Move
                        : filemanager::platform::ShellTransferEffect::Copy,
                    clipboardSequence, &completionError);
                if (!published && !completionError.contains("ownership changed")) {
                    statusBar()->showMessage(
                        "Transfer completed, but Windows clipboard completion could not be published: "
                            + completionError, 10000);
                }
            });
        return;
    }

    const auto clipboardItems = filemanager::platform::ShellClipboard::shellItems();
    if (!clipboardItems.available)
        return;
    if (!clipboardItems.error.isEmpty()) {
        QMessageBox::warning(this, "Paste Windows items", clipboardItems.error);
        return;
    }
    const quint64 clipboardSequence = filemanager::platform::ShellClipboard::sequenceNumber();
    if (!copyShellItemsToFolder(
            clipboardItems.items, active_->currentPath(), cut, this)) {
        return;
    }
    QString completionError;
    filemanager::platform::ShellClipboard::publishPerformedEffect(
        filemanager::platform::ShellTransferEffect::Copy, clipboardSequence, &completionError);
}

bool MainWindow::copyShellItemsToFolder(
    const QList<filemanager::platform::ShellItemReference>& items,
    const QString& destinationPath, bool moveRequested, QWidget* dialogParent)
{
    if (items.isEmpty())
        return false;
    QWidget* parent = dialogParent ? dialogParent : this;
    if (moveRequested && QMessageBox::question(parent, "Copy virtual Windows items instead?",
            "Moving a virtual Windows item can delete or alter its provider-owned source. "
            "File Manager has verified only the non-destructive copy path for virtual items. "
            "Copy the selection into the current folder instead?",
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return false;
    }
    const auto destination = filemanager::platform::ShellItem::resolve(destinationPath);
    if (!destination.valid || !destination.folder) {
        QMessageBox::warning(parent, "Copy Windows items",
            "The current folder could not be represented as a Windows Shell destination.");
        return false;
    }
    QStringList names;
    for (const auto& item : items) {
        if (names.size() < 12)
            names.append(item.displayName.isEmpty() ? item.parsingName : item.displayName);
    }
    const QString remainder = items.size() > names.size()
        ? QStringLiteral("\n…and %1 more").arg(items.size() - names.size())
        : QString{};
    QMessageBox confirmation(QMessageBox::Question, "Copy Windows items",
        QStringLiteral("Copy %1 into:\n%2\n\n%3%4\n\nWindows will keep existing destinations "
            "and rename collisions. Virtual providers may materialize content only after you "
            "choose Copy.")
            .arg(filemanager::ui::localizedItems(items.size()),
                QDir::toNativeSeparators(destinationPath), names.join('\n'), remainder),
        QMessageBox::Yes | QMessageBox::Cancel, parent);
    confirmation.setObjectName("VirtualShellCopyConfirmation");
    confirmation.setDefaultButton(QMessageBox::Cancel);
    if (confirmation.exec() != QMessageBox::Yes)
        return false;

    QStringList failures;
    for (const auto& item : items) {
        const auto result = filemanager::platform::ShellFileOperation::perform(
            filemanager::platform::ShellFileOperationKind::Copy, item, destination);
        if (!result.success)
            failures.append(QStringLiteral("%1: %2")
                .arg(item.displayName.isEmpty() ? item.parsingName : item.displayName, result.error));
    }
    for (FilePane* pane : workspace_->panes()) {
        if (QFileInfo(pane->currentPath()) == QFileInfo(destinationPath))
            pane->refresh();
    }
    if (!failures.isEmpty()) {
        QMessageBox::warning(parent, "Windows item copy incomplete",
            QStringLiteral("%1 of %2 item(s) could not be copied:\n%3")
                .arg(failures.size()).arg(items.size()).arg(failures.join('\n')));
        return false;
    }
    statusBar()->showMessage(QStringLiteral("Copied %1 from the Windows namespace")
        .arg(filemanager::ui::localizedItems(items.size())), 5000);
    return true;
}
void MainWindow::showProperties()
{
    const QString path = active_->selectedPath();
    if (path.isEmpty())
        return;
    showWindowsProperties(path);
}

void MainWindow::showDetails()
{
    QStringList paths = active_ ? active_->selectedPaths() : QStringList{};
    if (paths.isEmpty() && active_ && !active_->selectedPath().isEmpty())
        paths.append(active_->selectedPath());
    if (paths.isEmpty())
        return;
    filemanager::ui::FileDetailsDialog dialog(paths, this);
    connect(&dialog, &filemanager::ui::FileDetailsDialog::windowsPropertiesRequested,
        &dialog, [this](const QString& path) { showWindowsProperties(path); });
    dialog.exec();
}

void MainWindow::showWindowsProperties(const QString& path)
{
    if (!QFileInfo(path).exists()) {
        QMessageBox::information(this, "Properties", "The selected item is no longer available.");
        return;
    }
#ifdef Q_OS_WIN
    const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST;
    info.hwnd = reinterpret_cast<HWND>(winId());
    info.lpVerb = L"properties";
    info.lpFile = nativePath.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info))
        QMessageBox::warning(this, "Properties", "Windows could not open the Properties dialog.");
#endif
}
void MainWindow::analyzeSelected()
{
    const QString path = active_->selectedPath();
    if (path.isEmpty())
        return;
    bool refresh = false;
    for (;;) {
        QProgressDialog progress(refresh ? "Refreshing folder size..."
                                         : "Checking cached size and analyzing...",
            "Cancel", 0, 0, this);
        progress.setWindowTitle("Analyze");
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);
        filemanager::files::AnalysisService service;
        filemanager::files::AnalysisResult result;
        connect(&progress, &QProgressDialog::canceled, &service,
            &filemanager::files::AnalysisService::cancel);
        connect(&service, &filemanager::files::AnalysisService::progress, &progress,
            [&progress](quint64, const QString&,
                const filemanager::files::PathStatistics& partial) {
                progress.setLabelText(QString("Scanning… %1 files, %2 folders, %3")
                    .arg(partial.files).arg(partial.folders)
                    .arg(QLocale().formattedDataSize(static_cast<qint64>(partial.bytes))));
            });
        connect(&service, &filemanager::files::AnalysisService::finished, &progress,
            [&](quint64, const filemanager::files::AnalysisResult& completed) {
                result = completed;
                progress.accept();
            });
        service.start(path, refresh);
        progress.exec();
        if (result.path.isEmpty() || result.statistics.cancelled)
            return;
        if (!result.statistics.error.isEmpty()) {
            QMessageBox::warning(this, "Analysis failed", result.statistics.error);
            return;
        }

        QString details = QString("Path: %1\n\nSize: %2\nFiles: %3\nFolders: %4")
            .arg(QDir::toNativeSeparators(result.path),
                QLocale().formattedDataSize(static_cast<qint64>(result.statistics.bytes)))
            .arg(result.statistics.files)
            .arg(result.statistics.folders);
        if (!result.sha256.isEmpty())
            details += "\n\nSHA-256:\n" + result.sha256;
        if (QFileInfo(path).isDir()) {
            details += QString("\n\n%1: %2\nCached in memory for up to 5 minutes.")
                .arg(result.fromCache ? "Cached measurement" : "Measured")
                .arg(QLocale().toString(result.measuredAt.toLocalTime(), QLocale::ShortFormat));
        }
        QMessageBox results(QMessageBox::Information, "Analysis results", details,
            QMessageBox::Close, this);
        QPushButton* refreshButton = nullptr;
        if (QFileInfo(path).isDir())
            refreshButton = results.addButton("Refresh", QMessageBox::ActionRole);
        results.exec();
        if (!refreshButton || results.clickedButton() != refreshButton)
            return;
        refresh = true;
    }
}
void MainWindow::findDuplicateFiles()
{
    const QString root = active_->currentPath();
    QProgressDialog progress("Comparing files by size and content...", "Cancel", 0, 0, this);
    progress.setWindowTitle("Find duplicate files");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    filemanager::files::DuplicateService service;
    filemanager::files::DuplicateScanResult result;
    connect(&progress, &QProgressDialog::canceled, &service,
        &filemanager::files::DuplicateService::cancel);
    connect(&service, &filemanager::files::DuplicateService::finished, &progress,
        [&](quint64, const filemanager::files::DuplicateScanResult& completed) {
            result = completed;
            progress.accept();
        });
    service.start(root);
    progress.exec();
    if (result.cancelled)
        return;

    QDialog dialog(this);
    dialog.setWindowTitle("Duplicate files");
    dialog.resize(860, 560);
    auto* layout = new QVBoxLayout(&dialog);
    const quint64 rawReclaimable = filemanager::files::duplicateReclaimableBytes(result);
    const quint64 maximumDisplayBytes =
        static_cast<quint64>((std::numeric_limits<qint64>::max)());
    const qint64 reclaimableBytes = static_cast<qint64>(
        qMin(rawReclaimable, maximumDisplayBytes));
    auto* summary = new QLabel(QString(
        "Scanned %1. Found %2. Potentially reclaimable: %3.\n"
        "Review only—File Manager will not delete duplicates automatically.")
        .arg(filemanager::ui::localizedFiles(result.filesScanned))
        .arg(filemanager::ui::localizedCount(result.groups.size(),
            "verified group", "verified groups"))
        .arg(QLocale().formattedDataSize(reclaimableBytes)), &dialog);
    summary->setWordWrap(true);
    auto* tree = new QTreeWidget(&dialog);
    tree->setHeaderLabels({"File", "Size", "SHA-256"});
    tree->setAlternatingRowColors(true);
    for (int groupIndex = 0; groupIndex < result.groups.size(); ++groupIndex) {
        const auto& group = result.groups[groupIndex];
        auto* parent = new QTreeWidgetItem(tree,
            {QString("Duplicate group %1 (%2)").arg(QLocale().toString(groupIndex + 1),
                 filemanager::ui::localizedFiles(group.paths.size())),
                QLocale().formattedDataSize(static_cast<qint64>(group.bytesPerFile)), group.sha256});
        parent->setExpanded(true);
        for (const QString& path : group.paths) {
            auto* item = new QTreeWidgetItem(parent,
                {QDir::toNativeSeparators(path),
                    QLocale().formattedDataSize(static_cast<qint64>(group.bytesPerFile)), {}});
            item->setData(0, Qt::UserRole, path);
        }
    }
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->resizeSection(1, 110);
    tree->header()->resizeSection(2, 420);
    connect(tree, &QTreeWidget::itemDoubleClicked, &dialog, [](QTreeWidgetItem* item, int) {
        const QString path = item->data(0, Qt::UserRole).toString();
        if (!path.isEmpty())
            filemanager::platform::openWithSystemShell(path);
    });
    layout->addWidget(summary);
    layout->addWidget(tree, 1);
    if (!result.errors.isEmpty()) {
        auto* warning = new QLabel(QString("%1 could not be read. Results exclude them.")
            .arg(filemanager::ui::localizedFiles(result.errors.size())), &dialog);
        warning->setToolTip(result.errors.join('\n'));
        layout->addWidget(warning);
    }
    auto* actions = new QHBoxLayout;
    auto* copyPaths = new QPushButton("Copy selected paths", &dialog);
    auto* openLocation = new QPushButton("Open containing folder", &dialog);
    auto* exportCsv = new QPushButton("Export CSV…", &dialog);
    actions->addWidget(copyPaths);
    actions->addWidget(openLocation);
    actions->addWidget(exportCsv);
    actions->addStretch(1);
    layout->addLayout(actions);
    const auto selectedPaths = [tree] {
        QStringList paths;
        for (QTreeWidgetItem* item : tree->selectedItems()) {
            const QString path = item->data(0, Qt::UserRole).toString();
            if (!path.isEmpty())
                paths.append(path);
        }
        paths.removeDuplicates();
        return paths;
    };
    connect(copyPaths, &QPushButton::clicked, &dialog, [selectedPaths] {
        const QStringList paths = selectedPaths();
        if (!paths.isEmpty())
            QApplication::clipboard()->setText(paths.join('\n'));
    });
    connect(openLocation, &QPushButton::clicked, &dialog, [selectedPaths] {
        const QStringList paths = selectedPaths();
        if (!paths.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(paths.first()).absolutePath()));
    });
    connect(exportCsv, &QPushButton::clicked, &dialog, [this, &dialog, result] {
        const QString destination = QFileDialog::getSaveFileName(&dialog,
            "Export duplicate report", QDir(active_->currentPath()).filePath("duplicates.csv"),
            "CSV files (*.csv)");
        if (destination.isEmpty())
            return;
        QString error;
        if (!filemanager::files::writeDuplicateReportCsv(destination, result, &error))
            QMessageBox::warning(&dialog, "Could not export report", error);
        else
            statusBar()->showMessage("Duplicate report exported without changing any files", 5000);
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}
void MainWindow::openTerminal()
{
    const QString path = active_->currentPath();
    if (!QProcess::startDetached("wt.exe", {"-d", path}))
        QProcess::startDetached("cmd.exe", {"/K", "cd", "/D", QDir::toNativeSeparators(path)});
}
void MainWindow::updateStatus()
{
    if (!active_)
        return;
    const int selected = active_->selectedItemCount();
    const int total = active_->visibleItemCount();
    statusBar()->showMessage(QString("%1    %2 selected    %3")
                                 .arg(filemanager::ui::localizedItems(total))
                                 .arg(QLocale().toString(selected))
                                 .arg(active_->isFileSystemLocation()
                                     ? QDir::toNativeSeparators(active_->currentPath())
                                     : active_->currentLocation()));
}
void MainWindow::showSearch()
{
    showAsyncSearch();
}

void MainWindow::showAsyncSearch(const QString& savedSearchId)
{
    filemanager::ui::FileSearchDialog dialog(
        active_->currentPath().isEmpty() ? QDir::homePath() : active_->currentPath(),
        &filenameSearchHistory_, this);
    if (!savedSearchId.isEmpty() && !dialog.activateSavedSearch(savedSearchId)) {
        QMessageBox::information(this, "Saved search unavailable",
            "This saved search was removed or could not be decrypted.");
        return;
    }
    connect(&dialog, &filemanager::ui::FileSearchDialog::openRequested,
        this, [this, &dialog](const QString& path) {
            const QFileInfo info(path);
            if (!info.exists()) {
                QMessageBox::information(&dialog, "Search result unavailable",
                    "The selected item no longer exists.");
                return;
            }
            if (info.isDir()) {
                active_->navigate(info.absoluteFilePath());
                dialog.accept();
            } else {
                const auto result =
                    filemanager::platform::openWithSystemShell(info.absoluteFilePath());
                if (!result.success)
                    QMessageBox::warning(&dialog, "Could not open result", result.error);
            }
        });
    connect(&dialog, &filemanager::ui::FileSearchDialog::revealRequested,
        this, [this, &dialog](const QString& path) {
            active_->revealPath(path);
            dialog.accept();
        });
    connect(&dialog, &filemanager::ui::FileSearchDialog::clipboardRequested,
        this, [this](const QStringList& paths, bool cut) {
            copyPathsToClipboard(paths, cut);
        });
    connect(&dialog, &filemanager::ui::FileSearchDialog::shareRequested,
        this, [this](const QStringList& paths) {
            statusBar()->showMessage(
                tr("Preparing %1 for Windows Share…").arg(filemanager::ui::localizedCount(
                    paths.size(), "search result", "search results")));
            shareController_->showShareUi(static_cast<quintptr>(winId()), paths);
        });
    connect(&dialog, &filemanager::ui::FileSearchDialog::recycleRequested,
        this, [this, &dialog](const QStringList& paths) {
            recyclePaths(paths, &dialog, [&dialog](const QStringList& removed) {
                dialog.removeResults(removed);
            });
        });
    dialog.exec();
}

void MainWindow::showContentSearch()
{
    filemanager::ui::ContentSearchDialog dialog(active_->currentPath(), this);
    connect(&dialog, &filemanager::ui::ContentSearchDialog::openRequested,
        this, [this, &dialog](const QString& path) {
            const QFileInfo info(path);
            if (!info.exists()) {
                QMessageBox::information(&dialog, "Search result unavailable",
                    "The selected item no longer exists.");
                return;
            }
            if (info.isDir()) {
                active_->navigate(info.absoluteFilePath());
                dialog.accept();
            } else {
                const auto result =
                    filemanager::platform::openWithSystemShell(info.absoluteFilePath());
                if (!result.success)
                    QMessageBox::warning(&dialog, "Could not open result", result.error);
            }
        });
    connect(&dialog, &filemanager::ui::ContentSearchDialog::revealRequested,
        this, [this, &dialog](const QString& path) {
            active_->revealPath(path);
            dialog.accept();
        });
    connect(&dialog, &filemanager::ui::ContentSearchDialog::clipboardRequested,
        this, [this](const QStringList& paths, bool cut) {
            copyPathsToClipboard(paths, cut);
        });
    connect(&dialog, &filemanager::ui::ContentSearchDialog::shareRequested,
        this, [this](const QStringList& paths) {
            statusBar()->showMessage(
                tr("Preparing %1 for Windows Share…").arg(filemanager::ui::localizedCount(
                    paths.size(), "search result", "search results")));
            shareController_->showShareUi(static_cast<quintptr>(winId()), paths);
        });
    connect(&dialog, &filemanager::ui::ContentSearchDialog::recycleRequested,
        this, [this, &dialog](const QStringList& paths) {
            recyclePaths(paths, &dialog, [&dialog](const QStringList& removed) {
                dialog.removeResults(removed);
            });
        });
    dialog.exec();
}

void MainWindow::showCommandPalette()
{
    class CommandRowDelegate final : public QStyledItemDelegate {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;
        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
            return {option.rect.width(), index.data(Qt::UserRole + 3).toBool()
                ? option.fontMetrics.height() + 12
                : index.data(Qt::UserRole + 1).toString().isEmpty()
                    ? option.fontMetrics.height() + 12
                    : option.fontMetrics.height() * 2 + 14};
        }
        void paint(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override {
            QStyleOptionViewItem panel(option);
            initStyleOption(&panel, index);
            panel.text.clear();
            const auto* style = option.widget ? option.widget->style() : QApplication::style();
            style->drawControl(QStyle::CE_ItemViewItem, &panel, painter, option.widget);
            painter->save();
            const bool selected = option.state & QStyle::State_Selected;
            const bool header = index.data(Qt::UserRole + 3).toBool();
            const QRect rect = option.rect.adjusted(12, 3, -12, -3);
            if (header) {
                QFont font = option.font;
                font.setBold(true);
                painter->setFont(font);
                QColor headingColor = option.palette.color(QPalette::Text);
                if (!filemanager::ui::applicationHighContrastEnabled())
                    headingColor.setAlphaF(0.72f);
                painter->setPen(headingColor);
                painter->drawText(rect, Qt::AlignVCenter | Qt::AlignLeft,
                                  index.data(Qt::DisplayRole).toString());
            } else {
                const QString title = index.data(Qt::DisplayRole).toString();
                const QString detail = index.data(Qt::UserRole + 1).toString();
                const QString shortcut = index.data(Qt::UserRole + 2).toString();
                const int shortcutWidth = shortcut.isEmpty() ? 0
                    : option.fontMetrics.horizontalAdvance(shortcut) + 22;
                const int titleWidth = qMax(40, rect.width() - shortcutWidth - 8);
                painter->setPen(option.palette.color(selected
                    ? QPalette::HighlightedText : QPalette::Text));
                painter->drawText(QRect(rect.x(), rect.y(), titleWidth,
                    option.fontMetrics.height() + 2), Qt::AlignVCenter | Qt::AlignLeft,
                    option.fontMetrics.elidedText(title, Qt::ElideRight, titleWidth));
                if (!detail.isEmpty()) {
                    QColor detailColor = option.palette.color(selected
                        ? QPalette::HighlightedText : QPalette::Text);
                    if (!selected && !filemanager::ui::applicationHighContrastEnabled())
                        detailColor.setAlphaF(0.72f);
                    painter->setPen(detailColor);
                    painter->drawText(QRect(rect.x(), rect.y() + option.fontMetrics.height() + 1,
                        rect.width(), option.fontMetrics.height() + 2), Qt::AlignVCenter | Qt::AlignLeft,
                        option.fontMetrics.elidedText(detail, Qt::ElideRight, rect.width()));
                }
                if (!shortcut.isEmpty()) {
                    const QRect chip(rect.right() - shortcutWidth + 1, rect.y() + 3,
                                     shortcutWidth, option.fontMetrics.height() + 4);
                    painter->setBrush(option.palette.color(QPalette::Button));
                    painter->setPen(option.palette.color(QPalette::Mid));
                    painter->drawRoundedRect(chip, 5, 5);
                    painter->setPen(option.palette.color(QPalette::ButtonText));
                    painter->drawText(chip, Qt::AlignCenter, shortcut);
                }
            }
            painter->restore();
        }
    };

    QDialog dialog(this);
    dialog.setObjectName("CommandPaletteDialog");
    dialog.setWindowTitle("Command palette");
    dialog.resize(680, 540);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(6);
    auto* queryEdit = new QLineEdit(&dialog);
    queryEdit->setObjectName("CommandPaletteSearch");
    queryEdit->setAccessibleName("Find a command");
    queryEdit->setPlaceholderText("Find a command…");
    queryEdit->setClearButtonEnabled(true);
    auto* results = new QListWidget(&dialog);
    results->setObjectName("CommandPaletteResults");
    results->setAccessibleName("Matching commands");
    results->setItemDelegate(new CommandRowDelegate(results));
    results->setUniformItemSizes(false);
    results->setSpacing(2);
    results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(queryEdit);
    layout->addWidget(results, 1);
    auto* noResults = new QLabel("No commands match. Try another term.", &dialog);
    noResults->setObjectName("CommandPaletteNoResults");
    noResults->setAccessibleName("No matching commands");
    noResults->hide();
    layout->addWidget(noResults);
    auto* hint = new QLabel("↑ ↓  Navigate     Enter  Run command     Esc  Close", &dialog);
    hint->setObjectName("Muted");
    layout->addWidget(hint);

    const QHash<QString, QString> summaries = {
        {"navigation.goto", "Open a folder by path."},
        {"search.files", "Find files by name."},
        {"search.contents", "Search inside eligible files in this folder."},
        {"folder.new", "Create a folder in the current location."},
        {"file.batch_rename", "Preview names for multiple selected items before renaming."},
        {"edit.undo", "Reverse the last supported session operation when still safe."},
        {"panel.inspector", "Show or hide the preview and details pane."},
        {"panel.split_right", "Open a second pane beside the current pane."},
        {"panel.split_bottom", "Open a second pane below the current pane."},
        {"tab.new", "Open another folder tab."},
        {"options.open", "Find appearance, Windows and privacy settings."},
        {"options.shortcuts", "Review or change keyboard shortcuts."},
        {"transfers.open", "Review transfers, failures and recoverable work."},
        {"favorites.groups", "Organize favorite folder groups."},
        {"file.edit_tags", "Edit local labels used to identify this file."},
        {"tools.tv_organizer", "Review proposed TV or book-series moves."}
    };
    const QStringList quickIds = {"navigation.goto", "search.files", "folder.new",
        "tab.new", "panel.split_right", "panel.inspector", "options.open",
        "transfers.open"};
    const auto populate = [this, results, noResults, summaries, quickIds](const QString& query) {
        results->clear();
        QList<QAction*> matches;
        for (QAction* action : commands_->actions()) {
            const auto descriptor = commands_->descriptor(action);
            const QString detail = summaries.value(descriptor.id);
            const QString searchable = descriptor.title + ' ' + descriptor.category + ' '
                + descriptor.aliases.join(' ') + ' ' + detail;
            if (query.isEmpty() || searchable.contains(query, Qt::CaseInsensitive))
                matches.append(action);
        }
        std::sort(matches.begin(), matches.end(), [this, &quickIds](QAction* left, QAction* right) {
            const auto a = commands_->descriptor(left);
            const auto b = commands_->descriptor(right);
            const int aQuick = quickIds.indexOf(a.id);
            const int bQuick = quickIds.indexOf(b.id);
            if (aQuick >= 0 || bQuick >= 0)
                return aQuick >= 0 && (bQuick < 0 || aQuick < bQuick);
            if (a.category != b.category)
                return a.category.localeAwareCompare(b.category) < 0;
            return a.title.localeAwareCompare(b.title) < 0;
        });
        QString lastGroup;
        for (QAction* action : matches) {
            const auto descriptor = commands_->descriptor(action);
            const QString group = quickIds.contains(descriptor.id) ? "Quick" : descriptor.category;
            if (query.isEmpty() && group != lastGroup) {
                auto* heading = new QListWidgetItem(group, results);
                heading->setData(Qt::UserRole + 3, true);
                heading->setFlags(Qt::NoItemFlags);
                lastGroup = group;
            }
            auto* item = new QListWidgetItem(descriptor.title, results);
            const QString detail = summaries.value(descriptor.id);
            const QString shortcut = action->shortcut().toString(QKeySequence::NativeText);
            item->setData(Qt::UserRole, descriptor.id);
            item->setData(Qt::UserRole + 1, detail);
            item->setData(Qt::UserRole + 2, shortcut);
            item->setData(Qt::AccessibleTextRole, descriptor.title);
            item->setData(Qt::AccessibleDescriptionRole, shortcut.isEmpty()
                ? detail : detail.isEmpty() ? "Shortcut: " + shortcut
                    : detail + ". Shortcut: " + shortcut);
            item->setToolTip(action->toolTip());
            if (!action->isEnabled())
                item->setFlags(Qt::NoItemFlags);
        }
        for (int row = 0; row < results->count(); ++row) {
            if (results->item(row)->flags() & Qt::ItemIsSelectable) {
                results->setCurrentRow(row);
                break;
            }
        }
        const bool hasNoResults = !query.trimmed().isEmpty() && matches.isEmpty();
        results->setVisible(!hasNoResults);
        noResults->setVisible(hasNoResults);
    };
    populate({});
    connect(queryEdit, &QLineEdit::textChanged, &dialog, populate);
    const auto activate = [this, &dialog, results] {
        QListWidgetItem* item = results->currentItem();
        if (!item || !(item->flags() & Qt::ItemIsSelectable))
            return;
        QAction* action = commands_->action(item->data(Qt::UserRole).toString());
        if (action && action->isEnabled()) {
            dialog.accept();
            action->trigger();
        }
    };
    connect(queryEdit, &QLineEdit::returnPressed, &dialog, activate);
    connect(results, &QListWidget::itemActivated, &dialog,
        [activate](QListWidgetItem*) { activate(); });
    auto* down = new QShortcut(QKeySequence(Qt::Key_Down), queryEdit);
    down->setContext(Qt::WidgetShortcut);
    connect(down, &QShortcut::activated, results, [results] {
        if (results->currentRow() < 0)
            return;
        results->setFocus();
    });
    auto* up = new QShortcut(QKeySequence(Qt::Key_Up), queryEdit);
    up->setContext(Qt::WidgetShortcut);
    connect(up, &QShortcut::activated, results, [results] {
        for (int row = results->count() - 1; row >= 0; --row) {
            if (results->item(row)->flags() & Qt::ItemIsSelectable) {
                results->setCurrentRow(row);
                results->setFocus();
                return;
            }
        }
    });
    queryEdit->setFocus();
    dialog.exec();
}

void MainWindow::batchRename()
{
    clearBatchRenameUndoPrompt();
    const QStringList paths = active_->selectedPathsInSelectionOrder();
    if (paths.size() < 2) {
        QMessageBox::information(this, "Batch rename", "Select two or more items first.");
        return;
    }
    filemanager::files::BatchRenamePlan appliedPlan;
    const auto result = filemanager::ui::runBatchRenameDialog(this, paths, &appliedPlan);
    if (!result.ok && result.errors.isEmpty())
        return;
    if (!result.ok)
        QMessageBox::critical(this, "Batch rename failed", result.errors.join('\n'));
    else {
        QList<filemanager::tags::TagPathChange> changes;
        for (const auto& item : appliedPlan.items) {
            if (item.source == item.destination)
                continue;
            changes.append({item.source, item.destination, item.sourceSnapshot,
                filemanager::files::captureSourceSnapshot(item.destination), item.directory});
        }
        followCommittedFileLabels(this, changes);
        const bool undoRecorded = sessionUndo_.recordBatchRename(appliedPlan);
        QAction* undo = commands_->action("edit.undo");
        undo->setEnabled(sessionUndo_.canUndo());
        undo->setText(sessionUndo_.size() > 0
            ? sessionUndo_.nextDescription() : QStringLiteral("Undo"));
        statusBar()->showMessage(QString("Renamed %1. %2").arg(
            filemanager::ui::localizedItems(result.renamed),
            undoRecorded ? QStringLiteral("Undo batch rename is available here or in Edit.")
                         : QStringLiteral("Undo is unavailable because the result could not be verified.")),
            5000);
        if (undoRecorded)
            showBatchRenameUndoPrompt();
    }
    active_->refresh();
}

void MainWindow::editSelectedTags()
{
    if (!active_ || !active_->isFileSystemLocation())
        return;
    const QStringList paths = active_->selectedPaths();
    if (paths.isEmpty()) {
        QMessageBox::information(this, "Tag selected files", "Select one or more files first.");
        return;
    }
    filemanager::ui::TagEditorDialog dialog(paths, this);
    dialog.exec();
}

void MainWindow::showPreferences()
{
    QStringList openPaths;
    for (FilePane* pane : workspace_->panes())
        openPaths.append(pane->currentPath());

    filemanager::ui::PreferencesDialog dialog({
        commands_,
        operationJournal_,
        &filenameSearchHistory_,
        &diagnosticLog_,
        openPaths,
        [this] { rebuildQuickAccess(); },
        [this] { saveLayout(); },
        [this](const QString& message, int timeout) {
            statusBar()->showMessage(message, timeout);
        }}, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    for (FilePane* pane : workspace_->panes())
        pane->setSingleClickActivation(dialog.singleClickActivation());
    for (FilePane* pane : workspace_->panes())
        pane->setSelectionCheckBoxes(dialog.selectionCheckBoxes());
    filemanager::ui::applyApplicationTheme(*qApp);
    for (QTreeView* tree : findChildren<QTreeView*>())
        tree->setAnimated(filemanager::ui::applicationAnimationsEnabled());
    for (FilePane* pane : findChildren<FilePane*>())
        pane->refresh();
}

void MainWindow::showInterruptedOperations()
{
    const QList<filemanager::files::JournalRecord> interrupted =
        operationJournal_->interruptedRecords();
    if (interrupted.isEmpty())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle("Review interrupted file operations");
    dialog.resize(820, 480);
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(
        QStringLiteral("%1 operation(s) stopped before File Manager could verify their final "
            "state. Inspect both locations before retrying. Reviewing a record only removes its "
            "encrypted recovery reminder; it never changes files.").arg(interrupted.size()),
        &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* records = new QListWidget(&dialog);
    records->setAccessibleName("Interrupted file operations");
    for (const auto& record : interrupted) {
        const QString sourceName = record.request.sources.isEmpty()
            ? QStringLiteral("Unknown source")
            : QFileInfo(record.request.sources.first()).fileName();
        auto* item = new QListWidgetItem(QStringLiteral("%1: %2 → %3")
            .arg(record.request.operation == filemanager::files::TransferOperation::Move
                    ? QStringLiteral("Move") : QStringLiteral("Copy"), sourceName,
                QDir::toNativeSeparators(record.request.destinationDirectory)), records);
        item->setData(Qt::UserRole, record.id);
    }
    records->setCurrentRow(0);
    layout->addWidget(records, 1);
    auto* details = new QLabel(&dialog);
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    details->setAccessibleName("Selected interrupted operation details");
    layout->addWidget(details);
    auto* actions = new QHBoxLayout;
    auto* openSource = new QPushButton("Open source folder", &dialog);
    auto* openDestination = new QPushButton("Open destination folder", &dialog);
    auto* reviewed = new QPushButton("Mark selected reviewed", &dialog);
    auto* reviewedAll = new QPushButton("Mark all reviewed", &dialog);
    auto* cleanup = new QPushButton("Remove verified created items…", &dialog);
    auto* retry = new QPushButton("Retry as new operation…", &dialog);
    actions->addWidget(openSource);
    actions->addWidget(openDestination);
    actions->addWidget(cleanup);
    actions->addWidget(retry);
    actions->addStretch(1);
    actions->addWidget(reviewed);
    actions->addWidget(reviewedAll);
    layout->addLayout(actions);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const auto selectedRecord = [records, &interrupted]()
        -> const filemanager::files::JournalRecord* {
        const QListWidgetItem* item = records->currentItem();
        if (!item)
            return nullptr;
        const QString id = item->data(Qt::UserRole).toString();
        const auto found = std::find_if(interrupted.cbegin(), interrupted.cend(),
            [&](const auto& record) { return record.id == id; });
        return found == interrupted.cend() ? nullptr : &*found;
    };
    const auto refreshDetails = [this, details, openSource, openDestination, cleanup, retry,
                                    reviewed, selectedRecord] {
        const auto* record = selectedRecord();
        const bool selected = record != nullptr;
        openSource->setEnabled(selected && !record->request.sources.isEmpty());
        openDestination->setEnabled(selected);
        reviewed->setEnabled(selected);
        cleanup->setEnabled(selected
            && !operationJournal_->verifiedCleanupPaths(record->id).isEmpty());
        retry->setEnabled(selected && operationJournal_->retryRequest(record->id).has_value());
        if (!selected) {
            details->clear();
            return;
        }
        details->setText(QStringLiteral("Sources:\n%1\n\nDestination:\n%2\n\nLast recorded "
            "progress: %3 of %4 bytes")
            .arg(record->request.sources.join('\n'), record->request.destinationDirectory)
            .arg(record->completedBytes).arg(record->totalBytes));
    };
    connect(records, &QListWidget::currentRowChanged, &dialog,
        [refreshDetails](int) { refreshDetails(); });
    connect(openSource, &QPushButton::clicked, &dialog, [selectedRecord] {
        const auto* record = selectedRecord();
        if (record && !record->request.sources.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(
                QFileInfo(record->request.sources.first()).absolutePath()));
    });
    connect(openDestination, &QPushButton::clicked, &dialog, [selectedRecord] {
        const auto* record = selectedRecord();
        if (record)
            QDesktopServices::openUrl(QUrl::fromLocalFile(record->request.destinationDirectory));
    });
    connect(cleanup, &QPushButton::clicked, &dialog, [this, selectedRecord] {
        const auto* record = selectedRecord();
        if (!record)
            return;
        QStringList refused;
        const QStringList verified = operationJournal_->verifiedCleanupPaths(record->id, &refused);
        if (verified.isEmpty()) {
            QMessageBox::information(this, "Nothing safe to remove",
                "No destination item still matches its encrypted commit identity while its "
                "source remains available. No files were changed.");
            return;
        }
        const QString warning = QStringLiteral(
            "Permanently remove %1 verified as created by this interrupted "
            "operation?\n\nThe source still exists and each destination identity was rechecked. "
            "Changed, replaced, unverified, and pre-existing items will not be removed.%2")
            .arg(filemanager::ui::localizedCount(
                verified.size(), "destination item", "destination items"))
            .arg(refused.isEmpty() ? QString() : QStringLiteral("\n\n%1 were refused.")
                .arg(filemanager::ui::localizedItems(refused.size())));
        if (QMessageBox::warning(this, "Remove verified recovery items", warning,
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        const auto result = operationJournal_->cleanupVerifiedCreatedItems(record->id);
        QMessageBox::information(this, "Recovery cleanup finished",
            QStringLiteral("Removed: %1\nRefused: %2\nErrors: %3\n\nReview both locations "
                "before dismissing this reminder.")
                .arg(result.removedPaths.size()).arg(result.refusedPaths.size())
                .arg(result.errors.size()));
    });
    connect(retry, &QPushButton::clicked, &dialog, [this, selectedRecord, &dialog] {
        const auto* record = selectedRecord();
        if (!record)
            return;
        QStringList unavailable;
        const auto request = operationJournal_->retryRequest(record->id, &unavailable);
        if (!request) {
            QMessageBox::information(&dialog, "Nothing available to retry",
                "None of the original source paths are currently available. No operation was queued.");
            return;
        }
        const QString message = QStringLiteral(
            "Queue this %1 again as a new operation with %2 currently available source(s)?\n\n"
            "All preflight checks will run again. Existing destinations will raise conflict "
            "prompts and will never be overwritten automatically.%3")
            .arg(request->operation == filemanager::files::TransferOperation::Move
                    ? QStringLiteral("move") : QStringLiteral("copy"))
            .arg(request->sources.size())
            .arg(unavailable.isEmpty() ? QString()
                : QStringLiteral("\n\n%1 unavailable source(s) will not be queued.")
                    .arg(unavailable.size()));
        if (QMessageBox::question(&dialog, "Retry interrupted operation", message,
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        const QString interruptedId = record->id;
        const quint64 retryJob = transferQueue_->enqueue(*request);
        recoveryRetries_.insert(retryJob, interruptedId);
        dialog.accept();
        statusBar()->showMessage("Recovery retry queued as a new operation.", 7000);
    });
    const auto dismiss = [this, records, &dialog](const QStringList& ids) {
        QString error;
        if (!operationJournal_->dismissInterrupted(ids, &error)) {
            QMessageBox::warning(&dialog, "Could not update recovery history", error);
            return;
        }
        for (int row = records->count() - 1; row >= 0; --row) {
            if (ids.contains(records->item(row)->data(Qt::UserRole).toString()))
                delete records->takeItem(row);
        }
        if (records->count() == 0)
            dialog.accept();
    };
    connect(reviewed, &QPushButton::clicked, &dialog, [records, dismiss] {
        if (const QListWidgetItem* item = records->currentItem())
            dismiss({item->data(Qt::UserRole).toString()});
    });
    connect(reviewedAll, &QPushButton::clicked, &dialog, [records, dismiss] {
        QStringList ids;
        for (int row = 0; row < records->count(); ++row)
            ids.append(records->item(row)->data(Qt::UserRole).toString());
        dismiss(ids);
    });
    refreshDetails();
    dialog.exec();
}

void MainWindow::showShortcutEditor()
{
    filemanager::ui::ShortcutEditorDialog dialog(*commands_, this);
    dialog.exec();
}

void MainWindow::showTransfers()
{
    filemanager::ui::OperationCenterDialog dialog(
        transferQueue_, operationJournal_, this);
    connect(&dialog, &filemanager::ui::OperationCenterDialog::statusMessageRequested,
        this, [this](const QString& message, int timeoutMilliseconds) {
            statusBar()->showMessage(message, timeoutMilliseconds);
        });
    connect(&dialog, &filemanager::ui::OperationCenterDialog::reviewRecoveryRequested,
        this, [this] { QTimer::singleShot(0, this, &MainWindow::showInterruptedOperations); });
    dialog.exec();
}

filemanager::settings::Session MainWindow::captureSession() const
{
    filemanager::settings::Session session;
    session.workspaceLayoutJson = QString::fromUtf8(
        QJsonDocument(workspace_->layoutState().toJson()).toJson(QJsonDocument::Compact));
    session.geometry = saveGeometry();
    session.splitterState = workspaceSplitter_->saveState();
    session.inspectorVisible = preview_->isVisible();
    return session;
}

bool MainWindow::applySession(
    const filemanager::settings::Session& session, QString* error)
{
    if (session.workspaceLayoutJson.isEmpty()) {
        if (error)
            *error = "The saved workspace has no layout data.";
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(session.workspaceLayoutJson.toUtf8(), &parseError);
    QString layoutError;
    const auto state = parseError.error == QJsonParseError::NoError && document.isObject()
        ? filemanager::workspace::LayoutNode::fromJson(document.object(), &layoutError)
        : std::nullopt;
    if (!state) {
        if (error)
            *error = layoutError.isEmpty() ? "The saved workspace layout is malformed." : layoutError;
        return false;
    }
    if (!workspace_->restoreLayout(*state, &layoutError)) {
        if (error)
            *error = layoutError.isEmpty() ? "The workspace layout could not be restored." : layoutError;
        return false;
    }
    setActivePane(workspace_->currentPane());
    if (!session.geometry.isEmpty())
        restoreGeometry(session.geometry);
    if (!session.splitterState.isEmpty())
        workspaceSplitter_->restoreState(session.splitterState);
    preview_->setVisible(session.inspectorVisible);
    return true;
}

void MainWindow::saveLayout()
{
    const filemanager::settings::Session session = captureSession();
    const filemanager::settings::SessionStore store;
    QString error;
    if (!store.save(session, &error))
        statusBar()->showMessage("Layout could not be saved: " + error, 8000);
    else {
        const QString activeId = store.activeNamedWorkspaceId();
        if (!activeId.isEmpty() && !store.updateNamedWorkspace(activeId, session, &error))
            statusBar()->showMessage("Session saved, but the active named layout could not be updated: "
                + error, 8000);
        else
            statusBar()->showMessage("Workspace state saved", 2500);
    }
}

void MainWindow::loadLayout()
{
    QString sessionError;
    const auto session = filemanager::settings::SessionStore{}.load(&sessionError);
    if (!sessionError.isEmpty()) {
        statusBar()->showMessage("Saved layout is unavailable: " + sessionError, 8000);
        return;
    }
    if (session.workspaceLayoutJson.isEmpty())
        return;
    QString restoreError;
    if (!applySession(session, &restoreError))
        statusBar()->showMessage("Saved layout is unavailable: " + restoreError, 8000);
    else
        statusBar()->showMessage("Workspace restored", 2500);
}

void MainWindow::saveNamedLayout()
{
    const filemanager::settings::SessionStore store;
    QString loadError;
    const auto existing = store.namedWorkspaces(&loadError);
    if (!loadError.isEmpty()) {
        QMessageBox::warning(this, "Named layouts", "Saved layouts are unavailable: " + loadError);
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, "Save named layout", "Layout name:",
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;
    const auto duplicate = std::find_if(existing.cbegin(), existing.cend(),
        [&](const auto& record) { return record.name.compare(name, Qt::CaseInsensitive) == 0; });
    if (duplicate != existing.cend()
        && QMessageBox::question(this, "Replace named layout",
            QString("Replace the saved layout “%1” with the current workspace?").arg(
                duplicate->name), QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    QString id;
    QString error;
    if (!store.saveNamedWorkspace(name, captureSession(), &id, &error))
        QMessageBox::warning(this, "Named layouts", "The layout could not be saved: " + error);
    else
        statusBar()->showMessage(QString("Named layout “%1” saved").arg(name), 3500);
}

void MainWindow::manageNamedLayouts()
{
    const filemanager::settings::SessionStore store;
    QString error;
    QList<filemanager::settings::NamedWorkspace> records = store.namedWorkspaces(&error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, "Named layouts", "Saved layouts are unavailable: " + error);
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName("NamedLayoutsDialog");
    dialog.setWindowTitle("Named layouts");
    dialog.resize(620, 430);
    auto* layout = new QVBoxLayout(&dialog);
    auto* disclosure = new QLabel(
        "Named layouts include folder paths and detailed tab state. They are encrypted for your "
        "Windows account, limited to 20, and can be removed here. Automatic per-layout updates "
        "occur only while workspace restoration is enabled in Preferences.", &dialog);
    disclosure->setObjectName("NamedLayoutsDisclosure");
    disclosure->setAccessibleName("Named layout privacy and restoration behavior");
    disclosure->setTextFormat(Qt::PlainText);
    disclosure->setWordWrap(true);
    layout->addWidget(disclosure);
    auto* list = new QListWidget(&dialog);
    list->setObjectName("NamedLayoutsList");
    list->setAccessibleName("Saved named layouts");
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(list, 1);
    auto* actions = new QHBoxLayout;
    auto* load = new QPushButton("Load", &dialog);
    load->setObjectName("LoadNamedLayout");
    auto* replace = new QPushButton("Replace with current", &dialog);
    replace->setObjectName("ReplaceNamedLayout");
    auto* remove = new QPushButton("Delete", &dialog);
    remove->setObjectName("DeleteNamedLayout");
    auto* saveAs = new QPushButton("Save current as…", &dialog);
    saveAs->setObjectName("SaveNamedLayoutAs");
    actions->addWidget(load);
    actions->addWidget(replace);
    actions->addWidget(remove);
    actions->addStretch(1);
    actions->addWidget(saveAs);
    layout->addLayout(actions);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const auto populate = [&] {
        list->clear();
        std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
            return left.updatedAt > right.updatedAt;
        });
        const QString activeId = store.activeNamedWorkspaceId();
        for (const auto& record : records) {
            auto* item = new QListWidgetItem(
                QString("%1%2\nUpdated %3").arg(record.name,
                    record.id == activeId ? "  • active" : "",
                    QLocale().toString(record.updatedAt.toLocalTime(), QLocale::ShortFormat)), list);
            item->setData(Qt::UserRole, record.id);
            item->setToolTip(record.name);
        }
        if (list->count() > 0)
            list->setCurrentRow(0);
    };
    const auto selectedRecord = [&]() -> filemanager::settings::NamedWorkspace* {
        const QListWidgetItem* item = list->currentItem();
        if (!item)
            return nullptr;
        const QString id = item->data(Qt::UserRole).toString();
        auto found = std::find_if(records.begin(), records.end(),
            [&](const auto& record) { return record.id == id; });
        return found == records.end() ? nullptr : &*found;
    };
    const auto updateButtons = [&] {
        const bool selected = list->currentItem() != nullptr;
        load->setEnabled(selected);
        replace->setEnabled(selected);
        remove->setEnabled(selected);
    };
    connect(list, &QListWidget::currentItemChanged, &dialog,
        [&](QListWidgetItem*, QListWidgetItem*) { updateButtons(); });
    connect(list, &QListWidget::itemDoubleClicked, &dialog,
        [load](QListWidgetItem*) { load->click(); });
    connect(load, &QPushButton::clicked, &dialog, [&] {
        const auto* selected = selectedRecord();
        if (!selected)
            return;
        if (store.restorationEnabled()) {
            const QString activeId = store.activeNamedWorkspaceId();
            if (!activeId.isEmpty() && activeId != selected->id
                && !store.updateNamedWorkspace(activeId, captureSession(), &error)) {
                QMessageBox::warning(&dialog, "Named layouts",
                    "The current named layout could not be updated: " + error);
                return;
            }
        }
        if (!applySession(selected->session, &error)) {
            QMessageBox::warning(&dialog, "Named layouts",
                "The selected layout could not be restored: " + error);
            return;
        }
        if (!store.setActiveNamedWorkspaceId(selected->id, &error)) {
            QMessageBox::warning(&dialog, "Named layouts",
                "The layout opened, but its active state could not be recorded: " + error);
        }
        dialog.accept();
        statusBar()->showMessage(QString("Named layout “%1” restored").arg(selected->name), 3500);
    });
    connect(replace, &QPushButton::clicked, &dialog, [&] {
        auto* selected = selectedRecord();
        if (!selected)
            return;
        if (QMessageBox::question(&dialog, "Replace named layout",
            QString("Replace “%1” with the current workspace?").arg(selected->name),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        if (!store.updateNamedWorkspace(selected->id, captureSession(), &error)) {
            QMessageBox::warning(&dialog, "Named layouts", "The layout could not be replaced: " + error);
            return;
        }
        store.setActiveNamedWorkspaceId(selected->id);
        records = store.namedWorkspaces(&error);
        populate();
        updateButtons();
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        const auto* selected = selectedRecord();
        if (!selected)
            return;
        const QString id = selected->id;
        const QString name = selected->name;
        if (QMessageBox::question(&dialog, "Delete named layout",
            QString("Delete “%1”? This removes its encrypted saved state.").arg(name),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        if (!store.removeNamedWorkspace(id, &error)) {
            QMessageBox::warning(&dialog, "Named layouts", "The layout could not be deleted: " + error);
            return;
        }
        records.removeIf([&](const auto& record) { return record.id == id; });
        populate();
        updateButtons();
    });
    connect(saveAs, &QPushButton::clicked, &dialog, [&] {
        dialog.accept();
        QTimer::singleShot(0, this, &MainWindow::saveNamedLayout);
    });
    populate();
    updateButtons();
    dialog.exec();
}
void MainWindow::addWorkspaceTab()
{
    auto* window = new MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
}

void MainWindow::createZipArchive()
{
    if (archiveCancel_) {
        statusBar()->showMessage("Finish or cancel the current archive operation first.", 5000);
        return;
    }
    const QStringList sources = active_ ? active_->selectedPaths() : QStringList{};
    if (sources.isEmpty()) {
        QMessageBox::information(this, "Create ZIP archive", "Select one or more files or folders first.");
        return;
    }
    const QString suggested = QDir(active_->currentPath()).filePath(
        sources.size() == 1 ? QFileInfo(sources.first()).completeBaseName() + ".zip" : "Archive.zip");
    const QString destination = QFileDialog::getSaveFileName(
        this, "Create ZIP archive", suggested, "ZIP archives (*.zip)", nullptr,
        QFileDialog::DontConfirmOverwrite);
    if (destination.isEmpty()) return;
    bool expected = false;
    if (!archiveWorkActive.compare_exchange_strong(expected, true)) {
        statusBar()->showMessage("Another archive operation is still finishing.", 5000);
        return;
    }
    archiveCancel_ = std::make_shared<std::atomic_bool>(false);
    auto cancellation = archiveCancel_;
    auto* progress = new QProgressDialog(
        "Windows is creating the ZIP archive…", "Cancel", 0, 0, this);
    progress->setObjectName("CreateZipProgress");
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->show();
    QPointer<MainWindow> guard(this);
    QPointer<QProgressDialog> progressGuard(progress);
    connect(progress, &QProgressDialog::canceled, this, [guard, progressGuard, cancellation] {
        cancellation->store(true);
        if (progressGuard) {
            progressGuard->setLabelText("Cancelling archive creation…");
            QTimer::singleShot(0, progressGuard, [progressGuard] {
                if (progressGuard) {
                    progressGuard->setCancelButton(nullptr);
                    progressGuard->show();
                }
            });
        }
        if (guard)
            guard->statusBar()->showMessage("Cancelling archive creation…");
    });
    connect(this, &QObject::destroyed, [cancellation] { cancellation->store(true); });
    QThread* worker = QThread::create([guard, progressGuard, cancellation, sources, destination] {
        const auto result = filemanager::files::createZipWithWindowsTar(
            sources, destination, cancellation.get());
        archiveWorkActive.store(false);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, progressGuard, result, destination] {
            if (!guard)
                return;
            guard->archiveCancel_.reset();
            if (progressGuard) {
                progressGuard->close();
                progressGuard->deleteLater();
            }
            if (guard->closeAfterArchive_) {
                guard->closeAfterArchive_ = false;
                guard->close();
                return;
            }
            if (!result.success) {
                if (result.cancelled)
                    guard->statusBar()->showMessage("Archive creation cancelled.", 5000);
                else
                    QMessageBox::warning(guard, "Create ZIP archive", result.error);
                return;
            }
            if (guard->active_)
                guard->active_->refresh();
            guard->statusBar()->showMessage(
                tr("Created %1").arg(QFileInfo(destination).fileName()), 5000);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::extractZipArchive()
{
    if (archiveCancel_) {
        statusBar()->showMessage("Finish or cancel the current archive operation first.", 5000);
        return;
    }
    const QString archive = active_ ? active_->selectedPath() : QString{};
    if (archive.isEmpty() || QFileInfo(archive).suffix().compare("zip", Qt::CaseInsensitive) != 0) {
        QMessageBox::information(this, "Extract ZIP archive", "Select a ZIP archive first.");
        return;
    }
    const QFileInfo archiveInfo(archive);
    const QString destination = QFileDialog::getExistingDirectory(
        this, "Choose an empty extraction destination", archiveInfo.absolutePath());
    if (destination.isEmpty()) return;
    bool expected = false;
    if (!archiveWorkActive.compare_exchange_strong(expected, true)) {
        statusBar()->showMessage("Another archive operation is still finishing.", 5000);
        return;
    }
    archiveCancel_ = std::make_shared<std::atomic_bool>(false);
    auto cancellation = archiveCancel_;
    auto* progress = new QProgressDialog("Validating archive safety…", "Cancel", 0, 0, this);
    progress->setObjectName("ExtractZipProgress");
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->show();
    QPointer<MainWindow> guard(this);
    QPointer<QProgressDialog> progressGuard(progress);
    connect(progress, &QProgressDialog::canceled, this, [guard, progressGuard, cancellation] {
        cancellation->store(true);
        if (progressGuard) {
            progressGuard->setLabelText("Cancelling extraction and checking cleanup…");
            QTimer::singleShot(0, progressGuard, [progressGuard] {
                if (progressGuard) {
                    progressGuard->setCancelButton(nullptr);
                    progressGuard->show();
                }
            });
        }
        if (guard)
            guard->statusBar()->showMessage("Cancelling extraction and checking cleanup…");
    });
    connect(this, &QObject::destroyed, [cancellation] { cancellation->store(true); });
    QThread* worker = QThread::create([guard, progressGuard, cancellation, archive, destination] {
        QElapsedTimer updateTimer;
        updateTimer.start();
        const auto result = filemanager::files::extractZipSafely(
            archive, destination, cancellation.get(),
            [guard, progressGuard, cancellation, &updateTimer](int entries, qint64 bytes) {
                if (updateTimer.elapsed() < 100 || !guard)
                    return;
                updateTimer.restart();
                QMetaObject::invokeMethod(guard, [progressGuard, cancellation, entries, bytes] {
                    if (progressGuard && !cancellation->load())
                        progressGuard->setLabelText(
                            QObject::tr("Extracting entry %1 — %2 MiB written")
                                .arg(QLocale().toString(entries + 1),
                                    QLocale().toString(bytes / (1024 * 1024))));
                }, Qt::QueuedConnection);
            });
        archiveWorkActive.store(false);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, progressGuard, result] {
            if (!guard)
                return;
            guard->archiveCancel_.reset();
            if (progressGuard) {
                progressGuard->close();
                progressGuard->deleteLater();
            }
            if (guard->closeAfterArchive_) {
                guard->closeAfterArchive_ = false;
                guard->close();
                return;
            }
            if (!result.success) {
                if (result.cancelled)
                    guard->statusBar()->showMessage("ZIP extraction cancelled.", 5000);
                else
                    QMessageBox::warning(guard, "Extract ZIP archive", result.error);
                return;
            }
            if (guard->active_)
                guard->active_->refresh();
            guard->statusBar()->showMessage(
                tr("Extracted %1 safely").arg(
                    filemanager::ui::localizedEntries(result.entries)), 5000);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::openNativeArchive()
{
    const QString path = active_ ? active_->selectedPath() : QString{};
    const auto validation = filemanager::platform::validateNativeArchiveHandoff(path);
    if (!validation.valid) {
        QMessageBox::information(this, "Open archive using Windows", validation.error);
        return;
    }
    const auto result = filemanager::platform::openNativeArchiveWithWindows(path);
    if (!result.success) {
        QMessageBox::warning(this, "Open archive using Windows", result.error);
        return;
    }
    statusBar()->showMessage(tr("Handed %1 archive to the Windows-selected handler")
        .arg(filemanager::platform::nativeArchiveFormatName(validation.format)), 7000);
}

void MainWindow::shareSelected()
{
    const QStringList paths = active_ ? active_->selectedPaths() : QStringList{};
    if (paths.isEmpty()) {
        QMessageBox::information(this, "Share", "Select one or more files or folders to share.");
        return;
    }
    statusBar()->showMessage(tr("Preparing %1 for Windows Share…").arg(
        filemanager::ui::localizedItems(paths.size())));
    shareController_->showShareUi(static_cast<quintptr>(winId()), paths);
}

void MainWindow::showColumnHeaderMenu(FilePane* pane, const QPoint& globalPoint)
{
    if (!pane || !pane->isFileSystemLocation()
            || pane->viewMode() != FilePane::ViewMode::Details) {
        return;
    }
    setActivePane(pane);
    QMenu menu(this);
    menu.setObjectName("DetailsColumnMenu");
    menu.setAccessibleName("Choose folder detail columns");
    for (const auto& column : ViewColumns)
        menu.addAction(commands_->action(QString::fromLatin1(column.visibilityId)));
    menu.exec(globalPoint);
}

void MainWindow::showContextMenu(FilePane* pane, const QPoint& point)
{
    setActivePane(pane);
    QMenu menu(this);
    menu.setObjectName("FileContextMenu");
    struct ContextAction
    {
        QString id;
        QAction* action = nullptr;
        int section = 0;
        bool pinnable = true;
    };
    QList<ContextAction> contextActions;
    const auto addAction = [&](const QString& id, const QString& text, int section,
                               const std::function<void()>& callback, bool pinnable = true) {
        auto* action = new QAction(text, &menu);
        action->setProperty("contextActionId", id);
        connect(action, &QAction::triggered, &menu, callback);
        contextActions.append({id, action, section, pinnable});
        return action;
    };
    const auto addCommand = [&](const QString& id, int section, bool pinnable = true) {
        QAction* source = commands_->action(id);
        QAction* proxy = addAction(id, source ? source->text() : id, section,
            [source] { if (source) source->trigger(); }, pinnable);
        if (source) {
            proxy->setEnabled(source->isEnabled());
            proxy->setIcon(source->icon());
        }
        return proxy;
    };

    addAction("file.open", "Open", 0, [this] { openSelected(); });
    addAction("file.reveal", "Open containing folder", 0, [this] {
        const QString path = active_->selectedPath();
        if (!path.isEmpty())
            active_->revealPath(path);
    });
    if (QFileInfo(active_->selectedPath()).isDir()) {
        addCommand("folder.open_new_tab", 0);
        addCommand("folder.open_new_window", 0);
    }
    addAction("clipboard.cut", "Cut", 1, [this] { copySelection(true); });
    addAction("clipboard.copy", "Copy", 1, [this] { copySelection(false); });
    addAction("clipboard.paste", "Paste", 1, [this] { pasteClipboard(); });
    addCommand("panel.copy_to_other", 1);
    addCommand("panel.move_to_other", 1);
    addAction("file.rename", "Rename   F2", 2, [this] { renameSelected(); });
    addAction("file.batch_rename", "Batch rename", 2, [this] { batchRename(); });
    addCommand("file.edit_tags", 2);
    addCommand("file.create_shortcut", 2);
    addAction("clipboard.copy_path", "Copy path", 2,
        [this] { QApplication::clipboard()->setText(active_->selectedPath()); });
    addCommand("clipboard.copy_filename", 2);
    addAction("file.properties", "Properties", 2, [this] { showProperties(); });
    addCommand("file.details", 2);
    addCommand("file.analyze", 2);
    addCommand("tools.duplicates", 2);
    addCommand("file.share", 2);
    addCommand("archive.create_zip", 2);
    if (QFileInfo(active_->selectedPath()).suffix().compare("zip", Qt::CaseInsensitive) == 0)
        addCommand("archive.extract_zip", 2);
    if (filemanager::platform::validateNativeArchiveHandoff(active_->selectedPath()).valid)
        addCommand("archive.open_native", 2);
    const QStringList windowsSelection = active_->selectedPaths();
    if (!windowsSelection.isEmpty()) {
        if (filemanager::settings::loadContextMenuPermissions().allowWindowsExtensions) {
            const QPoint screenPoint = pane->view()->viewport()->mapToGlobal(point);
            addAction("windows.commands", "Windows commands…", 3,
                [this, windowsSelection, screenPoint] {
                statusBar()->showMessage("Loading Windows commands in an isolated helper…");
                QPointer<MainWindow> guard(this);
                QThread* worker = QThread::create([guard, windowsSelection, screenPoint] {
                    const auto result = filemanager::platform::showWindowsContextMenuIsolated(
                        windowsSelection, screenPoint.x(), screenPoint.y());
                    if (guard) QMetaObject::invokeMethod(guard, [guard, result] {
                        if (!guard) return;
                        if (!result.success)
                            QMessageBox::warning(guard, "Windows commands", result.error);
                        else {
                            guard->statusBar()->clearMessage();
                            if (!result.cancelled && guard->active_)
                                guard->active_->refresh();
                        }
                    });
                });
                connect(worker, &QThread::finished, worker, &QObject::deleteLater);
                worker->start();
            }, false);
        } else {
            QAction* disabled = addAction("windows.commands.disabled",
                "Windows commands (disabled in Preferences)", 3, [] {}, false);
            disabled->setEnabled(false);
        }
    }
    addCommand("file.delete", 4);
    addCommand("file.delete_permanently", 4);

    QSettings settings;
    QStringList pinnedIds = settings.value("interaction/pinnedContextActions").toStringList();
    pinnedIds.removeDuplicates();
    if (pinnedIds.size() > 8)
        pinnedIds.resize(8);

    auto* search = new QLineEdit(&menu);
    search->setObjectName("ContextMenuSearch");
    search->setAccessibleName("Search File Manager commands");
    search->setPlaceholderText("Search commands…");
    search->setClearButtonEnabled(true);
    auto* searchWidget = new QWidgetAction(&menu);
    searchWidget->setDefaultWidget(search);
    menu.addAction(searchWidget);

    QAction* pinnedHeading = menu.addAction("Pinned");
    pinnedHeading->setEnabled(false);
    pinnedHeading->setProperty("contextHeading", true);
    QList<QAction*> pinnedActions;
    for (const QString& id : pinnedIds) {
        const auto found = std::find_if(contextActions.cbegin(), contextActions.cend(),
            [&](const ContextAction& candidate) {
                return candidate.id == id && candidate.pinnable;
            });
        if (found == contextActions.cend())
            continue;
        auto* pinned = menu.addAction(found->action->icon(), found->action->text());
        pinned->setProperty("contextActionId", found->id);
        pinned->setProperty("contextPinned", true);
        pinned->setEnabled(found->action->isEnabled());
        connect(pinned, &QAction::triggered, &menu,
            [source = found->action] { source->trigger(); });
        pinnedActions.append(pinned);
    }
    pinnedHeading->setVisible(!pinnedActions.isEmpty());
    QAction* pinnedSeparator = menu.addSeparator();
    pinnedSeparator->setVisible(!pinnedActions.isEmpty());

    QAction* allHeading = menu.addAction("All actions");
    allHeading->setEnabled(false);
    allHeading->setProperty("contextHeading", true);
    int previousSection = -1;
    QList<QAction*> sectionSeparators;
    for (const ContextAction& candidate : std::as_const(contextActions)) {
        if (previousSection >= 0 && candidate.section != previousSection)
            sectionSeparators.append(menu.addSeparator());
        menu.addAction(candidate.action);
        previousSection = candidate.section;
    }

    QAction* customize = menu.addAction("Customize pinned actions…");
    customize->setProperty("contextActionId", "context.customize_pins");
    connect(customize, &QAction::triggered, this, [this, contextActions, pinnedIds] {
        QDialog dialog(this);
        dialog.setObjectName("PinnedContextActionsDialog");
        dialog.setWindowTitle("Pinned context actions");
        auto* layout = new QVBoxLayout(&dialog);
        auto* disclosure = new QLabel(
            "Choose up to 8 built-in actions. Only stable action IDs are saved; paths, selections "
            "and command usage are not recorded.", &dialog);
        disclosure->setWordWrap(true);
        disclosure->setTextFormat(Qt::PlainText);
        layout->addWidget(disclosure);
        QList<QCheckBox*> checks;
        for (const ContextAction& candidate : contextActions) {
            if (!candidate.pinnable)
                continue;
            auto* check = new QCheckBox(candidate.action->text(), &dialog);
            check->setProperty("contextActionId", candidate.id);
            check->setChecked(pinnedIds.contains(candidate.id));
            checks.append(check);
            layout->addWidget(check);
        }
        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;
        QStringList selected;
        for (QCheckBox* check : checks)
            if (check->isChecked()) selected.append(check->property("contextActionId").toString());
        if (selected.size() > 8) {
            QMessageBox::warning(this, "Pinned context actions", "Choose no more than 8 actions.");
            return;
        }
        QSettings().setValue("interaction/pinnedContextActions", selected);
    });
    QAction* noMatches = menu.addAction("No matching actions");
    noMatches->setEnabled(false);
    noMatches->setVisible(false);

    connect(search, &QLineEdit::textChanged, &menu, [=](const QString& rawQuery) {
        const QString query = rawQuery.trimmed();
        int matches = 0;
        for (const ContextAction& candidate : contextActions) {
            const bool visible = filemanager::search::match(
                query, candidate.action->text() + ' ' + candidate.id).matched;
            candidate.action->setVisible(visible);
            matches += visible;
        }
        for (QAction* pinned : pinnedActions)
            pinned->setVisible(query.isEmpty());
        pinnedHeading->setVisible(query.isEmpty() && !pinnedActions.isEmpty());
        pinnedSeparator->setVisible(query.isEmpty() && !pinnedActions.isEmpty());
        allHeading->setVisible(query.isEmpty() || matches > 0);
        for (QAction* separator : sectionSeparators)
            separator->setVisible(query.isEmpty());
        customize->setVisible(query.isEmpty()
            || filemanager::search::match(query, customize->text()).matched);
        noMatches->setVisible(!query.isEmpty() && matches == 0 && !customize->isVisible());
    });
    connect(&menu, &QMenu::aboutToShow, search, [search] {
        QTimer::singleShot(0, search, [search] { search->setFocus(); });
    });
    menu.exec(pane->view()->viewport()->mapToGlobal(point));
}
void MainWindow::closeEvent(QCloseEvent* event)
{
    if (archiveCancel_) {
        event->ignore();
        closeAfterArchive_ = true;
        archiveCancel_->store(true);
        statusBar()->showMessage("Finishing archive cancellation before closing…");
        return;
    }
    if (!diagnosticShutdownFinished_) {
        event->ignore();
        if (!diagnosticShutdownStarted_) {
            diagnosticShutdownStarted_ = true;
            statusBar()->showMessage("Finishing pending diagnostic saves…", 5000);
            diagnosticLog_.shutdown();
        }
        return;
    }
    if (filemanager::settings::SessionStore{}.restorationEnabled())
        saveLayout();
    QMainWindow::closeEvent(event);
}
