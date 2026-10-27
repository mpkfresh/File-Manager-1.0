#pragma once

#include <atomic>
#include <functional>

#include <QString>
#include <QStringList>

namespace filemanager::files {

struct ZipArchivePolicy final
{
    static constexpr int maximumEntries = 50000;
    static constexpr qint64 maximumEntryBytes = 16LL * 1024 * 1024 * 1024;
    static constexpr qint64 maximumTotalBytes = 64LL * 1024 * 1024 * 1024;
    static constexpr qint64 maximumExpansionRatio = 200;
    static constexpr int freeSpaceMarginPercent = 10;
};

struct ZipOperationResult final
{
    bool success = false;
    bool cancelled = false;
    int entries = 0;
    qint64 totalBytes = 0;
    QString error;
};

using ZipProgress = std::function<void(int entries, qint64 bytes)>;

ZipOperationResult extractZipSafely(const QString& archivePath,
                                    const QString& destinationDirectory,
                                    const std::atomic_bool* cancel = nullptr,
                                    const ZipProgress& progress = {});

ZipOperationResult createZipWithWindowsTar(const QStringList& sourcePaths,
                                           const QString& destinationPath,
                                           const std::atomic_bool* cancel = nullptr);

} // namespace filemanager::files
