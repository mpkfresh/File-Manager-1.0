#pragma once

#include <QDialog>
#include <QList>

class QAction;
class QTableWidget;

namespace filemanager::commands {
class CommandRegistry;
}

namespace filemanager::ui {

class ShortcutEditorDialog final : public QDialog
{
public:
    explicit ShortcutEditorDialog(
        filemanager::commands::CommandRegistry& commands, QWidget* parent = nullptr);

private:
    void resetDefaults();
    void applyChanges();

    filemanager::commands::CommandRegistry& commands_;
    QList<QAction*> actions_;
    QTableWidget* table_ = nullptr;
};

} // namespace filemanager::ui
