#pragma once

#include "tags/tagmoveplan.h"

#include <QDialog>
#include <QStringList>

#include <memory>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
namespace filemanager::files { class TransferQueue; }

namespace filemanager::ui {

class TagDestinationDialog final : public QDialog
{
public:
    explicit TagDestinationDialog(QStringList selectedPaths, QWidget* parent = nullptr,
        QString storagePath = {}, files::TransferQueue* transferQueue = nullptr);
    ~TagDestinationDialog() override;

private:
    struct Job;
    void startPreview();
    void resolveSelection();
    void confirmMove();
    void invalidateReview();
    void showPreview();
    void setBusy(bool busy);

    QStringList selectedPaths_;
    QString storagePath_;
    tags::TagMovePlan preview_;
    std::shared_ptr<Job> running_;
    std::shared_ptr<Job> reviewed_;
    files::TransferQueue* transferQueue_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPlainTextEdit* result_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* resolve_ = nullptr;
    QPushButton* move_ = nullptr;
};

} // namespace filemanager::ui
