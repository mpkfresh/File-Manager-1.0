#pragma once

#include "platform/shellitem.h"

#include <QDialog>
#include <QList>

class QLabel;
class QListWidget;
class QPushButton;
class FilePane;

namespace filemanager::ui {

class ShellNamespaceBrowser final : public QDialog
{
    Q_OBJECT

public:
    explicit ShellNamespaceBrowser(
        const platform::ShellItemReference& root, QWidget* parent = nullptr);

signals:
    void fileSystemFolderChosen(const QString& path);
    void shellItemOpenRequested(const QString& parsingName);
    void copyShellItemRequested(const platform::ShellItemReference& item);

private:
    void updateActions();
    FilePane* pane_ = nullptr;
    QPushButton* copy_ = nullptr;
};

} // namespace filemanager::ui
