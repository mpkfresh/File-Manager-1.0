#include "tageditordialog.h"

#include "files/sourcesnapshot.h"
#include "tags/tagcatalog.h"

#include <QCloseEvent>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QPair>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include <atomic>
#include <memory>
#include <utility>

namespace filemanager::ui {
namespace {

constexpr int MaximumSelectedFiles = 10'000;
constexpr int MaximumVisibleRows = 50;
constexpr int MaximumVisibleLabels = 100;
constexpr quint32 DirectoryAttribute = 0x10;
constexpr quint32 ReparsePointAttribute = 0x400;

QStringList enteredTags(const QString& text)
{
    QStringList result;
    for (const QString& line : text.split('\n')) {
        if (!line.trimmed().isEmpty())
            result.append(line);
    }
    return result;
}

QString nativePath(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

} // namespace

struct TagEditorDialog::EditJob
{
    Action action = Action::Inspect;
    QStringList paths;
    QStringList labels;
    QString storagePath;
    std::atomic_bool cancelled{false};
    bool ok = false;
    QString error;
    QString inspection;
};

TagEditorDialog::TagEditorDialog(QStringList selectedPaths, QWidget* parent,
    QString storagePath)
    : QDialog(parent), selectedPaths_(std::move(selectedPaths)),
      storagePath_(std::move(storagePath))
{
    setObjectName("TagEditorDialog");
    setWindowTitle("Tag selected files");
    resize(760, 620);
    auto* layout = new QVBoxLayout(this);
    auto* introduction = new QLabel(
        QString("Inspect, add or remove labels for %1 explicitly selected files. "
            "Only File Manager's local tag catalog changes; file contents and locations stay unchanged. "
            "No folders are scanned. Enter one tag per line.")
            .arg(selectedPaths_.size()), this);
    introduction->setWordWrap(true);
    introduction->setTextFormat(Qt::PlainText);
    layout->addWidget(introduction);

    auto* scope = new QPlainTextEdit(this);
    scope->setObjectName("TagEditorScope");
    scope->setAccessibleName("Files selected for tag edit");
    scope->setReadOnly(true);
    QStringList visible;
    for (qsizetype index = 0; index < selectedPaths_.size()
            && index < MaximumVisibleRows; ++index) {
        visible.append(nativePath(selectedPaths_.at(index)));
    }
    if (selectedPaths_.size() > visible.size())
        visible.append(QString("… and %1 more selected files")
            .arg(selectedPaths_.size() - visible.size()));
    scope->setPlainText(visible.join('\n'));
    scope->setMinimumHeight(115);
    layout->addWidget(scope);

    currentTags_ = new QPlainTextEdit(this);
    currentTags_->setObjectName("TagEditorCurrentTags");
    currentTags_->setAccessibleName("Current tags and file status");
    currentTags_->setReadOnly(true);
    currentTags_->setPlainText("Choose Show current tags to inspect the selected files.");
    currentTags_->setMinimumHeight(130);
    layout->addWidget(currentTags_);

    tags_ = new QPlainTextEdit(this);
    tags_->setObjectName("TagEditorLabels");
    tags_->setAccessibleName("Tag labels, one per line");
    tags_->setPlaceholderText("Books\nReading\nTo review");
    tags_->setMinimumHeight(85);
    layout->addWidget(tags_);

    status_ = new QLabel("Ready. A tag edit is one all-or-nothing catalog update.", this);
    status_->setObjectName("TagEditorStatus");
    status_->setAccessibleName("Tag edit status");
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* actions = new QHBoxLayout;
    inspect_ = new QPushButton("Show current tags", this);
    inspect_->setObjectName("TagEditorInspect");
    add_ = new QPushButton("Add tags", this);
    add_->setObjectName("TagEditorAdd");
    remove_ = new QPushButton("Remove tags", this);
    remove_->setObjectName("TagEditorRemove");
    stop_ = new QPushButton("Stop pending work", this);
    stop_->setObjectName("TagEditorStop");
    auto* close = new QPushButton("Close", this);
    close->setObjectName("TagEditorClose");
    actions->addWidget(inspect_);
    actions->addWidget(add_);
    actions->addWidget(remove_);
    actions->addWidget(stop_);
    actions->addStretch();
    actions->addWidget(close);
    layout->addLayout(actions);

    connect(inspect_, &QPushButton::clicked, this, [this] { startJob(Action::Inspect); });
    connect(add_, &QPushButton::clicked, this, [this] { startEdit(false); });
    connect(remove_, &QPushButton::clicked, this, [this] { startEdit(true); });
    connect(stop_, &QPushButton::clicked, this, [this] {
        if (running_) {
            running_->cancelled.store(true);
            status_->setText("Stopping after the current metadata check. "
                "An already-started catalog commit may finish; its result will be reported.");
            updateControls();
        }
    });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(tags_, &QPlainTextEdit::textChanged, this, [this] { updateControls(); });
    updateControls();
}

TagEditorDialog::~TagEditorDialog()
{
    if (running_)
        running_->cancelled.store(true);
}

void TagEditorDialog::updateControls()
{
    const bool idle = !running_;
    const bool validScope = !selectedPaths_.isEmpty()
        && selectedPaths_.size() <= MaximumSelectedFiles;
    const QString input = tags_->toPlainText();
    const bool hasTags = input.size() <= 4096 && !enteredTags(input).isEmpty();
    tags_->setEnabled(idle);
    inspect_->setEnabled(idle && validScope);
    add_->setEnabled(idle && validScope && hasTags);
    remove_->setEnabled(idle && validScope && hasTags);
    stop_->setEnabled(!idle && !running_->cancelled.load());
}

void TagEditorDialog::startEdit(bool remove)
{
    if (running_ || selectedPaths_.isEmpty())
        return;
    const QString input = tags_->toPlainText();
    if (input.size() > 4096) {
        status_->setText("The tag input is too long. Use at most 32 labels of 80 characters each.");
        return;
    }
    const QStringList labels = enteredTags(input);
    if (labels.isEmpty()) {
        status_->setText("Enter at least one tag label.");
        return;
    }
    startJob(remove ? Action::Remove : Action::Add, labels);
}

void TagEditorDialog::startJob(Action action, QStringList labels)
{
    if (running_ || selectedPaths_.isEmpty())
        return;
    if (selectedPaths_.size() > MaximumSelectedFiles) {
        status_->setText("Select at most 10,000 files per tag operation.");
        return;
    }
    auto job = std::make_shared<EditJob>();
    job->action = action;
    job->paths = selectedPaths_;
    job->labels = std::move(labels);
    job->storagePath = storagePath_;
    running_ = job;
    currentTags_->setPlainText("Checking the selected files…");
    status_->setText(QString("Checking %1 selected file identities in the background…")
        .arg(job->paths.size()));
    updateControls();

    auto* worker = QThread::create([job] {
        tags::TagCatalog catalog(job->storagePath);
        if (!catalog.load(&job->error))
            return;
        QList<tags::SelectedFile> selected;
        if (job->action != Action::Inspect)
            selected.reserve(job->paths.size());
        QStringList visible;
        QMap<QString, QPair<QString, int>> tagCounts;
        int tagged = 0;
        int untagged = 0;
        int stale = 0;
        int unavailable = 0;
        for (const QString& path : job->paths) {
            if (job->cancelled.load()) {
                job->error = QStringLiteral("Tag operation cancelled before writing.");
                return;
            }
            const auto snapshot = files::captureSourceSnapshot(path);
            const bool supported = snapshot.valid && snapshot.fileIndex != 0
                && !(snapshot.attributes & (DirectoryAttribute | ReparsePointAttribute));
            if (job->action == Action::Inspect) {
                QString description;
                if (!supported) {
                    ++unavailable;
                    description = QStringLiteral("Unavailable, folder or placeholder");
                } else {
                    const tags::TagMatch match = catalog.match(path, snapshot);
                    switch (match.state) {
                    case tags::TagMatchState::Current:
                        ++tagged;
                        description = match.tags.join(", ");
                        for (const QString& label : match.tags) {
                            const QString key = label.toCaseFolded();
                            auto it = tagCounts.find(key);
                            if (it == tagCounts.end())
                                tagCounts.insert(key, QPair<QString, int>{label, 1});
                            else
                                ++it.value().second;
                        }
                        break;
                    case tags::TagMatchState::Unassigned:
                        ++untagged;
                        description = QStringLiteral("No tags");
                        break;
                    case tags::TagMatchState::Replaced:
                        ++stale;
                        description = QStringLiteral("Replaced; stored tags are not active");
                        break;
                    case tags::TagMatchState::Modified:
                        ++stale;
                        description = QStringLiteral("File changed; review its tags");
                        break;
                    case tags::TagMatchState::Unavailable:
                        ++unavailable;
                        description = QStringLiteral("Catalog unavailable");
                        break;
                    }
                }
                if (visible.size() < MaximumVisibleRows)
                    visible.append(nativePath(path) + QStringLiteral(" — ") + description);
                continue;
            }
            if (!supported) {
                job->error = QStringLiteral("A selected item is unavailable, a folder, or a placeholder: ")
                    + nativePath(path);
                return;
            }
            selected.append({path, snapshot});
        }
        if (job->action == Action::Inspect) {
            QStringList lines;
            lines.append(QString("Current tags: %1 files; no tags: %2; replaced or stale: %3; unavailable: %4.")
                .arg(tagged).arg(untagged).arg(stale).arg(unavailable));
            QStringList counts;
            for (auto it = tagCounts.cbegin(); it != tagCounts.cend()
                    && counts.size() < MaximumVisibleLabels; ++it) {
                counts.append(QString("%1 (%2)").arg(it.value().first).arg(it.value().second));
            }
            QString countSummary = counts.isEmpty() ? QStringLiteral("none") : counts.join(", ");
            if (tagCounts.size() > counts.size())
                countSummary += QString(" … and %1 more labels").arg(tagCounts.size() - counts.size());
            lines.append(QStringLiteral("Tags across selection: ") + countSummary);
            lines.append(QString{});
            lines.append(visible);
            if (job->paths.size() > visible.size())
                lines.append(QString("… and %1 more selected files")
                    .arg(job->paths.size() - visible.size()));
            job->inspection = lines.join('\n');
            job->ok = true;
            return;
        }
        if (job->cancelled.load()) {
            job->error = QStringLiteral("Tag operation cancelled before writing.");
            return;
        }
        job->ok = job->action == Action::Remove
            ? catalog.removeTags(selected, job->labels, &job->error)
            : catalog.addTags(selected, job->labels, &job->error);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, job] {
        running_.reset();
        updateControls();
        if (job->ok && job->action == Action::Inspect) {
            currentTags_->setPlainText(job->inspection);
            status_->setText(QString("Inspected %1 selected files. No tags or file contents were changed.")
                .arg(job->paths.size()));
        } else if (job->ok) {
            currentTags_->setPlainText("Catalog changed. Choose Show current tags to refresh.");
            status_->setText(QString("Tag %1 completed for %2 selected files. "
                "Existing labels not named here were preserved; file contents and locations were unchanged.")
                .arg(job->action == Action::Remove ? "removal" : "addition")
                .arg(job->paths.size()));
        } else {
            currentTags_->setPlainText("Catalog state is not verified. Choose Show current tags after resolving the error.");
            status_->setText(job->error.isEmpty()
                ? QStringLiteral("Tag operation failed; no catalog update was committed.")
                : job->error);
        }
        if (closePending_) {
            closePending_ = false;
            if (job->ok && job->action != Action::Inspect)
                QMessageBox::information(this, "Tag edit completed", status_->text());
            else if (!job->ok && job->error != QStringLiteral("Tag operation cancelled before writing."))
                QMessageBox::warning(this, "Tag operation failed", status_->text());
            QDialog::done(QDialog::Rejected);
        }
    });
    worker->start();
}

void TagEditorDialog::closeEvent(QCloseEvent* event)
{
    if (running_) {
        done(QDialog::Rejected);
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void TagEditorDialog::done(int result)
{
    if (running_) {
        closePending_ = true;
        running_->cancelled.store(true);
        status_->setText("Waiting for the current metadata check or catalog commit to finish…");
        updateControls();
        return;
    }
    QDialog::done(result);
}

} // namespace filemanager::ui
