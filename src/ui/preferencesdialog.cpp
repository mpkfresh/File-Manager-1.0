#include "preferencesdialog.h"

#include "commands/commandregistry.h"
#include "diagnostics/diagnosticbundle.h"
#include "diagnostics/asyncdiagnosticlog.h"
#include "files/operationjournal.h"
#include "platform/windowsfolderintegration.h"
#include "platform/folderrouting.h"
#include "platform/wineintegration.h"
#include "search/savedsearchstore.h"
#include "search/sessionsearchhistory.h"
#include "settings/contextmenupermissions.h"
#include "settings/favoritegroups.h"
#include "settings/metadatapermissions.h"
#include "settings/portablepreferences.h"
#include "settings/sessionstore.h"
#include "settings/startuppreferences.h"
#include "settings/shellpermissions.h"
#include "tags/tagcatalog.h"
#include "ui/appearancesettings.h"
#include "ui/applicationtheme.h"
#include "ui/localizedformat.h"

#include <QAbstractItemView>
#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <optional>
#include <functional>

namespace filemanager::ui {

struct PreferencesDialog::State
{
    Dependencies dependencies;
    AppearanceSettings originalAppearance;
    settings::MetadataPermissions metadataPermissions;
    settings::ShellPermissions shellPermissions;
    settings::ContextMenuPermissions contextMenuPermissions;
    QString applicationPath;
    platform::FolderCommandStatus folderCommand;
    platform::AppPathsStatus appPaths;
    platform::WinEStatus winE;
    platform::FolderRoutingStatus folderRouting;
    settings::SessionStore sessionStore;
    QComboBox* font = nullptr;
    QComboBox* density = nullptr;
    QCheckBox* animations = nullptr;
    QCheckBox* shellMetadata = nullptr;
    QCheckBox* thirdPartyNamespaces = nullptr;
    QCheckBox* contextMenuExtensions = nullptr;
    QCheckBox* folderIntegration = nullptr;
    QCheckBox* defaultFolders = nullptr;
    QLabel* folderRoutingDetail = nullptr;
    QPushButton* restoreFolderRouting = nullptr;
    std::function<void()> refreshFolderRouting;
    QCheckBox* appPathsIntegration = nullptr;
    QCheckBox* winEIntegration = nullptr;
    QCheckBox* winEAtSignIn = nullptr;
    QCheckBox* restoreWorkspace = nullptr;
    QComboBox* startupDestination = nullptr;
    QCheckBox* singleClick = nullptr;
    QCheckBox* selectionCheckBoxes = nullptr;
    std::optional<settings::PortablePreferences> importedPreferences;
};

PreferencesDialog::PreferencesDialog(Dependencies dependencies, QWidget* parent)
    : QDialog(parent), state_(std::make_unique<State>())
{
    state_->dependencies = std::move(dependencies);
    state_->originalAppearance = loadAppearanceSettings();
    state_->metadataPermissions = settings::loadMetadataPermissions();
    state_->shellPermissions = settings::loadShellPermissions();
    state_->contextMenuPermissions = settings::loadContextMenuPermissions();
    state_->applicationPath = QCoreApplication::applicationFilePath();
    state_->folderCommand = platform::windowsFolderCommandStatus(state_->applicationPath);
    state_->appPaths = platform::windowsAppPathsStatus(state_->applicationPath);
    state_->winE = platform::winEStatus(state_->applicationPath);
    state_->folderRouting = platform::folderRoutingStatus(state_->applicationPath);

    setObjectName("PreferencesDialog");
    setWindowTitle("Preferences");
    resize(740, 620);
    auto* dialogLayout = new QVBoxLayout(this);
    auto* search = new QLineEdit(this);
    search->setObjectName("PreferencesSearch");
    search->setAccessibleName("Find a preference");
    search->setPlaceholderText("Find a setting…");
    search->setClearButtonEnabled(true);
    dialogLayout->addWidget(search);
    auto* noResults = new QLabel("No settings match. Try another term.", this);
    noResults->setObjectName("PreferencesNoResults");
    noResults->setAccessibleName("No matching preferences");
    noResults->setWordWrap(true);
    noResults->hide();
    dialogLayout->addWidget(noResults);
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName("PreferencesTabs");
    tabs->setAccessibleName("Preferences sections");
    const auto addSection = [tabs](const QString& name, const QString& description) {
        auto* scrollArea = new QScrollArea(tabs);
        scrollArea->setWidgetResizable(true);
        scrollArea->setFrameShape(QFrame::NoFrame);
        auto* content = new QWidget(scrollArea);
        auto* sectionForm = new QFormLayout(content);
        sectionForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        sectionForm->setVerticalSpacing(8);
        scrollArea->setWidget(content);
        tabs->addTab(scrollArea, name);
        tabs->setTabToolTip(tabs->count() - 1, description);
        return sectionForm;
    };
    auto* generalForm = addSection("General", "Appearance and everyday file behavior.");
    auto* windowsForm = addSection("Windows", "Choose how File Manager works with Windows for your account.");
    auto* privacyForm = addSection("Privacy and data", "Review permissions and local data. Clear items independently.");
    auto* portabilityForm = addSection("Import and export", "Move selected settings without exporting private local data.");
    dialogLayout->addWidget(tabs, 1);
    const auto addCompactRow = [this](QFormLayout* section, const QString& title,
                                      const QString& summary, QWidget* control) {
        auto* row = new QWidget(this);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(4, 3, 4, 3);
        rowLayout->setSpacing(12);
        auto* textLayout = new QVBoxLayout;
        textLayout->setSpacing(2);
        const bool isCheckBox = qobject_cast<QCheckBox*>(control) != nullptr;
        if (isCheckBox) {
            auto* check = qobject_cast<QCheckBox*>(control);
            check->setText(title);
            textLayout->addWidget(check);
        } else {
            auto* name = new QLabel(title, row);
            name->setWordWrap(true);
            textLayout->addWidget(name);
        }
        auto* detail = new QLabel(summary, row);
        detail->setObjectName("Muted");
        detail->setWordWrap(true);
        textLayout->addWidget(detail);
        rowLayout->addLayout(textLayout, 1);
        if (control->accessibleDescription().isEmpty())
            control->setAccessibleDescription(summary);
        if (!isCheckBox)
            rowLayout->addWidget(control, 0, Qt::AlignVCenter);
        section->addRow(row);
    };
    const auto searchableRowText = [](QFormLayout* section, int row) {
        QString value;
        for (QFormLayout::ItemRole role : {QFormLayout::LabelRole, QFormLayout::FieldRole,
                                         QFormLayout::SpanningRole}) {
            const QLayoutItem* item = section->itemAt(row, role);
            if (!item || !item->widget())
                continue;
            const QWidget* widget = item->widget();
            value += ' ' + widget->toolTip() + ' ' + widget->accessibleName();
            if (const auto* label = qobject_cast<const QLabel*>(widget))
                value += ' ' + label->text();
            if (const auto* button = qobject_cast<const QAbstractButton*>(widget))
                value += ' ' + button->text();
            for (const QLabel* label : widget->findChildren<QLabel*>())
                value += ' ' + label->text();
            for (const QAbstractButton* button : widget->findChildren<QAbstractButton*>())
                value += ' ' + button->text() + ' ' + button->toolTip();
        }
        return value;
    };
    const QList<QFormLayout*> sections = {generalForm, windowsForm, privacyForm, portabilityForm};
    const QStringList sectionNames = {"General", "Windows", "Privacy and data", "Import and export"};
    connect(search, &QLineEdit::textChanged, this,
        [tabs, noResults, sections, sectionNames, searchableRowText](const QString& query) {
            const QString term = query.trimmed();
            int firstMatch = -1;
            for (int sectionIndex = 0; sectionIndex < sections.size(); ++sectionIndex) {
                QFormLayout* section = sections.at(sectionIndex);
                int matches = 0;
                for (int row = 0; row < section->rowCount(); ++row) {
                    const bool visible = term.isEmpty()
                        || searchableRowText(section, row).contains(term, Qt::CaseInsensitive);
                    section->setRowVisible(row, visible);
                    matches += visible;
                }
                tabs->setTabText(sectionIndex, term.isEmpty() ? sectionNames.at(sectionIndex)
                    : QString("%1 (%2)").arg(sectionNames.at(sectionIndex)).arg(matches));
                tabs->setTabEnabled(sectionIndex, term.isEmpty() || matches > 0);
                if (matches && firstMatch < 0)
                    firstMatch = sectionIndex;
            }
            if (!term.isEmpty() && firstMatch >= 0)
                tabs->setCurrentIndex(firstMatch);
            const bool hasNoResults = !term.isEmpty() && firstMatch < 0;
            tabs->setVisible(!hasNoResults);
            noResults->setVisible(hasNoResults);
        });
    auto* findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, search, [search] {
        search->setFocus();
        search->selectAll();
    });
    auto* form = privacyForm;

