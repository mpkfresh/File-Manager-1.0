#include "ui/searchresultbar.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QSet>

namespace filemanager::ui {

SearchResultBar::SearchResultBar(QListWidget* results, QWidget* parent)
    : QWidget(parent), results_(results)
{
    Q_ASSERT(results_);
    setObjectName("SearchResultActions");
    setAccessibleName("Search result operations");
    results_->setSelectionMode(QAbstractItemView::ExtendedSelection);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    open_ = new QPushButton("Open", this);
    reveal_ = new QPushButton("Show in folder", this);
    copy_ = new QPushButton("Copy", this);
    cut_ = new QPushButton("Cut", this);
    share_ = new QPushButton("Share…", this);
    recycle_ = new QPushButton("Recycle…", this);
    open_->setObjectName("SearchResultOpen");
    reveal_->setObjectName("SearchResultReveal");
    copy_->setObjectName("SearchResultCopy");
    cut_->setObjectName("SearchResultCut");
    share_->setObjectName("SearchResultShare");
    recycle_->setObjectName("SearchResultRecycle");
    layout->addWidget(open_);
    layout->addWidget(reveal_);
    layout->addWidget(copy_);
    layout->addWidget(cut_);
    layout->addWidget(share_);
    layout->addWidget(recycle_);
    layout->addStretch(1);

    connect(results_, &QListWidget::itemSelectionChanged,
        this, &SearchResultBar::updateActions);
    connect(results_, &QListWidget::currentItemChanged,
        this, [this] { updateActions(); });
    connect(open_, &QPushButton::clicked, this, [this] {
        const QString path = currentPath();
        if (!path.isEmpty())
            emit openRequested(path);
    });
    connect(reveal_, &QPushButton::clicked, this, [this] {
        const QString path = currentPath();
        if (!path.isEmpty())
            emit revealRequested(path);
    });
    connect(copy_, &QPushButton::clicked, this,
        [this] { emit clipboardRequested(selectedPaths(), false); });
    connect(cut_, &QPushButton::clicked, this,
        [this] { emit clipboardRequested(selectedPaths(), true); });
    connect(share_, &QPushButton::clicked, this,
        [this] { emit shareRequested(selectedPaths()); });
    connect(recycle_, &QPushButton::clicked, this,
        [this] { emit recycleRequested(selectedPaths()); });
    updateActions();
}

QString SearchResultBar::currentPath() const
{
    const QListWidgetItem* item = results_->currentItem();
    if (!item)
        return {};
    const QFileInfo info(QDir::cleanPath(
        QDir::fromNativeSeparators(item->data(Qt::UserRole).toString())));
    return info.isAbsolute() ? info.absoluteFilePath() : QString{};
}

QStringList SearchResultBar::selectedPaths() const
{
    QList<QListWidgetItem*> items = results_->selectedItems();
    if (items.isEmpty() && results_->currentItem())
        items.append(results_->currentItem());
    QStringList paths;
    QSet<QString> seen;
    for (const QListWidgetItem* item : items) {
        const QFileInfo info(QDir::cleanPath(
            QDir::fromNativeSeparators(item->data(Qt::UserRole).toString())));
        if (!info.isAbsolute())
            continue;
        const QString absolute = info.absoluteFilePath();
        const QString comparison = QDir::toNativeSeparators(absolute).toCaseFolded();
        if (seen.contains(comparison))
            continue;
        seen.insert(comparison);
        paths.append(absolute);
    }
    return paths;
}

void SearchResultBar::updateActions()
{
    const bool hasCurrent = !currentPath().isEmpty();
    const bool hasSelection = !selectedPaths().isEmpty();
    open_->setEnabled(hasCurrent);
    reveal_->setEnabled(hasCurrent);
    copy_->setEnabled(hasSelection);
    cut_->setEnabled(hasSelection);
    share_->setEnabled(hasSelection);
    recycle_->setEnabled(hasSelection);
}

} // namespace filemanager::ui
