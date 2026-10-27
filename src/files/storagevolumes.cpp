#include "files/storagevolumes.h"

#include "files/nameordering.h"

#include <QDir>
#include <QSet>
#include <QStorageInfo>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace filemanager::files {

StorageLocationTraits classifyStorageDriveType(quint32 nativeDriveType)
{
    // Stable GetDriveType values keep this policy independently testable.
    switch (nativeDriveType) {
    case 2: return {StorageLocationKind::Removable, true, false};
    case 3: return {StorageLocationKind::Fixed, false, false};
    case 4: return {StorageLocationKind::Network, false, true};
    case 5: return {StorageLocationKind::Optical, true, false};
    case 6: return {StorageLocationKind::RamDisk, false, false};
    default: return {};
    }
}

StorageLocationTraits storageLocationTraits(const QString& path)
{
    const QString native = QDir::toNativeSeparators(path.trimmed());
    if (native.startsWith(QStringLiteral("\\\\")))
        return {StorageLocationKind::Network, false, true};
#ifdef Q_OS_WIN
    QString root;
    if (native.size() >= 2 && native.at(1) == QLatin1Char(':'))
        root = native.left(2) + QLatin1Char('\\');
    else {
        wchar_t volumeRoot[32768]{};
        if (GetVolumePathNameW(reinterpret_cast<LPCWSTR>(native.utf16()), volumeRoot,
                32768)) {
            root = QString::fromWCharArray(volumeRoot);
        }
    }
    if (root.isEmpty())
        return {};
    return classifyStorageDriveType(GetDriveTypeW(
        reinterpret_cast<LPCWSTR>(root.utf16())));
#else
    Q_UNUSED(native);
    return {};
#endif
}

QVector<StorageVolume> mountedStorageVolumes()
{
    QVector<StorageVolume> result;
    QSet<QString> seen;
    for (const QStorageInfo& storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isValid() || !storage.isReady() || storage.rootPath().isEmpty())
            continue;
        const QString root = QDir::cleanPath(storage.rootPath());
        const QString identity = root.toCaseFolded();
        if (seen.contains(identity))
            continue;
        seen.insert(identity);
        QString label = storage.displayName();
        if (label.isEmpty())
            label = QDir::toNativeSeparators(root);
        result.append({root, label, storage.isReadOnly(), storageLocationTraits(root).kind,
            static_cast<quint64>(qMax<qint64>(0, storage.bytesAvailable())),
            static_cast<quint64>(qMax<qint64>(0, storage.bytesTotal()))});
    }
    std::sort(result.begin(), result.end(), [](const StorageVolume& left, const StorageVolume& right) {
        return naturalNameCompare(left.rootPath, right.rootPath) < 0;
    });
    return result;
}

} // namespace filemanager::files
