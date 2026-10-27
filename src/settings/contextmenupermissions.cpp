#include "settings/contextmenupermissions.h"

#include <QSettings>

namespace filemanager::settings {

ContextMenuPermissions loadContextMenuPermissions(const QSettings& settings)
{
    return {settings.value("privacy/allowWindowsContextMenuExtensions", false).toBool()};
}

ContextMenuPermissions loadContextMenuPermissions()
{
    return loadContextMenuPermissions(QSettings{});
}

void saveContextMenuPermissions(QSettings& settings, const ContextMenuPermissions& permissions)
{
    settings.setValue("privacy/allowWindowsContextMenuExtensions", permissions.allowWindowsExtensions);
}

void saveContextMenuPermissions(const ContextMenuPermissions& permissions)
{
    QSettings settings;
    saveContextMenuPermissions(settings, permissions);
}

} // namespace filemanager::settings
