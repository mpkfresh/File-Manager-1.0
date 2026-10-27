#include "batchrenamedialog.h"
#include "localizedformat.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFrame>
#include <QFormLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace filemanager::ui {
namespace {

std::atomic_int activePreviewWorkers{0};

struct PreviewRow
{
    QString source;
    QString currentName;
    QString proposedName;
};

struct PreviewJob
{
    quint64 revision = 0;
    bool initialize = false;
    bool captured = false;
    std::atomic_bool cancelled{false};
    QStringList initialPaths;
    QStringList paths;
    int mode = 0;
    QString label;
    QStringList names;
    bool keepExtensions = true;
    int start = 1;
    int digits = 3;
    int step = 1;
    filemanager::files::BatchRenameRule rule;
    QHash<QString, filemanager::files::BatchRenameSource> originals;
    filemanager::files::BatchRenamePlan plan;
    QList<PreviewRow> rows;

};

void preparePreview(PreviewJob& job)
{
    using namespace filemanager::files;
    if (job.initialize) {
        const auto initial = createBatchRenamePlan(job.initialPaths, {}, &job.cancelled);
        if (job.cancelled.load())
            return;
        for (const auto& item : initial.items)
            job.originals.insert(item.source, item.sourceSnapshot);
        job.captured = true;
    }
    if (job.mode == 0)
        job.plan = createNumberedRenamePlan(job.paths, job.label, job.start, job.digits, job.step, &job.cancelled);
    else if (job.mode == 1)
        job.plan = createListedRenamePlan(job.paths, job.names, job.keepExtensions, &job.cancelled);
    else
        job.plan = createBatchRenamePlan(job.paths, job.rule, &job.cancelled);
    QHash<QString, QString> proposals;
    for (auto& item : job.plan.items) {
        if (job.cancelled.load())
            return;
        const auto original = job.originals.value(item.source);
        if (!original.valid || original != item.sourceSnapshot)
            job.plan.errors.append("Source changed or cannot be verified; close and reopen Batch rename: " + item.source);
        item.sourceSnapshot = original;
        proposals.insert(item.source, QFileInfo(item.destination).fileName());
    }
    job.rows.reserve(job.paths.size());
    for (const auto& path : job.paths) {
        if (job.cancelled.load())
            return;
        const QFileInfo source(path);
        job.rows.append({source.absoluteFilePath(), source.fileName(), proposals.value(source.absoluteFilePath())});
    }
}

QSpinBox* numberField(QWidget* parent, const QString& name, int value, bool digits = false)
{
    auto* field = new QSpinBox(parent);
    field->setObjectName(name);
    field->setRange(digits ? 1 : -999999999, digits ? 12 : 999999999);
    field->setValue(value);
    return field;
}

QStringList enteredNames(const QPlainTextEdit* field)
{
    QString text = field->toPlainText();
    // Accept one terminal newline from a copied column; blank rows remain errors.
    if (text.endsWith('\n'))
        text.chop(1);
    return text.isEmpty() ? QStringList() : text.split('\n', Qt::KeepEmptyParts);
}

} // namespace

class BatchRenameDialog::Preparation
{
public:
    QStringList initialPaths;
    quint64 revision = 0;
    bool originalsReady = false;
    bool closed = false;
    int row = 0;
    int focusRow = 0;
    QTimer* debounce = nullptr;
    QTimer* renderTimer = nullptr;
    std::shared_ptr<PreviewJob> running;
    std::shared_ptr<PreviewJob> rendering;
};

