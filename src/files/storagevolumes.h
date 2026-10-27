#pragma once

#include <QString>
#include <QVector>

namespace filemanager::files {

enum class StorageLocationKind { Unknown, Fixed, Removable, Network, Optical, RamDisk };

struct StorageLocationTraits
{
    StorageLocationKind kind = StorageLocationKind::Unknown;
    bool safeEjectRecommended = false;
    bool reconnectable = false;
};

struct StorageVolume
{
    QString rootPath;
    QString displayName;
    bool readOnly = false;
    StorageLocationKind kind = StorageLocationKind::Unknown;
    quint64 bytesAvailable = 0;
    quint64 bytesTotal = 0;
};

StorageLocationTraits classifyStorageDriveType(quint32 nativeDriveType);
StorageLocationTraits storageLocationTraits(const QString& path);
QVector<StorageVolume> mountedStorageVolumes();

} // namespace filemanager::files
