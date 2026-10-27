#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include "platform/shellitem.h"

namespace filemanager::platform {

struct RecycleBinItem
{
    QString originalPath;
    QByteArray absolutePidl;
};

struct RecycleBinResult
{
    bool success = false;
    bool aborted = false;
    long nativeCode = 0;
    QString error;
    RecycleBinItem item;
};

struct PermanentDeleteResult
{
    bool success = false;
    bool aborted = false;
    long nativeCode = 0;
    QString error;
    QStringList deletedPaths;
    QStringList remainingPaths;
};

struct RecycleBinBatchResult
{
    bool success = false;
    bool aborted = false;
    long nativeCode = 0;
    QString error;
    int requestedItems = 0;
    int restoredItems = 0;
};

struct RecycleBinSummary
{
    bool success = false;
    long nativeCode = 0;
    QString error;
    quint64 itemCount = 0;
    quint64 totalBytes = 0;
};

class RecycleBinOperation final
{
public:
    static RecycleBinResult recycle(const QString& path);
    static RecycleBinResult restore(const RecycleBinItem& item);
    static bool isAvailable(const RecycleBinItem& item);
    static PermanentDeleteResult permanentlyDelete(const QStringList& paths);
    static RecycleBinBatchResult restoreSelectedShellItemsIsolated(
        const QList<ShellItemReference>& items, quintptr ownerWindow,
        int timeoutMilliseconds = 120000, const QString& helperExecutable = {});
    static RecycleBinBatchResult restoreSelectedShellItemsInProcess(
        const QList<ShellItemReference>& items, quintptr ownerWindow);
    static RecycleBinSummary summary();
    static RecycleBinBatchResult empty(quintptr ownerWindow);
};

} // namespace filemanager::platform
