#include "preview/installcleanup.h"
#include "platform/wineintegration.h"
#include "platform/folderrouting.h"
#include "platform/windowsfolderintegration.h"

#include <QCoreApplication>
#include <QTextStream>
#include <QDir>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral(FILE_MANAGER_PRODUCT_NAME));
    app.setOrganizationName(QStringLiteral(FILE_MANAGER_PUBLISHER));

    const QString applicationPath = QDir(app.applicationDirPath()).filePath("FileManager.exe");
    if (app.arguments() == QStringList{app.arguments().constFirst(), "--win-e-helper"})
        return filemanager::platform::runWinEHelper(applicationPath);

    if (app.arguments() != QStringList{app.arguments().constFirst(),
            QStringLiteral("--uninstall-cleanup")})
        return 64;

    QString error;
    const auto folders = filemanager::platform::folderRoutingStatus(applicationPath);
    if (folders.state == filemanager::platform::FolderRoutingState::Unavailable
        || folders.state == filemanager::platform::FolderRoutingState::Conflict) {
        QTextStream(stderr) << folders.detail << '\n';
        return 1;
    }
    if (folders.owned) {
        const auto result = filemanager::platform::setFolderRouting(false, applicationPath);
        if (!result.success) {
            QTextStream(stderr) << result.error << '\n';
            return 1;
        }
    }
    const auto winE = filemanager::platform::winEStatus(applicationPath);
    if (winE.manageable && (winE.enabled || winE.atSignIn)) {
        const auto result = filemanager::platform::setWinEIntegration(applicationPath, false, false);
        if (!result.success) {
            QTextStream(stderr) << result.error << '\n';
            return 1;
        }
    }
    const auto optional = filemanager::platform::removeWindowsIntegrationForInstallation(applicationPath);
    if (!optional.success) {
        QTextStream(stderr) << optional.error << '\n';
        return 1;
    }
    if (!filemanager::preview::removeInstalledPreviewSandbox(&error)) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    return 0;
}
