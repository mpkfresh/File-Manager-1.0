#include "bookorganizer.h"

#include "epubseriesmetadata.h"
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

const QSet<QString> bookExtensions{"epub", "pdf", "mobi", "azw", "azw3", "m4b", "mp3", "flac"};
const QSet<QString> artworkExtensions{"jpg", "jpeg", "png", "webp"};

QString pathKey(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

QString bookKey(const BookSeriesName& name)
{
    return name.series.simplified().toCaseFolded() + QLatin1Char('/')
        + QString::number(name.number);
}

struct BookGroup
{
    BookSeriesName name;
    QString firstSource;
    QList<BookMoveCandidate> files;
    QString problem;
};

} // namespace

std::optional<BookSeriesName> parseBookSeriesName(const QString& fileName)
{
    const QFileInfo info(fileName);
    if (!bookExtensions.contains(info.suffix().toLower()))
        return std::nullopt;
    static const QRegularExpression pattern(R"(^(.+?) - ([0-9]{1,3}) - (.+)$)");
    const auto match = pattern.match(info.completeBaseName());
    if (!match.hasMatch())
        return std::nullopt;
    BookSeriesName name{match.captured(1).simplified(), match.captured(2).toInt(),
        match.captured(3).simplified()};
    if (name.number < 1 || !validateWindowsName(name.series).valid
            || !validateWindowsName(name.title).valid
            || name.series.contains(" - ") || name.title.contains(" - "))
        return std::nullopt;
    return name;
}

