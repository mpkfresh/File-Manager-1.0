#pragma once

#include "tags/tagcatalog.h"
#include "tags/tagrules.h"

#include <QStringList>

#include <atomic>

namespace filemanager::tags {

enum class TagMoveState
{
    Proposed,
    NoTags,
    NoMatchingRule,
    MultipleDestinations,
    SourceUnavailable,
    DestinationUnavailable,
    DestinationOccupied,
    DuplicateDestination,
    InvalidRules
};

struct TagMoveItem
{
    QString source;
    filemanager::files::SourceSnapshot sourceSnapshot;
    QStringList matchedTags;
    QList<RuleDestination> matches;
    QString destination;
    QString destinationDirectory;
    filemanager::files::SourceSnapshot destinationSnapshot;
    TagMoveState state = TagMoveState::SourceUnavailable;
    QString reason;
};

struct TagMovePlan
{
    QList<TagMoveItem> items;
    QStringList errors;
    bool cancelled = false;
    bool complete() const { return !cancelled && errors.isEmpty(); }
};

// Metadata-only planning for the exact caller-supplied file selection. Never
// enumerates folders, reads file contents, creates directories or moves files.
// Retained snapshots are previews, not execution authority: every selected
// item must be rechecked before an independently confirmed move.
TagMovePlan planSelectedTagMoves(const QStringList& selectedPaths,
    const TagCatalog& catalog, const std::atomic_bool* cancelled = nullptr);

} // namespace filemanager::tags
