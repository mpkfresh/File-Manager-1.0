#pragma once

#include <QString>
#include <QStringList>

namespace filemanager::platform {

struct ContextMenuHostResult final
{
    bool success = false;
    bool cancelled = false;
    int nativeCode = 0;
    QString error;
};

ContextMenuHostResult showWindowsContextMenuIsolated(const QStringList& absolutePaths,
    int screenX, int screenY, int timeoutMilliseconds = 120000,
    const QString& helperExecutable = {});

} // namespace filemanager::platform
