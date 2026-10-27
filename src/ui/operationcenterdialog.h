#pragma once

#include <QDialog>

class QLabel;
class QPushButton;
class QTabWidget;
class QTableWidget;

namespace filemanager::files {
class OperationJournal;
class TransferQueue;
}

namespace filemanager::ui {

class OperationCenterDialog final : public QDialog
{
    Q_OBJECT

public:
    OperationCenterDialog(files::TransferQueue* transferQueue,
        files::OperationJournal* operationJournal, QWidget* parent = nullptr);

signals:
    void reviewRecoveryRequested();
    void statusMessageRequested(const QString& message, int timeoutMilliseconds);

private:
    quint64 selectedJob() const;
    void populate();
    void updateActions();

    files::TransferQueue* transferQueue_ = nullptr;
    files::OperationJournal* operationJournal_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* transfersEmpty_ = nullptr;
    QLabel* recoveryEmpty_ = nullptr;
    QLabel* recoveryExplanation_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTableWidget* transfers_ = nullptr;
    QTableWidget* recovery_ = nullptr;
    QPushButton* pause_ = nullptr;
    QPushButton* resume_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* retry_ = nullptr;
    QPushButton* reviewRecovery_ = nullptr;
};

} // namespace filemanager::ui
