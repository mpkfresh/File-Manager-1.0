#include "tagmoveplan.h"

#include "files/filenamevalidator.h"
#include "files/scopeenumerator.h"
#include "platform/nativefilecopy.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <utility>

namespace filemanager::tags {
namespace {

QString key(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}

bool regularSelectedFile(const QString& path)
{
    return filemanager::platform::nativePathKind(path)
            == filemanager::platform::NativePathKind::Regular
        && QFileInfo(path).isFile()
        && filemanager::files::isRegularDirectoryChain(QFileInfo(path).absolutePath());
}

} // namespace

TagMovePlan planSelectedTagMoves(const QStringList& selectedPaths,
    const TagCatalog& catalog, const std::atomic_bool* cancelled)
{
    TagMovePlan plan;
    if (!catalog.ready()) {
        plan.errors.append(QStringLiteral("Load the tag catalog before reviewing destinations."));
        return plan;
    }
    if (selectedPaths.isEmpty() || selectedPaths.size() > 4096) {
        plan.errors.append(QStringLiteral("Select 1 to 4,096 files for one tag review."));
        return plan;
    }
    const QList<DestinationRule> rules = catalog.rules();
    QString rulesError;
    if (!validateDestinationRules(rules, &rulesError)) {
        plan.errors.append(rulesError);
        return plan;
    }
    QSet<QString> sourceKeys;
    QHash<QString, int> destinations;
    for (const QString& rawPath : selectedPaths) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            plan.cancelled = true;
            return plan;
        }
        if (!QDir::isAbsolutePath(rawPath)) {
            plan.errors.append(QStringLiteral("A selected path is not absolute."));
            return plan;
        }
        const QString path = QDir::cleanPath(QDir::fromNativeSeparators(rawPath));
        if (sourceKeys.contains(key(path))) {
            plan.errors.append(QStringLiteral("A selected file appears more than once."));
            return plan;
        }
        sourceKeys.insert(key(path));
        TagMoveItem item;
        item.source = path;
        if (!regularSelectedFile(path)) {
            item.reason = QStringLiteral("Source is unavailable, a folder, placeholder or reparse point.");
            plan.items.append(std::move(item));
            continue;
        }
        item.sourceSnapshot = filemanager::files::captureSourceSnapshot(path);
        if (!item.sourceSnapshot.valid || item.sourceSnapshot.fileIndex == 0) {
            item.reason = QStringLiteral("The source native identity is unavailable.");
            plan.items.append(std::move(item));
            continue;
        }
        const TagMatch match = catalog.match(path, item.sourceSnapshot);
        item.matchedTags = match.tags;
        if (!match.actionable()) {
            item.state = match.state == TagMatchState::Unassigned
                ? TagMoveState::NoTags : TagMoveState::SourceUnavailable;
            item.reason = match.state == TagMatchState::Unassigned
                ? QStringLiteral("No user tags are assigned.")
                : QStringLiteral("The stored tag identity is unavailable or replaced.");
            plan.items.append(std::move(item));
            continue;
        }
        const RuleEvaluation evaluation = evaluateDestinationRules(path, match, rules);
        item.matches = evaluation.matches;
        if (evaluation.state == RuleEvaluationState::InvalidRule) {
            item.state = TagMoveState::InvalidRules;
            item.reason = evaluation.error;
        } else if (evaluation.state == RuleEvaluationState::MultipleMatches) {
            item.state = TagMoveState::MultipleDestinations;
            item.reason = QStringLiteral("Matching rules propose different folders; choose one during review.");
        } else if (!evaluation.uniquelyProposed()) {
            item.state = TagMoveState::NoMatchingRule;
            item.reason = QStringLiteral("No rule proposes a different destination.");
        } else {
            item.destination = evaluation.matches.first().path;
            item.destinationDirectory = QFileInfo(item.destination).absolutePath();
            const bool destinationSafe = filemanager::files::validateWindowsPath(
                    item.destination).valid
                && filemanager::files::isRegularDirectoryChain(
                    item.destinationDirectory)
                && filemanager::platform::nativePathKind(item.destination)
                    == filemanager::platform::NativePathKind::Missing;
            if (!destinationSafe) {
                item.state = filemanager::platform::nativePathKind(item.destination)
                    == filemanager::platform::NativePathKind::Missing
                    ? TagMoveState::DestinationUnavailable : TagMoveState::DestinationOccupied;
                item.reason = item.state == TagMoveState::DestinationOccupied
                    ? QStringLiteral("The proposed destination already exists.")
                    : QStringLiteral("The destination folder or path is unavailable or unsafe.");
            } else {
                item.destinationSnapshot = filemanager::files::captureSourceSnapshot(
                    item.destinationDirectory);
                if (!item.destinationSnapshot.valid
                        || item.destinationSnapshot.fileIndex == 0
                        || !(item.destinationSnapshot.attributes & 0x10U)) {
                    item.state = TagMoveState::DestinationUnavailable;
                    item.reason = QStringLiteral("The destination folder identity is unavailable.");
                } else {
                    item.state = TagMoveState::Proposed;
                    const QString destinationKey = key(item.destination);
                    if (destinations.contains(destinationKey)) {
                        TagMoveItem& first = plan.items[destinations.value(destinationKey)];
                        first.state = TagMoveState::DuplicateDestination;
                        first.reason = QStringLiteral("Another selected file has the same proposed destination.");
                        item.state = TagMoveState::DuplicateDestination;
                        item.reason = first.reason;
                    } else {
                        destinations.insert(destinationKey,
                            static_cast<int>(plan.items.size()));
                    }
                }
            }
        }
        if (filemanager::files::captureSourceSnapshot(path) != item.sourceSnapshot) {
            item.state = TagMoveState::SourceUnavailable;
            item.reason = QStringLiteral("Source changed while preparing the preview; review it again.");
            item.destination.clear();
        }
        plan.items.append(std::move(item));
    }
    return plan;
}

} // namespace filemanager::tags
