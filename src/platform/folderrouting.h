#pragma once
#include <QString>

namespace filemanager::platform {
enum class FolderRoutingState { Off, Enabled, RecoveryNeeded, Conflict, OtherInstallation, Unavailable };
struct FolderRoutingStatus final {
    FolderRoutingState state = FolderRoutingState::Off;
    bool owned = false;
    bool canEnable = false;
    QString detail;
};
struct FolderRoutingResult final { bool success = false; QString error; };
QString folderRoutingCommand(const QString& executable);
FolderRoutingStatus folderRoutingStatus(const QString& executable, const QString& fixtureRoot = {});
FolderRoutingResult setFolderRouting(bool enabled, const QString& executable,
    const QString& fixtureRoot = {});
} // namespace filemanager::platform
