#pragma once

#include <QString>

namespace filemanager::platform {

struct FolderCommandStatus final
{
    bool registered = false;
    bool current = false;
    bool manageable = true;
    QString detail;
};

struct FolderCommandResult final
{
    bool success = false;
    QString error;
};

struct AppPathsStatus final
{
    bool registered = false;
    bool current = false;
    bool manageable = true;
    QString detail;
};

struct AppPathsResult final
{
    bool success = false;
    QString error;
};

QString windowsFolderCommandLine(const QString& executablePath);
FolderCommandStatus windowsFolderCommandStatus(
    const QString& executablePath, const QString& classesRoot = {});
FolderCommandResult setWindowsFolderCommandRegistered(bool enabled,
    const QString& executablePath, const QString& classesRoot = {});
AppPathsStatus windowsAppPathsStatus(
    const QString& executablePath, const QString& appPathsRoot = {});
AppPathsResult setWindowsAppPathsRegistered(bool enabled,
    const QString& executablePath, const QString& appPathsRoot = {});

// Uninstall only the optional registrations pointing to this executable. Unlike
// the interactive disable actions, retain foreign values and nonempty subkeys.
// A fixture root must be Software\FileManagerTests\FolderIntegration\<UUID>.
FolderCommandResult removeWindowsIntegrationForInstallation(
    const QString& executablePath, const QString& fixtureRoot = {});

} // namespace filemanager::platform