BookOrganizationPlan planBookOrganization(const QStringList& roots, const QString& booksDirectory,
    const std::atomic_bool* cancelled, qsizetype maxEntries,
    const QList<BookCorrection>& corrections)
{
    BookOrganizationPlan plan;
    const auto stopped = [&] {
        if (!cancelled || !cancelled->load(std::memory_order_relaxed))
            return false;
        plan.cancelled = true;
        plan.candidates.clear();
        return true;
    };
    if (stopped())
        return plan;
    if (roots.isEmpty() || booksDirectory.isEmpty() || !QDir::isAbsolutePath(booksDirectory)
            || maxEntries < 1 || maxEntries > 1'000'000
            || !isRegularDirectoryChain(booksDirectory, false)) {
        plan.errors.append("Choose scan folders and an existing regular Books destination; "
            "the entry limit must be from 1 to 1,000,000.");
        return plan;
    }
    const QString destination = QDir::cleanPath(booksDirectory);
    plan.destinationDirectory = destination;
    plan.destinationSnapshot = captureSourceSnapshot(destination);
    if (!plan.destinationSnapshot.valid) {
        plan.errors.append("The destination folder identity could not be captured.");
        return plan;
    }
    QHash<QString, BookCorrection> corrected;
    for (const auto& correction : corrections) {
        const QString sourceKey = pathKey(correction.source);
        if (!QDir::isAbsolutePath(correction.source) || corrected.contains(sourceKey)
                || !correction.sourceSnapshot.valid || correction.name.number < 1
                || correction.name.number > 999
                || !validateWindowsName(correction.name.series).valid
                || !validateWindowsName(correction.name.title).valid
                || correction.name.series.contains(" - ")
                || correction.name.title.contains(" - ")
                || isPathInside(correction.source, destination)) {
            plan.errors.append("A book correction has an unsafe name, source or retained identity.");
            return plan;
        }
        corrected.insert(sourceKey, correction);
    }
    QStringList scopes;
    for (const QString& root : roots) {
        if (root.isEmpty() || !QDir::isAbsolutePath(root) || !isRegularDirectoryChain(root, false)
                || isPathInside(root, destination)) {
            plan.errors.append(root + ": scan roots must be existing regular folders outside the destination.");
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
    for (const auto& correction : corrections) {
        bool inScope = false;
        for (const QString& scope : uniqueScopes)
            inScope = inScope || isPathInside(correction.source, scope);
        if (!inScope) {
            plan.errors.append("A book correction is outside the selected scan folders.");
            return plan;
        }
    }

    QStringList books;
    QStringList artwork;
    for (const QString& scope : uniqueScopes) {
        if (stopped())
            return plan;
        if (plan.entriesVisited >= maxEntries) {
            plan.limitReached = true;
            return plan;
        }
        const auto scan = enumerateRegularTree(scope, false, cancelled, [&](const QFileInfo& info) {
            const QString extension = info.suffix().toLower();
            if (corrected.contains(pathKey(info.absoluteFilePath()))
                    || bookExtensions.contains(extension))
                books.append(info.absoluteFilePath());
            else if (artworkExtensions.contains(extension))
                artwork.append(info.absoluteFilePath());
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
    books.sort(Qt::CaseInsensitive);
    artwork.sort(Qt::CaseInsensitive);
    QList<BookGroup> groups;
    QHash<QString, qsizetype> groupIndices;
    QHash<QString, QSet<qsizetype>> stems;
    QSet<QString> encounteredCorrections;
    for (const QString& book : books) {
        if (stopped())
            return plan;
        const QString sourceKey = pathKey(book);
        auto name = parseBookSeriesName(book);
        QString evidence = "Filename evidence; check the proposed series, number and title.";
        if (corrected.contains(sourceKey)) {
            encounteredCorrections.insert(sourceKey);
            const auto& correction = corrected[sourceKey];
            if (captureSourceSnapshot(book) != correction.sourceSnapshot) {
                plan.review.append({book, "File changed since its explicit book assignment."});
                continue;
            }
            name = correction.name;
            evidence = "Explicit one-review book assignment; check this proposal.";
        } else if (QFileInfo(book).suffix().compare("epub", Qt::CaseInsensitive) == 0) {
            const auto embedded = readEpubSeriesMetadata(book, cancelled);
            if (stopped())
                return plan;
            if (!embedded.valid() && embedded.blocksFilenameFallback) {
                plan.review.append({book, embedded.error.isEmpty()
                    ? "Explicit EPUB series claim needs review."
                    : embedded.error});
                continue;
            }
            if (embedded.valid()) {
                const BookSeriesName metadataName{embedded.series, embedded.number, embedded.title};
                if (name && (name->series.compare(metadataName.series, Qt::CaseInsensitive) != 0
                        || name->number != metadataName.number
                        || name->title.compare(metadataName.title, Qt::CaseInsensitive) != 0)) {
                    plan.review.append({book, "Filename and local EPUB series metadata disagree."});
                    continue;
                }
                name = metadataName;
                evidence = "Local EPUB package title, series and number; check the proposal.";
            }
        }
        if (!name) {
            plan.review.append({book,
                "No unambiguous 'Series - 01 - Title' filename or local EPUB series metadata."});
            continue;
        }
        const QString identity = bookKey(*name);
        qsizetype index = groupIndices.value(identity, -1);
        if (index < 0) {
            index = groups.size();
            groupIndices.insert(identity, index);
            groups.append({*name, book, {}, {}});
        }
        auto& group = groups[index];
        if (group.name.title.compare(name->title, Qt::CaseInsensitive) != 0
                || group.name.series.compare(name->series, Qt::CaseInsensitive) != 0)
            group.problem = "Files claim the same series and number with conflicting titles or spelling.";
        const QFileInfo info(book);
        const QString folder = QDir(destination).filePath(group.name.series + '/'
            + QStringLiteral("%1 - %2").arg(group.name.number, 2, 10, QLatin1Char('0'))
                .arg(group.name.title));
        group.files.append({book, QDir(folder).filePath(info.fileName()),
            group.firstSource, {}, false, evidence});
        stems[pathKey(info.dir().filePath(info.completeBaseName()))].insert(index);
    }
    for (const auto& correction : corrections) {
        if (!encounteredCorrections.contains(pathKey(correction.source)))
            plan.review.append({correction.source,
                "Explicitly assigned file is no longer an available regular file in this scan."});
    }
    for (const QString& cover : artwork) {
        if (stopped())
            return plan;
        const QFileInfo info(cover);
        QString stem = info.completeBaseName();
        if (stem.endsWith(".cover", Qt::CaseInsensitive))
            stem.chop(6);
        else if (stem.endsWith(".front", Qt::CaseInsensitive))
            stem.chop(6);
        const auto matches = stems.value(pathKey(info.dir().filePath(stem)));
        if (matches.size() != 1) {
            plan.review.append({cover, "Artwork does not match exactly one book basename in this folder."});
            continue;
        }
        auto& group = groups[*matches.cbegin()];
        group.files.append({cover, QDir(QFileInfo(group.files.first().destination).absolutePath())
            .filePath(info.fileName()), group.firstSource, {}, true});
    }
    QHash<QString, qsizetype> destinationOwners;
    for (qsizetype index = 0; index < groups.size(); ++index) {
        for (const auto& file : groups[index].files) {
            const QString target = pathKey(file.destination);
            if (destinationOwners.contains(target)) {
                groups[index].problem = "Multiple files have the same proposed destination.";
                groups[destinationOwners.value(target)].problem = groups[index].problem;
            } else {
                destinationOwners.insert(target, index);
            }
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
    for (auto& item : plan.review) {
        if (isRegularDirectoryChain(QFileInfo(item.path).absolutePath(), false)
                && platform::nativePathKind(item.path) == platform::NativePathKind::Regular
                && QFileInfo(item.path).isFile()) {
            item.sourceSnapshot = captureSourceSnapshot(item.path);
            if ((item.sourceSnapshot.attributes & (0x10U | 0x400U)) != 0)
                item.sourceSnapshot = {};
        }
    }
    stopped();
    return plan;
}

} // namespace filemanager::files
