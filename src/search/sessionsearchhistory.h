#pragma once

#include <QStringList>

namespace filemanager::search {

class SessionSearchHistory final
{
public:
    static constexpr int MaximumEntries = 20;

    void record(const QString& query);
    void clear();
    const QStringList& entries() const;

private:
    QStringList entries_;
};

} // namespace filemanager::search
