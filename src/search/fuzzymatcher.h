#pragma once

#include <QString>

namespace filemanager::search {

struct FuzzyMatch
{
    bool matched = false;
    int score = 0;
};

// Scores ordered character matches. Consecutive and word-boundary matches
// rank above scattered matches; shorter candidates win otherwise-equal ties.
FuzzyMatch match(const QString& query, const QString& candidate);

} // namespace filemanager::search
