#pragma once

#include "platform/folderrouting.h"
#include "platform/wineintegration.h"

#include <QDialog>
#include <QString>

class QCheckBox;
class QPushButton;

namespace filemanager::ui {

class FirstLaunchIntegrationDialog final : public QDialog
{
public:
    struct Choice {
        bool folders = false;
        bool winE = false;
        bool winEAtSignIn = false;
    };

    FirstLaunchIntegrationDialog(const platform::FolderRoutingStatus& folders,
        const platform::WinEStatus& winE, QWidget* parent = nullptr);
    Choice choice() const;

private:
    QCheckBox* folders_ = nullptr;
    QCheckBox* winE_ = nullptr;
    QCheckBox* winEAtSignIn_ = nullptr;
    QPushButton* apply_ = nullptr;
};

// Only a newly used, installed per-user profile becomes eligible. A deferred
// folder launch never interrupts navigation; the offer waits for an ordinary launch.
void prepareFirstLaunchIntegrationOffer(const QString& applicationPath);
void offerFirstLaunchIntegration(const QString& applicationPath, QWidget* parent);

} // namespace filemanager::ui
