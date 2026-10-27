#pragma once

#include <QStringList>
#include <QVector>
#include <QByteArray>

#include <atomic>

namespace filemanager::files {

struct DuplicateGroup
{
    quint64 bytesPerFile = 0;
    QString sha256;
    QStringList paths;
};

struct DuplicateScanResult
{
    QVector<DuplicateGroup> groups;
    quint64 filesScanned = 0;
    bool cancelled = false;
    QStringList errors;
};

DuplicateScanResult findDuplicates(
    const QString& root, const std::atomic_bool* cancelled = nullptr);
quint64 duplicateReclaimableBytes(const DuplicateScanResult& result);
QByteArray duplicateReportCsv(const DuplicateScanResult& result);
bool writeDuplicateReportCsv(
    const QString& destination, const DuplicateScanResult& result, QString* error = nullptr);

} // namespace filemanager::files
