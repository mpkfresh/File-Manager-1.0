#include "sessionsearchhistory.h"

namespace filemanager::search {

void SessionSearchHistory::record(const QString& query)
{
    const QString normalized = query.trimmed();
    if (normalized.isEmpty())
        return;
    for (qsizetype index = entries_.size() - 1; index >= 0; --index) {
        if (entries_.at(index).compare(normalized, Qt::CaseInsensitive) == 0)
            entries_.removeAt(index);
    }
    entries_.prepend(normalized);
    while (entries_.size() > MaximumEntries)
        entries_.removeLast();
}

void SessionSearchHistory::clear()
{
    entries_.clear();
}

const QStringList& SessionSearchHistory::entries() const
{
    return entries_;
}

} // namespace filemanager::search
