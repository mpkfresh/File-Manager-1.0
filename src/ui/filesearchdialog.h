#pragma once

#include <QDialog>

#include <memory>

namespace filemanager::search {
class SessionSearchHistory;
}

namespace filemanager::ui {

class FileSearchDialog final : public QDialog
{
    Q_OBJECT

public:
    FileSearchDialog(const QString& initialRoot,
        search::SessionSearchHistory* sessionHistory, QWidget* parent = nullptr,
        const QString& savedSearchStoragePath = {}, const QString& indexDatabasePath = {});
    ~FileSearchDialog() override;

    void removeResults(const QStringList& paths);
    bool activateSavedSearch(const QString& id);

signals:
    void openRequested(const QString& path);
    void revealRequested(const QString& path);
    void clipboardRequested(const QStringList& paths, bool cut);
    void shareRequested(const QStringList& paths);
    void recycleRequested(const QStringList& paths);

private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace filemanager::ui
