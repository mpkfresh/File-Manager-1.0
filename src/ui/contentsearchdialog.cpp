#include "contentsearchdialog.h"

#include "search/contentsearchservice.h"
#include "ui/localizedformat.h"
#include "ui/searchresultbar.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

namespace filemanager::ui {

class ContentSearchDialog::State
{
public:
    QString root;
    QLineEdit* query = nullptr;
    QLineEdit* extension = nullptr;
    QListWidget* results = nullptr;
    SearchResultBar* actions = nullptr;
    QLabel* empty = nullptr;
    QLabel* status = nullptr;
    search::ContentSearchService service;
    quint64 activeGeneration = 0;
};

ContentSearchDialog::ContentSearchDialog(const QString& searchRoot, QWidget* parent)
    : QDialog(parent), state_(std::make_unique<State>())
{
    state_->root = QDir::cleanPath(searchRoot);
    setObjectName("ContentSearchDialog");
    setWindowTitle("Search file contents");
    resize(820, 580);
    auto* layout = new QVBoxLayout(this);
    auto* disclosure = new QLabel(
        "On-demand only: files are read during this search. Contents and results are not indexed or saved.",
        this);
    disclosure->setObjectName("ContentSearchDisclosure");
    disclosure->setWordWrap(true);
    auto* row = new QHBoxLayout;
    state_->query = new QLineEdit(this);
    state_->extension = new QLineEdit(this);
    auto* searchButton = new QPushButton("Search", this);
    auto* cancelButton = new QPushButton("Cancel", this);
    state_->results = new QListWidget(this);
    state_->status = new QLabel(this);
    state_->empty = new QLabel("Enter text above to search inside eligible files.", this);
    state_->empty->setObjectName("ContentSearchEmpty");
    state_->empty->setAccessibleName("Content search guidance");
    state_->empty->setTextFormat(Qt::PlainText);
    state_->empty->setWordWrap(true);
    state_->empty->setAlignment(Qt::AlignCenter);
    state_->query->setObjectName("ContentSearchQuery");
    state_->query->setPlaceholderText("Text inside files");
    state_->extension->setObjectName("ContentSearchExtension");
    state_->extension->setPlaceholderText("Extension (optional)");
    state_->extension->setMaximumWidth(180);
    searchButton->setObjectName("ContentSearchStart");
    cancelButton->setObjectName("ContentSearchCancel");
    state_->status->setObjectName("ContentSearchStatus");
    row->addWidget(state_->query, 1);
    row->addWidget(state_->extension);
    row->addWidget(searchButton);
    row->addWidget(cancelButton);
    layout->addWidget(disclosure);
    layout->addLayout(row);
    layout->addWidget(state_->results, 1);
    layout->addWidget(state_->empty, 1);
    state_->results->setObjectName("ContentSearchResults");
    state_->results->setAccessibleName("Content search results");
    state_->actions = new SearchResultBar(state_->results, this);
    layout->addWidget(state_->actions);
    state_->results->setVisible(false);
    state_->actions->setVisible(false);
    layout->addWidget(state_->status);
    state_->status->setVisible(false);

    connect(searchButton, &QPushButton::clicked, this, [this] {
        const QString text = state_->query->text();
        if (text.isEmpty())
            return;
        state_->results->clear();
        state_->empty->setVisible(false);
        state_->results->setVisible(true);
        state_->actions->setVisible(true);
        state_->status->setText("Reading eligible text files in the background...");
        state_->status->setVisible(true);
        state_->activeGeneration = state_->service.start(
            state_->root, text, state_->extension->text());
    });
    connect(state_->query, &QLineEdit::returnPressed,
        searchButton, &QPushButton::click);
    connect(cancelButton, &QPushButton::clicked,
        &state_->service, &search::ContentSearchService::cancel);
    connect(&state_->service, &search::ContentSearchService::resultFound, this,
        [this](quint64 generation, const search::ContentSearchHit& hit) {
            if (generation != state_->activeGeneration)
                return;
            auto* item = new QListWidgetItem(QString("%1:%2\n%3")
                .arg(QDir::toNativeSeparators(hit.path)).arg(hit.line).arg(hit.snippet),
                state_->results);
            item->setData(Qt::UserRole, hit.path);
            item->setToolTip(hit.path);
        });
    connect(&state_->service, &search::ContentSearchService::finished, this,
        [this](quint64 generation, bool cancelled, int count, int skipped) {
            if (generation == state_->activeGeneration) {
                state_->status->setText(cancelled ? "Search cancelled."
                    : QString("%1; %2 skipped as binary, oversized, symbolic-link, or unreadable.")
                        .arg(localizedCount(count, "result", "results"),
                            localizedFiles(skipped)));
                if (count == 0) {
                    state_->empty->setText(cancelled
                        ? "Search cancelled. Enter another query to try again."
                        : "No matching text found in eligible files.");
                    state_->results->setVisible(false);
                    state_->actions->setVisible(false);
                    state_->empty->setVisible(true);
                    state_->status->setVisible(!cancelled && skipped > 0);
                }
            }
        });
    connect(state_->actions, &SearchResultBar::openRequested,
        this, &ContentSearchDialog::openRequested);
    connect(state_->results, &QListWidget::itemDoubleClicked, this,
        [this](QListWidgetItem* item) {
            emit openRequested(item->data(Qt::UserRole).toString());
        });
    connect(state_->actions, &SearchResultBar::revealRequested,
        this, &ContentSearchDialog::revealRequested);
    connect(state_->actions, &SearchResultBar::clipboardRequested,
        this, &ContentSearchDialog::clipboardRequested);
    connect(state_->actions, &SearchResultBar::shareRequested,
        this, &ContentSearchDialog::shareRequested);
    connect(state_->actions, &SearchResultBar::recycleRequested,
        this, &ContentSearchDialog::recycleRequested);
    state_->query->setFocus();
}

ContentSearchDialog::~ContentSearchDialog() = default;

void ContentSearchDialog::removeResults(const QStringList& paths)
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
    if (state_->results->count() == 0) {
        state_->empty->setText("No results remain. Search again to find more.");
        state_->results->setVisible(false);
        state_->actions->setVisible(false);
        state_->empty->setVisible(true);
        state_->status->setVisible(false);
    }
}

} // namespace filemanager::ui
