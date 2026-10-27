#include "settings/metadatapermissions.h"

#include <QSettings>

namespace filemanager::settings {

MetadataPermissions loadMetadataPermissions()
{
    return {QSettings().value("privacy/allowWindowsShellMetadataHandlers", false).toBool()};
}

void saveMetadataPermissions(const MetadataPermissions& permissions)
{
    QSettings().setValue(
        "privacy/allowWindowsShellMetadataHandlers", permissions.allowWindowsShellHandlers);
}

} // namespace filemanager::settings