BatchRenameDialog::BatchRenameDialog(const QStringList& paths, QWidget* parent)
    : QDialog(parent), preparation_(std::make_unique<Preparation>()), paths_(paths)
{
    preparation_->initialPaths = paths;
    setWindowTitle("Batch rename");
    resize(860, 650);
    auto* root = new QVBoxLayout(this);
    auto* modeRow = new QFormLayout;
    mode_ = new QComboBox(this);
    mode_->setObjectName("RenameMode");
    mode_->addItems({"Numbered names", "Names from list", "Rules"});
    modeRow->addRow("Naming method", mode_);
    root->addLayout(modeRow);

    auto* pages = new QStackedWidget(this);
    const auto addPage = [pages](QWidget* page) {
        auto* scroll = new QScrollArea(pages);
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        pages->addWidget(scroll);
    };
    auto* numberedPage = new QWidget(pages);
    auto* numberedForm = new QFormLayout(numberedPage);
    label_ = new QLineEdit("Chapter", numberedPage);
    label_->setObjectName("RenameLabel");
    label_->setPlaceholderText("Chapter");
    start_ = numberField(numberedPage, "RenameStart", 1);
    step_ = numberField(numberedPage, "RenameStep", 1);
    digits_ = numberField(numberedPage, "RenameDigits", 3, true);
    numberedForm->addRow("Name", label_);
    numberedForm->addRow("Start number", start_);
    numberedForm->addRow("Step", step_);
    numberedForm->addRow("Minimum digits", digits_);
    auto* numberedHint = new QLabel("Example: Chapter 001.pdf. Each file keeps its extension.", numberedPage);
    numberedHint->setWordWrap(true);
    numberedForm->addRow(numberedHint);
    addPage(numberedPage);

    auto* listPage = new QWidget(pages);
    auto* listLayout = new QVBoxLayout(listPage);
    auto* listHint = new QLabel("Type or paste one name per line, matching the row order below.", listPage);
    listHint->setWordWrap(true);
    listLayout->addWidget(listHint);
    names_ = new QPlainTextEdit(listPage);
    names_->setObjectName("RenameNames");
    names_->setAccessibleName("Replacement names, one per line");
    names_->setPlaceholderText("The Beginning\nA New Arrival\nThe Journey");
    names_->setMinimumHeight(80);
    names_->setMaximumHeight(120);
    listLayout->addWidget(names_);
    keepExtensions_ = new QCheckBox("Keep each file's extension (enter names without extensions)", listPage);
    keepExtensions_->setObjectName("RenameKeepExtensions");
    keepExtensions_->setChecked(true);
    listLayout->addWidget(keepExtensions_);
    addPage(listPage);

    auto* rulesPage = new QWidget(pages);
    auto* rulesForm = new QFormLayout(rulesPage);
    find_ = new QLineEdit(rulesPage);
    replace_ = new QLineEdit(rulesPage);
    prefix_ = new QLineEdit(rulesPage);
    suffix_ = new QLineEdit(rulesPage);
    regex_ = new QCheckBox("Use regular expression", rulesPage);
    caseSensitive_ = new QCheckBox("Case sensitive", rulesPage);
    caseConversion_ = new QComboBox(rulesPage);
    caseConversion_->addItems({"Keep original case", "lowercase", "UPPERCASE", "Title Case"});
    sequence_ = new QCheckBox("Add sequence number", rulesPage);
    ruleStart_ = numberField(rulesPage, "RuleStart", 1);
    ruleStep_ = numberField(rulesPage, "RuleStep", 1);
    ruleDigits_ = numberField(rulesPage, "RuleDigits", 3, true);
    rulesForm->addRow("Find", find_);
    rulesForm->addRow("Replace", replace_);
    rulesForm->addRow(regex_);
    rulesForm->addRow(caseSensitive_);
    rulesForm->addRow("Prefix", prefix_);
    rulesForm->addRow("Suffix", suffix_);
    rulesForm->addRow("Name case", caseConversion_);
    rulesForm->addRow(sequence_);
    rulesForm->addRow("Sequence start", ruleStart_);
    rulesForm->addRow("Sequence step", ruleStep_);
    rulesForm->addRow("Minimum digits", ruleDigits_);
    addPage(rulesPage);
    pages->setFixedHeight(170);

    root->addWidget(pages);

    auto* orderHint = new QLabel(
        "Preview follows selection order. Use Move up or Move down to change it.", this);
    orderHint->setWordWrap(true);
    root->addWidget(orderHint);
    auto* orderBar = new QHBoxLayout;
    up_ = new QPushButton("Move up", this);
    up_->setObjectName("RenameMoveUp");
    up_->setShortcut(QKeySequence("Alt+Up"));
    down_ = new QPushButton("Move down", this);
    down_->setObjectName("RenameMoveDown");
    down_->setShortcut(QKeySequence("Alt+Down"));
    for (auto* button : {up_, down_}) {
        button->setAutoDefault(false);
        orderBar->addWidget(button);
    }
    orderBar->addStretch();
    root->addLayout(orderBar);

    preview_ = new QTableWidget(this);
    preview_->setObjectName("RenamePreview");
    preview_->setAccessibleName("Rename order and preview");
    preview_->setColumnCount(2);
    preview_->setHorizontalHeaderLabels({"Current name", "Proposed name"});
    preview_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    preview_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    preview_->setSelectionMode(QAbstractItemView::SingleSelection);
    preview_->setSelectionBehavior(QAbstractItemView::SelectRows);
    preview_->setMinimumHeight(120);
    root->addWidget(preview_, 1);
    validation_ = new QLabel(this);
    validation_->setObjectName("RenameValidation");
    validation_->setTextFormat(Qt::PlainText);
    validation_->setWordWrap(true);
    root->addWidget(validation_);
    auto* recovery = new QLabel(
        "After Rename, use Edit > Undo while files are unchanged. Failures attempt rollback "
        "and report any recovery needed.", this);
    recovery->setWordWrap(true);
    root->addWidget(recovery);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setText("Rename");
    buttons_->button(QDialogButtonBox::Ok)->setObjectName("RenameApply");
    buttons_->button(QDialogButtonBox::Ok)->setAutoDefault(false);
    buttons_->button(QDialogButtonBox::Ok)->setDefault(false);
    root->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        if (plan_.isValid())
            accept();
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(up_, &QPushButton::clicked, this, [this] { moveCurrentRow(-1); });
    connect(down_, &QPushButton::clicked, this, [this] { moveCurrentRow(1); });
    connect(preview_, &QTableWidget::itemSelectionChanged, this, &BatchRenameDialog::updateOrderButtons);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this, pages](int index) {
        pages->setCurrentIndex(index);
        pages->setFixedHeight(index == 0 ? 170 : index == 1 ? 220 : 235);
        rebuildPlan();
    });
    for (auto* edit : {label_, find_, replace_, prefix_, suffix_})
        connect(edit, &QLineEdit::textChanged, this, &BatchRenameDialog::rebuildPlan);
    for (auto* spin : {start_, step_, digits_, ruleStart_, ruleStep_, ruleDigits_})
        connect(spin, &QSpinBox::valueChanged, this, &BatchRenameDialog::rebuildPlan);
    for (auto* check : {keepExtensions_, regex_, caseSensitive_, sequence_})
        connect(check, &QCheckBox::toggled, this, &BatchRenameDialog::rebuildPlan);
    connect(names_, &QPlainTextEdit::textChanged, this, &BatchRenameDialog::rebuildPlan);
    connect(caseConversion_, &QComboBox::currentIndexChanged, this, &BatchRenameDialog::rebuildPlan);
    preparation_->debounce = new QTimer(this);
    preparation_->debounce->setSingleShot(true);
    connect(preparation_->debounce, &QTimer::timeout, this, &BatchRenameDialog::startPreparation);
    preparation_->renderTimer = new QTimer(this);
    preparation_->renderTimer->setInterval(0);
    connect(preparation_->renderTimer, &QTimer::timeout, this, &BatchRenameDialog::renderNextRows);
    rebuildPlan();
    label_->selectAll();
    label_->setFocus();
}

