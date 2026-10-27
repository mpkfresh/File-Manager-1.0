#include "tagdestinationdialog.h"

#include "tags/tagcatalog.h"
#include "tags/tagmovereview.h"
#include "tags/tagmoveprepare.h"
#include "files/reviewedtransfer.h"
#include "files/transferqueue.h"

#include <QComboBox>
#include <QDir>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <utility>

namespace filemanager::ui {
namespace {

QString stateDescription(const tags::TagMoveItem& item)
{
    using tags::TagMoveState;
    switch (item.state) {
    case TagMoveState::Proposed: return QStringLiteral("Ready for review");
    case TagMoveState::NoTags: return QStringLiteral("No tags");
    case TagMoveState::NoMatchingRule: return QStringLiteral("No matching rule");
    case TagMoveState::MultipleDestinations: return QStringLiteral("Choose a rule");
    case TagMoveState::SourceUnavailable: return QStringLiteral("Source unavailable or changed");
    case TagMoveState::DestinationUnavailable: return QStringLiteral("Destination unavailable");
    case TagMoveState::DestinationOccupied: return QStringLiteral("Destination occupied");
    case TagMoveState::DuplicateDestination: return QStringLiteral("Duplicate target; exclude one");
    case TagMoveState::InvalidRules: return QStringLiteral("Invalid rules");
    }
    return QStringLiteral("Needs review");
}

bool reviewable(tags::TagMoveState state)
{
    return state == tags::TagMoveState::Proposed
        || state == tags::TagMoveState::MultipleDestinations
        || state == tags::TagMoveState::DuplicateDestination;
}

QSet<QString> matchKeys(const tags::TagMoveItem& item)
{
    QSet<QString> keys;
    for (const auto& match : item.matches) {
        keys.insert(match.ruleId + QChar(u'\n')
            + QDir::cleanPath(match.path).toCaseFolded());
    }
    return keys;
}

int moveCount(const tags::PreparedTagMoves& prepared)
{
    int count = 0;
    for (const auto& request : prepared.requests)
        count += request.reviewedMoves.size();
    return count;
}

} // namespace

struct TagDestinationDialog::Job
{
    enum class Action { Preview, Resolve } action = Action::Preview;
    QStringList paths;
    QString storagePath;
    tags::TagMovePlan input;
    QHash<QString, QString> choices;
    std::atomic_bool cancelled{false};
    tags::TagMovePlan output;
    tags::PreparedTagMoves prepared;
    QStringList metadataWarnings;
    QString error;
};

TagDestinationDialog::TagDestinationDialog(QStringList selectedPaths, QWidget* parent,
    QString storagePath, files::TransferQueue* transferQueue)
    : QDialog(parent), selectedPaths_(std::move(selectedPaths)),
      storagePath_(std::move(storagePath)), transferQueue_(transferQueue)
{
    setObjectName("TagDestinationDialog");
    setWindowTitle("Review tag destinations");
    resize(1050, 620);
    auto* layout = new QVBoxLayout(this);
    auto* disclosure = new QLabel(
        "Preview destinations for explicitly selected files. Choose a matching rule when "
        "folders disagree, or exclude a file. Review the mappings, then confirm "
        "a guarded move separately. Nothing moves during preview.", this);
    disclosure->setObjectName("TagDestinationDisclosure");
    disclosure->setWordWrap(true);
    layout->addWidget(disclosure);
    table_ = new QTableWidget(0, 4, this);
    table_->setObjectName("TagDestinationTable");
    table_->setAccessibleName("Selected-file tag destination preview");
    table_->setHorizontalHeaderLabels({"Include", "Source", "Destination or rule choice", "Status"});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table_, 1);
    result_ = new QPlainTextEdit(this);
    result_->setObjectName("TagDestinationResult");
    result_->setAccessibleName("Resolved destination mappings and skipped files");
    result_->setReadOnly(true);
    result_->setMinimumHeight(125);
    result_->setPlainText("A resolved review will list exact mappings, skipped files and metadata limits here.");
    layout->addWidget(result_);
    status_ = new QLabel(this);
    status_->setObjectName("TagDestinationStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* actions = new QHBoxLayout;
    refresh_ = new QPushButton("Refresh preview", this);
    refresh_->setObjectName("TagDestinationRefresh");
    resolve_ = new QPushButton("Review included destinations", this);
    resolve_->setObjectName("TagDestinationResolve");
    move_ = new QPushButton("Confirm and queue moves…", this);
    move_->setObjectName("TagDestinationMove");
    auto* close = new QPushButton("Close", this);
    close->setObjectName("TagDestinationClose");
    actions->addWidget(refresh_);
    actions->addWidget(resolve_);
    actions->addWidget(move_);
    actions->addStretch();
    actions->addWidget(close);
    layout->addLayout(actions);
    connect(refresh_, &QPushButton::clicked, this, [this] { startPreview(); });
    connect(resolve_, &QPushButton::clicked, this, [this] { resolveSelection(); });
    connect(move_, &QPushButton::clicked, this, [this] { confirmMove(); });
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item && item->column() == 0)
            invalidateReview();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    setBusy(false);
    QTimer::singleShot(0, this, [this] { startPreview(); });
}

