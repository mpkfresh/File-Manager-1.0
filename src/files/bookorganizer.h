#pragma once

#include "sourcesnapshot.h"

#include <QList>
#include <QStringList>
#include <atomic>
#include <optional>

namespace filemanager::files {

struct BookSeriesName
{
    QString series;
    int number = 0;
    QString title;
};

struct BookMoveCandidate
{
    QString source;
    QString destination;
    QString groupSource;
    SourceSnapshot sourceSnapshot;
    bool companion = false;
    QString evidence;
};

struct BookReviewItem
{
    QString path;
    QString reason;
    SourceSnapshot sourceSnapshot;
};

struct BookCorrection
{
    QString source;
    SourceSnapshot sourceSnapshot;
    BookSeriesName name;
};

struct BookOrganizationPlan
{
    QString destinationDirectory;
    SourceSnapshot destinationSnapshot;
    QList<BookMoveCandidate> candidates;
    QList<BookReviewItem> review;
    QStringList errors;
    qsizetype entriesVisited = 0;
    qsizetype ignoredFiles = 0;
    int skippedBoundaries = 0;
    bool cancelled = false;
    bool limitReached = false;
    bool complete() const { return !cancelled && !limitReached && errors.isEmpty(); }
};

// Only an explicit "Series - 01 - Title" filename is accepted. Metadata title/author
// alone cannot establish a series identity or sequence.
std::optional<BookSeriesName> parseBookSeriesName(const QString& fileName);

// Read-only proposals for explicitly selected roots and an existing Books folder.
BookOrganizationPlan planBookOrganization(const QStringList& roots, const QString& booksDirectory,
    const std::atomic_bool* cancelled = nullptr, qsizetype maxEntries = 100'000,
    const QList<BookCorrection>& corrections = {});

} // namespace filemanager::files
