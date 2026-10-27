#include "operationcenterdialog.h"

#include "files/operationjournal.h"
#include "files/transferqueue.h"
#include "ui/localizedformat.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace filemanager::ui {
namespace {

QString stateText(files::TransferJobState state)
{
    switch (state) {
    case files::TransferJobState::Queued: return "Queued";
    case files::TransferJobState::Running: return "Running";
    case files::TransferJobState::Paused: return "Paused";
    case files::TransferJobState::Succeeded: return "Completed";
    case files::TransferJobState::Failed: return "Failed";
    case files::TransferJobState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

} // namespace

OperationCenterDialog::OperationCenterDialog(files::TransferQueue* transferQueue,
    files::OperationJournal* operationJournal, QWidget* parent)
    : QDialog(parent), transferQueue_(transferQueue), operationJournal_(operationJournal)
{
    Q_ASSERT(transferQueue_);
    Q_ASSERT(operationJournal_);
    setObjectName("OperationCenterDialog");
    setWindowTitle("Operation center");
    resize(900, 500);
    auto* layout = new QVBoxLayout(this);
    summary_ = new QLabel(this);
    summary_->setObjectName("OperationCenterSummary");
    summary_->setAccessibleName("Operation center summary");
    layout->addWidget(summary_);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName("OperationCenterTabs");

    auto* transfersPage = new QWidget(tabs_);
    auto* transfersLayout = new QVBoxLayout(transfersPage);
    transfers_ = new QTableWidget(transfersPage);
    transfers_->setObjectName("OperationCenterTransfers");
    transfers_->setAccessibleName("Queued, active, paused, and completed operations");
    transfers_->setColumnCount(6);
    transfers_->setHorizontalHeaderLabels(
        {"ID", "Operation", "State", "Items", "Destination", "Started"});
    transfers_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    transfers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    transfers_->setSelectionBehavior(QAbstractItemView::SelectRows);
    transfersLayout->addWidget(transfers_, 1);
    transfersEmpty_ = new QLabel("No transfers yet. Copy or move files to see their progress here.", transfersPage);
    transfersEmpty_->setObjectName("OperationCenterTransfersEmpty");
    transfersEmpty_->setAccessibleName("No transfers yet");
    transfersEmpty_->setWordWrap(true);
    transfersEmpty_->setAlignment(Qt::AlignCenter);
    transfersLayout->addWidget(transfersEmpty_, 1);
    auto* controls = new QHBoxLayout;
    pause_ = new QPushButton("Pause", transfersPage);
    resume_ = new QPushButton("Resume", transfersPage);
    cancel_ = new QPushButton("Cancel job", transfersPage);
    retry_ = new QPushButton("Retry failed items", transfersPage);
    pause_->setObjectName("OperationCenterPause");
    resume_->setObjectName("OperationCenterResume");
    cancel_->setObjectName("OperationCenterCancel");
    retry_->setObjectName("OperationCenterRetry");
    controls->addWidget(pause_);
    controls->addWidget(resume_);
    controls->addWidget(cancel_);
    controls->addWidget(retry_);
    controls->addStretch(1);
    transfersLayout->addLayout(controls);
    tabs_->addTab(transfersPage, "Transfers");

    auto* recoveryPage = new QWidget(tabs_);
    auto* recoveryLayout = new QVBoxLayout(recoveryPage);
    recoveryExplanation_ = new QLabel(
        "Interrupted operations require review before retry or cleanup. Recovery actions recheck "
        "current file identities and never overwrite destinations automatically.", recoveryPage);
    recoveryExplanation_->setWordWrap(true);
    recoveryLayout->addWidget(recoveryExplanation_);
    recovery_ = new QTableWidget(recoveryPage);
    recovery_->setObjectName("OperationCenterRecovery");
    recovery_->setAccessibleName("Recoverable interrupted operations");
    recovery_->setColumnCount(4);
    recovery_->setHorizontalHeaderLabels({"Operation", "Items", "Destination", "Last activity"});
    recovery_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    recovery_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    recovery_->setSelectionBehavior(QAbstractItemView::SelectRows);
    recoveryLayout->addWidget(recovery_, 1);
    recoveryEmpty_ = new QLabel("No interrupted operations need recovery.", recoveryPage);
    recoveryEmpty_->setObjectName("OperationCenterRecoveryEmpty");
    recoveryEmpty_->setAccessibleName("No interrupted operations need recovery");
    recoveryEmpty_->setWordWrap(true);
    recoveryEmpty_->setAlignment(Qt::AlignCenter);
    recoveryLayout->addWidget(recoveryEmpty_, 1);
    reviewRecovery_ = new QPushButton("Review recovery options…", recoveryPage);
    reviewRecovery_->setObjectName("OperationCenterReviewRecovery");
    recoveryLayout->addWidget(reviewRecovery_, 0, Qt::AlignLeft);
    tabs_->addTab(recoveryPage, "Recoverable (0)");
    layout->addWidget(tabs_);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(close);

    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(transferQueue_, &files::TransferQueue::historyChanged,
        this, &OperationCenterDialog::populate);
    connect(transferQueue_, &files::TransferQueue::historyChanged,
        this, &OperationCenterDialog::updateActions);
    connect(operationJournal_, &files::OperationJournal::changed,
        this, &OperationCenterDialog::populate);
    connect(transfers_, &QTableWidget::itemSelectionChanged,
        this, &OperationCenterDialog::updateActions);
    connect(pause_, &QPushButton::clicked, this,
        [this] { transferQueue_->pause(selectedJob()); });
    connect(resume_, &QPushButton::clicked, this,
        [this] { transferQueue_->resume(selectedJob()); });
    connect(cancel_, &QPushButton::clicked, this,
        [this] { transferQueue_->cancel(selectedJob()); });
    connect(retry_, &QPushButton::clicked, this, [this] {
        const auto retryJob = transferQueue_->retryFailed(selectedJob());
        emit statusMessageRequested(retryJob
                ? QStringLiteral("Failed items queued as transfer %1").arg(*retryJob)
                : QStringLiteral("No retryable failed items are currently available"),
            4000);
    });
    connect(reviewRecovery_, &QPushButton::clicked, this, [this] {
        accept();
        emit reviewRecoveryRequested();
    });
    populate();
    updateActions();
}

quint64 OperationCenterDialog::selectedJob() const
{
    const int row = transfers_->currentRow();
    return row >= 0 && transfers_->item(row, 0)
        ? transfers_->item(row, 0)->text().toULongLong() : 0;
}

void OperationCenterDialog::populate()
{
    const auto records = transferQueue_->history();
    const auto interrupted = operationJournal_->interruptedRecords();
    transfers_->setRowCount(records.size());
    for (int row = 0; row < records.size(); ++row) {
        const auto& record = records.at(row);
        transfers_->setItem(row, 0, new QTableWidgetItem(QLocale().toString(record.id)));
        transfers_->setItem(row, 1, new QTableWidgetItem(
            record.request.operation == files::TransferOperation::Move ? "Move" : "Copy"));
        auto* state = new QTableWidgetItem(stateText(record.state));
        if (!record.errors.isEmpty())
            state->setToolTip(record.errors.join('\n'));
        transfers_->setItem(row, 2, state);
        transfers_->setItem(row, 3,
            new QTableWidgetItem(QLocale().toString(record.request.sources.size())));
        transfers_->setItem(row, 4,
            new QTableWidgetItem(QDir::toNativeSeparators(record.request.destinationDirectory)));
        transfers_->setItem(row, 5,
            new QTableWidgetItem(localizedDateTime(record.startedAt)));
    }
    recovery_->setRowCount(interrupted.size());
    for (int row = 0; row < interrupted.size(); ++row) {
        const auto& record = interrupted.at(row);
        recovery_->setItem(row, 0, new QTableWidgetItem(
            record.request.operation == files::TransferOperation::Move ? "Move" : "Copy"));
        recovery_->setItem(row, 1,
            new QTableWidgetItem(QLocale().toString(record.request.sources.size())));
        recovery_->setItem(row, 2,
            new QTableWidgetItem(QDir::toNativeSeparators(record.request.destinationDirectory)));
        const QDateTime activity = record.finishedAt.isValid() ? record.finishedAt
            : record.startedAt.isValid() ? record.startedAt : record.queuedAt;
        recovery_->setItem(row, 3, new QTableWidgetItem(localizedDateTime(activity)));
    }
    tabs_->setTabText(1, QStringLiteral("Recoverable (%1)").arg(interrupted.size()));
    const auto countState = [&records](files::TransferJobState state) {
        return std::count_if(records.cbegin(), records.cend(),
            [state](const auto& record) { return record.state == state; });
    };
    const bool hasTransfers = !records.isEmpty();
    const bool hasRecovery = !interrupted.isEmpty();
    transfers_->setVisible(hasTransfers);
    transfersEmpty_->setVisible(!hasTransfers);
    for (auto* action : {pause_, resume_, cancel_, retry_})
        action->setVisible(hasTransfers);
    recoveryExplanation_->setVisible(hasRecovery);
    recovery_->setVisible(hasRecovery);
    recoveryEmpty_->setVisible(!hasRecovery);
    reviewRecovery_->setVisible(hasRecovery);
    reviewRecovery_->setEnabled(hasRecovery);
    summary_->setText(!hasTransfers && !hasRecovery
        ? QStringLiteral("Nothing to review yet.")
        : QStringLiteral(
            "Queued: %1   Active: %2   Paused: %3   Failed: %4   Completed: %5   Recoverable: %6")
            .arg(countState(files::TransferJobState::Queued))
            .arg(countState(files::TransferJobState::Running))
            .arg(countState(files::TransferJobState::Paused))
            .arg(countState(files::TransferJobState::Failed))
            .arg(countState(files::TransferJobState::Succeeded))
            .arg(interrupted.size()));
    summary_->setVisible(hasTransfers || hasRecovery);
}

void OperationCenterDialog::updateActions()
{
    const quint64 id = selectedJob();
    const auto records = transferQueue_->history();
    const auto record = std::find_if(records.cbegin(), records.cend(),
        [id](const auto& candidate) { return candidate.id == id; });
    const bool selected = record != records.cend();
    pause_->setEnabled(selected && record->state == files::TransferJobState::Running);
    resume_->setEnabled(selected && record->state == files::TransferJobState::Paused);
    cancel_->setEnabled(selected && (record->state == files::TransferJobState::Queued
        || record->state == files::TransferJobState::Running
        || record->state == files::TransferJobState::Paused));
    retry_->setEnabled(selected
        && std::any_of(record->failures.cbegin(), record->failures.cend(),
            [](const auto& failure) {
                return failure.retryable && QFileInfo::exists(failure.source);
            }));
}

} // namespace filemanager::ui
