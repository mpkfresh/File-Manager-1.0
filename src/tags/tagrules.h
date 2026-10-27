#pragma once

#include "tags/tagcatalog.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace filemanager::tags {

// Rules are evaluated only for files supplied by an explicit caller selection.
// This layer neither enumerates folders nor reads file contents or metadata.
struct RuleDestination
{
    QString ruleId;
    QString path;
};

enum class RuleEvaluationState
{
    SourceNeedsReview,
    NoMatchingRule,
    OneMatch,
    MultipleMatches,
    InvalidRule
};

struct RuleEvaluation
{
    RuleEvaluationState state = RuleEvaluationState::NoMatchingRule;
    QList<RuleDestination> matches;
    QString error;
    bool uniquelyProposed() const { return state == RuleEvaluationState::OneMatch; }
};

// Returns every matching rule for review. Rules that agree on the same
// destination yield one proposal; different destinations remain unresolved.
bool validateDestinationRules(const QList<DestinationRule>& rules, QString* error = nullptr);

RuleEvaluation evaluateDestinationRules(const QString& selectedPath,
    const TagMatch& tagMatch, const QList<DestinationRule>& rules);

} // namespace filemanager::tags
