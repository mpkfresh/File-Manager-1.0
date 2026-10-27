#include "ui/shortcuteditordialog.h"

#include "commands/commandregistry.h"

#include <algorithm>

#include <QAction>
#include <QDialogButtonBox>
#include <QHash>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

namespace filemanager::ui {

ShortcutEditorDialog::ShortcutEditorDialog(
    filemanager::commands::CommandRegistry& commands, QWidget* parent)
    : QDialog(parent)
    , commands_(commands)
    , actions_(commands.actions())
{
    setObjectName("ShortcutEditorDialog");
    setWindowTitle("Keyboard shortcuts");
    resize(680, 520);

    std::sort(actions_.begin(), actions_.end(), [this](QAction* left, QAction* right) {
        return commands_.descriptor(left).title.localeAwareCompare(
                   commands_.descriptor(right).title) < 0;
    });

    auto* layout = new QVBoxLayout(this);
    table_ = new QTableWidget(this);
    table_->setObjectName("ShortcutEditorTable");
    table_->setColumnCount(3);
    table_->setHorizontalHeaderLabels({"Command", "Category", "Shortcut"});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->setRowCount(actions_.size());

    for (int row = 0; row < actions_.size(); ++row) {
        QAction* action = actions_.at(row);
        const auto descriptor = commands_.descriptor(action);
        auto* title = new QTableWidgetItem(descriptor.title);
        title->setData(Qt::UserRole, descriptor.id);
        table_->setItem(row, 0, title);
        table_->setItem(row, 1, new QTableWidgetItem(descriptor.category));
        table_->setCellWidget(row, 2, new QKeySequenceEdit(action->shortcut(), table_));
    }
    layout->addWidget(table_);

    auto* reset = new QPushButton("Reset defaults", this);
    reset->setObjectName("ShortcutEditorResetDefaults");
    layout->addWidget(reset, 0, Qt::AlignLeft);
    connect(reset, &QPushButton::clicked, this, [this] { resetDefaults(); });

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName("ShortcutEditorButtons");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { applyChanges(); });
}

void ShortcutEditorDialog::resetDefaults()
{
    for (int row = 0; row < actions_.size(); ++row) {
        auto* editor = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(row, 2));
        editor->setKeySequence(QKeySequence::fromString(
            actions_.at(row)->property("defaultShortcut").toString(),
            QKeySequence::PortableText));
    }
}

void ShortcutEditorDialog::applyChanges()
{
    QHash<QString, QString> owners;
    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto* editor = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(row, 2));
        const QString key = editor->keySequence().toString(QKeySequence::PortableText);
        if (!key.isEmpty() && owners.contains(key)) {
            QMessageBox::warning(this, "Shortcut conflict",
                QString("%1 is assigned to both %2 and %3.")
                    .arg(key, owners.value(key), table_->item(row, 0)->text()));
            return;
        }
        if (!key.isEmpty())
            owners.insert(key, table_->item(row, 0)->text());
    }

    QSettings settings;
    for (int row = 0; row < table_->rowCount(); ++row) {
        const QString id = table_->item(row, 0)->data(Qt::UserRole).toString();
        const auto* editor = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(row, 2));
        const QString key = editor->keySequence().toString(QKeySequence::PortableText);
        commands_.action(id)->setShortcut(editor->keySequence());
        settings.setValue("shortcuts/" + id, key);
    }
    settings.sync();
    accept();
}

} // namespace filemanager::ui