BatchRenameDialog::~BatchRenameDialog()
{
    if (preparation_->running)
        preparation_->running->cancelled.store(true);
}

void BatchRenameDialog::done(int result)
{
    if (preparation_->closed || (result == QDialog::Accepted && (pending_ || !plan_.isValid())))
        return;
    preparation_->closed = true;
    preparation_->debounce->stop();
    preparation_->renderTimer->stop();
    if (preparation_->running)
        preparation_->running->cancelled.store(true);
    if (result != QDialog::Accepted) {
        plan_ = {};
        plan_.cancelled = true;
    }
    pending_ = false;
    QDialog::done(result);
}

void BatchRenameDialog::rebuildPlan()
{
    auto& state = *preparation_;
    if (state.closed)
        return;
    ++state.revision;
    pending_ = true;
    plan_ = {};
    plan_.errors = {"Preparing the latest preview."};
    state.focusRow = qMax(0, preview_->currentRow());
    state.renderTimer->stop();
    state.rendering.reset();
    // Initial identity capture must finish once; edits cannot replace that baseline.
    if (state.running && state.originalsReady)
        state.running->cancelled.store(true);
    for (auto* field : {ruleStart_, ruleStep_, ruleDigits_})
        field->setEnabled(sequence_->isChecked());
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
    preview_->setEnabled(false);
    validation_->setText("Preparing the latest preview in the background. Cancel makes no file changes.");
    updateOrderButtons();
    state.debounce->start(80);
}

