#pragma once

#include <QDialog>

#include <memory>

namespace filemanager::ui {

class ContentSearchDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit ContentSearchDialog(const QString& searchRoot, QWidget* parent = nullptr);
    ~ContentSearchDialog() override;

    void removeResults(const QStringList& paths);

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
