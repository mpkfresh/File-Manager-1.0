#pragma once

#include <QString>

#include <atomic>
#include <functional>

namespace filemanager::files {

struct PathStatistics
{
    quint64 bytes = 0;
    quint64 files = 0;
    quint64 folders = 0;
    bool cancelled = false;
    QString error;

    bool operator==(const PathStatistics&) const = default;
};

using AnalysisProgress = std::function<void(const PathStatistics&)>;

PathStatistics measurePath(const QString& path, const std::atomic_bool* cancelled = nullptr,
    const AnalysisProgress& progress = {});
QString sha256File(const QString& path, QString* error = nullptr,
    const std::atomic_bool* cancelled = nullptr);

} // namespace filemanager::files
