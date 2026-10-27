#include "files/fileanalysis.h"
#include "files/scopeenumerator.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

namespace filemanager::files {

PathStatistics measurePath(const QString& path, const std::atomic_bool* cancelled,
    const AnalysisProgress& progress)
{
    PathStatistics result;
    const QFileInfo root(path);
    if (!root.exists()) {
        result.error = "The selected path no longer exists.";
        return result;
    }
    if (root.isSymLink()) {
        result.error = "Symbolic links are not followed during analysis.";
        return result;
    }
    if (root.isFile()) {
        result.files = 1;
        result.bytes = static_cast<quint64>(root.size());
        if (progress)
            progress(result);
        return result;
    }

    quint64 visited = 0;
    const auto scopeResult = enumerateRegularTree(path, true, cancelled,
        [&](const QFileInfo& entry) {
        if (entry.isDir())
            ++result.folders;
        else if (entry.isFile()) {
            ++result.files;
            result.bytes += static_cast<quint64>(entry.size());
        }
        ++visited;
        if (progress && visited % 64 == 0)
            progress(result);
        return true;
    });
    result.cancelled = scopeResult.cancelled;
    if (!scopeResult.error.isEmpty())
        result.error = scopeResult.error;
    if (progress)
        progress(result);
    return result;
}

QString sha256File(const QString& path, QString* error, const std::atomic_bool* cancelled)
{
    if (error)
        error->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qint64 chunkSize = 1024 * 1024;
    while (!file.atEnd()) {
        if (cancelled && cancelled->load()) {
            if (error)
                *error = "Analysis cancelled.";
            return {};
        }
        const QByteArray chunk = file.read(chunkSize);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            if (error)
                *error = file.errorString();
            return {};
        }
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace filemanager::files
