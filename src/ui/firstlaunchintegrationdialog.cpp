#include "ui/firstlaunchintegrationdialog.h"

#include <QCheckBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace filemanager::ui {
namespace {
constexpr auto pendingKey = "guidance/preferredFileManagerOfferPending";
constexpr auto shownKey = "guidance/preferredFileManagerOfferShown";

QLabel* explanation(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    return label;
}

void addContextualHelp(QVBoxLayout* body, QWidget* parent,
    const QString& summary, const QString& detail, const QString& name)
{
    auto* row = new QHBoxLayout;
    auto* summaryLabel = explanation(summary, parent);
    summaryLabel->setObjectName(name + "Summary");
    row->setContentsMargins(22, 0, 0, 0);
    row->addWidget(summaryLabel, 1);
    auto* toggle = new QToolButton(parent);
    toggle->setObjectName(name + "HelpToggle");
    toggle->setText("Details");
    toggle->setCheckable(true);
    toggle->setAccessibleName("Show " + name + " details");
    row->addWidget(toggle, 0, Qt::AlignTop);
    body->addLayout(row);

    auto* detailLabel = explanation(detail, parent);
    detailLabel->setObjectName(name + "HelpText");
    detailLabel->setVisible(false);
    detailLabel->setIndent(22);
    body->addWidget(detailLabel);
    QObject::connect(toggle, &QToolButton::toggled, detailLabel,
        [toggle, detailLabel, name, parent](bool expanded) {
            detailLabel->setVisible(expanded);
            toggle->setText(expanded ? "Hide details" : "Details");
            toggle->setAccessibleName((expanded ? "Hide " : "Show ") + name + " details");
            parent->adjustSize();
        });
}
} // namespace

FirstLaunchIntegrationDialog::FirstLaunchIntegrationDialog(
    const platform::FolderRoutingStatus& folders, const platform::WinEStatus& winE,
    QWidget* parent) : QDialog(parent)
{
    setObjectName("FirstLaunchIntegrationDialog");
    setWindowTitle("Choose how folders open");
    setModal(true);

    auto* body = new QVBoxLayout(this);
    body->setSpacing(9);
    body->addWidget(explanation("Select either choice, or keep Windows as it is.", this));

    folders_ = new QCheckBox("Open supported folders and drives in File Manager", this);
    folders_->setObjectName("FirstLaunchFolders");
    folders_->setAccessibleName("Open supported folders and drives with File Manager");
    folders_->setEnabled(folders.canEnable && folders.state == platform::FolderRoutingState::Off);
    body->addWidget(folders_);
    addContextualHelp(body, this,
        folders_->isEnabled() ? QStringLiteral("Only for this account; other locations still use Explorer.")
                              : QStringLiteral("Unavailable on this account."),
        folders_->isEnabled()
            ? QStringLiteral("Explicit Explorer commands and virtual locations may still open "
                             "in Windows. You can restore your prior settings in Preferences.")
            : folders.detail, "Folders");

    winE_ = new QCheckBox("Open File Manager with Win+E", this);
    winE_->setObjectName("FirstLaunchWinE");
    winE_->setAccessibleName("Use File Manager for Win plus E");
    winE_->setEnabled(winE.manageable && !winE.enabled);
    body->addWidget(winE_);
    addContextualHelp(body, this,
        winE_->isEnabled() ? QStringLiteral("A local helper reads key events; it never saves or sends typing.")
                           : QStringLiteral("Unavailable on this account."),
        winE_->isEnabled()
            ? QStringLiteral("It uses them only to recognize Win+E and passes other keys through. "
                             "It never records or transmits typing. "
                             "You can turn it off in Preferences.")
            : winE.error, "WinE");

    winEAtSignIn_ = new QCheckBox("Start the shortcut helper when I sign in", this);
    winEAtSignIn_->setObjectName("FirstLaunchWinEAtSignIn");
    winEAtSignIn_->setAccessibleName("Start the Win plus E helper when I sign in");
    winEAtSignIn_->setEnabled(false);
    winEAtSignIn_->setVisible(false);
    body->addWidget(winEAtSignIn_);

    auto* buttons = new QHBoxLayout;
    auto* later = new QPushButton("Keep Windows as is", this);
    later->setObjectName("FirstLaunchNotNow");
    apply_ = new QPushButton("Apply choices", this);
    apply_->setObjectName("FirstLaunchApply");
    apply_->setEnabled(false);
    buttons->addWidget(later);
    buttons->addStretch();
    buttons->addWidget(apply_);
    body->addLayout(buttons);
    connect(later, &QPushButton::clicked, this, &QDialog::reject);
    connect(apply_, &QPushButton::clicked, this, &QDialog::accept);
    connect(folders_, &QCheckBox::toggled, this, [this] {
        apply_->setEnabled(folders_->isChecked() || winE_->isChecked());
    });
    const bool signInStartupAvailable = winE.error.isEmpty();
    if (!signInStartupAvailable)
        winEAtSignIn_->setToolTip(winE.error);
    connect(winE_, &QCheckBox::toggled, this, [this, signInStartupAvailable](bool checked) {
        winEAtSignIn_->setEnabled(checked && signInStartupAvailable);
        winEAtSignIn_->setVisible(checked && signInStartupAvailable);
        if (!checked) winEAtSignIn_->setChecked(false);
        apply_->setEnabled(folders_->isChecked() || checked);
        adjustSize();
    });

    const QScreen* screen = parent && parent->screen()
        ? parent->screen() : QGuiApplication::primaryScreen();
    if (screen) {
        const QRect available = screen->availableGeometry();
        const int dialogWidth = qMin(500, qMax(320, available.width() - 40));
        setMinimumWidth(dialogWidth);
        resize(dialogWidth, sizeHint().height());
    }
}

