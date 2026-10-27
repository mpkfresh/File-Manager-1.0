#pragma once

#include "files/tvorganizer.h"

#include <QDialog>
#include <memory>

namespace filemanager::files { class TransferQueue; }

namespace filemanager::ui {

class TvOrganizerDialog final : public QDialog
{
public:
    explicit TvOrganizerDialog(const QString& initialDirectory, QWidget* parent = nullptr,
        files::TransferQueue* transferQueue = nullptr);
    ~TvOrganizerDialog() override;
    void setScope(const QStringList& roots, const QString& showsDirectory);
    bool scanPending() const;
    bool reviewPending() const;
    quint64 queuedJobId() const;
    const files::TvOrganizationPlan& reviewedPlan() const;
    QList<files::TvMoveCandidate> selectedCandidates() const;
    void done(int result) override;

private:
    void startScan();
    void startMoveReview();
    void cancelScan();
    void invalidateReview();
    void updateControls();
    void updateSummary();
    class State;
    std::unique_ptr<State> state_;
};

void showTvOrganizerHelp(QWidget* parent);

} // namespace filemanager::ui
