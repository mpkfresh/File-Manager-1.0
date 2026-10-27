#include "settings/shellpermissions.h"

#include <QSettings>
#include <QCoreApplication>

namespace filemanager::settings {

ShellPermissionNotifier* shellPermissionNotifier()
{
    static auto* notifier = new ShellPermissionNotifier(QCoreApplication::instance());
    return notifier;
}

ShellPermissions loadShellPermissions()
{
    return {QSettings().value("privacy/allowThirdPartyShellNamespaces", false).toBool()};
}

void saveShellPermissions(const ShellPermissions& permissions)
{
    QSettings().setValue(
        "privacy/allowThirdPartyShellNamespaces", permissions.allowThirdPartyNamespaces);
    emit shellPermissionNotifier()->changed();
}

} // namespace filemanager::settings
