#include "ui/shellnamespacebrowser.h"
#include "ui/filepane.h"

#include <QDialogButtonBox>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace filemanager::ui {

ShellNamespaceBrowser::ShellNamespaceBrowser(
    const platform::ShellItemReference& root, QWidget* parent)
    : QDialog(parent)
{
    setObjectName("ShellNamespaceBrowser");
    setWindowTitle(root.displayName.isEmpty() ? QStringLiteral("Windows location") : root.displayName);
    resize(760, 500);
    auto* layout = new QVBoxLayout(this);
    // Reuse the pane's consent, visit expiry and cancellation boundary instead of maintaining
    // another namespace loader with different permission behavior.
    pane_ = new FilePane(QString::fromLatin1(FilePane::HomeIdentity), this);
    auto* items = pane_->findChild<QListWidget*>("ShellNamespacePaneItems");
    if (items)
        items->setObjectName("ShellNamespaceItems");
    layout->addWidget(pane_);
    auto* actions = new QDialogButtonBox(QDialogButtonBox::Close, this);
    copy_ = actions->addButton("Copy to current folder…", QDialogButtonBox::ActionRole);
    layout->addWidget(actions);
    connect(actions, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, pane_, [this] { pane_->navigateHome(); });
    connect(pane_, &FilePane::shellItemOpenRequested, this, &ShellNamespaceBrowser::shellItemOpenRequested);
    connect(pane_, &FilePane::pathChanged, this, [this](const QString& path) {
        emit fileSystemFolderChosen(path);
        accept();
    });
    connect(pane_, &FilePane::selectionChanged, this, [this] { updateActions(); });
    connect(pane_, &FilePane::itemCountChanged, this, [this] { updateActions(); });
    connect(copy_, &QPushButton::clicked, this, [this] {
        const auto selection = pane_->selectedShellItems();
        if (selection.size() == 1)
            emit copyShellItemRequested(selection.first());
    });
    // This dialog intentionally presents the supplied folder through the virtual surface.
    auto virtualRoot = root;
    virtualRoot.fileSystem = false;
    pane_->navigateShellItem(virtualRoot);
    updateActions();
}

void ShellNamespaceBrowser::updateActions()
{
    copy_->setEnabled(pane_->selectedShellItems().size() == 1);
}

} // namespace filemanager::ui
