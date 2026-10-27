#include "tagrulesdialog.h"

#include "tags/tagrules.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <utility>

namespace filemanager::ui {
namespace {

bool editRuleFields(QWidget* parent, tags::DestinationRule* rule)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(rule->id.isEmpty() ? "New tag destination rule" : "Edit tag destination rule");
    dialog.setMinimumWidth(520);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("A saved rule proposes a destination during a separate reviewed move preview.", &dialog));
    auto* form = new QFormLayout;
    auto* labels = new QPlainTextEdit(&dialog);
    labels->setObjectName("TagRuleLabels");
    labels->setAccessibleName("Required tags, one per line");
    labels->setPlaceholderText("Books\nTo file");
    labels->setPlainText(rule->requiredTags.join('\n'));
    labels->setMinimumHeight(105);
    form->addRow("Required tags, one per line:", labels);
    auto* pathRow = new QWidget(&dialog);
    auto* pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    auto* destination = new QLineEdit(rule->destinationDirectory, pathRow);
    destination->setObjectName("TagRuleDestination");
    destination->setAccessibleName("Destination folder for matching tags");
    auto* browse = new QPushButton("Browse…", pathRow);
    pathLayout->addWidget(destination, 1);
    pathLayout->addWidget(browse);
    form->addRow("Destination folder:", pathRow);
    layout->addLayout(form);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
        const QString chosen = QFileDialog::getExistingDirectory(&dialog,
            "Choose a tag destination folder", destination->text());
        if (!chosen.isEmpty())
            destination->setText(chosen);
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        tags::DestinationRule candidate = *rule;
        if (candidate.id.isEmpty())
            candidate.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        candidate.requiredTags.clear();
        for (QString label : labels->toPlainText().split('\n')) {
            label = label.trimmed().normalized(QString::NormalizationForm_C);
            if (!label.isEmpty())
                candidate.requiredTags.append(label);
        }
        candidate.destinationDirectory = QDir::cleanPath(
            QDir::fromNativeSeparators(destination->text().trimmed()));
        QString error;
        if (!tags::validateDestinationRules({candidate}, &error)) {
            QMessageBox::warning(&dialog, "Invalid tag rule", error);
            return;
        }
        *rule = std::move(candidate);
        dialog.accept();
    });
    return dialog.exec() == QDialog::Accepted;
}

} // namespace

TagRulesDialog::TagRulesDialog(QWidget* parent, QString storagePath)
    : QDialog(parent), storagePath_(std::move(storagePath))
{
    setWindowTitle("Tag destination rules");
    setMinimumSize(700, 400);
    auto* layout = new QVBoxLayout(this);
    auto* disclosure = new QLabel("Rules use user tags to propose folders for explicit review. "
        "Saving or changing a rule never scans or moves files.", this);
    disclosure->setWordWrap(true);
    disclosure->setObjectName("TagRulesDisclosure");
    layout->addWidget(disclosure);
    table_ = new QTableWidget(0, 2, this);
    table_->setObjectName("TagRulesTable");
    table_->setAccessibleName("Saved tag destination rules");
    table_->setHorizontalHeaderLabels({"Required tags", "Destination folder"});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table_, 1);
    status_ = new QLabel(this);
    status_->setObjectName("TagRulesStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* actions = new QHBoxLayout;
    auto* add = new QPushButton("Add rule", this);
    add->setObjectName("TagRulesAdd");
    edit_ = new QPushButton("Edit selected", this);
    edit_->setObjectName("TagRulesEdit");
    remove_ = new QPushButton("Remove selected", this);
    remove_->setObjectName("TagRulesRemove");
    auto* reloadButton = new QPushButton("Reload", this);
    reloadButton->setObjectName("TagRulesReload");
    save_ = new QPushButton("Save rules", this);
    save_->setObjectName("TagRulesSave");
    auto* close = new QPushButton("Close", this);
    close->setObjectName("TagRulesClose");
    for (QPushButton* button : {add, edit_, remove_, reloadButton, save_})
        actions->addWidget(button);
    actions->addStretch();
    actions->addWidget(close);
    layout->addLayout(actions);
    connect(add, &QPushButton::clicked, this, [this] { editRule(true); });
    connect(edit_, &QPushButton::clicked, this, [this] { editRule(false); });
    connect(remove_, &QPushButton::clicked, this, [this] { removeRule(); });
    connect(reloadButton, &QPushButton::clicked, this, [this] { reload(); });
    connect(save_, &QPushButton::clicked, this, [this] { save(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] { refresh(); });
    reload();
}

void TagRulesDialog::done(int result)
{
    if (draft_ != baseline_
            && QMessageBox::question(this, "Discard unsaved tag rules?",
                "Close without saving your destination-rule changes?") != QMessageBox::Yes)
        return;
    QDialog::done(result);
}

void TagRulesDialog::reload()
{
    if (draft_ != baseline_
            && QMessageBox::question(this, "Discard unsaved tag rules?",
                "Reload saved rules and discard your unsaved changes?") != QMessageBox::Yes)
        return;
    tags::TagCatalog catalog(storagePath_);
    QString error;
    if (!catalog.load(&error)) {
        status_->setText(error);
        return;
    }
    baseline_ = catalog.rules();
    draft_ = baseline_;
    status_->setText(QString("Loaded %1 saved rules. No files were moved.").arg(draft_.size()));
    refresh();
}

void TagRulesDialog::editRule(bool create)
{
    const int row = table_->currentRow();
    if (!create && (row < 0 || row >= draft_.size()))
        return;
    tags::DestinationRule candidate = create ? tags::DestinationRule{} : draft_.at(row);
    if (!editRuleFields(this, &candidate))
        return;
    if (create)
        draft_.append(std::move(candidate));
    else
        draft_[row] = std::move(candidate);
    refresh();
}

void TagRulesDialog::removeRule()
{
    const int row = table_->currentRow();
    if (row < 0 || row >= draft_.size())
        return;
    draft_.removeAt(row);
    refresh();
}

void TagRulesDialog::save()
{
    QString error;
    tags::TagCatalog catalog(storagePath_);
    if (!catalog.load(&error) || !catalog.replaceRules(draft_, &error, &baseline_)) {
        status_->setText(error);
        return;
    }
    baseline_ = draft_;
    status_->setText(QString("Saved %1 rules. No files were moved.").arg(draft_.size()));
    refresh();
}

void TagRulesDialog::refresh()
{
    const QSignalBlocker blockSelection(table_);
    const int selected = table_->currentRow();
    table_->setRowCount(draft_.size());
    for (int row = 0; row < draft_.size(); ++row) {
        const auto& rule = draft_.at(row);
        table_->setItem(row, 0, new QTableWidgetItem(rule.requiredTags.join(", ")));
        table_->setItem(row, 1, new QTableWidgetItem(QDir::toNativeSeparators(
            rule.destinationDirectory)));
    }
    if (selected >= 0 && selected < draft_.size())
        table_->selectRow(selected);
    const bool hasSelection = table_->currentRow() >= 0;
    edit_->setEnabled(hasSelection);
    remove_->setEnabled(hasSelection);
    save_->setEnabled(draft_ != baseline_);
}

} // namespace filemanager::ui
