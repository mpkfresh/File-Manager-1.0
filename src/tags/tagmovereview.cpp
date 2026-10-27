#include "tagmovereview.h"

#include "files/filenamevalidator.h"
#include "files/scopeenumerator.h"
#include "platform/nativefilecopy.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <utility>

namespace filemanager::tags {
namespace {

QString key(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}

void reject(TagMoveItem& item, TagMoveState state, const QString& reason)
{
    item.state = state;
    item.reason = reason;
    item.destinationSnapshot = {};
}

} // namespace

TagMovePlan resolveTagMoveReview(const TagMovePlan& preview,
    const QStringList& includedSources,
    const QHash<QString, QString>& chosenRuleIds,
    const std::atomic_bool* cancelled)
{
    TagMovePlan resolved;
    if (!preview.complete() || includedSources.isEmpty()
            || includedSources.size() > 4096) {
        resolved.errors.append(QStringLiteral(
            "A complete preview and 1 to 4,096 included files are required."));
        return resolved;
    }

    QHash<QString, const TagMoveItem*> rows;
    for (const TagMoveItem& item : preview.items) {
        if (item.source.isEmpty() || rows.contains(key(item.source))) {
            resolved.errors.append(QStringLiteral("The retained preview has duplicate or empty sources."));
            return resolved;
        }
        rows.insert(key(item.source), &item);
    }

    QHash<QString, QString> choices;
    for (auto it = chosenRuleIds.cbegin(); it != chosenRuleIds.cend(); ++it) {
        if (!QDir::isAbsolutePath(it.key()) || choices.contains(key(it.key()))
                || !rows.contains(key(it.key())) || it.value().isEmpty()) {
            resolved.errors.append(QStringLiteral("A rule choice is invalid or not in the preview."));
            return resolved;
        }
        choices.insert(key(it.key()), it.value());
    }

    QSet<QString> included;
    for (const QString& source : includedSources) {
        const QString sourceKey = key(source);
        if (!QDir::isAbsolutePath(source) || included.contains(sourceKey)
                || !rows.contains(sourceKey)) {
            resolved.errors.append(QStringLiteral("The included selection differs from the preview."));
            return resolved;
        }
        included.insert(sourceKey);
        const TagMoveItem& item = *rows.value(sourceKey);
        if (item.state != TagMoveState::Proposed
                && item.state != TagMoveState::DuplicateDestination
                && item.state != TagMoveState::MultipleDestinations) {
            resolved.errors.append(QStringLiteral(
                "An included file has no reviewable destination; make a fresh preview."));
            return resolved;
        }
        if (item.state == TagMoveState::MultipleDestinations) {
            const QString chosenId = choices.value(sourceKey);
            int matchingChoices = 0;
            for (const RuleDestination& match : item.matches) {
                if (match.ruleId == chosenId)
                    ++matchingChoices;
            }
            if (matchingChoices != 1) {
                resolved.errors.append(QStringLiteral(
                    "Every conflicting destination needs one matching rule choice."));
                return resolved;
            }
        } else if (choices.contains(sourceKey)) {
            resolved.errors.append(QStringLiteral(
                "Rule choices are accepted only for conflicting destinations."));
            return resolved;
        }
    }
    for (auto it = choices.cbegin(); it != choices.cend(); ++it) {
        if (!included.contains(it.key())) {
            resolved.errors.append(QStringLiteral("A rule choice refers to an excluded file."));
            return resolved;
        }
    }

    QHash<QString, int> destinations;
    for (const QString& source : includedSources) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            resolved.cancelled = true;
            resolved.items.clear();
            return resolved;
        }
        const TagMoveItem& original = *rows.value(key(source));
        TagMoveItem item = original;
        if (original.state == TagMoveState::MultipleDestinations) {
            for (const RuleDestination& match : original.matches) {
                if (match.ruleId == choices.value(key(source))) {
                    item.destination = match.path;
                    break;
                }
            }
            item.destinationDirectory = QFileInfo(item.destination).absolutePath();
        }
        item.state = TagMoveState::Proposed;
        item.reason.clear();

        if (!item.sourceSnapshot.valid || item.sourceSnapshot.fileIndex == 0
                || filemanager::platform::nativePathKind(item.source)
                    != filemanager::platform::NativePathKind::Regular
                || !filemanager::files::isRegularDirectoryChain(
                    QFileInfo(item.source).absolutePath())
                || filemanager::files::captureSourceSnapshot(item.source)
                    != item.sourceSnapshot) {
            reject(item, TagMoveState::SourceUnavailable,
                QStringLiteral("Source changed after preview; make a fresh preview."));
        } else if (item.destination.isEmpty()
                || !QDir::isAbsolutePath(item.destination)
                || !filemanager::files::validateWindowsPath(item.destination).valid
                || !filemanager::files::isRegularDirectoryChain(item.destinationDirectory)
                || filemanager::files::isPathInside(item.source,
                    item.destinationDirectory)) {
            reject(item, TagMoveState::DestinationUnavailable,
                QStringLiteral("The chosen destination path or folder is unsafe."));
        } else if (filemanager::platform::nativePathKind(item.destination)
                != filemanager::platform::NativePathKind::Missing) {
            reject(item, TagMoveState::DestinationOccupied,
                QStringLiteral("The chosen destination is now occupied."));
        } else {
            const auto directorySnapshot = filemanager::files::captureSourceSnapshot(
                item.destinationDirectory);
            if (!directorySnapshot.valid || directorySnapshot.fileIndex == 0
                    || !(directorySnapshot.attributes & 0x10U)
                    || (original.state != TagMoveState::MultipleDestinations
                        && directorySnapshot != original.destinationSnapshot)) {
                reject(item, TagMoveState::DestinationUnavailable,
                    QStringLiteral("Destination folder changed after preview; review it again."));
            } else {
                item.destinationSnapshot = directorySnapshot;
                const QString destinationKey = key(item.destination);
                if (destinations.contains(destinationKey)) {
                    TagMoveItem& prior = resolved.items[destinations.value(destinationKey)];
                    reject(prior, TagMoveState::DuplicateDestination,
                        QStringLiteral("Another included file claims the same destination."));
                    reject(item, TagMoveState::DuplicateDestination, prior.reason);
                } else {
                    destinations.insert(destinationKey,
                        static_cast<int>(resolved.items.size()));
                }
            }
        }
        resolved.items.append(std::move(item));
    }
    return resolved;
}

} // namespace filemanager::tags
