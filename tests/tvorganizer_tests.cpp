#include "files/tvorganizer.h"
#include "files/filenamevalidator.h"
#include "files/scopeenumerator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <iostream>

namespace {

bool writeFixture(const QString& path, const QByteArray& contents = "fixture")
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray contentsOf(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

} // namespace

int runTvOrganizerTests()
{
    using namespace filemanager::files;
    int failures = 0;
    const auto check = [&](bool passed, const char* message) {
        if (!passed) {
            std::cerr << "FAIL: TV organizer: " << message << '\n';
            ++failures;
        }
    };
    const auto standard = parseTvEpisodeName("A.Show.2024.S01E02.Title.1080p.mkv");
    check(standard && standard->series == "A Show 2024" && standard->season == 1
        && standard->episode == 2, "SxxExx and year/title parsing");
    const auto alternate = parseTvEpisodeName(QString::fromUtf8("برنامج_2x03.mp4"));
    check(alternate && alternate->series == QString::fromUtf8("برنامج")
        && alternate->season == 2 && alternate->episode == 3, "NxNN and Unicode series parsing");
    const auto special = parseTvEpisodeName("Series S00E01.mkv");
    check(special && special->season == 0, "explicit season zero is retained");
    for (const QString& ambiguous : QStringList{"Movie.2024.mkv", "S01E02.mkv",
            "Show.S01E02E03.mkv", "Show.S01E02-E03.mkv", "Show.S01E02-03.mkv",
            "Show.S01E02.S01E03.mkv", "Show.1x02-1x03.mkv", "Show.S01E00.mkv",
            "Show.S001E02.mkv", "Show.S01E02.txt", "[Group].Show.S01E02.mkv",
            "CON.S01E02.mkv", "Show.S01E02&03.mkv"}) {
        check(!parseTvEpisodeName(ambiguous), qPrintable("leave uncertain name: " + ambiguous));
    }

    QTemporaryDir fixture;
    check(fixture.isValid(), "temporary fixture directory");
    if (!fixture.isValid())
        return failures;
    const QDir base(fixture.path());
    const QString input = base.filePath("Input");
    const QString shows = base.filePath("Input/Shows");
    check(QDir().mkpath(shows), "create chosen destination");
    const QString episode = base.filePath("Input/Series.S01E02.1080p.mkv");
    const QString subtitle = base.filePath("Input/Series.S01E02.1080p.en.forced.srt");
    const QString artwork = base.filePath("Input/Series.S01E02.1080p.poster.jpg");
    const QString otherEpisode = base.filePath("Input/Nested/Other.2x03.mp4");
    const QString unknown = base.filePath("Input/movie.mp4");
    const QString loose = base.filePath("Input/poster.jpg");
    const QString unfamiliarTag = base.filePath("Input/Series.S01E02.1080p.zzz.srt");
    const QString destinationVideo = base.filePath("Input/Shows/Existing.S01E01.mkv");
    const QString outside = base.filePath("Outside/Outside.S01E01.mkv");
    for (const QString& path : QStringList{episode, subtitle, artwork, otherEpisode, unknown,
            loose, unfamiliarTag, destinationVideo, outside, base.filePath("Input/notes.txt")})
        check(writeFixture(path), "write source fixture");

    const auto initial = planTvOrganization({input, input, base.filePath("Input/Nested")}, shows);
    check(initial.complete() && initial.candidates.size() == 4 && initial.review.size() == 3,
        "scan overlapping roots once; preserve ambiguous files for review");
    check(initial.ignoredFiles == 1, "count unrelated files without classifying them");
    QSet<QString> sources;
    for (const auto& candidate : initial.candidates) {
        sources.insert(candidate.source);
        check(candidate.sourceSnapshot.valid, "capture native file identity for every candidate");
        check(candidate.sourceSnapshot == captureSourceSnapshot(candidate.source),
            "captured file identity matches source");
        check(QFileInfo(candidate.source).fileName() == QFileInfo(candidate.destination).fileName(),
            "preserve filenames and extensions");
        check(candidate.destination.startsWith(shows + '/'), "destination remains under Shows");
        if (candidate.source == episode)
            check(candidate.destination == shows + "/Series/Season 01/Series.S01E02.1080p.mkv"
                && !candidate.companion, "series/season proposal");
        if (candidate.companion)
            check(candidate.episodeSource == episode, "exact-stem companions linked to episode");
    }
    check(sources == QSet<QString>{episode, subtitle, artwork, otherEpisode},
        "exclude destination subtree and unselected outside folders");
    check(!QFileInfo::exists(shows + "/Series") && contentsOf(episode) == "fixture"
            && contentsOf(subtitle) == "fixture" && contentsOf(outside) == "fixture",
        "planning neither creates directories nor changes source contents");
    check(isPathInside(episode, QStorageInfo(fixture.path()).rootPath())
            && !isPathInside(input + "Extra/file.mkv", input),
        "containment supports drive roots without confusing sibling prefixes");

    std::atomic_bool cancelled{true};
    const auto cancelledPlan = planTvOrganization({input}, shows, &cancelled);
    check(cancelledPlan.cancelled && cancelledPlan.candidates.isEmpty()
            && cancelledPlan.entriesVisited == 0, "pre-cancellation performs no enumeration");
    const auto limited = planTvOrganization({input}, shows, nullptr, 2);
    check(limited.limitReached && !limited.complete() && limited.entriesVisited == 2
            && limited.candidates.isEmpty(), "truncated scans never publish partial candidates");
    check(!planTvOrganization({}, shows).complete()
            && !planTvOrganization({shows}, shows).complete()
            && !planTvOrganization({"relative"}, shows).complete()
            && !planTvOrganization({input}, base.filePath("Missing")).complete()
            && !planTvOrganization({input}, shows, nullptr, 0).complete(),
        "reject missing, relative, destination-only and invalid-limit scopes");

    std::atomic_bool duringScan{false};
    int visited = 0;
    const auto cancelledEnumeration = enumerateRegularTree(input, false, &duringScan,
        [&](const QFileInfo&) { ++visited; duringScan.store(true); return true; }, {{shows}, 100});
    check(cancelledEnumeration.cancelled && visited == 1, "enumeration checks cancellation between entries");

    const QString collision = shows + "/Series/Season 01/Series.S01E02.1080p.en.forced.srt";
    check(writeFixture(collision, "existing subtitle"), "create companion collision");
    const auto collisionPlan = planTvOrganization({input}, shows);
    check(collisionPlan.complete() && collisionPlan.candidates.size() == 1
            && collisionPlan.review.size() == 6 && contentsOf(collision) == "existing subtitle",
        "a companion collision withholds the whole episode group, never overwrites");
    check(QFile::remove(collision), "remove disposable collision fixture");

    const QString duplicate = base.filePath("Input/Nested/Series.S01E02.720p.mp4");
    check(writeFixture(duplicate), "create duplicate episode");
    const auto duplicatePlan = planTvOrganization({input}, shows);
    check(duplicatePlan.candidates.size() == 1 && duplicatePlan.review.size() == 7,
        "duplicate episode variants and their companions require review");
    check(QFile::remove(duplicate), "remove disposable duplicate fixture");

    const QString oldEpisode = base.filePath("Input/retained-original.bin");
    check(QFile::rename(episode, oldEpisode) && writeFixture(episode, "replacement"),
        "replace original path while retaining original file identity");
    const auto originalCandidate = std::find_if(initial.candidates.cbegin(), initial.candidates.cend(),
        [&](const TvMoveCandidate& candidate) { return candidate.source == episode; });
    const auto replacementSnapshot = captureSourceSnapshot(episode);
    check(originalCandidate != initial.candidates.cend()
            && replacementSnapshot.valid
            && originalCandidate->sourceSnapshot.fileIndex != replacementSnapshot.fileIndex,
        "retained candidate identity distinguishes replacement at the same path");

#ifdef Q_OS_WIN
    const QString junction = base.filePath("Input/LinkedOutside");
    QProcess link;
    link.start("cmd.exe", {"/d", "/c", "mklink", "/J", QDir::toNativeSeparators(junction),
        QDir::toNativeSeparators(base.filePath("Outside"))});
    const bool linkFinished = link.waitForFinished(30'000);
    const bool linked = linkFinished && link.exitStatus() == QProcess::NormalExit && link.exitCode() == 0;
    if (!linked) {
        std::cerr << "Junction fixture (finished=" << linkFinished << ", exit=" << link.exitCode()
                  << "): " << link.errorString().toStdString() << '\n'
                  << link.readAllStandardOutput().toStdString()
                  << link.readAllStandardError().toStdString();
        if (!linkFinished) {
            link.kill();
            link.waitForFinished(5000);
        }
    }
    check(linked, "create disposable junction for boundary test");
    if (linked) {
        const auto bounded = planTvOrganization({input}, shows);
        check(bounded.skippedBoundaries == 1 && bounded.candidates.size() == 4,
            "skip junction without following out-of-scope files");
        check(!planTvOrganization({junction}, shows).complete(), "reject reparse scan root");
        check(QDir().mkpath(base.filePath("Outside/Child")), "create reparse descendant fixture");
        check(!planTvOrganization({junction + "/Child"}, shows).complete(),
            "reject regular child reached through a reparse ancestor");
        check(QDir().rmdir(junction),
            "remove only the disposable junction, preserving its target");
        check(contentsOf(outside) == "fixture", "junction target remains untouched");
    }
#endif
    return failures;
}