TagDestinationDialog::~TagDestinationDialog()
{
    if (running_)
        running_->cancelled.store(true);
}

void TagDestinationDialog::setBusy(bool busy)
{
    refresh_->setEnabled(!busy);
    resolve_->setEnabled(!busy && preview_.complete() && !preview_.items.isEmpty());
    move_->setEnabled(!busy && transferQueue_ && reviewed_
        && moveCount(reviewed_->prepared) > 0);
    table_->setEnabled(!busy);
}

void TagDestinationDialog::startPreview()
{
    if (running_)
        return;
    preview_ = {};
    reviewed_.reset();
    table_->setRowCount(0);
    result_->clear();
    if (selectedPaths_.isEmpty() || selectedPaths_.size() > 4096) {
        status_->setText("Select 1 to 4,096 individual files for this preview.");
        setBusy(false);
        return;
    }
    auto job = std::make_shared<Job>();
    job->paths = selectedPaths_;
    job->storagePath = storagePath_;
    running_ = job;
    status_->setText(QString("Checking %1 selected files and saved rules…")
        .arg(selectedPaths_.size()));
    setBusy(true);
    auto* worker = QThread::create([job] {
        tags::TagCatalog catalog(job->storagePath);
        if (!catalog.load(&job->error))
            return;
        job->output = tags::planSelectedTagMoves(job->paths, catalog, &job->cancelled);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, job] {
        running_.reset();
        if (!job->error.isEmpty() || !job->output.complete()) {
            status_->setText(job->error.isEmpty()
                ? (job->output.cancelled ? QStringLiteral("Preview cancelled.")
                    : job->output.errors.join('\n')) : job->error);
        } else {
            preview_ = std::move(job->output);
            showPreview();
        }
        setBusy(false);
    });
    worker->start();
}

void TagDestinationDialog::showPreview()
{
    const QSignalBlocker block(table_);
    table_->setUpdatesEnabled(false);
    table_->setRowCount(preview_.items.size());
    int ready = 0;
    for (int row = 0; row < preview_.items.size(); ++row) {
        const tags::TagMoveItem& item = preview_.items.at(row);
        auto* include = new QTableWidgetItem;
        include->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        include->setCheckState(item.state == tags::TagMoveState::Proposed
            ? Qt::Checked : Qt::Unchecked);
        if (!reviewable(item.state))
            include->setFlags(Qt::ItemIsEnabled);
        table_->setItem(row, 0, include);
        table_->setItem(row, 1, new QTableWidgetItem(QDir::toNativeSeparators(item.source)));
        if (item.state == tags::TagMoveState::MultipleDestinations) {
            auto* choices = new QComboBox(table_);
            choices->setAccessibleName(QString("Destination rule for %1")
                .arg(QDir::toNativeSeparators(item.source)));
            choices->addItem("Choose a destination…", QString{});
            for (const tags::RuleDestination& match : item.matches) {
                choices->addItem(QDir::toNativeSeparators(match.path)
                    + " — " + match.ruleId, match.ruleId);
            }
            connect(choices, &QComboBox::currentIndexChanged, this,
                [this] { invalidateReview(); });
            table_->setCellWidget(row, 2, choices);
        } else {
            table_->setItem(row, 2, new QTableWidgetItem(
                QDir::toNativeSeparators(item.destination)));
        }
        auto* state = new QTableWidgetItem(stateDescription(item));
        state->setToolTip(item.reason);
        table_->setItem(row, 3, state);
        if (item.state == tags::TagMoveState::Proposed)
            ++ready;
    }
    table_->setUpdatesEnabled(true);
    status_->setText(QString("Previewed %1 files; %2 have one proposed destination. "
        "Check collisions or choose among conflicting rules, then review the included set. "
        "No files were moved.").arg(preview_.items.size()).arg(ready));
}

