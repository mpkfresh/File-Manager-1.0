#include "files/reviewedtransfer.h"
#include "files/operationjournal.h"
#include "files/transferqueue.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

bool writeFixture(const QString& path, const QByteArray& contents = "fixture")
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

#ifdef Q_OS_WIN
HANDLE holdWriter(const QString& path)
{
    return CreateFileW(reinterpret_cast<LPCWSTR>(
        filemanager::platform::extendedLengthPath(path).utf16()), GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}
#endif

} // namespace

bool runReviewedTransferTests()
{
    using namespace filemanager::files;
    bool passed = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: reviewed transfer: " << message << '\n';
            passed = false;
        }
    };
    QTemporaryDir fixture;
    check(fixture.isValid(), "disposable fixture directory");
    if (!fixture.isValid())
        return false;
    const QString input = fixture.filePath("Input");
    const QString shows = fixture.filePath("Shows");
    check(QDir().mkpath(input) && QDir().mkpath(shows), "create explicit scope fixtures");
    const QString first = QDir(input).filePath("Series.S01E01.mkv");
    const QString subtitle = QDir(input).filePath("Series.S01E01.en.srt");
    const QString second = QDir(input).filePath("Series.S01E02.mkv");
    check(writeFixture(first) && writeFixture(subtitle) && writeFixture(second), "create episode group fixtures");
    const auto plan = planTvOrganization({input}, shows);
    check(plan.complete() && plan.destinationSnapshot.valid && plan.candidates.size() == 3,
        "planner retains destination root identity");
    const auto constructed = createReviewedTvMove(plan, {first});
    check(constructed.valid() && constructed.request.sources.size() == 2
            && constructed.request.sources.contains(subtitle) && !constructed.request.sources.contains(second)
            && constructed.request.reviewedMoves.size() == 2,
        "construction selects a complete episode group, not individual sidecars");
    check(constructed.request.reviewedDestination == plan.destinationSnapshot,
        "request construction retains, rather than recaptures, destination identity");
    for (const auto& move : constructed.request.reviewedMoves) {
        check(move.sourceSnapshot == captureSourceSnapshot(move.source), "retained source snapshot");
        check(move.destination.startsWith(shows + "/Series/Season 01/"), "exact reviewed nested destination");
    }
    check(validateReviewedMove(constructed.request).valid(), "read-only preflight accepts unchanged group");
    check(!QFileInfo::exists(shows + "/Series"), "preflight never creates proposed directories");
    check(!createReviewedTvMove(plan, {}).valid()
            && !createReviewedTvMove(plan, {subtitle}).valid()
            && !createReviewedTvMove(plan, {fixture.filePath("unknown")}).valid(),
        "empty, companion-only and unknown group selections are refused");
    auto cancelledPlan = plan;
    cancelledPlan.cancelled = true;
    check(!createReviewedTvMove(cancelledPlan, {first}).valid(), "cancelled preview cannot construct a request");
    auto oversizedPlan = plan;
    oversizedPlan.candidates.clear();
    for (qsizetype index = 0; index <= MaximumReviewedMoveFiles; ++index)
        oversizedPlan.candidates.append(plan.candidates.first());
    check(!createReviewedTvMove(oversizedPlan, {oversizedPlan.candidates.first().episodeSource}).valid(),
        "oversized groups are refused rather than silently truncated");
    std::atomic_bool cancel{true};
    const auto cancelledValidation = validateReviewedMove(constructed.request, &cancel);
    check(cancelledValidation.cancelled && !cancelledValidation.valid(), "pre-cancellation invalidates preflight");

    auto malformed = constructed.request;
    malformed.operation = TransferOperation::Copy;
    check(!validateReviewedMove(malformed).valid(), "reviewed moves cannot become copies");
    TransferFailure permissionFailure;
    permissionFailure.category = TransferFailureCategory::PermissionDenied;
    permissionFailure.nativeCode = 5;
    check(!isElevationEligibleCopy(malformed, {permissionFailure}),
        "elevation cannot bypass reviewed constraints through a copy retry");
    malformed = constructed.request;
    malformed.reviewedMove = false;
    check(!validateReviewedMove(malformed).valid() && malformed.hasReviewConstraints(),
        "clearing the marker cannot turn identity-bearing requests into plain moves");
    malformed = constructed.request;
    malformed.reviewedMoves.clear();
    check(!validateReviewedMove(malformed).valid(), "missing retained mappings are rejected");
    malformed = constructed.request;
    malformed.sources[0] = second;
    check(!validateReviewedMove(malformed).valid(), "source list must agree with retained mapping order");
    const QString previousDirectory = QDir::currentPath();
    check(QDir::setCurrent(input), "enter disposable source directory for relative-path test");
    malformed = constructed.request;
    malformed.sources[0] = QFileInfo(malformed.sources.first()).fileName();
    check(!validateReviewedMove(malformed).valid(), "a matching relative source path is still invalid");
    check(QDir::setCurrent(previousDirectory), "restore test working directory");
    malformed = constructed.request;
    malformed.reviewedMoves[0].destination = fixture.filePath(QFileInfo(first).fileName());
    check(!validateReviewedMove(malformed).valid(), "mapping cannot escape the reviewed destination");
    malformed = constructed.request;
    malformed.reviewedMoves[0].destination = shows + "/Series/Season 01/Changed.mkv";
    check(!validateReviewedMove(malformed).valid(), "move cannot silently rename the episode");
    malformed = constructed.request;
    malformed.sources[1] = malformed.sources[0];
    malformed.reviewedMoves[1] = malformed.reviewedMoves[0];
    check(!validateReviewedMove(malformed).valid(), "duplicate mappings are refused");
    malformed = constructed.request;
    malformed.reviewedMoves[1].sourceSnapshot.valid = false;
    check(!validateReviewedMove(malformed).valid(), "every file, including companions, requires a retained identity");

    const QString collision = constructed.request.reviewedMoves.first().destination;
    check(QDir().mkpath(QFileInfo(collision).absolutePath()) && writeFixture(collision, "existing"),
        "create late destination collision");
    check(!validateReviewedMove(constructed.request).valid(), "a late collision rejects the entire batch");
    check(QFile::remove(collision) && validateReviewedMove(constructed.request).valid(),
        "destination contents/timestamps may change without replacing the root identity");
    const QString retained = fixture.filePath("original-retained.mkv");
    check(QFile::rename(first, retained) && writeFixture(first), "replace source at its original path");
    const auto unchangedConstruction = createReviewedTvMove(plan, {first});
    check(unchangedConstruction.valid() && !validateReviewedMove(unchangedConstruction.request).valid(),
        "construction never adopts a replacement source when given the original preview");
    check(QFile::remove(first) && QFile::rename(retained, first), "restore only the disposable original fixture");
    const QString oldShows = fixture.filePath("OldShows");
    check(QDir().rename(shows, oldShows) && QDir().mkpath(shows), "replace destination root at the same path");
    check(!validateReviewedMove(constructed.request).valid(), "destination root replacement is rejected");

