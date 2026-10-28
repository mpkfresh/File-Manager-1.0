#include "mainwindow.h"
#include "ui/applicationtheme.h"
#include "ui/brandidentity.h"
#include "ui/firstlaunchintegrationdialog.h"
#include "platform/singleinstancecoordinator.h"
#include "platform/folderlaunch.h"
#include "platform/wineintegration.h"
#include "settings/settingshealth.h"
#include "preview/sandboxedpreviewprocess.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(filemanager::ui::BrandIdentity::productName());
    app.setApplicationDisplayName(
        QStringLiteral("%1 %2").arg(filemanager::ui::BrandIdentity::productName(),
            QStringLiteral(FILE_MANAGER_DISPLAY_VERSION)));
    app.setApplicationVersion(FILE_MANAGER_DISPLAY_VERSION);
    app.setOrganizationName(QStringLiteral(FILE_MANAGER_PUBLISHER));
    app.setWindowIcon(filemanager::ui::BrandIdentity::applicationIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription("A fast multi-panel file manager for Windows.");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption openFolder(QStringLiteral("open-folder"),
        QStringLiteral("Open only existing folders; report unavailable targets without startup fallback."));
    parser.addOption(openFolder);
    QCommandLineOption deploymentProbe(QStringLiteral("deployment-probe"),
        QStringLiteral("Verify the packaged runtime without opening the UI or user settings."));
    parser.addOption(deploymentProbe);
    QCommandLineOption previewSandboxProbe(QStringLiteral("preview-sandbox-probe"),
        QStringLiteral("Verify the packaged preview sandbox against a local fixture."),
        QStringLiteral("file"));
    parser.addOption(previewSandboxProbe);
    parser.addPositionalArgument(
        "path", "File or folder to open. Additional paths open in tabs.", "[path...]");
    parser.process(app);
    if (parser.isSet(deploymentProbe)) {
        const QDir runtime(QCoreApplication::applicationDirPath());
        const QStringList required{
            QStringLiteral("FileManagerShellHost.exe"),
            QStringLiteral("FileManagerPreviewHost.exe"),
            QStringLiteral("FileManagerContextMenuHost.exe"),
            QStringLiteral("FileManagerMaintenance.exe"),
            QStringLiteral("Qt6Core.dll"),
            QStringLiteral("Qt6Widgets.dll"),
            QStringLiteral("Qt6Svg.dll"),
            QStringLiteral("iconengines/qsvgicon.dll"),
            QStringLiteral("imageformats/qsvg.dll"),
            QStringLiteral("imageformats/qico.dll"),
            QStringLiteral("platforms/qwindows.dll"),
            QStringLiteral("sqldrivers/qsqlite.dll"),
            QStringLiteral("tls/qschannelbackend.dll"),
            QStringLiteral("licenses/Qt-LICENSE.txt"),
            QStringLiteral("sbom/qtbase-6.11.2.cdx.json"),
            QStringLiteral("THIRD_PARTY_NOTICES.md"),
            QStringLiteral("SECURITY.md")};
        for (const QString& relative : required) {
            if (!QFileInfo::exists(runtime.filePath(relative)))
                return 2;
        }
        // The build uses Qt's vendor SBOM. A 1.0 package replaces it with
        // the provenance record for the source-built media files it ships.
        if (!QFileInfo::exists(runtime.filePath(
                QStringLiteral("sbom/qtmultimedia-6.11.2.cdx.json")))
            && !QFileInfo::exists(runtime.filePath(
                QStringLiteral("manifest/media-provenance.json"))))
            return 2;
        // Exercise the deployed SVG plugin, including rendering beyond nominal size.
        for (const int side : {16, 256, 2048}) {
            const QPixmap logo = filemanager::ui::BrandIdentity::logo(QSize(side, side));
            if (logo.isNull() || qMax(logo.width(), logo.height()) != side)
                return 5;
            if (side <= 256
                && app.windowIcon().pixmap(QSize(side, side), 1.0).size() != QSize(side, side))
                return 6;
        }
        return 0;
    }
    if (parser.isSet(previewSandboxProbe)) {
        const QString source = QFileInfo(parser.value(previewSandboxProbe)).absoluteFilePath();
        if (!QFileInfo(source).isFile())
            return 3;
        const auto result = filemanager::preview::runSandboxedPreviewHost(
            source, QStringLiteral("probe"), 5000, 1024 * 1024);
        QJsonParseError parseError;
        const QJsonObject evidence = QJsonDocument::fromJson(
            result.standardOutput, &parseError).object();
        return result.success && result.sandboxed && result.jobRestricted
                && parseError.error == QJsonParseError::NoError
                && evidence.value(QStringLiteral("appContainer")).toBool()
                && evidence.value(QStringLiteral("job")).toBool()
                && evidence.value(QStringLiteral("capabilityCount")).toInteger(-1) == 0
                && evidence.value(QStringLiteral("readable")).toBool()
                && evidence.value(QStringLiteral("writeDenied")).toBool()
            ? 0 : 4;
    }

    const bool foldersOnly = parser.isSet(openFolder);
    QStringList launchPaths = parser.positionalArguments();
    if (foldersOnly) {
        const auto request = filemanager::platform::resolveFolderLaunch(launchPaths);
        if (!request.error.isEmpty()) {
            QMessageBox::warning(nullptr, "Could not open folder", request.error);
            return 64;
        }
        launchPaths = request.folders;
    }

    const auto settingsHealth = filemanager::settings::repairSettings();
    app.setProperty("settingsRecovered", settingsHealth.repaired());
    filemanager::ui::applyApplicationTheme(app);

    filemanager::platform::SingleInstanceCoordinator instance;
    QString instanceError;
    const auto role = instance.startOrForward(launchPaths, &instanceError, foldersOnly);
    if (role == filemanager::platform::SingleInstanceCoordinator::Role::Secondary)
        return 0;
    if (role == filemanager::platform::SingleInstanceCoordinator::Role::Error && foldersOnly) {
        QMessageBox::warning(nullptr, "Could not open folder",
            "File Manager could not deliver this folder request. Close the existing instance and try again.\n\n"
            + instanceError);
        return 65;
    }
    if (role == filemanager::platform::SingleInstanceCoordinator::Role::Error)
        qWarning("Single-instance coordination failed: %s", qPrintable(instanceError));

    QString initialLocation;
    if (foldersOnly) {
        // Revalidate after instance coordination, which can wait for a peer.
        const auto request = filemanager::platform::resolveFolderLaunch(launchPaths);
        if (!request.error.isEmpty()) {
            QMessageBox::warning(nullptr, "Could not open folder", request.error);
            return 64;
        }
        initialLocation = request.folders.constFirst();
    } else {
        for (const QString& rawPath : launchPaths) {
            const QFileInfo info(QDir::cleanPath(QDir::fromNativeSeparators(rawPath)));
            if (!info.exists())
                continue;
            initialLocation = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
            break;
        }
    }
    const QString applicationPath = QCoreApplication::applicationFilePath();
    filemanager::ui::prepareFirstLaunchIntegrationOffer(applicationPath);
    MainWindow window(initialLocation);
    QObject::connect(&instance,
        &filemanager::platform::SingleInstanceCoordinator::pathsReceived,
        &window, [&window](const QStringList& paths) {
            window.openInitialPaths(paths);
            if (window.isMinimized())
                window.showNormal();
            window.show();
            window.raise();
            window.activateWindow();
        });
    QObject::connect(&instance,
        &filemanager::platform::SingleInstanceCoordinator::foldersReceived,
        &window, [&window](const QStringList& paths) {
            // Recheck in the receiving process; a target can disappear after
            // the sender validates it. Do not fall back to opening file parents.
            const auto request = filemanager::platform::resolveFolderLaunch(paths);
            if (!request.error.isEmpty()) {
                QMessageBox::warning(&window, "Could not open folder", request.error);
                return;
            }
            window.openInitialPaths(request.folders);
            if (window.isMinimized()) window.showNormal();
            window.show();
            window.raise();
            window.activateWindow();
        });
    window.openInitialPaths(launchPaths);
    window.show();
    if (!foldersOnly && launchPaths.isEmpty()) {
        QTimer::singleShot(0, &window, [&window, applicationPath] {
            filemanager::ui::offerFirstLaunchIntegration(applicationPath, &window);
        });
    }
    // Only a previously enabled integration may launch the resident helper. Startup checks
    // and its bounded readiness handshake must not block navigation on the GUI thread.
    (void)QtConcurrent::run([applicationPath] {
        filemanager::platform::startConsentedWinEHelper(applicationPath);
    });
    return app.exec();
}
