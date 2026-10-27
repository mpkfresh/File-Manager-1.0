#include "files/duplicatefinder.h"

#include "files/fileanalysis.h"
#include "files/scopeenumerator.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QTextStream>

#include <algorithm>
#include <limits>

namespace filemanager::files {
namespace {

QByteArray sampleDigest(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return {};
    }
    constexpr qint64 sampleSize = 64 * 1024;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(file.read(sampleSize));
    if (file.size() > sampleSize) {
        if (!file.seek(qMax<qint64>(sampleSize, file.size() - sampleSize))) {
            *error = file.errorString();
            return {};
        }
        hash.addData(file.read(sampleSize));
    }
    return hash.result();
}

} // namespace

DuplicateScanResult findDuplicates(const QString& root, const std::atomic_bool* cancelled)
{
    DuplicateScanResult result;
    if (!QFileInfo(root).isDir()) {
        result.errors.append("Duplicate scanning requires a folder.");
        return result;
    }

    QHash<quint64, QStringList> bySize;
    const auto scopeResult = enumerateRegularTree(root, false, cancelled,
        [&](const QFileInfo& info) {
        ++result.filesScanned;
        bySize[static_cast<quint64>(info.size())].append(info.absoluteFilePath());
        return true;
    });
    if (scopeResult.cancelled) {
        result.cancelled = true;
        return result;
    }
    if (!scopeResult.error.isEmpty()) {
        result.errors.append(scopeResult.error);
        return result;
    }

    for (auto sizeIt = bySize.cbegin(); sizeIt != bySize.cend(); ++sizeIt) {
        if (sizeIt.value().size() < 2)
            continue;
        QHash<QByteArray, QStringList> bySample;
        for (const QString& path : sizeIt.value()) {
            if (cancelled && cancelled->load()) {
                result.cancelled = true;
                return result;
            }
            QString error;
            const QByteArray digest = sampleDigest(path, &error);
            if (!error.isEmpty()) {
                result.errors.append(path + ": " + error);
                continue;
            }
            bySample[digest].append(path);
        }
        for (auto sampleIt = bySample.cbegin(); sampleIt != bySample.cend(); ++sampleIt) {
            if (sampleIt.value().size() < 2)
                continue;
            QHash<QString, QStringList> byFullHash;
            for (const QString& path : sampleIt.value()) {
                QString error;
                const QString digest = sha256File(path, &error, cancelled);
                if (!error.isEmpty()) {
                    if (cancelled && cancelled->load()) {
                        result.cancelled = true;
                        return result;
                    }
                    result.errors.append(path + ": " + error);
                    continue;
                }
                byFullHash[digest].append(path);
            }
            for (auto hashIt = byFullHash.cbegin(); hashIt != byFullHash.cend(); ++hashIt) {
                if (hashIt.value().size() >= 2)
                    result.groups.append({sizeIt.key(), hashIt.key(), hashIt.value()});
            }
        }
    }
    return result;
}

quint64 duplicateReclaimableBytes(const DuplicateScanResult& result)
{
    quint64 total = 0;
    for (const DuplicateGroup& group : result.groups) {
        if (group.paths.size() < 2)
            continue;
        const quint64 copies = static_cast<quint64>(group.paths.size() - 1);
        const quint64 contribution = group.bytesPerFile > std::numeric_limits<quint64>::max() / copies
            ? std::numeric_limits<quint64>::max() : group.bytesPerFile * copies;
        if (total > std::numeric_limits<quint64>::max() - contribution)
            return std::numeric_limits<quint64>::max();
        total += contribution;
    }
    return total;
}

QByteArray duplicateReportCsv(const DuplicateScanResult& result)
{
    const auto csvCell = [](QString value) {
        if (!value.isEmpty() && QStringLiteral("=+-@").contains(value.front()))
            value.prepend(QLatin1Char('\''));
        value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + value + QLatin1Char('"');
    };
    QVector<DuplicateGroup> groups = result.groups;
    std::sort(groups.begin(), groups.end(), [](const DuplicateGroup& left,
                                               const DuplicateGroup& right) {
        if (left.bytesPerFile != right.bytesPerFile)
            return left.bytesPerFile < right.bytesPerFile;
        return left.sha256 < right.sha256;
    });
    QByteArray report("\xEF\xBB\xBF");
    QTextStream stream(&report, QIODevice::WriteOnly | QIODevice::Append);
    stream << "Group,Bytes per file,SHA-256,Path\r\n";
    for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        QStringList paths = groups[groupIndex].paths;
        paths.sort(Qt::CaseInsensitive);
        for (const QString& path : paths) {
            stream << groupIndex + 1 << ',' << groups[groupIndex].bytesPerFile << ','
                   << csvCell(groups[groupIndex].sha256) << ',' << csvCell(path) << "\r\n";
        }
    }
    stream.flush();
    return report;
}

bool writeDuplicateReportCsv(
    const QString& destination, const DuplicateScanResult& result, QString* error)
{
    QSaveFile report(destination);
    if (!report.open(QIODevice::WriteOnly)) {
        if (error)
            *error = report.errorString();
        return false;
    }
    const QByteArray bytes = duplicateReportCsv(result);
    if (report.write(bytes) != bytes.size() || !report.commit()) {
        if (error)
            *error = report.errorString();
        return false;
    }
    return true;
}

} // namespace filemanager::files
