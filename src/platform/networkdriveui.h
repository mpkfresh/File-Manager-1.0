#pragma once

#include <QString>
#include <QtGlobal>

namespace filemanager::platform {

enum class NetworkDriveDialogResult
{
    Completed,
    Cancelled,
    Failed,
    Unsupported
};

bool networkDriveDialogsSupported();
NetworkDriveDialogResult showMapNetworkDriveDialog(quintptr owner, QString* error = nullptr);
NetworkDriveDialogResult showDisconnectNetworkDriveDialog(quintptr owner, QString* error = nullptr);

} // namespace filemanager::platform
