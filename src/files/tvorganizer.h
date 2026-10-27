#pragma once

#include "sourcesnapshot.h"

#include <QList>
#include <QStringList>
#include <atomic>
#include <optional>

namespace filemanager::files {

struct TvEpisodeName
{
    QString series;
    int season = 0;
    int episode = 0;
};

struct TvMoveCandidate
{
    QString source;
    QString destination;
    QString episodeSource;
    SourceSnapshot sourceSnapshot;
    bool companion = false;
    QString evidence;
};

struct TvReviewItem
{
    QString path;
    QString reason;
    SourceSnapshot sourceSnapshot;
};

struct TvOrganizationPlan
{
    QString destinationDirectory;
    SourceSnapshot destinationSnapshot;
    QList<TvMoveCandidate> candidates;
    QList<TvReviewItem> review;
    QStringList errors;
    qsizetype entriesVisited = 0;
    qsizetype ignoredFiles = 0;
    int skippedBoundaries = 0;
    bool cancelled = false;
    bool limitReached = false;
    bool complete() const { return !cancelled && !limitReached && errors.isEmpty(); }
};

std::optional<TvEpisodeName> parseTvEpisodeName(const QString& fileName);

// Read-only proposals, not executable transfer requests or permission to move files.
// showsDirectory must be an explicitly selected, existing regular directory.
TvOrganizationPlan planTvOrganization(const QStringList& roots, const QString& showsDirectory,
    const std::atomic_bool* cancelled = nullptr, qsizetype maxEntries = 100'000);

} // namespace filemanager::files