void TagDestinationDialog::resolveSelection()
{
    if (running_ || !preview_.complete())
        return;
    reviewed_.reset();
    setBusy(false);
    auto job = std::make_shared<Job>();
    job->action = Job::Action::Resolve;
    job->input = preview_;
    for (int row = 0; row < preview_.items.size(); ++row) {
        if (table_->item(row, 0)->checkState() != Qt::Checked)
            continue;
        const tags::TagMoveItem& item = preview_.items.at(row);
        job->paths.append(item.source);
        if (item.state == tags::TagMoveState::MultipleDestinations) {
            auto* choice = qobject_cast<QComboBox*>(table_->cellWidget(row, 2));
            job->choices.insert(item.source, choice ? choice->currentData().toString() : QString{});
        }
    }
    if (job->paths.isEmpty()) {
        status_->setText("Include at least one reviewable file.");
        return;
    }
    running_ = job;
    status_->setText("Rechecking chosen files and destinations…");
    setBusy(true);
    auto* worker = QThread::create([job] {
        job->output = tags::resolveTagMoveReview(job->input,
            job->paths, job->choices, &job->cancelled);
        if (!job->output.complete())
            return;
        QStringList proposed;
        for (const tags::TagMoveItem& item : job->output.items) {
            if (item.state == tags::TagMoveState::Proposed)
                proposed.append(item.source);
        }
        if (proposed.isEmpty())
            return;
        job->prepared = tags::prepareSelectedTagMoves(job->output,
            proposed, &job->cancelled);
        if (!job->prepared.valid())
            return;
        for (const auto& request : job->prepared.requests) {
            const auto metadata = files::reviewReviewedMoveMetadata(
                request, &job->cancelled);
            if (!metadata.valid()) {
                job->error = metadata.cancelled
                    ? QStringLiteral("Metadata review cancelled.")
                    : metadata.errors.join('\n');
                return;
            }
            for (const auto& item : metadata.items) {
                for (const QString& detail : item.degradations) {
                    job->metadataWarnings.append(QDir::toNativeSeparators(item.source)
                        + ": " + detail);
                }
            }
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, job] {
        running_.reset();
        if (!job->output.complete()) {
            status_->setText(job->output.cancelled ? QStringLiteral("Review cancelled.")
                : job->output.errors.join('\n'));
            setBusy(false);
            return;
        }
        if (!job->prepared.valid() || !job->error.isEmpty()) {
            status_->setText(job->error.isEmpty()
                ? (job->prepared.cancelled ? QStringLiteral("Move preparation cancelled.")
                    : job->prepared.errors.join('\n')) : job->error);
            setBusy(false);
            return;
        }
        QStringList lines;
        int proposed = 0;
        for (const tags::TagMoveItem& item : job->output.items) {
            if (item.state == tags::TagMoveState::Proposed) {
                ++proposed;
            } else {
                lines.append(QDir::toNativeSeparators(item.source) + "\n  Skipped: "
                    + item.reason);
            }
        }
        for (const auto& request : job->prepared.requests) {
            for (const auto& move : request.reviewedMoves) {
                lines.append(QDir::toNativeSeparators(move.source) + "\n  → "
                    + QDir::toNativeSeparators(move.destination));
            }
        }
        for (const tags::SkippedTagMove& item : job->prepared.skipped)
            lines.append(QDir::toNativeSeparators(item.source)
                + "\n  Skipped during preparation: " + item.reason);
        if (!job->metadataWarnings.isEmpty()) {
            lines.append(QStringLiteral("Metadata that may not be preserved at the destination:"));
            lines.append(job->metadataWarnings);
        }
        result_->setPlainText(lines.join("\n\n"));
        int ready = 0;
        for (const auto& request : job->prepared.requests)
            ready += request.reviewedMoves.size();
        status_->setText(QString("%1 of %2 included files are in %3 guarded requests; "
            "%4 skipped after recheck. %5 %6")
            .arg(ready).arg(job->output.items.size())
            .arg(job->prepared.requests.size())
            .arg(job->output.items.size() - proposed + job->prepared.skipped.size())
            .arg(job->metadataWarnings.isEmpty()
                ? QStringLiteral("No metadata loss was identified.")
                : QStringLiteral("Review metadata warnings below."))
            .arg(transferQueue_ && ready > 0
                ? QStringLiteral("Confirm and queue moves when ready.")
                : QStringLiteral("No move is ready to queue.")));
        reviewed_ = job;
        setBusy(false);
    });
    worker->start();
}

void TagDestinationDialog::invalidateReview()
{
    if (!reviewed_)
        return;
    reviewed_.reset();
    result_->setPlainText("The selection or rule choice changed. Review the included destinations again.");
    status_->setText("Review the changed selection before confirming a move. No files were moved.");
    setBusy(false);
}

void TagDestinationDialog::confirmMove()
{
    if (running_ || !transferQueue_ || !reviewed_
            || moveCount(reviewed_->prepared) == 0)
        return;
    auto job = std::make_shared<Job>();
    job->input = reviewed_->output;
    job->storagePath = storagePath_;
    for (const auto& item : job->input.items) {
        if (item.state == tags::TagMoveState::Proposed)
            job->paths.append(item.source);
    }
    if (job->paths.isEmpty())
        return;
    running_ = job;
    status_->setText("Rechecking current tags, rules, sources and destinations before final confirmation…");
    setBusy(true);
    auto* worker = QThread::create([job] {
        tags::TagCatalog catalog(job->storagePath);
        if (!catalog.load(&job->error))
            return;
        const auto fresh = tags::planSelectedTagMoves(job->paths, catalog,
            &job->cancelled);
        if (!fresh.complete() || fresh.items.size() != job->paths.size()) {
            job->error = fresh.cancelled ? QStringLiteral("Final review cancelled.")
                : fresh.errors.isEmpty()
                    ? QStringLiteral("Selected files changed; refresh the preview.")
                    : fresh.errors.join('\n');
            return;
        }
        int index = 0;
        for (const auto& retained : job->input.items) {
            if (retained.state != tags::TagMoveState::Proposed)
                continue;
            if (job->cancelled.load()) {
                job->error = QStringLiteral("Final review cancelled.");
                return;
            }
            const auto& current = fresh.items.at(index++);
            if (retained.source != current.source) {
                job->error = QStringLiteral("Selected files changed; refresh the preview.");
                return;
            }
            // A changed/replaced source is reported by preparation as a skip.
            // An unchanged source must still have exactly the reviewed tag and
            // rule evidence, even when the final destination happens to match.
            if (retained.sourceSnapshot == current.sourceSnapshot
                    && (retained.matchedTags != current.matchedTags
                        || matchKeys(retained) != matchKeys(current))) {
                job->error = QStringLiteral(
                    "Tags or destination rules changed; refresh the preview before moving.");
                return;
            }
        }
        job->prepared = tags::prepareSelectedTagMoves(job->input,
            job->paths, &job->cancelled);
        if (!job->prepared.valid())
            return;
        for (auto& request : job->prepared.requests) {
            const auto metadata = files::reviewReviewedMoveMetadata(request,
                &job->cancelled);
            if (!metadata.valid()) {
                job->error = metadata.cancelled
                    ? QStringLiteral("Metadata review cancelled.")
                    : metadata.errors.join('\n');
                return;
            }
            for (const auto& item : metadata.items) {
                for (const QString& detail : item.degradations) {
                    const QString warning = QDir::toNativeSeparators(item.source)
                        + ": " + detail;
                    request.reviewedApprovedMetadataWarnings.append(warning);
                    job->metadataWarnings.append(warning);
                }
            }
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, job] {
        running_.reset();
        if (!job->error.isEmpty() || !job->prepared.valid()) {
            status_->setText(job->error.isEmpty()
                ? (job->prepared.cancelled ? QStringLiteral("Final review cancelled.")
                    : job->prepared.errors.join('\n')) : job->error);
            setBusy(false);
            return;
        }
        QStringList mappings;
        for (const auto& request : job->prepared.requests) {
            for (const auto& move : request.reviewedMoves) {
                mappings.append(QDir::toNativeSeparators(move.source) + "\n  → "
                    + QDir::toNativeSeparators(move.destination));
            }
        }
        QStringList skipped;
        for (const auto& item : job->input.items) {
            if (item.state != tags::TagMoveState::Proposed) {
                skipped.append(QDir::toNativeSeparators(item.source)
                    + ": " + item.reason);
            }
        }
        for (const auto& item : job->prepared.skipped) {
            skipped.append(QDir::toNativeSeparators(item.source)
                + ": " + item.reason);
        }
        const int ready = moveCount(job->prepared);
        if (ready == 0) {
            result_->setPlainText("No reviewed mappings remain.\n\n"
                + skipped.join("\n"));
            status_->setText("All selected files were skipped after recheck. Refresh the preview; no move was queued.");
            reviewed_.reset();
            setBusy(false);
            return;
        }
        QString details = mappings.join("\n\n");
        if (!skipped.isEmpty())
            details += "\n\nSKIPPED FILES\n" + skipped.join('\n');
        if (!job->metadataWarnings.isEmpty()) {
            details += "\n\nMETADATA THAT MAY BE LOST\n"
                + job->metadataWarnings.join('\n');
        }
        result_->setPlainText(details);
        QMessageBox confirmation(this);
        confirmation.setObjectName("TagDestinationConfirmation");
        confirmation.setAccessibleName("Final tag destination move confirmation");
        confirmation.setIcon(job->metadataWarnings.isEmpty()
            ? QMessageBox::Question : QMessageBox::Warning);
        confirmation.setWindowTitle("Confirm tag destination moves");
        confirmation.setText(QString("Move %1 reviewed files in %2 Operation Center requests?\n"
            "%3 files were skipped after recheck.")
            .arg(ready).arg(job->prepared.requests.size()).arg(skipped.size()));
        QString explanation = "Review the exact mappings and skipped files below. "
            "This is not one atomic undoable batch: completed moves remain if "
            "you cancel or a later file fails. Operation Center reports each result.";
        if (!job->metadataWarnings.isEmpty()) {
            explanation += "\n\nThe destination cannot preserve all listed metadata. "
                "Continuing explicitly accepts that loss for these requests only.";
        }
        confirmation.setInformativeText(explanation);
        confirmation.setDetailedText(details);
        auto* moveButton = confirmation.addButton(
            QString("Move %1 files").arg(ready), QMessageBox::AcceptRole);
        auto* cancelButton = confirmation.addButton(QMessageBox::Cancel);
        confirmation.setDefaultButton(qobject_cast<QPushButton*>(cancelButton));
        confirmation.exec();
        if (confirmation.clickedButton() != moveButton) {
            status_->setText("Move cancelled before queueing. No files were changed.");
            setBusy(false);
            return;
        }
        QStringList jobIds;
        for (auto& request : job->prepared.requests) {
            request.reviewedMetadataDegradationApproved =
                !request.reviewedApprovedMetadataWarnings.isEmpty();
            jobIds.append(QString::number(transferQueue_->enqueue(std::move(request))));
        }
        reviewed_.reset();
        status_->setText(QString("Queued %1 files as Operation Center requests %2.")
            .arg(ready).arg(jobIds.join(", ")));
        QDialog::done(QDialog::Accepted);
    });
    worker->start();
}

} // namespace filemanager::ui
