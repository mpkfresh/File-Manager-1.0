#include "fuzzymatcher.h"

#include <algorithm>

namespace filemanager::search {

FuzzyMatch match(const QString& rawQuery, const QString& rawCandidate)
{
    const QString query = rawQuery.trimmed().toCaseFolded();
    const QString candidate = rawCandidate.toCaseFolded();
    if (query.isEmpty())
        return {true, 0};

    int cursor = 0;
    int score = 0;
    int previous = -2;
    for (const QChar wanted : query) {
        const int found = candidate.indexOf(wanted, cursor);
        if (found < 0)
            return {};

        score += 10;
        if (found == previous + 1)
            score += 12;
        if (found == 0 || !candidate.at(found - 1).isLetterOrNumber())
            score += 8;
        score -= std::min(found - cursor, 6);
        previous = found;
        cursor = found + 1;
    }

    score -= static_cast<int>(std::max<qsizetype>(0, candidate.size() - query.size()) / 3);
    return {true, score};
}

} // namespace filemanager::search
