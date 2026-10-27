#include "tagrules.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace filemanager::tags {
namespace {

QString normalizedTag(QString tag)
{
    return tag.trimmed().normalized(QString::NormalizationForm_C).toCaseFolded();
}

bool validRule(const DestinationRule& rule)
{
    if (rule.id.isEmpty() || rule.id.size() > 64 || rule.id != rule.id.trimmed()
            || rule.requiredTags.isEmpty()
            || rule.requiredTags.size() > TagCatalog::MaximumTagsPerFile
            || rule.destinationDirectory.size() > 32767
            || !QDir::isAbsolutePath(rule.destinationDirectory))
        return false;
    for (const QChar character : rule.id) {
        if (!character.isLetterOrNumber() && character != QLatin1Char('-')
                && character != QLatin1Char('_'))
            return false;
    }
    QSet<QString> seen;
    for (const QString& tag : rule.requiredTags) {
        const QString key = normalizedTag(tag);
        if (key.isEmpty() || key.size() > 80 || seen.contains(key))
            return false;
        for (const QChar character : key) {
            if (character.unicode() < 0x20 || character.unicode() == 0x7f)
                return false;
        }
        seen.insert(key);
    }
    return true;
}

} // namespace

bool validateDestinationRules(const QList<DestinationRule>& rules, QString* error)
{
    if (rules.size() > TagCatalog::MaximumRules) {
        if (error) *error = QStringLiteral("At most 256 destination rules are supported.");
        return false;
    }
    QSet<QString> ids;
    for (const DestinationRule& rule : rules) {
        const QString key = rule.id.toCaseFolded();
        if (!validRule(rule) || ids.contains(key)) {
            if (error) *error = QStringLiteral("A rule needs a unique ID, 1–32 distinct tags and an absolute destination folder.");
            return false;
        }
        ids.insert(key);
    }
    return true;
}

RuleEvaluation evaluateDestinationRules(const QString& selectedPath,
    const TagMatch& tagMatch, const QList<DestinationRule>& rules)
{
    RuleEvaluation result;
    if (!QDir::isAbsolutePath(selectedPath)
            || tagMatch.state == TagMatchState::Unavailable
            || tagMatch.state == TagMatchState::Modified
            || tagMatch.state == TagMatchState::Replaced) {
        result.state = RuleEvaluationState::SourceNeedsReview;
        return result;
    }
    if (!tagMatch.actionable())
        return result;
    const QString source = QDir::cleanPath(QDir::fromNativeSeparators(selectedPath));
    const QString name = QFileInfo(source).fileName();
    if (name.isEmpty()) {
        result.state = RuleEvaluationState::SourceNeedsReview;
        return result;
    }

    QSet<QString> current;
    for (const QString& tag : tagMatch.tags)
        current.insert(normalizedTag(tag));

    if (!validateDestinationRules(rules, &result.error)) {
        result.state = RuleEvaluationState::InvalidRule;
        return result;
    }
    QSet<QString> destinationKeys;
    for (const DestinationRule& rule : rules) {
        bool matches = true;
        for (const QString& required : rule.requiredTags) {
            if (!current.contains(normalizedTag(required))) {
                matches = false;
                break;
            }
        }
        if (!matches)
            continue;
        const QString folder = QDir::cleanPath(QDir::fromNativeSeparators(rule.destinationDirectory));
        const QString destination = QDir(folder).filePath(name);
        if (destination.compare(source, Qt::CaseInsensitive) == 0)
            continue;
        result.matches.append({rule.id, destination});
        destinationKeys.insert(destination.toCaseFolded());
    }
    result.state = result.matches.isEmpty() ? RuleEvaluationState::NoMatchingRule
        : destinationKeys.size() == 1 ? RuleEvaluationState::OneMatch
        : RuleEvaluationState::MultipleMatches;
    return result;
}

} // namespace filemanager::tags
