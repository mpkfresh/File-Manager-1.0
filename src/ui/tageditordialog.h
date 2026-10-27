#pragma once

#include <QDialog>
#include <QStringList>
#include <memory>

class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace filemanager::ui {

class TagEditorDialog final : public QDialog
{
public:
    explicit TagEditorDialog(QStringList selectedPaths, QWidget* parent = nullptr,
        QString storagePath = {});
    ~TagEditorDialog() override;
    void done(int result) override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class Action { Inspect, Add, Remove };
    struct EditJob;
    void startEdit(bool remove);
    void startJob(Action action, QStringList labels = {});
    void updateControls();

    QStringList selectedPaths_;
    QString storagePath_;
    QPlainTextEdit* tags_ = nullptr;
    QPlainTextEdit* currentTags_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* inspect_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* stop_ = nullptr;
    std::shared_ptr<EditJob> running_;
    bool closePending_ = false;
};

} // namespace filemanager::ui