#ifdef Q_OS_WIN
    {
        QTemporaryDir tagFixture;
        const QString tagInput = tagFixture.filePath("Input");
        const QString tagOutput = tagFixture.filePath("Sorted");
        const QString changedSource = tagInput + "/changed.txt";
        const QString steadySource = tagInput + "/steady.txt";
        check(tagFixture.isValid() && QDir().mkpath(tagInput)
                && QDir().mkpath(tagOutput)
                && writeFixture(changedSource, "original")
                && writeFixture(steadySource, "steady"),
            "create two explicitly reviewed tag-move fixtures");
        TransferRequest tagRequest;
        tagRequest.sources = {changedSource, steadySource};
        tagRequest.destinationDirectory = tagOutput;
        tagRequest.operation = TransferOperation::Move;
        tagRequest.reviewedMove = true;
        tagRequest.reviewedSkipStaleItems = true;
        tagRequest.reviewedDestination = captureSourceSnapshot(tagOutput);
        tagRequest.reviewedMoves = {
            {changedSource, tagOutput + "/changed.txt", captureSourceSnapshot(changedSource)},
            {steadySource, tagOutput + "/steady.txt", captureSourceSnapshot(steadySource)}};
        auto malformedTagRequest = tagRequest;
        malformedTagRequest.reviewedMoves[1].destination =
            malformedTagRequest.reviewedMoves.first().destination;
        const auto malformedTag = executeReviewedMove(malformedTagRequest);
        check(!malformedTag.complete() && malformedTag.items.isEmpty()
                && QFileInfo::exists(changedSource) && QFileInfo::exists(steadySource)
                && !QFileInfo::exists(tagOutput + "/changed.txt"),
            "reject a malformed tag batch before changing any source");
        QFile changedTag(changedSource);
        check(changedTag.open(QIODevice::Append)
                && changedTag.write(" later") == 6,
            "change one source after tag-move confirmation");
        changedTag.close();
        QList<ReviewedMoveItemResult> reported;
        const auto partialTag = executeReviewedMove(tagRequest, {}, nullptr, false, {}, {},
            [&](const ReviewedMoveItemResult& item, const SourceSnapshot&) {
                reported.append(item);
            });
        check(!partialTag.complete() && partialTag.items.size() == 2
                && reported.size() == 2 && !partialTag.errors.isEmpty()
                && !partialTag.items.first().moved
                && partialTag.items.last().moved
                && QFileInfo::exists(changedSource)
                && !QFileInfo::exists(tagOutput + "/changed.txt")
                && !QFileInfo::exists(steadySource)
                && QFileInfo::exists(tagOutput + "/steady.txt"),
            "skip and report a changed tag item while moving its unchanged peer");
    }
    {
        QTemporaryDir executionFixture;
        const QString executionInput = executionFixture.filePath("Input");
        const QString executionShows = executionFixture.filePath("Shows");
        const QString episode = executionInput + "/Batch.S01E01.mkv";
        const QString companion = executionInput + "/Batch.S01E01.en.srt";
        check(executionFixture.isValid() && QDir().mkpath(executionInput)
                && QDir().mkpath(executionShows) && writeFixture(episode, "video")
                && writeFixture(companion, "subtitle"),
            "create disposable guarded-batch fixtures");
        const auto executionPlan = planTvOrganization({executionInput}, executionShows);
        const auto executionRequest = createReviewedTvMove(executionPlan, {episode});
        const auto execution = executeReviewedMove(executionRequest.request);
        check(execution.complete() && execution.items.size() == 2
                && execution.createdDirectories.size() == 2,
            "execute the complete reviewed group and create its retained folder chain");
        for (const auto& item : execution.items) {
            check(item.moved && item.destinationCommitted && item.sourceRemoved
                    && item.destinationSnapshot.valid && !QFileInfo::exists(item.source)
                    && QFileInfo::exists(item.destination),
                "record every committed and source-removed batch item");
        }
    }
    {
        QTemporaryDir cancelledFixture;
        const QString cancelledInput = cancelledFixture.filePath("Input");
        const QString cancelledShows = cancelledFixture.filePath("Shows");
        const QString episode = cancelledInput + "/Cancelled.S01E01.mkv";
        check(cancelledFixture.isValid() && QDir().mkpath(cancelledInput)
                && QDir().mkpath(cancelledShows) && writeFixture(episode),
            "create cancelled guarded-batch fixture");
        const auto cancellationExecutionPlan = planTvOrganization({cancelledInput}, cancelledShows);
        const auto cancelledRequest = createReviewedTvMove(cancellationExecutionPlan, {episode});
        std::atomic_bool cancelled{true};
        const auto execution = executeReviewedMove(cancelledRequest.request, {}, &cancelled);
        check(execution.cancelled && !execution.complete() && QFileInfo::exists(episode)
                && !QFileInfo::exists(cancelledShows + "/Cancelled"),
            "pre-cancel a reviewed batch without creating folders or moving files");
    }
    {
        QTemporaryDir partialFixture;
        const QString partialInput = partialFixture.filePath("Input");
        const QString partialShows = partialFixture.filePath("Shows");
        const QString firstEpisode = partialInput + "/Partial.S01E01.mkv";
        const QString secondEpisode = partialInput + "/Partial.S01E02.mkv";
        check(partialFixture.isValid() && QDir().mkpath(partialInput)
                && QDir().mkpath(partialShows) && writeFixture(firstEpisode, "first")
                && writeFixture(secondEpisode, "second"),
            "create a mid-batch sharing-conflict fixture");
        const auto partialPlan = planTvOrganization({partialInput}, partialShows);
        const auto partialRequest = createReviewedTvMove(
            partialPlan, {firstEpisode, secondEpisode});
        check(partialRequest.valid() && partialRequest.request.reviewedMoves.size() == 2,
            "retain both ordered batch items before interference");
        const QString blockedSource = partialRequest.request.reviewedMoves.at(1).source;
        const HANDLE writer = holdWriter(blockedSource);
        check(writer != INVALID_HANDLE_VALUE, "hold a writer that conflicts only with guarded execution");
        const auto execution = executeReviewedMove(partialRequest.request);
        if (writer != INVALID_HANDLE_VALUE)
            CloseHandle(writer);
        check(!execution.complete() && !execution.cancelled && execution.items.size() == 2
                && execution.items.first().moved && !execution.items.last().moved
                && QFileInfo::exists(execution.items.first().destination)
                && QFileInfo::exists(execution.items.last().source)
                && !execution.errors.isEmpty(),
            "stop after a mid-batch failure and report the committed prefix without rollback");
    }
    {
        QTemporaryDir queueFixture;
        const QString queueInput = queueFixture.filePath("Input");
        const QString queueShows = queueFixture.filePath("Shows");
        const QString episode = queueInput + "/Queued.S01E01.mkv";
        const QString companion = queueInput + "/Queued.S01E01.en.srt";
        check(queueFixture.isValid() && QDir().mkpath(queueInput) && QDir().mkpath(queueShows)
                && writeFixture(episode, "queued video") && writeFixture(companion, "queued subtitle"),
            "create live guarded-queue fixtures");
        const auto queuePlan = planTvOrganization({queueInput}, queueShows);
        const auto queueRequest = createReviewedTvMove(queuePlan, {episode});
        TransferQueue guardedQueue;
        QEventLoop guardedQueueLoop;
        int manifestChanges = 0;
        QObject::connect(&guardedQueue, &TransferQueue::recoveryManifestChanged,
            &guardedQueueLoop, [&](quint64) { ++manifestChanges; });
        QObject::connect(&guardedQueue, &TransferQueue::jobFinished,
            &guardedQueueLoop, &QEventLoop::quit);
        QTimer::singleShot(15'000, &guardedQueueLoop, &QEventLoop::quit);
        const quint64 guardedId = guardedQueue.enqueue(queueRequest.request);
        guardedQueueLoop.exec();
        const auto history = guardedQueue.history();
        const auto guardedRecord = history.isEmpty() ? TransferRecord{} : history.first();
        const auto fileManifests = std::count_if(guardedRecord.manifest.cbegin(),
            guardedRecord.manifest.cend(), [](const TransferManifestEntry& entry) {
                return !entry.directory;
            });
        const auto directoryManifests = std::count_if(guardedRecord.manifest.cbegin(),
            guardedRecord.manifest.cend(), [](const TransferManifestEntry& entry) {
                return entry.directory;
            });
        check(guardedRecord.id == guardedId && guardedRecord.state == TransferJobState::Succeeded
                && guardedRecord.errors.isEmpty() && guardedRecord.failures.isEmpty()
                && guardedRecord.completedBytes == guardedRecord.totalBytes
                && fileManifests == 2 && directoryManifests == 2 && manifestChanges >= 4,
            "run a reviewed batch through the live queue with per-item manifest updates");
        for (const auto& entry : guardedRecord.manifest) {
            check(entry.destinationCreated && entry.identityAvailable
                    && (entry.directory
                        ? entry.sourcePath.isEmpty() && !entry.sourceIdentityAvailable
                        : entry.sourceRemoved && entry.sourceIdentityAvailable),
                "record safe directory and file recovery semantics separately");
        }
        OperationJournal guardedOperationJournal(queueFixture.filePath("guarded-journal.dat"));
        guardedOperationJournal.synchronize({guardedRecord});
        const QByteArray guardedJournal = serializeOperationJournal(
            guardedOperationJournal.records());
        QList<JournalRecord> decodedGuarded;
        int guardedSchema = 0;
        QString guardedError;
        check(parseOperationJournal(guardedJournal, &decodedGuarded, &guardedSchema, &guardedError)
                && guardedSchema == 3 && decodedGuarded.size() == 1
                && decodedGuarded.first().manifest.size() == guardedRecord.manifest.size(),
            "persist live reviewed manifests while discarding replay authority");
        check(!guardedQueue.retryFailed(guardedId).has_value(),
            "never synthesize a retry from a completed reviewed request");
    }
#endif

    int copyCalls = 0;
    TransferController controller([&](const QString&, const QString&, bool,
            const std::function<bool(qint64, qint64)>&, bool) {
        ++copyCalls;
        return filemanager::platform::NativeCopyResult{true, false, {}};
    });
    QEventLoop controllerLoop;
    bool controllerFinished = false;
    QStringList controllerErrors;
    QObject::connect(&controller, &TransferController::finished, &controllerLoop,
        [&](quint64, bool, const QStringList& errors) {
            controllerFinished = true;
            controllerErrors = errors;
            controllerLoop.quit();
        });
    QTimer controllerTimeout;
    controllerTimeout.setSingleShot(true);
    QObject::connect(&controllerTimeout, &QTimer::timeout, &controllerLoop, &QEventLoop::quit);
    controllerTimeout.start(15'000);
    controller.start(constructed.request);
    controllerLoop.exec();
    check(controllerFinished && !controllerErrors.isEmpty() && copyCalls == 0 && QFileInfo::exists(first),
        "guarded requests cannot fall through to path-only copy/delete execution");
    controller.cancel();

    TransferQueue queue;
    QEventLoop queueLoop;
    bool queueFinished = false;
    QObject::connect(&queue, &TransferQueue::jobFinished, &queueLoop, [&](quint64, bool, const QStringList&) {
        queueFinished = true;
        queueLoop.quit();
    });
    QTimer queueTimeout;
    queueTimeout.setSingleShot(true);
    QObject::connect(&queueTimeout, &QTimer::timeout, &queueLoop, &QEventLoop::quit);
    queueTimeout.start(15'000);
    const quint64 id = queue.enqueue(constructed.request);
    queueLoop.exec();
    check(queueFinished && !queue.retryFailed(id).has_value(), "queue retry never drops review authority");
    queue.cancel(id);

    OperationJournal journal(fixture.filePath("journal.dat"));
    auto record = queue.history().first();
    record.state = TransferJobState::Running;
    record.queuedAt = QDateTime::currentDateTimeUtc();
    record.startedAt = record.queuedAt;
    record.finishedAt = {};
    journal.synchronize({record});
    const auto encoded = serializeOperationJournal(journal.records(), 1);
    QList<JournalRecord> decoded;
    int schema = 0;
    QString error;
    check(parseOperationJournal(encoded, &decoded, &schema, &error) && schema == 3
            && decoded.size() == 1 && decoded.first().request.reviewedMove
            && decoded.first().request.reviewedMoves.isEmpty()
            && !validateReviewedMove(decoded.first().request).valid(),
        "schema 3 preserves non-replayable review marker without inventing persisted authority");
    auto downgraded = QJsonDocument::fromJson(encoded).object();
    downgraded.insert("schema", 2);
    check(!parseOperationJournal(QJsonDocument(downgraded).toJson(), &decoded, nullptr, &error),
        "review marker cannot be decoded as legacy schema");
    auto missingMarker = QJsonDocument::fromJson(encoded).object();
    auto missingRecord = missingMarker.value("records").toArray().first().toObject();
    missingRecord.remove("reviewedMove");
    missingMarker.insert("records", QJsonArray{missingRecord});
    check(!parseOperationJournal(QJsonDocument(missingMarker).toJson(), &decoded, nullptr, &error),
        "schema 3 rejects records with missing review marker");
    check(journal.flush(&error), "write encrypted disposable journal");
    OperationJournal restored(fixture.filePath("journal.dat"));
    check(restored.load(&error) && restored.interruptedRecords().size() == 1
            && !restored.retryRequest(record.durableId).has_value(),
        "restart recovery requires a new review, never a path-only replay");
    QFile original(first);
    check(original.open(QIODevice::ReadOnly) && original.readAll() == "fixture"
            && QDir(shows).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        "request, preflight and refused execution preserve source contents and destination");
    return passed;
}
