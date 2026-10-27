#pragma once

#include <QString>
#include <QStringList>

namespace filemanager::platform {

struct ShellLinkBatchResult final
{
    QStringList createdPaths;
    QStringList errors;

    bool success() const { return errors.isEmpty(); }
};

// Creates ordinary Windows .lnk files through IShellLink/IPersistFile. Existing files are never
// replaced; colliding names receive a numeric suffix.
ShellLinkBatchResult createShellLinks(
    const QStringList& absoluteTargets, const QString& destinationDirectory);

} // namespace filemanager::platform
