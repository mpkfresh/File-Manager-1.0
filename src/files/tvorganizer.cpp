#include "tvorganizer.h"

#include "filenamevalidator.h"
#include "scopeenumerator.h"
#include "platform/nativefilecopy.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace filemanager::files {
namespace {

QString key(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

const QSet<QString> videoExtensions{
    "avi", "m4v", "mkv", "mov", "mp4", "mpeg", "mpg", "ts", "webm", "wmv"};
const QSet<QString> subtitleExtensions{"srt", "ass", "ssa", "vtt", "sub", "idx"};
const QSet<QString> artworkExtensions{"png", "jpg", "jpeg", "webp"};
const QSet<QString> subtitleTags{
    "en", "eng", "ar", "ara", "de", "deu", "ger", "es", "spa", "fr", "fra", "fre",
    "it", "ita", "ja", "jpn", "ko", "kor", "pt", "por", "ru", "rus", "zh", "zho",
    "chi", "forced", "sdh", "cc", "default"};
const QSet<QString> artworkTags{"poster", "thumb", "thumbnail", "fanart"};

struct EpisodeGroup
{
    QString source;
    TvEpisodeName name;
    QList<TvMoveCandidate> files;
    QString problem;
};

} // namespace

std::optional<TvEpisodeName> parseTvEpisodeName(const QString& fileName)
{
    const QFileInfo info(fileName);
    if (!videoExtensions.contains(info.suffix().toLower()))
        return std::nullopt;
    const QString stem = info.completeBaseName();
    static const QRegularExpression marker(
        R"(^(.+?)[ ._-]+(?:S([0-9]{1,2})E([0-9]{2,3})|([0-9]{1,2})x([0-9]{2,3}))(?=$|[ ._-]))",
        QRegularExpression::CaseInsensitiveOption);
    const auto match = marker.match(stem);
    if (!match.hasMatch())
        return std::nullopt;
    const QString tail = stem.mid(match.capturedEnd());
    static const QRegularExpression extraEpisode(
        R"((?:^|[ ._+&-])(?:(?:S[0-9]{1,2})?E[0-9]{1,3}|[0-9]{1,2}x[0-9]{1,3})(?:$|[^0-9])|^[ ._+&-]*[0-9]+(?:$|[ ._+&-]))",
        QRegularExpression::CaseInsensitiveOption);
    if (tail.contains(extraEpisode))
        return std::nullopt;
    QString series = match.captured(1);
    if (series.contains('[') || series.contains(']'))
        return std::nullopt; // Release-group prefixes require a human interpretation.
    series.replace(QRegularExpression("[._]+"), " ");
    series = series.simplified();
    while (series.endsWith('-') || series.endsWith(' '))
        series.chop(1);
    if (!validateWindowsName(series).valid)
        return std::nullopt;
    const bool sFormat = !match.captured(2).isEmpty();
    TvEpisodeName episode{series, match.captured(sFormat ? 2 : 4).toInt(),
        match.captured(sFormat ? 3 : 5).toInt()};
    if (episode.episode == 0)
        return std::nullopt;
    return episode;
}

TvOrganizationPlan planTvOrganization(const QStringList& roots, const QString& showsDirectory,
    const std::atomic_bool* cancelled, qsizetype maxEntries)
{
    TvOrganizationPlan plan;
    const auto stopped = [&] {
        if (!cancelled || !cancelled->load(std::memory_order_relaxed))
            return false;
        plan.cancelled = true;
        plan.candidates.clear();
        return true;
    };
    if (stopped())
        return plan;
    if (roots.isEmpty() || showsDirectory.isEmpty() || !QDir::isAbsolutePath(showsDirectory)
            || maxEntries < 1 || maxEntries > 1'000'000
            || !isRegularDirectoryChain(showsDirectory, false)) {
        plan.errors.append("Choose scan folders and an existing regular Shows destination; "
            "the entry limit must be from 1 to 1,000,000.");
        return plan;
    }
    const QString destination = QDir::cleanPath(showsDirectory);
    plan.destinationDirectory = destination;
    plan.destinationSnapshot = captureSourceSnapshot(destination);
    if (!plan.destinationSnapshot.valid) {
        plan.errors.append("The destination folder identity could not be captured.");
        return plan;
    }
    QStringList scopes;
    for (const QString& root : roots) {
        if (root.isEmpty() || !QDir::isAbsolutePath(root) || !isRegularDirectoryChain(root, false)) {
            plan.errors.append(root + ": scan roots must be existing regular folders without reparse ancestors.");
            continue;
        }
        if (isPathInside(root, destination)) {
            plan.errors.append(root + ": a scan root cannot be inside the destination.");
            continue;
        }
        scopes.append(QDir::cleanPath(root));
    }
    if (!plan.errors.isEmpty())
        return plan;
    std::sort(scopes.begin(), scopes.end(), [](const QString& left, const QString& right) {
        return left.size() == right.size() ? left < right : left.size() < right.size();
    });
    QStringList uniqueScopes;
    for (const QString& scope : scopes) {
        bool covered = false;
        for (const QString& prior : uniqueScopes)
            covered = covered || isPathInside(scope, prior);
        if (!covered)
            uniqueScopes.append(scope);
    }

    QStringList videos;
    QStringList companions;
    for (const QString& scope : uniqueScopes) {
        if (stopped())
            return plan;
        if (plan.entriesVisited >= maxEntries) {
            plan.limitReached = true;
            return plan;
        }
        const auto scan = enumerateRegularTree(scope, false, cancelled, [&](const QFileInfo& info) {
            const QString extension = info.suffix().toLower();
            if (videoExtensions.contains(extension))
                videos.append(info.absoluteFilePath());
            else if (subtitleExtensions.contains(extension) || artworkExtensions.contains(extension))
                companions.append(info.absoluteFilePath());
            else
                ++plan.ignoredFiles;
            return true;
        }, {{destination}, maxEntries - plan.entriesVisited});
        plan.entriesVisited += scan.entriesVisited;
        plan.skippedBoundaries += scan.skippedBoundaries;
        plan.limitReached = scan.limitReached;
        plan.cancelled = scan.cancelled;
        if (!scan.error.isEmpty())
            plan.errors.append(scope + ": " + scan.error);
        if (!plan.complete())
            return plan;
    }
    videos.sort(Qt::CaseInsensitive);
    companions.sort(Qt::CaseInsensitive);
    QList<EpisodeGroup> groups;
    QHash<QString, QList<qsizetype>> episodeKeys;
    QHash<QString, QList<qsizetype>> stems;
    for (const QString& video : videos) {
        if (stopped())
            return plan;
        const auto episode = parseTvEpisodeName(video);
        if (!episode) {
            plan.review.append({video, "No unambiguous single-episode filename pattern."});
            continue;
        }
        const qsizetype index = groups.size();
        groups.append({video, *episode, {}, {}});
        episodeKeys[episode->series.toCaseFolded() + '/' + QString::number(episode->season)
            + '/' + QString::number(episode->episode)].append(index);
        const QFileInfo info(video);
        stems[key(info.dir().filePath(info.completeBaseName()))].append(index);
        const QString folder = QDir(destination).filePath(episode->series + '/'
            + QStringLiteral("Season %1").arg(episode->season, 2, 10, QLatin1Char('0')));
        groups.last().files.append({video, QDir(folder).filePath(info.fileName()), video, {}, false});
    }
    for (const auto& indices : episodeKeys) {
        if (indices.size() > 1) {
            for (const qsizetype index : indices)
                groups[index].problem = "Multiple videos claim the same series/season/episode.";
        }
    }
    for (const QString& companion : companions) {
        if (stopped())
            return plan;
        const QFileInfo info(companion);
        QString stem = info.completeBaseName();
        const bool subtitle = subtitleExtensions.contains(info.suffix().toLower());
        const auto& tags = subtitle ? subtitleTags : artworkTags;
        QSet<qsizetype> matches;
        for (;;) {
            const auto indices = stems.value(key(info.dir().filePath(stem)));
            for (const qsizetype index : indices)
                matches.insert(index);
            const qsizetype dot = stem.lastIndexOf('.');
            if (dot < 0 || !tags.contains(stem.mid(dot + 1).toLower()))
                break;
            stem.truncate(dot);
        }
        if (matches.size() != 1) {
            plan.review.append({companion, "Companion does not match exactly one episode basename in this folder."});
            continue;
        }
        auto& group = groups[*matches.cbegin()];
        const QString folder = QFileInfo(group.files.first().destination).absolutePath();
        group.files.append({companion, QDir(folder).filePath(info.fileName()), group.source, {}, true});
    }

    QHash<QString, QSet<qsizetype>> destinationKeys;
    for (qsizetype index = 0; index < groups.size(); ++index) {
        for (const auto& file : groups[index].files)
            destinationKeys[key(file.destination)].insert(index);
    }
    for (const auto& indices : destinationKeys) {
        if (indices.size() > 1) {
            for (const qsizetype index : indices)
                groups[index].problem = "Different candidates have the same destination.";
        }
    }
    for (auto& group : groups) {
        for (auto& file : group.files) {
            if (stopped())
                return plan;
            if (!group.problem.isEmpty())
                break;
            if (!isRegularDirectoryChain(QFileInfo(file.source).absolutePath(), false)
                    || platform::nativePathKind(file.source) != platform::NativePathKind::Regular
                    || !QFileInfo(file.source).isFile()) {
                group.problem = "Source is no longer an available regular file.";
                break;
            }
            file.sourceSnapshot = captureSourceSnapshot(file.source);
            if (!file.sourceSnapshot.valid || (file.sourceSnapshot.attributes & (0x10U | 0x400U)) != 0) {
                group.problem = "Source file identity could not be captured safely.";
                break;
            }
            if (!validateWindowsPath(file.destination).valid
                    || !isPathInside(file.destination, destination)
                    || !isRegularDirectoryChain(QFileInfo(file.destination).absolutePath(), true)
                    || platform::nativePathKind(file.destination) != platform::NativePathKind::Missing) {
                group.problem = "Destination exists or its folder path is not safe/available.";
                break;
            }
        }
        if (!group.problem.isEmpty()) {
            for (const auto& file : group.files)
                plan.review.append({file.source, group.problem});
        } else {
            plan.candidates.append(group.files);
        }
    }
    stopped();
    return plan;
}

} // namespace filemanager::files