void BatchRenameDialog::startPreparation()
{
    auto& state = *preparation_;
    if (state.closed || state.running)
        return;
    // Closed dialogs can leave an OS metadata read in flight. Bound those workers
    // across dialogs as well as serializing requests within each dialog.
    if (activePreviewWorkers.load() >= 2) {
        state.debounce->start(50);
        return;
    }
    auto job = std::make_shared<PreviewJob>();
    job->revision = state.revision;
    job->initialize = !state.originalsReady;
    job->initialPaths = state.initialPaths;
    job->paths = paths_;
    job->mode = mode_->currentIndex();
    job->label = label_->text();
    job->names = enteredNames(names_);
    job->keepExtensions = keepExtensions_->isChecked();
    job->start = start_->value();
    job->digits = digits_->value();
    job->step = step_->value();
    job->originals = originalSources_;
    job->rule = {.find = find_->text(), .replace = replace_->text(),
            .useRegularExpression = regex_->isChecked(), .caseSensitive = caseSensitive_->isChecked(),
            .prefix = prefix_->text(), .suffix = suffix_->text(),
            .caseConversion = static_cast<filemanager::files::BatchRenameCase>(caseConversion_->currentIndex()),
            .addSequence = sequence_->isChecked(), .sequenceStart = ruleStart_->value(),
            .sequencePadding = ruleDigits_->value(), .sequenceStep = ruleStep_->value()};
    state.running = job;
    activePreviewWorkers.fetch_add(1);
    auto* worker = QThread::create([job] {
        preparePreview(*job);
        activePreviewWorkers.fetch_sub(1);
    });
    connect(worker, &QThread::finished, this, [this, job] {
        auto& current = *preparation_;
        current.running.reset();
        if (current.closed)
            return;
        if (job->captured) {
            originalSources_ = job->originals;
            current.originalsReady = true;
        }
        if (job->cancelled.load() || job->revision != current.revision) {
            current.debounce->start(0);
            return;
        }
        current.rendering = job;
        current.row = 0;
        // The source count is fixed for this dialog. Replace old items inside the bounded
        // rendering callbacks instead of deleting the entire previous table in one UI turn.
        if (preview_->rowCount() != job->rows.size())
            preview_->setRowCount(static_cast<int>(job->rows.size()));
        current.renderTimer->start();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void BatchRenameDialog::renderNextRows()
{
    auto& state = *preparation_;
    const auto job = state.rendering;
    if (!job || state.closed || job->revision != state.revision) {
        state.renderTimer->stop();
        return;
    }
    QElapsedTimer budget;
    budget.start();
    int populated = 0;
    while (state.row < job->rows.size() && populated < 128 && budget.elapsed() < 8) {
        const auto& row = job->rows[state.row];
        auto* current = new QTableWidgetItem(row.currentName);
        current->setToolTip(row.source);
        preview_->setItem(state.row, 0, current);
        if (state.closed || job->revision != state.revision)
            return;
        preview_->setItem(state.row, 1, new QTableWidgetItem(row.proposedName));
        if (state.closed || job->revision != state.revision)
            return;
        ++state.row;
        ++populated;
    }
    if (state.row < job->rows.size())
        return;
    state.renderTimer->stop();
    plan_ = job->plan;
    pending_ = false;
    preview_->setEnabled(true);
    if (!job->rows.isEmpty()) {
        const int focus = qMin(state.focusRow, static_cast<int>(job->rows.size()) - 1);
        preview_->setCurrentCell(focus, 0);
        preview_->selectRow(focus);
    }
    QString status;
    if (!plan_.errors.isEmpty()) {
        status = plan_.errors.first();
        if (plan_.errors.size() > 1)
            status += QString(" (%1 errors in total.)").arg(plan_.errors.size());
    } else if (!plan_.hasChanges()) {
        status = "Adjust the names or rules to produce at least one change.";
    } else {
        status = QString("%1 ready. Review the mapping before choosing Rename.")
            .arg(localizedItems(plan_.items.size()));
    }
    if (job->mode == 1)
        status.prepend(QString("%1 names / %2 items. ").arg(job->names.size()).arg(job->paths.size()));
    validation_->setText(status);
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(plan_.isValid());
    updateOrderButtons();
    state.rendering.reset();
}

void BatchRenameDialog::moveCurrentRow(int offset)
{
    const int row = preview_->currentRow();
    const int target = row + offset;
    if (row < 0 || target < 0 || target >= paths_.size())
        return;
    paths_.swapItemsAt(row, target);
    preview_->setCurrentCell(target, 0);
    preview_->selectRow(target);
    rebuildPlan();
}

void BatchRenameDialog::updateOrderButtons()
{
    const int row = preview_->currentRow();
    up_->setEnabled(!pending_ && row > 0);
    down_->setEnabled(!pending_ && row >= 0 && row + 1 < paths_.size());
}

filemanager::files::BatchRenameResult runBatchRenameDialog(QWidget* parent,
    const QStringList& paths, filemanager::files::BatchRenamePlan* appliedPlan)
{
    BatchRenameDialog dialog(paths, parent);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    const auto result = filemanager::files::executeBatchRename(dialog.renamePlan());
    if (result.ok && appliedPlan)
        *appliedPlan = dialog.renamePlan();
    return result;
}

void showBatchRenameHelp(QWidget* parent, const QKeySequence& shortcut)
{
    QDialog dialog(parent);
    dialog.setObjectName("BatchRenameHelpDialog");
    dialog.setWindowTitle("Batch Rename help");
    dialog.resize(660, 390);
    auto* layout = new QVBoxLayout(&dialog);
    const QString binding = shortcut.isEmpty() ? QStringLiteral("No shortcut assigned")
        : shortcut.toString(QKeySequence::NativeText);
    auto* tabs = new QTabWidget(&dialog);
    tabs->setObjectName("BatchRenameHelpTabs");
    tabs->setAccessibleName("Batch Rename help sections");
    const auto addSection = [tabs, &dialog](const QString& title, const QString& objectName,
                                            const QString& text) {
        auto* content = new QLabel(text, &dialog);
        content->setObjectName(objectName);
        content->setAccessibleName(title + " instructions");
        content->setWordWrap(true);
        content->setTextFormat(Qt::PlainText);
        content->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
        content->setMargin(14);
        content->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        auto* scroll = new QScrollArea(tabs);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(content);
        tabs->addTab(scroll, title);
    };
    addSection("Quick steps", "BatchRenameHelpQuickSteps", QStringLiteral(
        "Current shortcut: %1. Default shortcut: Ctrl+F2.\n"
        "Change it under Workspace > Keyboard shortcuts.\n\n"
        "1  Select two or more files, then use the shortcut or Batch rename command.\n\n"
        "2  Choose Numbered names, Names from list, or Rules.\n\n"
        "3  Compare Current name and Proposed name for every row. Choose Rename only when the preview is right.\n\n"
        "Cancel makes no changes before applying. Rename cannot be cancelled once it starts. "
        "Undo works only in this session while every renamed file remains unchanged.").arg(binding));
    addSection("Names and order", "BatchRenameHelpNames", QStringLiteral(
        "ORDER\n"
        "Ctrl-click files one by one in the order you want. For keyboard selection, move with Ctrl+Arrow "
        "and toggle with Ctrl+Space. Range selection uses row order; it does not infer chapter order. "
        "In the popup, use Move up / Move down (Alt+Up / Alt+Down).\n\n"
        "NUMBERED NAMES\n"
        "Enter Chapter or another label, then set the start number, step and minimum digits. "
        "The preview shows names such as Chapter 001.pdf. Each file keeps its extension.\n\n"
        "NAMES FROM LIST\n"
        "Paste one name per line in preview-row order. With Keep each file's extension checked, omit extensions; "
        "uncheck it for full filenames. Missing, extra, blank, invalid or colliding names block the batch. "
        "Moving a row changes its name mapping.\n\n"
        "RULES\n"
        "Find/replace, prefixes, suffixes, case changes and regular expressions remain available. "
        "This workflow does not read chapter contents or use Kai UI."));
    addSection("Undo and recovery", "BatchRenameHelpRecovery", QStringLiteral(
        "BEFORE APPLYING\n"
        "Rename stays disabled until the latest background preview is ready. If a source changed or cannot "
        "be verified, close and reopen Batch rename to review it again. Keep backups of important files.\n\n"
        "UNDO\n"
        "A successful batch can be undone in this session while every file is unchanged. "
        "Undo refuses occupied original names or changed files.\n\n"
        "IF SOMETHING FAILS\n"
        "File Manager attempts rollback and reports manual recovery paths when needed. If File Manager or "
        "Windows stops during execution, hidden .file-manager-rename-*.tmp staging files may remain beside "
        "the originals. Do not delete them; inspect and recover them manually. There is no durable "
        "batch-rename crash recovery."));
    layout->addWidget(tabs);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

} // namespace filemanager::ui
