#include "platform/guardedfilerename.h"
#include "platform/nativefilecopy.h"
#include "files/reviewedtransfer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <atomic>
#include <iostream>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QStringList transferArtifacts(const QString& directory)
{
    return QDir(directory).entryList(
        {QStringLiteral(".file-manager-copy-*"), QStringLiteral(".file-manager-guard-*")},
        QDir::Files | QDir::Hidden | QDir::System);
}

#ifdef Q_OS_WIN
HANDLE openNative(const QString& path, DWORD access)
{
    return CreateFileW(reinterpret_cast<LPCWSTR>(
        filemanager::platform::extendedLengthPath(path).utf16()), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

QString writableFixedVolumeTemplate(quint64 excludedVolumeSerial)
{
    const DWORD drives = GetLogicalDrives();
    for (int index = 0; index < 26; ++index) {
        if (!(drives & (1UL << index)))
            continue;
        const wchar_t root[] = {static_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
        if (GetDriveTypeW(root) != DRIVE_FIXED)
            continue;
        ULARGE_INTEGER available{};
        if (!GetDiskFreeSpaceExW(root, &available, nullptr, nullptr)
                || available.QuadPart < 64ULL * 1024 * 1024)
            continue;
        DWORD serial = 0;
        if (!GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)
                || serial == excludedVolumeSerial)
            continue;
        return QDir::fromNativeSeparators(QString::fromWCharArray(root))
            + QStringLiteral("FileManagerCrossVolume-XXXXXX");
    }
    return {};
}
#endif

} // namespace

bool runGuardedCrossVolumeTests()
{
    using namespace filemanager::files;
    using namespace filemanager::platform;
    bool passed = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: guarded cross-volume move: " << message << '\n';
            passed = false;
        }
    };

    QTemporaryDir destinationFixture(QDir::current().filePath(
        "out/guarded-cross-volume-XXXXXX"));
    const auto destinationIdentity = captureSourceSnapshot(destinationFixture.path());
#ifdef Q_OS_WIN
    const QString sourceTemplate = destinationIdentity.valid
        ? writableFixedVolumeTemplate(destinationIdentity.volumeSerial) : QString{};
    auto sourceFixture = std::make_unique<QTemporaryDir>(sourceTemplate);
#else
    auto sourceFixture = std::make_unique<QTemporaryDir>();
#endif
    check(sourceFixture->isValid() && destinationFixture.isValid(),
        "create disposable fixtures on the system and workspace volumes");
    if (!sourceFixture->isValid() || !destinationFixture.isValid())
        return false;

    const QString shows = destinationFixture.filePath("Shows");
    const QString season = shows + "/Series/Season 01";
    check(QDir().mkpath(season), "create an existing reviewed destination tree");
    const auto rootIdentity = captureSourceSnapshot(shows);
    const auto seasonIdentity = captureSourceSnapshot(season);

