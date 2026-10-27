#pragma once

#include <QStringList>

namespace filemanager::platform {

struct FolderLaunch final
{
    QStringList folders;
    QString error;
};

// Default-folder requests are all-or-nothing: never reinterpret a file as its
// parent or a missing target as the user's normal startup location.
FolderLaunch resolveFolderLaunch(const QStringList& paths);

} // namespace filemanager::platform
