#pragma once

#include <QObject>

namespace filemanager::settings {

struct ShellPermissions
{
    bool allowThirdPartyNamespaces = false;
};

// Process-wide revocation reaches panes in every window. Grants themselves remain pane-local.
class ShellPermissionNotifier final : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
signals:
    void changed();
};
ShellPermissionNotifier* shellPermissionNotifier();

ShellPermissions loadShellPermissions();
void saveShellPermissions(const ShellPermissions& permissions);

} // namespace filemanager::settings
