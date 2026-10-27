#include "tvorganizerdialog.h"

#include "files/bookorganizer.h"
#include "files/filenamevalidator.h"
#include "files/reviewedtransfer.h"
#include "files/transferqueue.h"

#include <QAbstractTableModel>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHash>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QTableView>
#include <QThread>
#include <QVBoxLayout>

namespace filemanager::ui {
namespace {

std::atomic_int activeOrganizerScans{0};

struct ScanJob
{
    QStringList roots;
    QString destination;
    QList<files::BookCorrection> corrections;
    bool books = false;
    qsizetype limit = 100'000;
    std::atomic_bool cancelled{false};
    files::TvOrganizationPlan result;
};

files::TvOrganizationPlan displayBookPlan(files::BookOrganizationPlan book)
{
    files::TvOrganizationPlan result;
    result.destinationDirectory = std::move(book.destinationDirectory);
    result.destinationSnapshot = book.destinationSnapshot;
    for (auto& file : book.candidates) {
        result.candidates.append({std::move(file.source), std::move(file.destination),
            std::move(file.groupSource), file.sourceSnapshot, file.companion,
            std::move(file.evidence)});
    }
    for (auto& item : book.review)
        result.review.append({std::move(item.path), std::move(item.reason),
            item.sourceSnapshot});
    result.errors = std::move(book.errors);
    result.entriesVisited = book.entriesVisited;
    result.ignoredFiles = book.ignoredFiles;
    result.skippedBoundaries = book.skippedBoundaries;
    result.cancelled = book.cancelled;
    result.limitReached = book.limitReached;
    return result;
}

struct MoveReviewJob
{
    files::TransferRequest request;
    files::ReviewedMetadataReview result;
    std::atomic_bool cancelled{false};
};

class ReviewModel final : public QAbstractTableModel
{
public:
    explicit ReviewModel(QObject* parent) : QAbstractTableModel(parent) { plan.cancelled = true; }

    files::TvOrganizationPlan plan;
    bool books = false;
    QSet<QString> includedGroups;
    QHash<QString, int> groupSizes;
    int includedFiles = 0;

    void clear()
    {
        files::TvOrganizationPlan empty;
        empty.cancelled = true;
        replace(std::move(empty));
    }

    void replace(files::TvOrganizationPlan result)
    {
        beginResetModel();
        plan = std::move(result);
        includedGroups.clear();
        groupSizes.clear();
        includedFiles = 0;
        for (const auto& candidate : plan.candidates) {
            includedGroups.insert(candidate.episodeSource);
            ++groupSizes[candidate.episodeSource];
            ++includedFiles;
        }
        endResetModel();
    }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(plan.candidates.size()
            + plan.review.size() + plan.errors.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 4; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            const QStringList headings{"Source (include group)", "Proposed destination", "Status", "Reason"};
            return section >= 0 && section < headings.size() ? headings.at(section) : QVariant{};
        }
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        auto result = QAbstractTableModel::flags(index);
        if (index.isValid() && index.column() == 0 && index.row() < plan.candidates.size())
            result |= Qt::ItemIsUserCheckable;
        return result;
    }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
            return {};
        const bool candidateRow = index.row() < plan.candidates.size();
        if (role == Qt::CheckStateRole && index.column() == 0 && candidateRow)
            return includedGroups.contains(plan.candidates.at(index.row()).episodeSource)
                ? Qt::Checked : Qt::Unchecked;
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole && role != Qt::AccessibleTextRole)
            return {};
        if (candidateRow) {
            const auto& candidate = plan.candidates.at(index.row());
            switch (index.column()) {
            case 0: return QDir::toNativeSeparators(candidate.source);
            case 1: return QDir::toNativeSeparators(candidate.destination);
            case 2: return candidate.companion ? "Companion" : books ? "Book candidate" : "Episode candidate";
            case 3: return candidate.companion ? (books ? "Matched basename; included with its book."
                                                    : "Matched basename; included with its episode.")
                         : books ? candidate.evidence
                                 : "Filename evidence only; check the proposed series and season.";
            default: return {};
            }
        }
        const qsizetype reviewRow = index.row() - plan.candidates.size();
        if (reviewRow < plan.review.size()) {
            const auto& review = plan.review.at(reviewRow);
            switch (index.column()) {
            case 0: return QDir::toNativeSeparators(review.path);
            case 2: return "Needs review — excluded";
            case 3: return review.reason;
            default: return {};
            }
        }
        return index.column() == 2 ? QVariant("Scan error") : index.column() == 3
            ? QVariant(plan.errors.at(reviewRow - plan.review.size())) : QVariant{};
    }
    bool setData(const QModelIndex& index, const QVariant& value, int role) override
    {
        if (role != Qt::CheckStateRole || !index.isValid() || index.column() != 0
                || index.row() < 0 || index.row() >= plan.candidates.size())
            return false;
        const QString group = plan.candidates.at(index.row()).episodeSource;
        const bool include = value.toInt() == Qt::Checked;
        if (include != includedGroups.contains(group)) {
            includedFiles += include ? groupSizes.value(group) : -groupSizes.value(group);
            if (include)
                includedGroups.insert(group);
            else
                includedGroups.remove(group);
            emit dataChanged(this->index(0, 0), this->index(static_cast<int>(plan.candidates.size()) - 1, 0),
                {Qt::CheckStateRole});
        }
        return true;
    }
    void includeAll(bool include)
    {
        includedGroups.clear();
        includedFiles = include ? static_cast<int>(plan.candidates.size()) : 0;
        if (include) {
            for (auto it = groupSizes.cbegin(); it != groupSizes.cend(); ++it)
                includedGroups.insert(it.key());
        }
        if (!plan.candidates.isEmpty())
            emit dataChanged(index(0, 0), index(static_cast<int>(plan.candidates.size()) - 1, 0),
                {Qt::CheckStateRole});
    }
};

} // namespace

