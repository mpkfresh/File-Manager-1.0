#pragma once

#include "files/transfercontroller.h"
#include "files/transferqueue.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QTimer>

class QLabel;
class QProgressBar;
class QPushButton;
class QWidget;

namespace filemanager::ui {

class SpeedGraph;

class TransferProgressDialog final : public QDialog
{
    Q_OBJECT

public:
    TransferProgressDialog(bool move, const QStringList& sources,
        const QString& destination, bool removableStorage, QWidget* parent = nullptr);

    void setProgress(const filemanager::files::TransferProgress& progress);
    void setJobState(filemanager::files::TransferJobState state);

signals:
    void pauseRequested();
    void resumeRequested();
    void cancelRequested();

private:
    void renderProgress();
    void setExpanded(bool expanded);

    QLabel* currentFile_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* transferred_ = nullptr;
    QLabel* speed_ = nullptr;
    QLabel* remaining_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QPushButton* pause_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* detailsToggle_ = nullptr;
    QWidget* details_ = nullptr;
    SpeedGraph* graph_ = nullptr;
    QTimer refreshTimer_;
    QElapsedTimer graphClock_;
    filemanager::files::TransferProgress latest_;
    filemanager::files::TransferJobState state_ =
        filemanager::files::TransferJobState::Queued;
    bool hasProgress_ = false;
};

} // namespace filemanager::ui
