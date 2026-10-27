#include "filesearchdialog.h"

#include "search/metadataindex.h"
#include "search/savedsearchstore.h"
#include "search/searchservice.h"
#include "search/sessionsearchhistory.h"
#include "ui/localizedformat.h"
#include "ui/searchresultbar.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStringListModel>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace filemanager::ui {
namespace {

void insertRankedSearchHit(QListWidget* results, const search::SearchHit& hit)
{
    auto* item = new QListWidgetItem((hit.directory ? "Folder  " : "File  ") + hit.name);
    item->setData(Qt::UserRole, hit.path);
    item->setData(Qt::UserRole + 1, hit.score);
    item->setData(Qt::UserRole + 2, hit.name);
    item->setData(Qt::UserRole + 3, hit.directory);
    item->setToolTip(hit.path);

    int insertionRow = results->count();
    for (int row = 0; row < results->count(); ++row) {
        const QListWidgetItem* current = results->item(row);
        const search::SearchHit currentHit{
            current->data(Qt::UserRole).toString(),
            current->data(Qt::UserRole + 2).toString(),
            current->data(Qt::UserRole + 3).toBool(),
            current->data(Qt::UserRole + 1).toInt()};
        if (search::searchHitPrecedes(hit, currentHit)) {
            insertionRow = row;
            break;
        }
    }
    results->insertItem(insertionRow, item);
}

} // namespace

class FileSearchDialog::State
{
public:
    search::SessionSearchHistory* history = nullptr;
    QLineEdit* query = nullptr;
    QLineEdit* extension = nullptr;
    QComboBox* itemType = nullptr;
    QComboBox* sizeFilter = nullptr;
    QComboBox* dateFilter = nullptr;
    QCheckBox* indexedSearch = nullptr;
    QCheckBox* advancedQuery = nullptr;
    QPushButton* clearRecent = nullptr;
    QPushButton* manageIndex = nullptr;
    QComboBox* savedSearches = nullptr;
    QPushButton* saveSearch = nullptr;
    QPushButton* deleteSavedSearch = nullptr;
    QPushButton* clearSavedSearches = nullptr;
    QListWidget* results = nullptr;
    QLabel* status = nullptr;
    QLabel* historyDisclosure = nullptr;
    QLabel* scopeDisclosure = nullptr;
    QLabel* advancedDisclosure = nullptr;
    QCompleter* historyCompleter = nullptr;
    search::SearchService search;
    search::MetadataIndex metadataIndex;
    search::SavedSearchStore savedSearchStore;
    QSettings indexSettings;
    QTimer debounce;
    QString searchRoot;
    QString indexDatabase;
    QStringList indexedRoots;
    quint64 activeGeneration = 0;
    bool usingIndex = false;
    bool lastExecutedWasAqs = false;
    QString searchError;
    bool savedSearchesAvailable = false;
};