class TvOrganizerDialog::State
{
public:
    QString initialDirectory;
    QComboBox* kind = nullptr;
    QListWidget* roots = nullptr;
    QLineEdit* destination = nullptr;
    QSpinBox* limit = nullptr;
    QLabel* status = nullptr;
    QPushButton* add = nullptr;
    QPushButton* remove = nullptr;
    QPushButton* choose = nullptr;
    QPushButton* scan = nullptr;
    QPushButton* cancel = nullptr;
    QPushButton* include = nullptr;
    QPushButton* exclude = nullptr;
    QPushButton* assign = nullptr;
    QPushButton* move = nullptr;
    ReviewModel* model = nullptr;
    QTableView* table = nullptr;
    QPlainTextEdit* details = nullptr;
    std::shared_ptr<ScanJob> running;
    std::shared_ptr<MoveReviewJob> reviewing;
    QList<files::BookCorrection> corrections;
    files::TransferQueue* transferQueue = nullptr;
    quint64 queuedJobId = 0;
    bool closed = false;
};

TvOrganizerDialog::TvOrganizerDialog(const QString& initialDirectory, QWidget* parent,
    files::TransferQueue* transferQueue)
    : QDialog(parent), state_(std::make_unique<State>())
{
    state_->initialDirectory = initialDirectory;
    state_->transferQueue = transferQueue;
    setObjectName("TvOrganizerDialog");
    setWindowTitle(transferQueue ? "Media Organizer" : "Media Organizer — preview only");
    resize(1020, 660);
    auto* layout = new QVBoxLayout(this);
    auto* disclosure = new QLabel(transferQueue
        ? "TV shows or book series: one-time scan of folders you choose. Uses filenames and, "
          "for books, bounded EPUB package metadata from selected storage. A NAS or mapped drive may "
          "be read if you choose it. No online metadata lookup or saved scan history. "
          "Files move only after a final batch-specific confirmation."
        : "TV shows or book series: one-time scan of chosen folders using filenames and, "
          "for books, bounded EPUB package metadata. Chosen network storage may be read. "
          "This standalone dialog has no transfer queue.", this);
    disclosure->setObjectName("TvOrganizerDisclosure");
    disclosure->setWordWrap(true);
    layout->addWidget(disclosure);
    auto* kindRow = new QHBoxLayout;
    auto* kindLabel = new QLabel("&Organize:", this);
    state_->kind = new QComboBox(this);
    state_->kind->setObjectName("MediaOrganizerKind");
    state_->kind->setAccessibleName("Media type to organize");
    state_->kind->addItems({"TV shows", "Book series"});
    kindLabel->setBuddy(state_->kind);
    kindRow->addWidget(kindLabel);
    kindRow->addWidget(state_->kind);
    kindRow->addStretch();
    layout->addLayout(kindRow);
    layout->addWidget(new QLabel("Folders to scan once:", this));

    auto* scopeRow = new QHBoxLayout;
    state_->roots = new QListWidget(this);
    state_->roots->setObjectName("TvOrganizerRoots");
    state_->roots->setAccessibleName("Folders to scan once");
    state_->roots->setSelectionMode(QAbstractItemView::ExtendedSelection);
    state_->roots->setMaximumHeight(64);
    scopeRow->addWidget(state_->roots, 1);
    auto* scopeButtons = new QVBoxLayout;
    state_->add = new QPushButton("&Add folder…", this);
    state_->add->setObjectName("TvOrganizerAdd");
    state_->remove = new QPushButton("&Remove selected", this);
    state_->remove->setObjectName("TvOrganizerRemove");
    scopeButtons->addWidget(state_->add);
    scopeButtons->addWidget(state_->remove);
    scopeButtons->addStretch();
    scopeRow->addLayout(scopeButtons);
    layout->addLayout(scopeRow);

    auto* destinationRow = new QHBoxLayout;
    state_->destination = new QLineEdit(this);
    state_->destination->setReadOnly(true);
    state_->destination->setObjectName("TvOrganizerDestination");
    state_->destination->setAccessibleName("Chosen Shows destination folder");
    state_->destination->setPlaceholderText("Choose the existing Shows folder itself");
    state_->choose = new QPushButton("Choose &Shows folder…", this);
    state_->choose->setObjectName("TvOrganizerChooseDestination");
    destinationRow->addWidget(state_->destination, 1);
    destinationRow->addWidget(state_->choose);
    layout->addLayout(destinationRow);
    auto* scanRow = new QHBoxLayout;
    auto* limitLabel = new QLabel("&Entry limit:", this);
    state_->limit = new QSpinBox(this);
    state_->limit->setObjectName("TvOrganizerLimit");
    state_->limit->setRange(1, 1'000'000);
    state_->limit->setValue(100'000);
    state_->limit->setGroupSeparatorShown(true);
    state_->limit->setToolTip("Counts files and folders, including ignored formats. A truncated scan has no proposals.");
    limitLabel->setBuddy(state_->limit);
    state_->scan = new QPushButton("S&can folders", this);
    state_->scan->setObjectName("TvOrganizerScan");
    state_->scan->setAutoDefault(false);
    state_->cancel = new QPushButton("S&top scan", this);
    state_->cancel->setObjectName("TvOrganizerCancel");
    scanRow->addWidget(limitLabel);
    scanRow->addWidget(state_->limit);
    scanRow->addStretch();
    scanRow->addWidget(state_->scan);
    scanRow->addWidget(state_->cancel);
    layout->addLayout(scanRow);

    auto* table = new QTableView(this);
    state_->table = table;
    table->setObjectName("TvOrganizerReview");
    table->setAccessibleName("Episode destinations and excluded files; Space toggles an episode group");
    state_->model = new ReviewModel(this);
    table->setModel(state_->model);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->setAlternatingRowColors(true);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideMiddle);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    table->setColumnWidth(0, 260);
    table->setColumnWidth(1, 300);
    table->setColumnWidth(2, 185);
    table->horizontalHeader()->setMinimumSectionSize(100);
    layout->addWidget(table, 1);
    state_->details = new QPlainTextEdit(this);
    state_->details->setObjectName("TvOrganizerSelectedMapping");
    state_->details->setAccessibleName("Selected organizer mapping, full source and destination paths");
    state_->details->setReadOnly(true);
    state_->details->setMaximumHeight(80);
    state_->details->setPlaceholderText("Select a row to read and copy its full paths and reason.");
    layout->addWidget(state_->details);
    connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
        [this](const QModelIndex& current) {
            if (!current.isValid()) {
                state_->details->clear();
                return;
            }
            const auto value = [this, &current](int column) {
                return state_->model->index(current.row(), column).data(Qt::ToolTipRole).toString();
            };
            state_->details->setPlainText("Source: " + value(0) + "\nProposed destination: " + value(1)
                + "\n" + value(2) + ": " + value(3));
        });
    connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, this,
        [this] { updateControls(); });
    connect(state_->model, &QAbstractItemModel::modelReset, state_->details, &QPlainTextEdit::clear);
    auto* selectionRow = new QHBoxLayout;
    state_->include = new QPushButton("&Include all candidates", this);
    state_->include->setObjectName("TvOrganizerIncludeAll");
    state_->exclude = new QPushButton("&Exclude all candidates", this);
    state_->exclude->setObjectName("TvOrganizerExcludeAll");
    state_->assign = new QPushButton("Assign selected files to &book…", this);
    state_->assign->setObjectName("MediaOrganizerAssignBook");
    state_->assign->setAccessibleName("Assign selected reviewed files to a book for this scan only");
    selectionRow->addWidget(state_->include);
    selectionRow->addWidget(state_->exclude);
    selectionRow->addWidget(state_->assign);
    selectionRow->addStretch();
    layout->addLayout(selectionRow);
    state_->status = new QLabel("Choose scan folders and a Shows destination, then choose Scan folders.", this);
    state_->status->setObjectName("TvOrganizerStatus");
    state_->status->setWordWrap(true);
    state_->status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(state_->status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Help | QDialogButtonBox::Close, this);
    state_->move = buttons->addButton("&Move selected…", QDialogButtonBox::AcceptRole);
    state_->move->setObjectName("TvOrganizerMove");
    state_->move->setAccessibleName("Review and move selected TV show groups");
    layout->addWidget(buttons);

    connect(state_->add, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getExistingDirectory(this, "Choose a folder to scan once", state_->initialDirectory);
        if (chosen.isEmpty())
            return;
        const QString display = QDir::toNativeSeparators(chosen);
        for (int row = 0; row < state_->roots->count(); ++row) {
            if (state_->roots->item(row)->text().compare(display, Qt::CaseInsensitive) == 0)
                return;
        }
        state_->roots->addItem(display);
        invalidateReview();
    });
    connect(state_->remove, &QPushButton::clicked, this, [this] {
        for (auto* item : state_->roots->selectedItems())
            delete item;
        invalidateReview();
    });
    connect(state_->choose, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getExistingDirectory(this,
            state_->kind->currentIndex() == 1 ? "Choose the existing Books folder"
                                              : "Choose the existing Shows folder", state_->initialDirectory);
        if (!chosen.isEmpty())
            state_->destination->setText(QDir::toNativeSeparators(chosen));
    });
    connect(state_->roots, &QListWidget::itemSelectionChanged, this, [this] { updateControls(); });
    connect(state_->kind, &QComboBox::currentIndexChanged, this, [this](int index) {
        const bool books = index == 1;
        state_->model->books = books;
        state_->destination->setAccessibleName(books ? "Chosen Books destination folder"
            : "Chosen Shows destination folder");
        state_->destination->setPlaceholderText(books ? "Choose the existing Books folder itself"
            : "Choose the existing Shows folder itself");
        state_->choose->setText(books ? "Choose &Books folder…" : "Choose &Shows folder…");
        state_->table->setAccessibleName(books
            ? "Book destinations and excluded files; Space toggles a book group"
            : "Episode destinations and excluded files; Space toggles an episode group");
        state_->move->setAccessibleName(books ? "Review and move selected book groups"
            : "Review and move selected TV show groups");
        state_->destination->clear();
        invalidateReview();
    });
    connect(state_->destination, &QLineEdit::textChanged, this, [this] { invalidateReview(); });
    connect(state_->limit, &QSpinBox::valueChanged, this, [this] { invalidateReview(); });
    connect(state_->scan, &QPushButton::clicked, this, [this] { startScan(); });
    connect(state_->cancel, &QPushButton::clicked, this, [this] { cancelScan(); });
    connect(state_->move, &QPushButton::clicked, this, [this] { startMoveReview(); });
    connect(state_->include, &QPushButton::clicked, this, [this] { state_->model->includeAll(true); });
    connect(state_->exclude, &QPushButton::clicked, this, [this] { state_->model->includeAll(false); });
    connect(state_->assign, &QPushButton::clicked, this, [this] {
        struct SelectedFile { QString source; files::SourceSnapshot snapshot; };
        QList<SelectedFile> selected;
        for (const QModelIndex& row : state_->table->selectionModel()->selectedRows()) {
            if (row.row() < state_->model->plan.candidates.size()) {
                const auto& candidate = state_->model->plan.candidates.at(row.row());
                if (candidate.sourceSnapshot.valid)
                    selected.append({candidate.source, candidate.sourceSnapshot});
            } else {
                const qsizetype index = row.row() - state_->model->plan.candidates.size();
                if (index < state_->model->plan.review.size()) {
                    const auto& item = state_->model->plan.review.at(index);
                    if (item.sourceSnapshot.valid)
                        selected.append({item.path, item.sourceSnapshot});
                }
            }
        }
        if (selected.isEmpty() || !state_->model->plan.complete())
            return;
        QDialog correction(this);
        correction.setWindowTitle("Assign selected files to one book");
        auto* form = new QFormLayout(&correction);
        auto* explanation = new QLabel(QString("Assign %1 selected files to the same book. "
            "This assignment lasts only for this review; the new preview must still be confirmed.")
            .arg(selected.size()), &correction);
        explanation->setWordWrap(true);
        form->addRow(explanation);
        auto* series = new QLineEdit(&correction);
        series->setObjectName("MediaOrganizerCorrectionSeries");
        auto* number = new QSpinBox(&correction);
        number->setObjectName("MediaOrganizerCorrectionNumber");
        number->setRange(1, 999);
        auto* title = new QLineEdit(&correction);
        title->setObjectName("MediaOrganizerCorrectionTitle");
        form->addRow("&Series:", series);
        form->addRow("Book &number:", number);
        form->addRow("Book &title:", title);
        auto* actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
            &correction);
        form->addRow(actions);
        connect(actions, &QDialogButtonBox::accepted, &correction, &QDialog::accept);
        connect(actions, &QDialogButtonBox::rejected, &correction, &QDialog::reject);
        if (correction.exec() != QDialog::Accepted)
            return;
        files::BookSeriesName name{series->text().simplified(), number->value(),
            title->text().simplified()};
        if (!files::validateWindowsName(name.series).valid
                || !files::validateWindowsName(name.title).valid
                || name.series.contains(" - ") || name.title.contains(" - ")) {
            QMessageBox::warning(this, "Invalid book assignment",
                "Enter safe, nonempty Windows folder names for series and title. "
                "The separator ' - ' is reserved for the numbered book folder.");
            return;
        }
        for (const auto& file : selected) {
            bool replaced = false;
            for (auto& prior : state_->corrections) {
                if (prior.source.compare(file.source, Qt::CaseInsensitive) == 0) {
                    prior = {file.source, file.snapshot, name};
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                state_->corrections.append({file.source, file.snapshot, name});
        }
        state_->model->clear();
        startScan();
    });
    connect(state_->model, &QAbstractItemModel::dataChanged, this, [this] { updateSummary(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::helpRequested, this, [this] { showTvOrganizerHelp(this); });
    updateControls();
    state_->add->setFocus();
}

TvOrganizerDialog::~TvOrganizerDialog()
{
    if (state_->running)
        state_->running->cancelled.store(true);
    if (state_->reviewing)
        state_->reviewing->cancelled.store(true);
}

void TvOrganizerDialog::setScope(const QStringList& roots, const QString& showsDirectory)
{
    if (state_->running || state_->closed)
        return;
    state_->roots->clear();
    for (const QString& root : roots)
        state_->roots->addItem(QDir::toNativeSeparators(root));
    state_->destination->setText(QDir::toNativeSeparators(showsDirectory));
    invalidateReview();
}

bool TvOrganizerDialog::scanPending() const { return bool(state_->running); }
bool TvOrganizerDialog::reviewPending() const { return bool(state_->reviewing); }
quint64 TvOrganizerDialog::queuedJobId() const { return state_->queuedJobId; }
const files::TvOrganizationPlan& TvOrganizerDialog::reviewedPlan() const { return state_->model->plan; }

QList<files::TvMoveCandidate> TvOrganizerDialog::selectedCandidates() const
{
    QList<files::TvMoveCandidate> selected;
    if (state_->running || state_->reviewing || state_->closed || !state_->model->plan.complete())
        return selected;
    for (const auto& candidate : state_->model->plan.candidates) {
        if (state_->model->includedGroups.contains(candidate.episodeSource))
            selected.append(candidate);
    }
    return selected;
}

void TvOrganizerDialog::invalidateReview()
{
    if (state_->running)
        state_->running->cancelled.store(true);
    state_->corrections.clear();
    state_->model->clear();
    state_->status->setText("Scan options changed. Choose Scan folders to prepare a new preview.");
    updateControls();
}

void TvOrganizerDialog::updateControls()
{
    const bool idle = !state_->running && !state_->reviewing && !state_->closed;
    state_->kind->setEnabled(idle);
    state_->add->setEnabled(idle);
    state_->remove->setEnabled(idle && !state_->roots->selectedItems().isEmpty());
    state_->choose->setEnabled(idle);
    state_->limit->setEnabled(idle);
    state_->scan->setEnabled(idle && state_->roots->count() > 0 && !state_->destination->text().isEmpty());
    state_->cancel->setEnabled(!idle && !state_->closed && state_->running
        && !state_->running->cancelled.load());
    const bool candidates = idle && !state_->model->plan.candidates.isEmpty();
    state_->include->setEnabled(candidates);
    state_->exclude->setEnabled(candidates);
    bool assignable = false;
    if (idle && state_->kind->currentIndex() == 1 && state_->model->plan.complete()) {
        for (const QModelIndex& row : state_->table->selectionModel()->selectedRows()) {
            if (row.row() < state_->model->plan.candidates.size()) {
                assignable = assignable || state_->model->plan.candidates.at(row.row()).sourceSnapshot.valid;
            } else {
                const qsizetype index = row.row() - state_->model->plan.candidates.size();
                assignable = assignable || (index < state_->model->plan.review.size()
                    && state_->model->plan.review.at(index).sourceSnapshot.valid);
            }
        }
    }
    state_->assign->setEnabled(assignable);
    state_->table->setEnabled(idle);
    state_->move->setEnabled(candidates && state_->transferQueue
        && state_->model->includedFiles > 0 && state_->model->plan.complete());
}

void TvOrganizerDialog::startMoveReview()
{
    if (!state_->transferQueue || state_->running || state_->reviewing || state_->closed
            || !state_->model->plan.complete() || state_->model->includedFiles <= 0)
        return;
    QStringList includedEpisodes;
    for (const auto& candidate : state_->model->plan.candidates) {
        if (!candidate.companion && state_->model->includedGroups.contains(candidate.episodeSource))
            includedEpisodes.append(candidate.episodeSource);
    }
    const auto reviewed = files::createReviewedTvMove(state_->model->plan, includedEpisodes);
    if (!reviewed.valid()) {
        state_->status->setText("The selected review is no longer valid. Scan and review again. "
            + reviewed.errors.join(' '));
        updateControls();
        return;
    }
    auto job = std::make_shared<MoveReviewJob>();
    job->request = reviewed.request;
    state_->reviewing = job;
    state_->status->setText("Rechecking every selected source, destination and metadata capability… "
        "No files are changed during this review.");
    updateControls();
    auto* worker = QThread::create([job] {
        job->result = files::reviewReviewedMoveMetadata(job->request, &job->cancelled);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, job] {
        state_->reviewing.reset();
        if (state_->closed)
            return;
        if (job->result.cancelled) {
            state_->status->setText("Move review cancelled. No files were changed.");
            updateControls();
            return;
        }
        if (!job->result.valid()) {
            state_->status->setText("Move review failed. No files were changed; scan and review again.");
            QMessageBox::warning(this, "Media Organizer review failed",
                job->result.errors.join('\n'));
            updateControls();
            return;
        }

        QStringList mappings;
        for (const auto& move : job->request.reviewedMoves) {
            mappings.append(QDir::toNativeSeparators(move.source) + "\n  → "
                + QDir::toNativeSeparators(move.destination));
        }
        QStringList degradations;
        for (const auto& item : job->result.items) {
            for (const QString& detail : item.degradations)
                degradations.append(QDir::toNativeSeparators(item.source) + ": " + detail);
        }
        QMessageBox confirmation(this);
        confirmation.setObjectName("TvOrganizerConfirmation");
        confirmation.setAccessibleName("Final Media Organizer move confirmation");
        confirmation.setIcon(degradations.isEmpty() ? QMessageBox::Question : QMessageBox::Warning);
        confirmation.setWindowTitle(state_->kind->currentIndex() == 1
            ? "Confirm book-series moves" : "Confirm TV show moves");
        confirmation.setText(QString("Move %1 selected files into:\n%2")
            .arg(job->request.reviewedMoves.size())
            .arg(QDir::toNativeSeparators(job->request.destinationDirectory)));
        QString explanation = "Review the complete mappings below. This is not one atomic undoable batch. "
            "If you stop it or a file fails, completed moves remain and Operation Center reports the exact result.";
        if (!degradations.isEmpty()) {
            explanation += "\n\nThe destination cannot preserve all listed metadata. Continuing explicitly accepts "
                "that loss for this batch only.";
        }
        confirmation.setInformativeText(explanation);
        confirmation.setDetailedText(mappings.join("\n\n")
            + (degradations.isEmpty() ? QString{} : "\n\nMETADATA THAT MAY BE LOST\n" + degradations.join('\n')));
        auto* moveButton = confirmation.addButton(
            QString("Move %1 files").arg(job->request.reviewedMoves.size()), QMessageBox::AcceptRole);
        auto* cancelButton = confirmation.addButton(QMessageBox::Cancel);
        confirmation.setDefaultButton(qobject_cast<QPushButton*>(cancelButton));
        confirmation.exec();
        if (confirmation.clickedButton() != moveButton) {
            state_->status->setText("Move cancelled before queueing. No files were changed.");
            updateControls();
            return;
        }
        job->request.reviewedMetadataDegradationApproved = !degradations.isEmpty();
        job->request.reviewedApprovedMetadataWarnings = degradations;
        state_->queuedJobId = state_->transferQueue->enqueue(std::move(job->request));
        state_->status->setText(QString("Move queued as operation %1. Progress and cancellation are in Operation Center.")
            .arg(state_->queuedJobId));
        QDialog::done(QDialog::Accepted);
    });
    worker->start();
}

void TvOrganizerDialog::startScan()
{
    if (state_->running || state_->closed)
        return;
    int active = activeOrganizerScans.load();
    do {
        if (active >= 2) {
            state_->status->setText("Earlier scans are still stopping. Try Scan folders again when they finish.");
            return;
        }
    } while (!activeOrganizerScans.compare_exchange_weak(active, active + 1));
    auto job = std::make_shared<ScanJob>();
    for (int row = 0; row < state_->roots->count(); ++row)
        job->roots.append(QDir::fromNativeSeparators(state_->roots->item(row)->text()));
    job->destination = QDir::fromNativeSeparators(state_->destination->text());
    job->corrections = state_->corrections;
    job->books = state_->kind->currentIndex() == 1;
    job->limit = state_->limit->value();
    state_->model->clear();
    state_->running = job;
    state_->status->setText("Scanning chosen folders in the background… No files are changed. "
        "Stop scan or Close requests cancellation; an in-progress Windows filesystem call may finish first.");
    updateControls();
    auto* worker = QThread::create([job] {
        job->result = job->books
            ? displayBookPlan(files::planBookOrganization(
                job->roots, job->destination, &job->cancelled, job->limit, job->corrections))
            : files::planTvOrganization(job->roots, job->destination, &job->cancelled, job->limit);
        --activeOrganizerScans;
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, job] {
        state_->running.reset();
        if (state_->closed)
            return;
        if (job->cancelled.load()) {
            state_->model->clear();
            state_->status->setText("Scan cancelled. No files were changed; choose Scan folders to try again.");
        } else {
            state_->model->replace(std::move(job->result));
            if (state_->model->rowCount() > 0)
                state_->table->setCurrentIndex(state_->model->index(0, 0));
            updateSummary();
        }
        updateControls();
    });
    worker->start();
}

void TvOrganizerDialog::cancelScan()
{
    if (!state_->running)
        return;
    state_->running->cancelled.store(true);
    state_->model->clear();
    state_->status->setText("Cancellation requested. No files changed. You can close this window now.");
    updateControls();
}

void TvOrganizerDialog::updateSummary()
{
    const auto& plan = state_->model->plan;
    QString summary = plan.limitReached ? "Entry limit reached: no proposals. Narrow the scope or raise the limit. "
        : !plan.errors.isEmpty() ? "Scan could not complete. Review the errors below. "
                                : "Preview ready — filenames are evidence, not proof. ";
    const QLocale locale;
    summary += QString("%1 entries visited; %2 files included; %3 need review; %4 unrelated files ignored; "
        "%5 reparse/cloud/unavailable boundaries skipped. Readable regular files only; not a complete-PC inventory. "
        "%6")
        .arg(locale.toString(plan.entriesVisited), locale.toString(state_->model->includedFiles),
            locale.toString(plan.review.size()), locale.toString(plan.ignoredFiles),
            locale.toString(plan.skippedBoundaries), state_->transferQueue
                ? "Choose Move selected to recheck and confirm this batch."
                : "No transfer queue is attached to this dialog.");
    state_->status->setText(summary);
    updateControls();
}

void TvOrganizerDialog::done(int result)
{
    if (result == QDialog::Accepted && state_->queuedJobId == 0)
        return; // Only the explicit reviewed Move button can accept this dialog.
    state_->closed = true;
    cancelScan();
    if (state_->reviewing)
        state_->reviewing->cancelled.store(true);
    state_->model->clear();
    QDialog::done(result);
}

void showTvOrganizerHelp(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setObjectName("TvOrganizerHelpDialog");
    dialog.setWindowTitle("Media Organizer — review and move");
    dialog.resize(700, 560);
    auto* layout = new QVBoxLayout(&dialog);
    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* text = new QLabel(
        "Open File > Media Organizer and choose TV shows or Book series. There is no default keyboard shortcut; "
        "you can assign one in shortcut settings.\n\n"
        "1. Add only folders you want scanned. Choose the existing Shows or Books folder itself as the destination. "
        "Opening the window does not scan anything.\n\n"
        "2. Choose Scan folders. This is a one-time background scan of filenames and filesystem identity "
        "in the chosen folders. Book mode may also read bounded container and package metadata inside EPUB "
        "files. A chosen NAS or mapped drive may be read; there is no online metadata lookup and book chapters "
        "are not read. The destination subtree is excluded. "
        "The entry limit counts files and folders; a truncated scan gives no proposals.\n\n"
        "3. Review every source and proposed destination. A filename such as Series.S01E02.mkv suggests "
        "Shows/Series/Season 01/Series.S01E02.mkv. The 1x02 form is also recognized. "
        "Episode names stay unchanged. Filename evidence can be wrong.\n\n"
        "For books, a strict Series - 01 - Book Title filename proposes "
        "Books/Series/01 - Book Title/original filename. Matching EPUB, PDF, audiobook and exact-basename "
        "cover artwork go into the same book folder. EPUB 3 package metadata can supply an explicit "
        "series and integer position. Conflicting filename and EPUB evidence stays excluded. "
        "Select one or more rows and choose Assign selected files to book to enter series, number and title; "
        "this correction is in memory for this review only and creates a new preview. "
        "Current title/author metadata alone does not identify a series or number. "
        "No online lookup or automatic move occurs.\n\n"
        "4. Check or uncheck a group using the Source checkbox (or Space on that cell). "
        "Matched companions follow their group's inclusion state. Ambiguous names, duplicate "
        "episodes or book claims, unmatched companions and destination conflicts remain excluded for review. "
        "Hover a cell for its full path or reason.\n\n"
        "5. Choose Move selected. File Manager rechecks every retained source, destination and filesystem "
        "metadata capability without changing files, then shows one final confirmation with the full mappings. "
        "If the destination cannot preserve metadata, the confirmation lists the loss and consent applies only "
        "to that batch. Cancel queues nothing.\n\n"
        "6. Confirmed work appears in Operation Center, where progress and cancellation remain available. "
        "The batch is not one atomic undo: if it is stopped or a file fails, completed moves remain and exact "
        "partial results are recorded. Restart never replays an organizer review; scan and confirm again.\n\n"
        "Stop scan and Close request cancellation without changing files. A blocked Windows filesystem "
        "call may finish after the window closes. Reparse/cloud boundaries are skipped; unreadable files "
        "and concurrent filesystem changes mean this is not a guaranteed complete inventory. "
        "File Manager does not save scan paths or results, and no Kai UI permissions are granted.", scroll);
    text->setObjectName("TvOrganizerHelpContent");
    text->setAccessibleName("Media Organizer instructions and safety limits");
    text->setWordWrap(true);
    text->setMinimumWidth(0);
    text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    text->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    text->setTextFormat(Qt::PlainText);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    text->setMargin(12);
    scroll->setWidget(text);
    layout->addWidget(scroll);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

} // namespace filemanager::ui
