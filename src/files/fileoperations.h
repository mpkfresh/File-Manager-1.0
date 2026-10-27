#pragma once

#include <QByteArray>
#include <QString>
#include <utility>

namespace filemanager::files {

struct OperationResult
{
    bool ok = false;
    QString error;

    static OperationResult success() { return {true, {}}; }
    static OperationResult failure(QString message) { return {false, std::move(message)}; }
};

struct TrashResult
{
    bool ok = false;
    QString originalPath;
    QString pathInTrash;
    QByteArray recycledItemId;
    QString error;
};

OperationResult copy(const QString& source, const QString& destination);
OperationResult move(const QString& source, const QString& destination);
OperationResult renameItem(const QString& source, const QString& destination);
OperationResult moveToTrash(const QString& path);
TrashResult moveToTrashDetailed(const QString& path);

} // namespace filemanager::files