    auto* permissionGroup = new QGroupBox("Privacy and permission dashboard", this);
    permissionGroup->setObjectName("PermissionDashboard");
    permissionGroup->setAccessibleName("Privacy and permission dashboard");
    auto* permissionLayout = new QVBoxLayout(permissionGroup);
    auto* permissionSummary = new QTableWidget(8, 2, permissionGroup);
    permissionSummary->setObjectName("PermissionDashboardSummary");
    permissionSummary->setAccessibleName("Current privacy permissions and one-time access");
    permissionSummary->setHorizontalHeaderLabels({"Capability", "Current access"});
    permissionSummary->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    permissionSummary->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    permissionSummary->verticalHeader()->hide();
    permissionSummary->setEditTriggers(QAbstractItemView::NoEditTriggers);
    permissionSummary->setSelectionMode(QAbstractItemView::NoSelection);
    permissionSummary->setFocusPolicy(Qt::NoFocus);
    const QStringList indexedRoots = QSettings().value("search/indexedRoots").toStringList();
    const QList<QStringList> permissionRows = {
        {"Windows metadata handlers", state_->metadataPermissions.allowWindowsShellHandlers ? "On" : "Off",
            "May inspect browsed files; extracted values are not persisted"},
        {"Third-party Shell namespaces", state_->shellPermissions.allowThirdPartyNamespaces ? "On" : "Off",
            "Persistent permission; separate temporary visits can be ended in each pane"},
        {"Windows context-menu extensions", state_->contextMenuPermissions.allowWindowsExtensions ? "On" : "Off",
            "Loaded only for Windows commands in an isolated helper"},
        {"Workspace restoration", state_->sessionStore.restorationEnabled() ? "On" : "Off",
            "Encrypted local paths/state; clearing is immediate"},
        {"Metadata index", indexedRoots.isEmpty() ? "Off" : localizedCount(indexedRoots.size(), "folder", "folders"),
            "Local paths/names/types/sizes/dates only; contents excluded"},
        {"Network discovery", "One-time only",
            "Runs only after Scan network; results disappear when the window closes"},
        {"Content search", "One-time only",
            "Reads eligible files after Search; contents and results are not saved"},
        {"Win+E shortcut helper", state_->winE.enabled ? "Enabled" : "Off",
            "Receives keyboard events to recognize Win+E; no typing is recorded or transmitted"}};
    QStringList permissionDetails;
    for (int row = 0; row < permissionRows.size(); ++row) {
        for (int column = 0; column < 2; ++column)
            permissionSummary->setItem(row, column, new QTableWidgetItem(permissionRows.at(row).at(column)));
        permissionDetails.append(permissionRows.at(row).at(0) + ": " + permissionRows.at(row).at(2));
    }
    permissionSummary->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    permissionSummary->setMinimumHeight(285);
    permissionSummary->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    permissionLayout->addWidget(permissionSummary);
    auto* showPermissionDetails = new QPushButton("Show permission details", permissionGroup);
    showPermissionDetails->setObjectName("PermissionDetailsToggle");
    showPermissionDetails->setAccessibleName("Show permission details");
    permissionLayout->addWidget(showPermissionDetails);
    auto* permissionDetailText = new QLabel(permissionDetails.join("\n"), permissionGroup);
    permissionDetailText->setObjectName("PermissionDetails");
    permissionDetailText->setWordWrap(true);
    permissionDetailText->setVisible(false);
    permissionLayout->addWidget(permissionDetailText);
    connect(showPermissionDetails, &QPushButton::clicked, this,
        [showPermissionDetails, permissionDetailText] {
            const bool visible = !permissionDetailText->isVisible();
            permissionDetailText->setVisible(visible);
            showPermissionDetails->setText(visible ? "Hide permission details" : "Show permission details");
            showPermissionDetails->setAccessibleName(showPermissionDetails->text());
        });

