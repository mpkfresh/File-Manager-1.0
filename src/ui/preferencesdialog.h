#pragma once

#include <QDialog>
#include <QStringList>

#include <functional>
#include <memory>

namespace filemanager::commands { class CommandRegistry; }
namespace filemanager::diagnostics { class AsyncDiagnosticLog; }
namespace filemanager::files { class OperationJournal; }
namespace filemanager::search { class SessionSearchHistory; }

namespace filemanager::ui {

class PreferencesDialog final : public QDialog
{
    Q_OBJECT

public:
    struct Dependencies {
        commands::CommandRegistry* commands = nullptr;
        files::OperationJournal* operationJournal = nullptr;
        search::SessionSearchHistory* sessionSearchHistory = nullptr;
        diagnostics::AsyncDiagnosticLog* diagnosticLog = nullptr;
        QStringList openPaths;
        std::function<void()> rebuildQuickAccess;
        std::function<void()> saveWorkspace;
        std::function<void(const QString&, int)> showStatus;
        QString fileLabelStoragePath;
    };

    explicit PreferencesDialog(Dependencies dependencies, QWidget* parent = nullptr);
    ~PreferencesDialog() override;

    bool singleClickActivation() const;
    bool selectionCheckBoxes() const;

protected:
    void accept() override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace filemanager::ui
