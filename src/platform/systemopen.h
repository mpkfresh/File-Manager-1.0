#pragma once

#include <QString>
#include <QtGlobal>

namespace filemanager::platform {

struct SystemOpenResult final
{
    bool success = false;
    quintptr nativeCode = 0;
    QString error;
};

// Delegates to the Windows Shell without suppressing its association, reputation,
// attachment-manager, SmartScreen, or Mark-of-the-Web user interface.
SystemOpenResult openWithSystemShell(const QString& absolutePath);

} // namespace filemanager::platform