    const QString indexDatabase = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation)).filePath("metadata-index.sqlite");
    auto* clearIndexes = new QPushButton(indexedRoots.isEmpty()
        ? "No persisted metadata indexes" : "Clear all persisted metadata indexes", permissionGroup);
    clearIndexes->setObjectName("ClearAllMetadataIndexes");
    clearIndexes->setAccessibleName("Clear all persisted metadata indexes");
    clearIndexes->setEnabled(!indexedRoots.isEmpty() || QFileInfo::exists(indexDatabase));
    permissionLayout->addWidget(clearIndexes);
    connect(clearIndexes, &QPushButton::clicked, this,
        [this, clearIndexes, permissionSummary, indexDatabase] {
            if (QMessageBox::question(this, "Clear metadata indexes",
                    "Delete all locally persisted metadata indexes and revoke every indexed folder?",
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
                return;
            QStringList failures;
            for (const QString& path : {indexDatabase, indexDatabase + "-wal", indexDatabase + "-shm"})
                if (QFileInfo::exists(path) && !QFile::remove(path))
                    failures.append(QDir::toNativeSeparators(path));
            if (!failures.isEmpty()) {
                QMessageBox::warning(this, "Could not clear metadata indexes",
                    "These local files could not be removed:\n" + failures.join('\n'));
                return;
            }
            QSettings().remove("search/indexedRoots");
            permissionSummary->item(4, 1)->setText("Off");
            clearIndexes->setText("No persisted metadata indexes");
            clearIndexes->setEnabled(false);
        });

    auto* clearDiagnostics = new QPushButton("Loading diagnostic history…", permissionGroup);
    clearDiagnostics->setObjectName("ClearDiagnosticHistory");
    clearDiagnostics->setAccessibleName("Clear local diagnostic history");
    clearDiagnostics->setToolTip("The local history retains up to 512 path-free event codes for seven days.");
    permissionLayout->addWidget(clearDiagnostics);
    auto* diagnosticStatus = new QLabel(permissionGroup);
    diagnosticStatus->setObjectName("DiagnosticPersistenceStatus");
    diagnosticStatus->setWordWrap(true);
    permissionLayout->addWidget(diagnosticStatus);
    const auto refreshDiagnostics = [this, clearDiagnostics, diagnosticStatus] {
        const auto* log = state_->dependencies.diagnosticLog;
        clearDiagnostics->setEnabled(log && !log->isLoading() && !log->isClearing()
            && (log->eventCount() > 0 || log->pendingCount() > 0 || !log->errorString().isEmpty()));
        if (!log) {
            clearDiagnostics->setText("Diagnostic history unavailable");
            return;
        }
        clearDiagnostics->setText(log->isLoading() ? "Loading diagnostic history…"
            : log->isClearing() ? "Clearing diagnostic history…"
            : !log->isAvailable() ? "Clear unavailable diagnostic history"
            : log->eventCount() == 0 && log->pendingCount() == 0 ? "No local diagnostic history"
            : QString("Clear local diagnostic history (%1 saved, %2 pending)")
                .arg(localizedItems(log->eventCount()), localizedItems(log->pendingCount())));
        diagnosticStatus->setText(!log->errorString().isEmpty() ? log->errorString()
            : log->pendingCount() > 0 ? "Diagnostic events are pending background saving."
            : "Diagnostic saves run in the background. Unsaved events may be lost on exit.");
    };
    if (auto* log = state_->dependencies.diagnosticLog) {
        connect(log, &diagnostics::AsyncDiagnosticLog::stateChanged, this, refreshDiagnostics);
        connect(log, &diagnostics::AsyncDiagnosticLog::clearFinished, this,
            [this](bool success, const QString& error) {
                if (!success)
                    QMessageBox::warning(this, "Could not clear diagnostic history", error);
                else if (state_->dependencies.showStatus)
                    state_->dependencies.showStatus("Local diagnostic history cleared", 4000);
            });
    }
    refreshDiagnostics();
    connect(clearDiagnostics, &QPushButton::clicked, this, [this] {
        if (!state_->dependencies.diagnosticLog
                || QMessageBox::question(this, "Clear diagnostic history",
                    "Delete all retained local diagnostic event codes now?",
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        if (!state_->dependencies.diagnosticLog->clear())
            QMessageBox::warning(this, "Could not clear diagnostic history",
                state_->dependencies.diagnosticLog->errorString());
    });
    auto* includePaths = new QCheckBox(
        "Include open and indexed folder paths in an exported diagnostic bundle", permissionGroup);
    includePaths->setObjectName("IncludeDiagnosticPaths");
    includePaths->setAccessibleName("Include sensitive paths in diagnostic bundle");
    includePaths->setChecked(false);
    includePaths->setToolTip("Off by default. Paths and filenames can reveal personal or confidential information.");
    permissionLayout->addWidget(includePaths);
    auto* exportDiagnostics = new QPushButton("Export diagnostic bundle…", permissionGroup);
    exportDiagnostics->setObjectName("ExportDiagnosticBundle");
    exportDiagnostics->setAccessibleName("Export diagnostic bundle");
    permissionLayout->addWidget(exportDiagnostics);
    if (auto* log = state_->dependencies.diagnosticLog) {
        const auto refreshExport = [log, exportDiagnostics] {
            exportDiagnostics->setEnabled(log->isAvailable() && !log->isClearing() && !log->isExporting());
            exportDiagnostics->setText(log->isExporting() ? "Exporting diagnostic bundle…" : "Export diagnostic bundle…");
        };
        connect(log, &diagnostics::AsyncDiagnosticLog::stateChanged, this, refreshExport);
        connect(log, &diagnostics::AsyncDiagnosticLog::exportFinished, this,
            [this](bool success, const QString& error) {
                if (!success)
                    QMessageBox::warning(this, "Could not export diagnostics", error);
                else if (state_->dependencies.showStatus)
                    state_->dependencies.showStatus("Diagnostic bundle saved with the selected privacy options", 6000);
            });
        refreshExport();
    } else {
        exportDiagnostics->setEnabled(false);
    }
    connect(exportDiagnostics, &QPushButton::clicked, this, [this, includePaths, indexedRoots] {
        const QString destination = QFileDialog::getSaveFileName(this, "Export diagnostic bundle",
            QDir::home().filePath("file-manager-diagnostics.json"), "JSON diagnostic bundle (*.json)");
        if (destination.isEmpty()) return;
        diagnostics::DiagnosticBundleInput input;
        input.productVersion = QStringLiteral(FILE_MANAGER_VERSION);
        input.releaseChannel = QStringLiteral(FILE_MANAGER_RELEASE_CHANNEL);
        input.operatingSystem = QSysInfo::prettyProductName();
        input.qtVersion = QString::fromLatin1(qVersion());
        input.featureStates = {
            {"windowsMetadataHandlers", settings::loadMetadataPermissions().allowWindowsShellHandlers},
            {"thirdPartyShellNamespaces", settings::loadShellPermissions().allowThirdPartyNamespaces},
            {"windowsContextMenuExtensions", settings::loadContextMenuPermissions().allowWindowsExtensions},
            {"workspaceRestoration", settings::SessionStore{}.restorationEnabled()}};
        input.openPaths = state_->dependencies.openPaths;
        input.indexedRoots = indexedRoots;
        if (auto* log = state_->dependencies.diagnosticLog) {
            if (!log->exportBundle(destination, input, includePaths->isChecked()))
                QMessageBox::warning(this, "Could not export diagnostics", log->errorString());
        }
    });
    form->addRow(permissionGroup);

    QSettings localDataSettings;
    const auto favoriteGroups = settings::loadFavoriteGroups(localDataSettings);
    int favoriteCount = 0;
    for (const auto& group : favoriteGroups.groups) favoriteCount += group.paths.size();
    const int recentCount = localDataSettings.value("navigation/recents").toStringList().size();
    auto savedSearches = std::make_shared<search::SavedSearchStore>();
    QString savedSearchError;
    const bool savedSearchAvailable = savedSearches->load(&savedSearchError);
    auto* storedDataGroup = new QGroupBox("Stored local data", this);
    storedDataGroup->setObjectName("StoredLocalDataControls");
    auto* storedDataLayout = new QVBoxLayout(storedDataGroup);
    auto* disclosure = new QLabel(
        "Favorites, recent locations, encrypted saved searches, file labels and workspace layouts can contain "
        "local paths. Clear them independently below; saved workspace state includes every named "
        "layout. Diagnostic exports are user-chosen files and are never tracked after export.", storedDataGroup);
    disclosure->setWordWrap(true);
    disclosure->setObjectName("StoredLocalDataDisclosure");
    storedDataLayout->addWidget(disclosure);
    auto* clearFavorites = new QPushButton(favoriteCount == 0 ? "No saved favorites"
        : QString("Clear saved favorites (%1)").arg(localizedItems(favoriteCount)), storedDataGroup);
    clearFavorites->setObjectName("ClearSavedFavorites");
    clearFavorites->setAccessibleName("Clear all saved favorite locations");
    clearFavorites->setEnabled(favoriteCount > 0);
    storedDataLayout->addWidget(clearFavorites);
    connect(clearFavorites, &QPushButton::clicked, this, [this, clearFavorites] {
        if (QMessageBox::question(this, "Clear favorites",
                "Delete every saved favorite location and favorite group?",
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        QSettings settings;
        settings.remove("navigation/favorites");
        settings.remove("navigation/favoriteGroups");
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            QMessageBox::warning(this, "Could not clear favorites", "The saved favorites could not be removed.");
            return;
        }
        if (state_->dependencies.rebuildQuickAccess) state_->dependencies.rebuildQuickAccess();
        clearFavorites->setText("No saved favorites");
        clearFavorites->setEnabled(false);
    });
    auto* clearRecents = new QPushButton(recentCount == 0 ? "No recent locations"
        : QString("Clear recent locations (%1)").arg(localizedItems(recentCount)), storedDataGroup);
    clearRecents->setObjectName("ClearRecentLocations");
    clearRecents->setAccessibleName("Clear recent folder locations");
    clearRecents->setEnabled(recentCount > 0);
    storedDataLayout->addWidget(clearRecents);
    connect(clearRecents, &QPushButton::clicked, this, [this, clearRecents] {
        QSettings settings;
        settings.remove("navigation/recents"); settings.sync();
        if (settings.status() != QSettings::NoError) {
            QMessageBox::warning(this, "Could not clear recent locations", "The recent locations could not be removed.");
            return;
        }
        if (state_->dependencies.rebuildQuickAccess) state_->dependencies.rebuildQuickAccess();
        clearRecents->setText("No recent locations"); clearRecents->setEnabled(false);
    });
    auto* clearSavedSearches = new QPushButton(!savedSearchAvailable ? "Encrypted saved searches unavailable"
        : savedSearches->entries().isEmpty() ? "No encrypted saved searches"
        : QString("Clear encrypted saved searches (%1)").arg(localizedItems(savedSearches->entries().size())), storedDataGroup);
    clearSavedSearches->setObjectName("ClearAllSavedSearchesFromPreferences");
    clearSavedSearches->setAccessibleName("Clear all encrypted saved searches");
    clearSavedSearches->setEnabled(savedSearchAvailable && !savedSearches->entries().isEmpty());
    if (!savedSearchAvailable) clearSavedSearches->setToolTip(savedSearchError);
    storedDataLayout->addWidget(clearSavedSearches);
    connect(clearSavedSearches, &QPushButton::clicked, this, [this, clearSavedSearches, savedSearches] {
        if (QMessageBox::question(this, "Clear saved searches", "Delete every encrypted saved filename search?",
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        QString error;
        if (!savedSearches->clear(&error)) { QMessageBox::warning(this, "Could not clear saved searches", error); return; }
        clearSavedSearches->setText("No encrypted saved searches"); clearSavedSearches->setEnabled(false);
    });
    auto fileLabels = std::make_shared<tags::TagCatalog>(state_->dependencies.fileLabelStoragePath);
    QString fileLabelsError;
    const bool fileLabelsAvailable = fileLabels->load(&fileLabelsError);
    const int fileLabelCount = fileLabelsAvailable ? fileLabels->entries().size() : 0;
    const bool hasLegacyTagRules = fileLabelsAvailable && !fileLabels->rules().isEmpty();
    auto* clearFileLabels = new QPushButton(!fileLabelsAvailable
        ? "Encrypted file labels unavailable"
        : fileLabelCount == 0 && !hasLegacyTagRules ? "No encrypted file labels"
        : fileLabelCount == 0 ? "Clear encrypted label catalog (legacy rules)"
        : QString("Clear all file labels (%1)").arg(localizedItems(fileLabelCount)), storedDataGroup);
    clearFileLabels->setObjectName("ClearAllFileLabels");
    clearFileLabels->setAccessibleName("Clear all encrypted file labels");
    clearFileLabels->setEnabled(fileLabelsAvailable && (fileLabelCount > 0 || hasLegacyTagRules));
    if (!fileLabelsAvailable) clearFileLabels->setToolTip(fileLabelsError);
    storedDataLayout->addWidget(clearFileLabels);
    connect(clearFileLabels, &QPushButton::clicked, this, [this, clearFileLabels, fileLabels] {
        if (QMessageBox::question(this, "Clear file labels",
                "Remove every File Manager label from the encrypted local catalog? "
                "This also removes any dormant legacy destination rules. It cannot be undone. Files and folders will not change.",
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        QString error;
        if (!fileLabels->clear(&error)) {
            QMessageBox::warning(this, "Could not clear file labels", error);
            return;
        }
        clearFileLabels->setText("No encrypted file labels");
        clearFileLabels->setEnabled(false);
        if (state_->dependencies.showStatus)
            state_->dependencies.showStatus("All file labels cleared.", 5000);
    });
    const int sessionSearchCount = state_->dependencies.sessionSearchHistory
        ? state_->dependencies.sessionSearchHistory->entries().size() : 0;
    auto* clearSessionSearches = new QPushButton(sessionSearchCount == 0
        ? "No session-only filename search history"
        : QString("Clear session-only filename search history (%1)").arg(localizedItems(sessionSearchCount)), storedDataGroup);
    clearSessionSearches->setObjectName("ClearSessionSearchHistory");
    clearSessionSearches->setAccessibleName("Clear session-only filename search history");
    clearSessionSearches->setEnabled(sessionSearchCount > 0);
    storedDataLayout->addWidget(clearSessionSearches);
    connect(clearSessionSearches, &QPushButton::clicked, this, [this, clearSessionSearches] {
        if (state_->dependencies.sessionSearchHistory) state_->dependencies.sessionSearchHistory->clear();
        clearSessionSearches->setText("No session-only filename search history"); clearSessionSearches->setEnabled(false);
    });
    form->addRow(storedDataGroup);

    form = generalForm;

    state_->font = new QComboBox(this);
    state_->font->addItems({"9", "9.5", "10", "11", "12", "13", "14"});
    state_->font->setCurrentText(QString::number(state_->originalAppearance.fontPointSize));
    state_->density = new QComboBox(this);
    state_->density->addItems({"Compact", "Comfortable", "Spacious"});
    state_->density->setCurrentText(state_->originalAppearance.density);
    state_->animations = new QCheckBox("Enable animations", this);
    state_->animations->setChecked(state_->originalAppearance.animations);
    const auto accessibility = systemAccessibilityPreferences();
    const bool allowAnimations = !accessibility.highContrast && accessibility.clientAreaAnimations;
    state_->animations->setEnabled(allowAnimations);
    if (!allowAnimations) {
        state_->animations->setToolTip("Animations are currently disabled by Windows accessibility settings.");
        state_->animations->setAccessibleDescription("Unavailable because Windows accessibility settings disable animation.");
    }
    addCompactRow(form, "Font size", "Scale text throughout File Manager.", state_->font);
    addCompactRow(form, "Density", "Choose spacing for lists and controls.", state_->density);
    addCompactRow(form, "Animations", "Use motion where Windows accessibility settings allow it.", state_->animations);
    form = windowsForm;
    state_->shellMetadata = new QCheckBox(
        "Allow Windows Shell metadata handlers (may include third-party extensions)", this);
    state_->shellMetadata->setChecked(state_->metadataPermissions.allowWindowsShellHandlers);
    state_->shellMetadata->setToolTip("Off by default. When enabled, installed property handlers may inspect files while browsing.");
    state_->shellMetadata->setAccessibleName("Allow Windows Shell metadata handlers");
    addCompactRow(form, "Windows metadata handlers", "Optional handlers may inspect browsed files; off by default.", state_->shellMetadata);
    state_->thirdPartyNamespaces = new QCheckBox(
        "Allow third-party Shell namespace extensions in an isolated helper", this);
    state_->thirdPartyNamespaces->setChecked(state_->shellPermissions.allowThirdPartyNamespaces);
    state_->thirdPartyNamespaces->setToolTip("Persistent permission is off by default. You can instead allow one visit in a virtual-location pane. Providers run outside the main app with your Windows access; this is not a file-access sandbox. Saving permissions ends temporary visits in all windows.");
    state_->thirdPartyNamespaces->setAccessibleName("Allow isolated third-party Shell namespaces");
    addCompactRow(form, "Third-party Shell locations", "Allow isolated providers for this account; off by default.", state_->thirdPartyNamespaces);
    state_->contextMenuExtensions = new QCheckBox(
        "Allow Windows context-menu extensions in an isolated helper", this);
    state_->contextMenuExtensions->setChecked(state_->contextMenuPermissions.allowWindowsExtensions);
    state_->contextMenuExtensions->setToolTip("Off by default. When enabled, Windows and installed third-party commands are loaded only after choosing Windows commands, outside the main app with a strict timeout.");
    state_->contextMenuExtensions->setAccessibleName("Allow isolated Windows context-menu extensions");
    addCompactRow(form, "Windows context-menu extensions", "Load only after you choose Windows commands; off by default.", state_->contextMenuExtensions);
    state_->folderIntegration = new QCheckBox("Add ‘Open in File Manager’ for Windows folders and drives", this);
    state_->folderIntegration->setChecked(state_->folderCommand.registered);
    state_->folderIntegration->setEnabled(state_->folderCommand.manageable);
    state_->folderIntegration->setToolTip(state_->folderCommand.manageable
        ? "Off by default. Adds a separate per-user context-menu command. Folder defaults and Win+E are controlled independently below."
        : state_->folderCommand.detail);
    state_->folderIntegration->setAccessibleName("Add Open in File Manager Windows folder command");
    addCompactRow(form, "Open in File Manager command", "Add a separate per-user command for folders and drives.", state_->folderIntegration);
    state_->defaultFolders = new QCheckBox("Open folders with File Manager", this);
    state_->defaultFolders->setObjectName("DefaultFolderRouting");
    state_->defaultFolders->setAccessibleName("Open folders with File Manager");
    state_->defaultFolders->setToolTip("Off by default. Changes supported folder and drive launches for your Windows account. Explicit Explorer commands and virtual locations may still open in Windows.");
    addCompactRow(form, "Open supported folders with File Manager", "For this account; previous settings can be restored.", state_->defaultFolders);
    auto* routingDisclosure = new QLabel("Applies to this installed copy. Your previous folder-opening settings are saved locally for restoration. Disabling restores them only where File Manager's values remain unchanged.", this);
    routingDisclosure->setWordWrap(true);
    form->addRow(routingDisclosure);
    state_->folderRoutingDetail = new QLabel(this);
    state_->folderRoutingDetail->setObjectName("FolderRoutingStatus");
    state_->folderRoutingDetail->setWordWrap(true);
    state_->folderRoutingDetail->setTextFormat(Qt::PlainText);
    form->addRow(state_->folderRoutingDetail);
    state_->restoreFolderRouting = new QPushButton("Restore previous folder-opening behavior", this);
    state_->restoreFolderRouting->setObjectName("RestoreFolderRouting");
    form->addRow(state_->restoreFolderRouting);
    state_->refreshFolderRouting = [this] {
        state_->folderRouting = platform::folderRoutingStatus(state_->applicationPath);
        const bool enabled = state_->folderRouting.state == platform::FolderRoutingState::Enabled;
        state_->defaultFolders->setChecked(enabled);
        state_->defaultFolders->setEnabled(state_->folderRouting.canEnable || (enabled && state_->folderRouting.owned));
        state_->folderRoutingDetail->setText(state_->folderRouting.detail);
        state_->restoreFolderRouting->setEnabled(state_->folderRouting.owned);
    };
    state_->refreshFolderRouting();
    connect(state_->restoreFolderRouting, &QPushButton::clicked, this, [this] {
        const auto result = platform::setFolderRouting(false, state_->applicationPath);
        state_->refreshFolderRouting();
        if (!result.success) QMessageBox::warning(this, "Folder opening", result.error);
    });
    state_->appPathsIntegration = new QCheckBox("Register FileManager.exe with Windows App Paths", this);
    state_->appPathsIntegration->setChecked(state_->appPaths.registered);
    state_->appPathsIntegration->setEnabled(state_->appPaths.manageable);
    state_->appPathsIntegration->setToolTip(state_->appPaths.manageable
        ? "Off by default. Uses Microsoft's per-user App Paths lookup and does not change the user or system PATH environment variable."
        : state_->appPaths.detail);
    state_->appPathsIntegration->setAccessibleName("Register File Manager with Windows App Paths");
    addCompactRow(form, "Windows App Paths", "Find this app by name without changing PATH.", state_->appPathsIntegration);
    state_->winEIntegration = new QCheckBox("Use File Manager for Win+E", this);
    state_->winEIntegration->setObjectName("WinEIntegration");
    state_->winEIntegration->setChecked(state_->winE.enabled);
    state_->winEIntegration->setEnabled(state_->winE.manageable);
    const QString winEUnavailable = state_->winE.manageable ? QString{} : state_->winE.error;
    state_->winEIntegration->setToolTip(winEUnavailable.isEmpty()
        ? "Off by default. Starts a background shortcut helper; disabling it stops the helper and restores Windows shortcut handling."
        : winEUnavailable);
    if (!winEUnavailable.isEmpty())
        state_->winEIntegration->setAccessibleDescription(winEUnavailable);
    addCompactRow(form, "Use File Manager for Win+E",
        winEUnavailable.isEmpty()
            ? "Runs a local keyboard helper; other keys pass through and typing is never recorded or sent."
            : winEUnavailable,
        state_->winEIntegration);
    state_->winEAtSignIn = new QCheckBox("Start the Win+E helper when I sign in", this);
    state_->winEAtSignIn->setObjectName("WinEAtSignIn");
    state_->winEAtSignIn->setChecked(state_->winE.atSignIn);
    state_->winEAtSignIn->setEnabled(state_->winE.manageable && state_->winE.enabled);
    addCompactRow(form, "Start Win+E helper at sign-in", "Optional; keeps Win+E available before opening File Manager.", state_->winEAtSignIn);
    auto* shortcutDisclosure = new QLabel(
        "The helper stays running after File Manager closes. It receives keyboard events to recognize Win+E, "
        "passes other keys through, and never records or transmits typing. Sign-in startup is optional. "
        "Windows restrictions and other shortcut tools may affect availability.", this);
    shortcutDisclosure->setWordWrap(true);
    form->addRow(shortcutDisclosure);
    if (state_->winE.manageable && !state_->winE.error.isEmpty()) {
        auto* error = new QLabel(state_->winE.error, this);
        error->setWordWrap(true);
        form->addRow(error);
    }
    connect(state_->winEIntegration, &QCheckBox::toggled, this, [this](bool enabled) {
        state_->winEAtSignIn->setEnabled(enabled && state_->winE.manageable);
        if (!enabled) state_->winEAtSignIn->setChecked(false);
    });
    form = privacyForm;
    state_->restoreWorkspace = new QCheckBox(
        "Restore tabs, paths, selection, focus, scroll and expanded folders at startup", this);
    state_->restoreWorkspace->setObjectName("RestorePreviousWorkspace");
    state_->restoreWorkspace->setChecked(state_->sessionStore.restorationEnabled());
    state_->restoreWorkspace->setToolTip("Off by default. When enabled, local workspace state containing filesystem paths is saved when File Manager closes and restored at the next start.");
    state_->restoreWorkspace->setAccessibleName("Restore previous workspace at startup");
    addCompactRow(form, "Restore workspace at startup", "Save tabs and paths in encrypted local state.", state_->restoreWorkspace);
    auto* clearWorkspace = new QPushButton(state_->sessionStore.hasSavedSession()
        ? "Clear saved workspace state" : "No saved workspace state", this);
    clearWorkspace->setObjectName("ClearSavedWorkspace");
    clearWorkspace->setAccessibleName("Clear saved workspace state");
    clearWorkspace->setEnabled(state_->sessionStore.hasSavedSession());
    form->addRow("Workspace data", clearWorkspace);
    connect(clearWorkspace, &QPushButton::clicked, this, [this, clearWorkspace] {
        QString error;
        if (!state_->sessionStore.clear(&error)) QMessageBox::warning(this, "Could not clear workspace", error);
        else { clearWorkspace->setText("No saved workspace state"); clearWorkspace->setEnabled(false); }
    });
    form = generalForm;
    state_->startupDestination = new QComboBox(this);
    state_->startupDestination->setObjectName("StartupDestination");
    state_->startupDestination->setAccessibleName("Open File Manager to");
    state_->startupDestination->addItem("Home",
        static_cast<int>(settings::StartupDestination::Home));
    state_->startupDestination->addItem("This PC",
        static_cast<int>(settings::StartupDestination::ThisPc));
    state_->startupDestination->addItem("Last folder",
        static_cast<int>(settings::StartupDestination::LastFolder));
    QSettings interactionSettings;
    const auto startupDestination = settings::loadStartupDestination(interactionSettings);
    state_->startupDestination->setCurrentIndex(state_->startupDestination->findData(
        static_cast<int>(startupDestination)));
    state_->startupDestination->setToolTip(
        "Used when workspace restoration is off or no saved workspace is available. Last folder "
        "uses the most recent folder if it is still available and otherwise falls back to Home.");
    addCompactRow(form, "Open File Manager to", "Used when no saved workspace is restored.", state_->startupDestination);
    state_->singleClick = new QCheckBox("Open items with one click", this);
    state_->singleClick->setObjectName("SingleClickActivation");
    state_->singleClick->setAccessibleName("Open files and folders with one click");
    state_->singleClick->setChecked(QSettings().value("interaction/singleClickActivation", false).toBool());
    state_->singleClick->setToolTip("Off by default. Ctrl, Shift, Alt and Windows-key clicks remain selection-only.");
    addCompactRow(form, "One-click activation", "Open an item with one click; off by default.", state_->singleClick);
    state_->selectionCheckBoxes = new QCheckBox("Show item-selection check boxes", this);
    state_->selectionCheckBoxes->setObjectName("ItemSelectionCheckBoxes");
    state_->selectionCheckBoxes->setAccessibleName("Show item-selection check boxes");
    state_->selectionCheckBoxes->setChecked(
        interactionSettings.value("interaction/itemSelectionCheckBoxes", false).toBool());
    state_->selectionCheckBoxes->setToolTip(
        "Off by default. Check boxes mirror the existing selection and can be toggled with Space "
        "without changing file contents.");
    addCompactRow(form, "Selection check boxes", "Show a check box beside each item; off by default.", state_->selectionCheckBoxes);

    form = portabilityForm;
    QSet<QString> portableCommandIds;
    if (state_->dependencies.commands)
        for (QAction* action : state_->dependencies.commands->actions()) portableCommandIds.insert(action->objectName());
    auto* portabilityGroup = new QGroupBox("Settings portability", this);
    portabilityGroup->setObjectName("SettingsPortability");
    auto* portabilityLayout = new QVBoxLayout(portabilityGroup);
    auto* portabilityDisclosure = new QLabel(
        "Exports appearance, item activation, shortcuts and pinned commands only. Privacy permissions, Windows integrations, paths, favorites, searches, layouts, indexes, history and diagnostics are excluded.", portabilityGroup);
    portabilityDisclosure->setWordWrap(true);
    portabilityLayout->addWidget(portabilityDisclosure);
    auto* portabilityActions = new QHBoxLayout;
    auto* exportPreferences = new QPushButton("Export settings…", portabilityGroup);
    exportPreferences->setObjectName("ExportPortableSettings");
    exportPreferences->setAccessibleName("Export portable settings");
    auto* importPreferences = new QPushButton("Import settings…", portabilityGroup);
    importPreferences->setObjectName("ImportPortableSettings");
    importPreferences->setAccessibleName("Import portable settings");
    portabilityActions->addWidget(exportPreferences); portabilityActions->addWidget(importPreferences);
    portabilityActions->addStretch(1); portabilityLayout->addLayout(portabilityActions);
    auto* portabilityStatus = new QLabel(portabilityGroup);
    portabilityStatus->setWordWrap(true); portabilityStatus->setAccessibleName("Settings import status");
    portabilityLayout->addWidget(portabilityStatus); form->addRow(portabilityGroup);
    connect(exportPreferences, &QPushButton::clicked, this, [this] {
        const QString destination = QFileDialog::getSaveFileName(this, "Export settings",
            QDir::home().filePath("file-manager-settings.json"), "File Manager settings (*.json)");
        if (destination.isEmpty()) return;
        settings::PortablePreferences preferences;
        preferences.fontPointSize = state_->font->currentText().toDouble();
        preferences.density = state_->density->currentText();
        preferences.animations = state_->animations->isChecked();
        preferences.singleClickActivation = state_->singleClick->isChecked();
        if (state_->dependencies.commands)
            for (QAction* action : state_->dependencies.commands->actions())
                preferences.shortcuts.insert(action->objectName(), action->shortcut().toString(QKeySequence::PortableText));
        preferences.pinnedContextActions = QSettings().value("interaction/pinnedContextActions").toStringList();
        QString error;
        if (!settings::writePortablePreferences(destination, preferences, &error)) {
            QMessageBox::warning(this, "Could not export settings", error); return;
        }
        if (state_->dependencies.showStatus)
            state_->dependencies.showStatus("Settings exported without permissions, paths, history or diagnostics", 6000);
    });
    connect(importPreferences, &QPushButton::clicked, this,
        [this, portableCommandIds, portabilityStatus] {
            const QString source = QFileDialog::getOpenFileName(this, "Import settings",
                QDir::homePath(), "File Manager settings (*.json)");
            if (source.isEmpty()) return;
            settings::PortablePreferences candidate; QString error;
            if (!settings::readPortablePreferences(source, portableCommandIds, &candidate, &error)) {
                QMessageBox::warning(this, "Could not import settings", error); return;
            }
            QHash<QString, QString> owners;
            if (state_->dependencies.commands) {
                for (QAction* action : state_->dependencies.commands->actions()) {
                    const QString id = action->objectName();
                    const QString sequence = candidate.shortcuts.contains(id)
                        ? candidate.shortcuts.value(id) : action->shortcut().toString(QKeySequence::PortableText);
                    if (!sequence.isEmpty() && owners.contains(sequence)) {
                        QMessageBox::warning(this, "Could not import settings",
                            QString("The imported shortcut %1 conflicts between %2 and %3.").arg(sequence, owners.value(sequence), id));
                        return;
                    }
                    if (!sequence.isEmpty()) owners.insert(sequence, id);
                }
            }
            state_->importedPreferences = candidate;
            const QString importedFont = QString::number(candidate.fontPointSize);
            if (state_->font->findText(importedFont) < 0) state_->font->addItem(importedFont);
            state_->font->setCurrentText(importedFont);
            state_->density->setCurrentText(candidate.density);
            state_->animations->setChecked(candidate.animations);
            state_->singleClick->setChecked(candidate.singleClickActivation);
            portabilityStatus->setText("Validated. The imported settings will be applied when you choose OK; excluded privacy permissions and local data remain unchanged.");
        });

    form = privacyForm;
    const int operationCount = state_->dependencies.operationJournal
        ? state_->dependencies.operationJournal->records().size() : 0;
    auto* clearOperations = new QPushButton(
        QStringLiteral("Clear encrypted operation history (%1 record(s))").arg(operationCount), this);
    clearOperations->setAccessibleName("Clear encrypted operation history");
    clearOperations->setEnabled(operationCount > 0);
    form->addRow("Operation privacy", clearOperations);
    connect(clearOperations, &QPushButton::clicked, this, [this, clearOperations] {
        if (!state_->dependencies.operationJournal
                || QMessageBox::question(this, "Clear operation history",
                    "Delete the encrypted operation journal and all retained recovery/undo records?",
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        QString error;
        if (!state_->dependencies.operationJournal->clear(&error))
            QMessageBox::warning(this, "Could not clear history", error);
        else { clearOperations->setText("Clear encrypted operation history (0 record(s))"); clearOperations->setEnabled(false); }
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    dialogLayout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

PreferencesDialog::~PreferencesDialog() = default;

bool PreferencesDialog::singleClickActivation() const
{
    return state_->singleClick->isChecked();
}

bool PreferencesDialog::selectionCheckBoxes() const
{
    return state_->selectionCheckBoxes->isChecked();
}

void PreferencesDialog::accept()
{
    const bool foldersEnabled = state_->folderRouting.state == platform::FolderRoutingState::Enabled;
    if (state_->defaultFolders->isEnabled() && state_->defaultFolders->isChecked() != foldersEnabled) {
        const auto result = platform::setFolderRouting(state_->defaultFolders->isChecked(), state_->applicationPath);
        state_->refreshFolderRouting();
        if (!result.success) {
            QMessageBox::warning(this, "Folder opening", result.error);
            return;
        }
    }
    if (state_->winE.manageable
        && (state_->winEIntegration->isChecked() != state_->winE.enabled
            || state_->winEAtSignIn->isChecked() != state_->winE.atSignIn)) {
        const auto result = platform::setWinEIntegration(state_->applicationPath,
            state_->winEIntegration->isChecked(), state_->winEAtSignIn->isChecked());
        state_->winE = platform::winEStatus(state_->applicationPath);
        if (!result.success) {
            state_->winEIntegration->setChecked(state_->winE.enabled);
            state_->winEAtSignIn->setChecked(state_->winE.atSignIn);
            QMessageBox::warning(this, "Win+E integration", result.error);
            return;
        }
    }
    saveAppearanceSettings({state_->font->currentText().toDouble(), state_->density->currentText(),
        state_->animations->isChecked()});
    settings::saveMetadataPermissions({state_->shellMetadata->isChecked()});
    settings::saveShellPermissions({state_->thirdPartyNamespaces->isChecked()});
    settings::saveContextMenuPermissions({state_->contextMenuExtensions->isChecked()});
    QSettings interactionSettings;
    interactionSettings.setValue("interaction/singleClickActivation",
        state_->singleClick->isChecked());
    interactionSettings.setValue("interaction/itemSelectionCheckBoxes",
        state_->selectionCheckBoxes->isChecked());
    settings::saveStartupDestination(interactionSettings,
        static_cast<settings::StartupDestination>(
            state_->startupDestination->currentData().toInt()));
    if (state_->importedPreferences && state_->dependencies.commands) {
        QSettings importedSettings;
        for (auto it = state_->importedPreferences->shortcuts.constBegin();
             it != state_->importedPreferences->shortcuts.constEnd(); ++it) {
            const QKeySequence sequence = QKeySequence::fromString(it.value(), QKeySequence::PortableText);
            state_->dependencies.commands->setShortcut(it.key(), sequence);
            importedSettings.setValue("shortcuts/" + it.key(), it.value());
        }
        importedSettings.setValue("interaction/pinnedContextActions",
            state_->importedPreferences->pinnedContextActions);
        importedSettings.sync();
        if (importedSettings.status() != QSettings::NoError)
            QMessageBox::warning(this, "Settings import", "The validated settings could not be completely saved.");
    }
    const bool previouslyRestored = state_->sessionStore.restorationEnabled();
    state_->sessionStore.setRestorationEnabled(state_->restoreWorkspace->isChecked());
    if (!state_->restoreWorkspace->isChecked() && previouslyRestored) {
        QString error;
        if (!state_->sessionStore.clear(&error)) QMessageBox::warning(this, "Could not clear workspace", error);
    } else if (state_->restoreWorkspace->isChecked() && state_->dependencies.saveWorkspace) {
        state_->dependencies.saveWorkspace();
    }
    if (state_->folderCommand.manageable
            && (state_->folderIntegration->isChecked() != state_->folderCommand.registered
                || (state_->folderIntegration->isChecked() && !state_->folderCommand.current))) {
        const auto result = platform::setWindowsFolderCommandRegistered(
            state_->folderIntegration->isChecked(), state_->applicationPath);
        if (!result.success) QMessageBox::warning(this, "Windows integration", result.error);
    }
    if (state_->appPaths.manageable
            && (state_->appPathsIntegration->isChecked() != state_->appPaths.registered
                || (state_->appPathsIntegration->isChecked() && !state_->appPaths.current))) {
        const auto result = platform::setWindowsAppPathsRegistered(
            state_->appPathsIntegration->isChecked(), state_->applicationPath);
        if (!result.success) QMessageBox::warning(this, "Windows integration", result.error);
    }
    QDialog::accept();
}

} // namespace filemanager::ui