FirstLaunchIntegrationDialog::Choice FirstLaunchIntegrationDialog::choice() const
{
    return {folders_->isChecked(), winE_->isChecked(), winEAtSignIn_->isChecked()};
}

void prepareFirstLaunchIntegrationOffer(const QString& applicationPath)
{
    QSettings settings;
    if (settings.value(shownKey, false).toBool()
        || settings.value(pendingKey, false).toBool()
        || settings.value("guidance/firstRunHintShown", false).toBool()) return;
    const auto folders = platform::folderRoutingStatus(applicationPath);
    if (folders.state != platform::FolderRoutingState::Off || !folders.canEnable) return;
    if (platform::winEStatus(applicationPath).enabled) return;
    settings.setValue(pendingKey, true);
    settings.sync();
}

void offerFirstLaunchIntegration(const QString& applicationPath, QWidget* parent)
{
    QSettings settings;
    if (!settings.value(pendingKey, false).toBool()
        || settings.value(shownKey, false).toBool()) return;
    const auto folders = platform::folderRoutingStatus(applicationPath);
    const auto winE = platform::winEStatus(applicationPath);
    if ((!folders.canEnable || folders.state != platform::FolderRoutingState::Off)
        && (!winE.manageable || winE.enabled)) {
        settings.remove(pendingKey);
        return;
    }

    FirstLaunchIntegrationDialog dialog(folders, winE, parent);
    const bool apply = dialog.exec() == QDialog::Accepted;
    const auto choice = dialog.choice();
    settings.setValue(shownKey, true);
    settings.remove(pendingKey);
    settings.sync();
    if (!apply) return;

    QStringList errors;
    if (choice.folders) {
        const auto result = platform::setFolderRouting(true, applicationPath);
        if (!result.success) errors << QStringLiteral("Folder opening: ") + result.error;
    }
    if (choice.winE) {
        const auto result = platform::setWinEIntegration(
            applicationPath, true, choice.winEAtSignIn);
        if (!result.success) errors << QStringLiteral("Win+E: ") + result.error;
    }
    if (!errors.isEmpty()) {
        QMessageBox::warning(parent, "Some choices could not be applied",
            errors.join("\n\n") + "\n\nReview their current state in Preferences.");
    }
}

} // namespace filemanager::ui
