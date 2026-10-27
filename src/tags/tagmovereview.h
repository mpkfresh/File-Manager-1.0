#pragma once

#include "tags/tagmoveplan.h"

#include <QHash>

namespace filemanager::tags {

// Resolves only the caller's explicitly included preview rows. A source with
// competing destinations needs a matching rule ID in chosenRuleIds. Excluded
// rows cannot reserve a target. The result is another preview, never consent
// to enqueue or execute a move.
TagMovePlan resolveTagMoveReview(const TagMovePlan& preview,
    const QStringList& includedSources,
    const QHash<QString, QString>& chosenRuleIds = {},
    const std::atomic_bool* cancelled = nullptr);

} // namespace filemanager::tags