#ifdef Q_OS_WIN
    check(rootIdentity.valid && seasonIdentity.valid
            && captureSourceSnapshot(sourceFixture->path()).valid,
        "capture both volume identities");
    const bool distinctVolumes = seasonIdentity.volumeSerial
        != captureSourceSnapshot(sourceFixture->path()).volumeSerial;
    check(distinctVolumes, "exercise two genuinely different filesystem volumes");
    if (!distinctVolumes)
        return passed;

    const QString reviewSource = sourceFixture->filePath("ReviewGate.S01E00.mkv");
    const QString reviewDestination = shows + "/ReviewGate/Season 01/ReviewGate.S01E00.mkv";
    check(writeFile(reviewSource, "review gate"), "create whole-batch metadata-review fixture");
    TransferRequest reviewedRequest;
    reviewedRequest.sources = {reviewSource};
    reviewedRequest.destinationDirectory = shows;
    reviewedRequest.operation = TransferOperation::Move;
    reviewedRequest.reviewedMove = true;
    reviewedRequest.reviewedDestination = rootIdentity;
    reviewedRequest.reviewedMoves = {{reviewSource, reviewDestination,
        captureSourceSnapshot(reviewSource)}};
    int metadataProbes = 0;
    const ReviewedMetadataProbe lossyProbe = [&](const QString&, const QString&) {
        ++metadataProbes;
        NativeMetadataReview review;
        review.inspected = true;
        review.degradations = {"Synthetic destination cannot retain named streams."};
        return review;
    };
    const auto metadataReview = reviewReviewedMoveMetadata(reviewedRequest, nullptr, lossyProbe);
    check(metadataReview.valid() && metadataReview.confirmationRequired()
            && metadataReview.items.size() == 1 && metadataProbes == 1,
        "review every cross-volume item before mutation and expose degradation details");
    const auto refusedReview = executeReviewedMove(reviewedRequest, {}, nullptr, false, lossyProbe);
    check(refusedReview.metadataConfirmationRequired && !refusedReview.complete()
            && QFileInfo::exists(reviewSource) && !QFileInfo::exists(reviewDestination)
            && !QFileInfo::exists(shows + "/ReviewGate"),
        "require whole-batch metadata consent before creating folders or moving any file");
    const ReviewedMetadataProbe failedProbe = [](const QString&, const QString&) {
        NativeMetadataReview review;
        review.error = "Synthetic metadata inspection failure";
        return review;
    };
    const auto failedReview = executeReviewedMove(reviewedRequest, {}, nullptr, false, failedProbe);
    check(!failedReview.metadataConfirmationRequired && !failedReview.errors.isEmpty()
            && QFileInfo::exists(reviewSource) && !QFileInfo::exists(reviewDestination),
        "fail closed when metadata capabilities cannot be inspected");
    reviewedRequest.reviewedApprovedMetadataWarnings = {
        QDir::toNativeSeparators(reviewSource)
            + ": Synthetic destination cannot retain named streams."};
    const ReviewedMetadataProbe newLossProbe = [](const QString&, const QString&) {
        NativeMetadataReview review;
        review.inspected = true;
        review.degradations = {"Synthetic destination no longer retains encryption."};
        return review;
    };
    const auto newLoss = executeReviewedMove(reviewedRequest, {}, nullptr, true, newLossProbe);
    check(newLoss.metadataConfirmationRequired && !newLoss.complete()
            && QFileInfo::exists(reviewSource) && !QFileInfo::exists(reviewDestination),
        "a new loss is refused even after a different loss was approved");
    const auto approvedReview = executeReviewedMove(reviewedRequest, {}, nullptr, true, lossyProbe);
    check(approvedReview.complete() && !QFileInfo::exists(reviewSource)
            && readFile(reviewDestination) == "review gate",
        "apply an explicit batch-scoped degradation approval through guarded execution");

    const QString lateSource = sourceFixture->filePath("NativeConsent.S01E00.mkv");
    const QString lateDestination = season + "/NativeConsent.S01E00.mkv";
    check(writeFile(lateSource, "native consent gate"),
        "create a separate final-copy metadata consent fixture");
    const NativeMetadataProbe lateLoss = [](const QString&, const QString&) {
        NativeMetadataReview review;
        review.inspected = true;
        review.degradations = {"Synthetic newly detected native loss."};
        return review;
    };
    const QStringList earlierApproval{"Previously approved different loss."};
    {
        GuardedCrossVolumeMove move(lateSource, captureSourceSnapshot(lateSource),
            lateDestination, shows, rootIdentity);
        const auto refused = move.commit({}, nullptr, true, &earlierApproval, lateLoss);
        check(refused.metadataConfirmationRequired && !refused.destinationCommitted
                && refused.metadataDegradations.contains(
                    "Synthetic newly detected native loss.")
                && QFileInfo::exists(lateSource) && !QFileInfo::exists(lateDestination),
            "a later native warning outside the exact approval stops before staging");
    }
    check(transferArtifacts(season).isEmpty(),
        "native consent refusal leaves no transfer staging or guard files");
    const QStringList exactApproval{"Synthetic newly detected native loss."};
    GuardedCrossVolumeResult accepted;
    {
        GuardedCrossVolumeMove move(lateSource, captureSourceSnapshot(lateSource),
            lateDestination, shows, rootIdentity);
        accepted = move.commit({}, nullptr, true, &exactApproval, lateLoss);
    }
    // The guard retains a DELETE-access destination handle until destruction.
    // QFile's read handle need not share DELETE, so inspect contents afterward.
    const bool sourceGone = !QFileInfo::exists(lateSource);
    const QByteArray destinationBytes = readFile(lateDestination);
    const bool acceptedPass = accepted.moved && accepted.destinationCommitted
        && accepted.sourceRemoved && sourceGone
        && destinationBytes == "native consent gate";
    if (!acceptedPass) {
        std::cerr << "Exact native consent diagnostic: error="
                  << accepted.error.toStdString() << " code=" << accepted.nativeCode
                  << " confirmation=" << accepted.metadataConfirmationRequired
                  << " moved=" << accepted.moved
                  << " committed=" << accepted.destinationCommitted
                  << " removed=" << accepted.sourceRemoved
                  << " source_gone=" << sourceGone
                  << " destination_bytes=" << destinationBytes.size()
                  << " warnings=" << accepted.metadataDegradations.join(" | ").toStdString()
                  << "\n";
    }
    check(acceptedPass,
        "the exact approved native warning permits the guarded move");
    check(transferArtifacts(season).isEmpty(),
        "approved native consent leaves no transfer artifacts");

    const auto destinationFor = [&](const QString& source) {
        return season + "/" + QFileInfo(source).fileName();
    };

    const QString successSource = sourceFixture->filePath(QString::fromUtf8("Série.S01E01.mkv"));
    const QByteArray successBytes("episode bytes across volumes");
    check(writeFile(successSource, successBytes)
            && writeFile(successSource + ":subtitle", "retained stream"),
        "create source bytes and an alternate data stream");
    const auto successSnapshot = captureSourceSnapshot(successSource);
    const QString successDestination = destinationFor(successSource);
    {
        GuardedCrossVolumeMove move(successSource, successSnapshot, successDestination,
            shows, rootIdentity);
        if (!move.ready())
            std::cerr << "Cross-volume preparation: " << move.error().toStdString() << '\n';
        check(move.ready(), "prepare the reviewed cross-volume move");
        const HANDLE writer = openNative(successSource, GENERIC_WRITE);
        check(writer == INVALID_HANDLE_VALUE, "block source writes while copying is guarded");
        if (writer != INVALID_HANDLE_VALUE)
            CloseHandle(writer);
        check(!MoveFileExW(reinterpret_cast<LPCWSTR>(extendedLengthPath(season).utf16()),
                reinterpret_cast<LPCWSTR>(extendedLengthPath(shows + "/ChangedSeason").utf16()), 0),
            "retain the exact destination directory while prepared");
        int progressCalls = 0;
        const auto result = move.commit([&](qint64, qint64) {
            ++progressCalls;
            return true;
        });
        if (!result.moved)
            std::cerr << "Cross-volume commit: " << result.error.toStdString() << '\n';
        check(result.moved && result.destinationCommitted && result.sourceRemoved
                && !result.cancelled && result.error.isEmpty() && progressCalls > 0,
            "copy, commit and remove the reviewed source");
        check(result.destinationSnapshot.valid
                && result.destinationSnapshot.volumeSerial == destinationIdentity.volumeSerial
                && result.destinationSnapshot.volumeSerial != successSnapshot.volumeSerial,
            "report the exact committed destination identity on the other volume");
        check(!QFileInfo::exists(successSource), "remove the exact reviewed source after commit");
        check(!move.commit().moved, "allow only one commit attempt");
    }
    check(readFile(successDestination) == successBytes,
        "preserve default file content before deleting the source");
    check(readFile(successDestination + ":subtitle") == "retained stream",
        "preserve the named stream before deleting the source");
    check(transferArtifacts(season).isEmpty(), "leave no staging or guard files after success");

    const QString collisionSource = sourceFixture->filePath("Collision.S01E02.mkv");
    const QString collisionDestination = destinationFor(collisionSource);
    check(writeFile(collisionSource, "source") && writeFile(collisionDestination, "existing"),
        "create a late destination collision fixture");
    {
        const auto snapshot = captureSourceSnapshot(collisionSource);
        QFile::remove(collisionDestination);
        GuardedCrossVolumeMove move(collisionSource, snapshot, collisionDestination,
            shows, rootIdentity);
        check(move.ready() && writeFile(collisionDestination, "existing"),
            "introduce the collision after preparation");
        const auto result = move.commit();
        check(!result.moved && !result.destinationCommitted && QFileInfo::exists(collisionSource)
                && readFile(collisionDestination) == "existing",
            "never overwrite a late destination collision");
    }
    check(transferArtifacts(season).isEmpty(), "clean staging and guard files after a collision");
    QFile::remove(collisionDestination);

    const QString cancelledSource = sourceFixture->filePath("Cancelled.S01E03.mkv");
    const QString cancelledDestination = destinationFor(cancelledSource);
    check(writeFile(cancelledSource, QByteArray(8 * 1024 * 1024, 'c')),
        "create a cancellable transfer fixture");
    {
        GuardedCrossVolumeMove move(cancelledSource, captureSourceSnapshot(cancelledSource),
            cancelledDestination, shows, rootIdentity);
        check(move.ready(), "prepare cancellable cross-volume copy");
        const auto result = move.commit([](qint64, qint64) { return false; });
        check(result.cancelled && !result.moved && !result.destinationCommitted
                && QFileInfo::exists(cancelledSource) && !QFileInfo::exists(cancelledDestination),
            "cancellation removes staging and preserves the source");
    }
    check(transferArtifacts(season).isEmpty(), "leave no transfer artifacts after cancellation");

    const QString partialSource = sourceFixture->filePath("Partial.S01E04.mkv");
    const QString partialDestination = destinationFor(partialSource);
    check(writeFile(partialSource, QByteArray(1024 * 1024, 'p')),
        "create a post-commit cancellation fixture");
    {
        std::atomic_bool cancel{false};
        GuardedCrossVolumeMove move(partialSource, captureSourceSnapshot(partialSource),
            partialDestination, shows, rootIdentity);
        const auto result = move.commit([&](qint64 transferred, qint64 total) {
            if (total > 0 && transferred == total)
                cancel.store(true, std::memory_order_relaxed);
            return true;
        }, &cancel);
        check(result.cancelled && result.destinationCommitted && !result.sourceRemoved
                && !result.moved && QFileInfo::exists(partialSource)
                && QFileInfo::exists(partialDestination),
            "report an honest partial result when cancellation arrives after destination commit");
    }
    check(transferArtifacts(season).isEmpty(), "leave no transfer artifacts after partial completion");

    const QString changedSource = sourceFixture->filePath("Changed.S01E05.mkv");
    const QString changedDestination = destinationFor(changedSource);
    check(writeFile(changedSource, "unchanged bytes"), "create metadata-change fixture");
    {
        const auto snapshot = captureSourceSnapshot(changedSource);
        GuardedCrossVolumeMove move(changedSource, snapshot, changedDestination,
            shows, rootIdentity);
        const HANDLE attributes = openNative(changedSource, FILE_WRITE_ATTRIBUTES);
        check(move.ready() && attributes != INVALID_HANDLE_VALUE,
            "prepare before an attribute-only source change");
        if (attributes != INVALID_HANDLE_VALUE) {
            ULARGE_INTEGER changed{};
            changed.QuadPart = snapshot.lastWriteTime + 10'000'000;
            const FILETIME time{changed.LowPart, changed.HighPart};
            check(SetFileTime(attributes, nullptr, nullptr, &time) != FALSE,
                "change source timestamp through independent attribute access");
            CloseHandle(attributes);
        }
        const auto result = move.commit();
        check(!result.moved && !result.destinationCommitted && QFileInfo::exists(changedSource)
                && !QFileInfo::exists(changedDestination),
            "reject a source changed after preview without copying it");
    }

    const QString replacementSource = sourceFixture->filePath("Replacement.S01E06.mkv");
    const QString retained = sourceFixture->filePath("retained.mkv");
    check(writeFile(replacementSource, "original"), "create replacement fixture");
    const auto replacementSnapshot = captureSourceSnapshot(replacementSource);
    check(QFile::rename(replacementSource, retained) && writeFile(replacementSource, "replacement"),
        "replace the source path after its snapshot");
    {
        GuardedCrossVolumeMove move(replacementSource, replacementSnapshot,
            destinationFor(replacementSource), shows, rootIdentity);
        check(!move.ready() && !move.commit().moved,
            "do not adopt a same-path replacement after review");
    }

    const QString sameVolumeSource = destinationFixture.filePath("same-volume.mkv");
    check(writeFile(sameVolumeSource, "same volume"), "create same-volume rejection fixture");
    {
        GuardedCrossVolumeMove move(sameVolumeSource, captureSourceSnapshot(sameVolumeSource),
            destinationFor(sameVolumeSource), shows, rootIdentity);
        check(!move.ready() && !move.commit().moved && QFileInfo::exists(sameVolumeSource),
            "require same-volume requests to use the guarded rename primitive");
    }
#else
    const QString source = sourceFixture->filePath("episode.mkv");
    check(writeFile(source, "episode"), "create unsupported-platform fixture");
    GuardedCrossVolumeMove move(source, captureSourceSnapshot(source), season + "/episode.mkv",
        shows, rootIdentity);
    check(!move.ready() && !move.commit().moved, "unsupported platform fails closed");
#endif
    return passed;
}
