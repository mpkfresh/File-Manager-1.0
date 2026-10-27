#pragma once

#include "platform/shellitem.h"

#include <QList>
#include <QString>

namespace filemanager::platform {

enum class ShellFileOperationKind { Copy, Move };

struct ShellFileOperationResult
{
    bool success = false;
    bool aborted = false;
    long nativeCode = 0;
    QString error;
};

struct ShellFileOperationOptions
{
    bool requestElevationPrompt = false;
    quintptr ownerWindow = 0;
};

class ShellFileOperation final
{
public:
    static ShellFileOperationResult perform(
        ShellFileOperationKind kind, const QString& source, const QString& destinationDirectory);
    static ShellFileOperationResult perform(ShellFileOperationKind kind,
        const ShellItemReference& source, const ShellItemReference& destinationFolder);
    static ShellFileOperationResult perform(ShellFileOperationKind kind,
        const QList<ShellItemReference>& sources, const ShellItemReference& destinationFolder,
        const ShellFileOperationOptions& options = {});
};

} // namespace filemanager::platform