FileSearchDialog::FileSearchDialog(const QString& initialRoot,
    search::SessionSearchHistory* sessionHistory, QWidget* parent,
    const QString& savedSearchStoragePath, const QString& indexDatabasePath)
    : QDialog(parent), state_(std::make_unique<State>())
{
    Q_ASSERT(sessionHistory);
    state_->history = sessionHistory;
    state_->searchRoot = QDir::cleanPath(initialRoot);
    state_->indexDatabase = indexDatabasePath.isEmpty()
        ? QDir(QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation)).filePath("metadata-index.sqlite")
        : indexDatabasePath;
    if (!savedSearchStoragePath.isEmpty())
        state_->savedSearchStore = search::SavedSearchStore(savedSearchStoragePath);
    state_->indexedRoots = state_->indexSettings.value("search/indexedRoots").toStringList();

    setObjectName("FileSearchDialog");
    setWindowTitle("Search files");
    resize(760, 560);
    auto* layout = new QVBoxLayout(this);
    auto* row = new QHBoxLayout;
    state_->query = new QLineEdit(this);
    state_->extension = new QLineEdit(this);
    state_->itemType = new QComboBox(this);
    state_->sizeFilter = new QComboBox(this);
    state_->dateFilter = new QComboBox(this);
    auto* cancel = new QPushButton("Cancel", this);
    state_->clearRecent = new QPushButton("Clear recent", this);
    state_->indexedSearch = new QCheckBox("Use metadata index", this);
    state_->advancedQuery = new QCheckBox(
        "Use Windows Advanced Query Syntax (indexed folders only)", this);
    state_->manageIndex = new QPushButton(this);
    state_->savedSearches = new QComboBox(this);
    state_->saveSearch = new QPushButton("Save current...", this);
    state_->deleteSavedSearch = new QPushButton("Delete saved", this);
    state_->clearSavedSearches = new QPushButton("Clear all saved", this);
    state_->results = new QListWidget(this);
    state_->status = new QLabel("Enter a file name to search recursively.", this);
    state_->historyDisclosure = new QLabel(
        "Recent filename searches are kept only until File Manager closes. Saved filename searches "
        "are written only after Save current and encrypted for your Windows account.", this);
    state_->scopeDisclosure = new QLabel(
        "Search scope: " + QDir::toNativeSeparators(state_->searchRoot), this);
    state_->advancedDisclosure = new QLabel(
        "AQS is sent once to the existing Windows Search index for the displayed scope. "
        "It is not added to recent searches or saved by File Manager, and an unindexed folder "
        "is not silently scanned or added to the index.", this);

    state_->historyDisclosure->setObjectName("FileSearchHistoryDisclosure");
    state_->historyDisclosure->setWordWrap(true);
    state_->scopeDisclosure->setObjectName("FileSearchScope");
    state_->scopeDisclosure->setAccessibleName("Current Windows Search scope");
    state_->scopeDisclosure->setTextInteractionFlags(Qt::TextSelectableByKeyboard
        | Qt::TextSelectableByMouse);
    state_->advancedDisclosure->setObjectName("FileSearchAqsDisclosure");
    state_->advancedDisclosure->setAccessibleName("Advanced Query Syntax privacy and scope notice");
    state_->advancedDisclosure->setWordWrap(true);
    state_->savedSearches->setObjectName("SavedSearches");
    state_->savedSearches->setAccessibleName("Encrypted saved filename searches");
    state_->saveSearch->setObjectName("SaveSearch");
    state_->deleteSavedSearch->setObjectName("DeleteSavedSearch");
    state_->clearSavedSearches->setObjectName("ClearSavedSearches");
    state_->query->setObjectName("FileSearchQuery");
    state_->query->setPlaceholderText("Fuzzy file name");
    state_->historyCompleter = new QCompleter(state_->history->entries(), state_->query);
    state_->historyCompleter->setObjectName("FileSearchHistoryCompleter");
    state_->historyCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    state_->historyCompleter->setFilterMode(Qt::MatchContains);
    state_->historyCompleter->setCompletionMode(QCompleter::PopupCompletion);
    state_->query->setCompleter(state_->historyCompleter);
    state_->extension->setObjectName("FileSearchExtension");
    state_->extension->setPlaceholderText("Extension");
    state_->extension->setMaximumWidth(160);
    state_->itemType->setObjectName("FileSearchItemType");
    state_->sizeFilter->setObjectName("FileSearchSizeFilter");
    state_->dateFilter->setObjectName("FileSearchDateFilter");
    state_->indexedSearch->setObjectName("FileSearchUseMetadataIndex");
    state_->advancedQuery->setObjectName("FileSearchUseAqs");
    state_->manageIndex->setObjectName("FileSearchManageIndex");
    state_->clearRecent->setObjectName("FileSearchClearRecent");
    cancel->setObjectName("FileSearchCancel");
    state_->itemType->addItems({"Files and folders", "Files only", "Folders only"});
    state_->sizeFilter->addItems({"Any size", "Empty (0 bytes)", "Under 1 MB", "1–100 MB",
        "100 MB–1 GB", "Over 1 GB"});
    state_->dateFilter->addItems({"Any modified date", "Today", "Past 7 days", "Past 30 days",
        "Past year"});
    row->addWidget(state_->query, 1);
    row->addWidget(state_->extension);
    row->addWidget(cancel);
    layout->addLayout(row);
    layout->addWidget(state_->scopeDisclosure);
    auto* historyRow = new QHBoxLayout;
    historyRow->addWidget(state_->historyDisclosure, 1);
    historyRow->addWidget(state_->clearRecent);
    layout->addLayout(historyRow);
    auto* savedRow = new QHBoxLayout;
    savedRow->addWidget(state_->savedSearches, 1);
    savedRow->addWidget(state_->saveSearch);
    savedRow->addWidget(state_->deleteSavedSearch);
    savedRow->addWidget(state_->clearSavedSearches);
    layout->addLayout(savedRow);
    auto* filterRow = new QHBoxLayout;
    filterRow->addWidget(state_->itemType);
    filterRow->addWidget(state_->sizeFilter);
    filterRow->addWidget(state_->dateFilter);
    filterRow->addStretch(1);
    layout->addLayout(filterRow);
    layout->addWidget(state_->advancedQuery);
    layout->addWidget(state_->advancedDisclosure);
    auto* indexRow = new QHBoxLayout;
    indexRow->addWidget(state_->indexedSearch);
    indexRow->addStretch(1);
    indexRow->addWidget(state_->manageIndex);
    layout->addLayout(indexRow);
    layout->addWidget(state_->results, 1);
    state_->results->setObjectName("FileSearchResults");
    state_->results->setAccessibleName("File search results");
    auto* resultActions = new SearchResultBar(state_->results, this);
    layout->addWidget(resultActions);
    layout->addWidget(state_->status);

    QString savedSearchError;
    state_->savedSearchesAvailable = state_->savedSearchStore.load(&savedSearchError);
    const auto refreshSavedSearches = [this] {
        QSignalBlocker blocker(state_->savedSearches);
        state_->savedSearches->clear();
        state_->savedSearches->addItem("Saved searches...");
        for (const auto& saved : state_->savedSearchStore.entries())
            state_->savedSearches->addItem(saved.name, saved.id);
        state_->savedSearches->setCurrentIndex(0);
        const bool hasSaved = state_->savedSearchesAvailable
            && !state_->savedSearchStore.entries().isEmpty();
        const bool ordinarySearch = !state_->advancedQuery->isChecked();
        state_->savedSearches->setEnabled(hasSaved && ordinarySearch);
        state_->deleteSavedSearch->setEnabled(false);
        state_->clearSavedSearches->setEnabled(hasSaved && ordinarySearch);
        state_->saveSearch->setEnabled(state_->savedSearchesAvailable && ordinarySearch);
    };
    refreshSavedSearches();
    if (!state_->savedSearchesAvailable) {
        state_->historyDisclosure->setText(state_->historyDisclosure->text()
            + " Encrypted saved searches are unavailable: " + savedSearchError);
    }
    const auto refreshIndexControls = [this] {
        const bool enabled = state_->indexedRoots.contains(
            state_->searchRoot, Qt::CaseInsensitive);
        QSignalBlocker blocker(state_->indexedSearch);
        state_->indexedSearch->setEnabled(enabled && !state_->advancedQuery->isChecked());
        if (!state_->advancedQuery->isChecked())
            state_->indexedSearch->setChecked(enabled);
        state_->manageIndex->setText(enabled ? "Remove folder index" : "Index this folder...");
        state_->manageIndex->setEnabled(!state_->advancedQuery->isChecked());
    };
    refreshIndexControls();
    state_->clearRecent->setEnabled(!state_->history->entries().isEmpty());

    state_->debounce.setSingleShot(true);
    state_->debounce.setInterval(180);
    connect(state_->query, &QLineEdit::textChanged,
        &state_->debounce, qOverload<>(&QTimer::start));
    connect(state_->extension, &QLineEdit::textChanged,
        &state_->debounce, qOverload<>(&QTimer::start));
    connect(state_->itemType, &QComboBox::currentIndexChanged,
        &state_->debounce, qOverload<>(&QTimer::start));
    connect(state_->sizeFilter, &QComboBox::currentIndexChanged,
        &state_->debounce, qOverload<>(&QTimer::start));
    connect(state_->dateFilter, &QComboBox::currentIndexChanged,
        &state_->debounce, qOverload<>(&QTimer::start));
    connect(state_->indexedSearch, &QCheckBox::toggled,
        &state_->debounce, qOverload<>(&QTimer::start));
    connect(state_->advancedQuery, &QCheckBox::toggled,
        &state_->debounce, qOverload<>(&QTimer::start));

    const auto currentFilters = [this] {
        search::SearchFilters filters;
        filters.name = state_->query->text().trimmed();
        filters.extension = state_->extension->text();
        filters.itemType = state_->itemType->currentIndex() == 1
            ? search::SearchItemType::Files
            : state_->itemType->currentIndex() == 2
                ? search::SearchItemType::Folders : search::SearchItemType::Any;
        constexpr qint64 MiB = 1024 * 1024;
        constexpr qint64 GiB = 1024 * MiB;
        switch (state_->sizeFilter->currentIndex()) {
        case 1: filters.minimumSize = 0; filters.maximumSize = 0; break;
        case 2: filters.minimumSize = 0; filters.maximumSize = MiB - 1; break;
        case 3: filters.minimumSize = MiB; filters.maximumSize = 100 * MiB; break;
        case 4: filters.minimumSize = 100 * MiB + 1; filters.maximumSize = GiB; break;
        case 5: filters.minimumSize = GiB + 1; break;
        default: break;
        }
        const QDateTime now = QDateTime::currentDateTime();
        switch (state_->dateFilter->currentIndex()) {
        case 1: filters.modifiedAfter = QDateTime(now.date().startOfDay()); break;
        case 2: filters.modifiedAfter = now.addDays(-7); break;
        case 3: filters.modifiedAfter = now.addDays(-30); break;
        case 4: filters.modifiedAfter = now.addYears(-1); break;
        default: break;
        }
        return filters;
    };

    const auto applyQueryMode = [this, refreshIndexControls, refreshSavedSearches] {
        const bool advanced = state_->advancedQuery->isChecked();
        state_->extension->setEnabled(!advanced);
        state_->itemType->setEnabled(!advanced);
        state_->sizeFilter->setEnabled(!advanced);
        state_->dateFilter->setEnabled(!advanced);
        state_->query->setCompleter(advanced ? nullptr : state_->historyCompleter);
        state_->query->setPlaceholderText(advanced
            ? "AQS, for example kind:picture datemodified:this month"
            : "Fuzzy file name");
        state_->advancedDisclosure->setVisible(advanced);
        refreshIndexControls();
        refreshSavedSearches();
    };
    connect(state_->advancedQuery, &QCheckBox::toggled, this,
        [this, applyQueryMode] {
            state_->search.cancel();
            state_->metadataIndex.cancel();
            state_->results->clear();
            applyQueryMode();
        });
    applyQueryMode();

    connect(state_->clearRecent, &QPushButton::clicked, this, [this] {
        state_->history->clear();
        if (auto* model = qobject_cast<QStringListModel*>(state_->historyCompleter->model()))
            model->setStringList({});
        state_->clearRecent->setEnabled(false);
    });
    connect(state_->savedSearches, &QComboBox::currentIndexChanged, this,
        [this, refreshIndexControls](int index) {
            state_->deleteSavedSearch->setEnabled(index > 0);
            if (index <= 0)
                return;
            state_->advancedQuery->setChecked(false);
            const QString id = state_->savedSearches->itemData(index).toString();
            const auto saved = std::find_if(state_->savedSearchStore.entries().cbegin(),
                state_->savedSearchStore.entries().cend(), [&](const auto& entry) {
                    return entry.id == id;
                });
            if (saved == state_->savedSearchStore.entries().cend())
                return;
            state_->searchRoot = saved->root;
            state_->scopeDisclosure->setText(
                QStringLiteral("Search scope: %1")
                    .arg(QDir::toNativeSeparators(state_->searchRoot)));
            state_->query->setText(saved->filters.name);
            state_->extension->setText(saved->filters.extension);
            state_->itemType->setCurrentIndex(static_cast<int>(saved->filters.itemType));
            state_->sizeFilter->setCurrentIndex(saved->sizePreset);
            state_->dateFilter->setCurrentIndex(saved->datePreset);
            refreshIndexControls();
            state_->debounce.start();
        });
    connect(state_->saveSearch, &QPushButton::clicked, this,
        [this, currentFilters, refreshSavedSearches] {
            if (state_->advancedQuery->isChecked())
                return;
            const auto filters = currentFilters();
            if (filters.isDefault()) {
                QMessageBox::information(this, "Nothing to save",
                    "Enter a filename or choose at least one metadata filter first.");
                return;
            }
            bool accepted = false;
            const QString name = QInputDialog::getText(this, "Save search", "Saved search name:",
                QLineEdit::Normal, {}, &accepted).trimmed();
            if (!accepted || name.isEmpty())
                return;
            QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto sameName = std::find_if(state_->savedSearchStore.entries().cbegin(),
                state_->savedSearchStore.entries().cend(), [&](const auto& entry) {
                    return entry.name.compare(name, Qt::CaseInsensitive) == 0;
                });
            if (sameName != state_->savedSearchStore.entries().cend()) {
                if (QMessageBox::question(this, "Replace saved search",
                        "A saved search with this name already exists. Replace it?",
                        QMessageBox::Yes | QMessageBox::Cancel,
                        QMessageBox::Cancel) != QMessageBox::Yes)
                    return;
                id = sameName->id;
            }
            search::SavedSearch saved{id, name, state_->searchRoot, filters,
                state_->sizeFilter->currentIndex(), state_->dateFilter->currentIndex()};
            QString error;
            if (!state_->savedSearchStore.save(saved, &error))
                QMessageBox::warning(this, "Could not save search", error);
            else
                refreshSavedSearches();
        });
    connect(state_->deleteSavedSearch, &QPushButton::clicked, this,
        [this, refreshSavedSearches] {
            const int index = state_->savedSearches->currentIndex();
            if (index <= 0)
                return;
            const QString id = state_->savedSearches->itemData(index).toString();
            if (QMessageBox::question(this, "Delete saved search",
                    "Delete this encrypted saved search?",
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel) != QMessageBox::Yes)
                return;
            QString error;
            if (!state_->savedSearchStore.remove(id, &error))
                QMessageBox::warning(this, "Could not delete saved search", error);
            else
                refreshSavedSearches();
        });
    connect(state_->clearSavedSearches, &QPushButton::clicked, this,
        [this, refreshSavedSearches] {
            if (QMessageBox::question(this, "Clear saved searches",
                    "Delete every encrypted saved filename search?",
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel) != QMessageBox::Yes)
                return;
            QString error;
            if (!state_->savedSearchStore.clear(&error))
                QMessageBox::warning(this, "Could not clear saved searches", error);
            else
                refreshSavedSearches();
        });
    connect(&state_->debounce, &QTimer::timeout, this, [this, currentFilters] {
        state_->results->clear();
        state_->searchError.clear();
        const bool advanced = state_->advancedQuery->isChecked();
        const search::SearchFilters filters = currentFilters();
        if ((advanced && state_->query->text().trimmed().isEmpty())
                || (!advanced && filters.isDefault())) {
            state_->search.cancel();
            state_->metadataIndex.cancel();
            state_->status->setText(advanced
                ? "Enter an Advanced Query Syntax expression."
                : "Enter a file name to search recursively.");
            return;
        }
        state_->lastExecutedWasAqs = advanced;
        state_->usingIndex = !advanced && state_->indexedSearch->isChecked();
        if (advanced) {
            state_->status->setText("Searching the displayed scope with Windows Search AQS...");
            state_->activeGeneration = state_->search.startAqs(
                state_->searchRoot, state_->query->text().trimmed());
        } else {
            state_->status->setText(state_->usingIndex
                ? "Searching the local metadata index..."
                : "Searching the folder in the background...");
            state_->activeGeneration = state_->usingIndex
                ? state_->metadataIndex.query(
                    state_->indexDatabase, state_->searchRoot, filters)
                : state_->search.start(state_->searchRoot, filters);
        }
    });
    connect(cancel, &QPushButton::clicked, this, [this] {
        state_->search.cancel();
        state_->metadataIndex.cancel();
    });
    connect(&state_->search, &search::SearchService::resultFound, this,
        [this](quint64 generation, const search::SearchHit& hit) {
            if (!state_->usingIndex && generation == state_->activeGeneration)
                insertRankedSearchHit(state_->results, hit);
        });
    connect(&state_->search, &search::SearchService::backendSelected, this,
        [this](quint64 generation, const QString& backend) {
            if (!state_->usingIndex && generation == state_->activeGeneration)
                state_->status->setText("Searching with " + backend + "...");
        });
    connect(&state_->search, &search::SearchService::errorOccurred, this,
        [this](quint64 generation, const QString& error) {
            if (!state_->usingIndex && generation == state_->activeGeneration)
                state_->searchError = error;
        });
    connect(&state_->metadataIndex, &search::MetadataIndex::resultFound, this,
        [this](quint64 generation, const search::SearchHit& hit) {
            if (state_->usingIndex && generation == state_->activeGeneration)
                insertRankedSearchHit(state_->results, hit);
        });
    connect(&state_->search, &search::SearchService::finished, this,
        [this](quint64 generation, bool cancelled, int count) {
            if (!state_->usingIndex && generation == state_->activeGeneration) {
                state_->status->setText(cancelled ? "Search cancelled."
                    : !state_->searchError.isEmpty() ? "Windows Search: " + state_->searchError
                    : localizedCount(count, "result", "results") + '.');
            }
        });
    connect(&state_->metadataIndex, &search::MetadataIndex::finished, this,
        [this](quint64 generation, bool cancelled, const QString& error, int count) {
            if (generation != state_->activeGeneration)
                return;
            if (!error.isEmpty())
                state_->status->setText("Index error: " + error);
            else if (cancelled)
                state_->status->setText("Index operation cancelled.");
            else
                state_->status->setText(QString("%1 indexed or returned.")
                    .arg(localizedItems(count)));
        });
    connect(&state_->metadataIndex, &search::MetadataIndex::indexingProgress, this,
        [this](quint64 generation, int count, const QString& path) {
            if (generation == state_->activeGeneration) {
                state_->status->setText(QString("Indexed %1: %2")
                    .arg(localizedItems(count), QDir::toNativeSeparators(path)));
            }
        });
    connect(state_->manageIndex, &QPushButton::clicked, this,
        [this, refreshIndexControls] {
            const bool enabled = state_->indexedRoots.contains(
                state_->searchRoot, Qt::CaseInsensitive);
            if (!enabled) {
                const QString explanation = QString(
                    "Index this folder using metadata only?\n\nStored locally: paths, names, types, "
                    "sizes, and modification times. File contents are not read or stored.\n\nDatabase: %1")
                    .arg(QDir::toNativeSeparators(state_->indexDatabase));
                if (QMessageBox::question(this, "Enable metadata index", explanation,
                        QMessageBox::Yes | QMessageBox::Cancel,
                        QMessageBox::Cancel) != QMessageBox::Yes)
                    return;
                state_->indexedRoots.append(state_->searchRoot);
                state_->indexSettings.setValue("search/indexedRoots", state_->indexedRoots);
                refreshIndexControls();
                state_->usingIndex = true;
                state_->status->setText("Building metadata-only index...");
                state_->activeGeneration = state_->metadataIndex.rebuild(
                    state_->indexDatabase, state_->searchRoot);
            } else {
                if (QMessageBox::question(this, "Remove folder index",
                        "Delete all indexed metadata for this folder?",
                        QMessageBox::Yes | QMessageBox::Cancel,
                        QMessageBox::Cancel) != QMessageBox::Yes)
                    return;
                state_->indexedRoots.removeAll(state_->searchRoot);
                state_->indexSettings.setValue("search/indexedRoots", state_->indexedRoots);
                refreshIndexControls();
                state_->usingIndex = true;
                state_->status->setText("Deleting indexed metadata...");
                state_->activeGeneration = state_->metadataIndex.removeRoot(
                    state_->indexDatabase, state_->searchRoot);
            }
        });

    connect(resultActions, &SearchResultBar::openRequested,
        this, &FileSearchDialog::openRequested);
    connect(state_->results, &QListWidget::itemDoubleClicked, this,
        [this](QListWidgetItem* item) {
            emit openRequested(item->data(Qt::UserRole).toString());
        });
    connect(resultActions, &SearchResultBar::revealRequested,
        this, &FileSearchDialog::revealRequested);
    connect(resultActions, &SearchResultBar::clipboardRequested,
        this, &FileSearchDialog::clipboardRequested);
    connect(resultActions, &SearchResultBar::shareRequested,
        this, &FileSearchDialog::shareRequested);
    connect(resultActions, &SearchResultBar::recycleRequested,
        this, &FileSearchDialog::recycleRequested);
    connect(this, &QDialog::finished, this, [this] {
        if (state_->activeGeneration != 0 && !state_->lastExecutedWasAqs)
            state_->history->record(state_->query->text());
    });
    state_->query->setFocus();
}

FileSearchDialog::~FileSearchDialog() = default;

bool FileSearchDialog::activateSavedSearch(const QString& id)
{
    if (!state_ || id.trimmed().isEmpty())
        return false;
    for (int index = 1; index < state_->savedSearches->count(); ++index) {
        if (state_->savedSearches->itemData(index).toString() == id) {
            state_->savedSearches->setCurrentIndex(index);
            return true;
        }
    }
    return false;
}

void FileSearchDialog::removeResults(const QStringList& paths)
{
    QSet<QString> removedKeys;
    for (const QString& path : paths)
        removedKeys.insert(QDir::toNativeSeparators(path).toCaseFolded());
    for (int row = state_->results->count() - 1; row >= 0; --row) {
        const QString path = state_->results->item(row)->data(Qt::UserRole).toString();
        if (removedKeys.contains(
                QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toCaseFolded())) {
            delete state_->results->takeItem(row);
        }
    }
}

} // namespace filemanager::ui
