#pragma once

#include <QString>
#include <QStringList>

#include <atomic>

namespace filemanager::search {

struct WindowsSearchResult
{
    bool usedIndex = false;
    bool cancelled = false;
    QStringList paths;
    QString diagnostic;
};

class WindowsSearch final
{
public:
    static WindowsSearchResult query(const QString& root, const QString& name,
        int maximumCandidates, const std::atomic_bool* cancellation = nullptr);
    static WindowsSearchResult queryAqs(const QString& root, const QString& advancedQuery,
        int maximumCandidates, const std::atomic_bool* cancellation = nullptr);
};

} // namespace filemanager::search
