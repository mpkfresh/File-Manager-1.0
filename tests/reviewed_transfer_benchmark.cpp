#include "files/reviewedtransfer.h"
#include "files/transferqueue.h"
#include "files/tvorganizer.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

namespace {

constexpr int FileCount = 1'024;
constexpr int CancellationFileCount = filemanager::files::MaximumReviewedMoveFiles;

bool writeFixture(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("x", 1) == 1;
}

int countFiles(const QString& root)
{
    int count = 0;
    QDirIterator iterator(root, QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        ++count;
    }
    return count;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 1) {
        std::fprintf(stderr, "This benchmark takes no arguments.\n");
        return 2;
    }

    QTemporaryDir fixture;
    if (!fixture.isValid()) {
        std::fprintf(stderr, "Could not create the disposable benchmark root.\n");
        return 2;
    }
    const QString input = fixture.filePath("Input");
    const QString shows = fixture.filePath("Shows");
    if (!QDir().mkpath(input) || !QDir().mkpath(shows)) {
        std::fprintf(stderr, "Could not create benchmark folders.\n");
        return 2;
    }

    QStringList selectedEpisodes;
    selectedEpisodes.reserve(FileCount);
    QElapsedTimer phase;
    phase.start();
    for (int index = 0; index < FileCount; ++index) {
        const int series = index / 999 + 1;
        const int episode = index % 999 + 1;
        const QString path = QDir(input).filePath(QString("Benchmark Series %1 S01E%2.mkv")
            .arg(series).arg(episode, 3, 10, QLatin1Char('0')));
        if (!writeFixture(path)) {
            std::fprintf(stderr, "Could not populate the benchmark fixture.\n");
            return 2;
        }
        selectedEpisodes.append(path);
    }
    const qint64 fixtureMs = phase.restart();
    const auto plan = filemanager::files::planTvOrganization({input}, shows);
    const qint64 planningMs = phase.restart();
    const auto reviewed = filemanager::files::createReviewedTvMove(plan, selectedEpisodes);
    const qint64 requestMs = phase.restart();
    if (!reviewed.valid() || reviewed.request.reviewedMoves.size() != FileCount) {
        std::fprintf(stderr, "The benchmark could not construct a complete reviewed batch.\n");
        return 1;
    }

    filemanager::files::TransferQueue queue;
    QElapsedTimer queueClock;
    QElapsedTimer heartbeatClock;
    int heartbeatTicks = 0;
    qint64 maximumHeartbeatGapMs = 0;
    bool finished = false;
    bool cancelled = false;
    QStringList errors;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        maximumHeartbeatGapMs = qMax(maximumHeartbeatGapMs, heartbeatClock.restart());
        ++heartbeatTicks;
    });
    QObject::connect(&queue, &filemanager::files::TransferQueue::jobFinished,
        &application, [&](quint64, bool wasCancelled, const QStringList& jobErrors) {
            finished = true;
            cancelled = wasCancelled;
            errors = jobErrors;
            application.quit();
        });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &application, &QCoreApplication::quit);
    heartbeatClock.start();
    heartbeat.start();
    queueClock.start();
    const quint64 jobId = queue.enqueue(reviewed.request);
    timeout.start(120'000);
    application.exec();
    const qint64 queueMs = queueClock.elapsed();
    heartbeat.stop();

    const auto history = queue.history();
    const auto record = history.isEmpty() ? filemanager::files::TransferRecord{} : history.first();
    const int destinationFiles = countFiles(shows);
    const int sourceFiles = countFiles(input);
    std::printf("reviewed_transfer_benchmark files=%d fixture_ms=%lld planning_ms=%lld "
                "request_ms=%lld queue_ms=%lld heartbeat_ticks=%d max_heartbeat_gap_ms=%lld "
                "manifest_entries=%lld\n",
        FileCount, static_cast<long long>(fixtureMs), static_cast<long long>(planningMs),
        static_cast<long long>(requestMs), static_cast<long long>(queueMs), heartbeatTicks,
        static_cast<long long>(maximumHeartbeatGapMs),
        static_cast<long long>(record.manifest.size()));

    const bool responsive = heartbeatTicks > 0 && maximumHeartbeatGapMs < 1'000;
    const bool correct = finished && !cancelled && errors.isEmpty() && record.id == jobId
        && record.state == filemanager::files::TransferJobState::Succeeded
        && record.manifest.size() == FileCount + 4 && sourceFiles == 0
        && destinationFiles == FileCount;
    if (!correct || !responsive) {
        std::fprintf(stderr,
            "Benchmark failed: finished=%d cancelled=%d errors=%lld state=%d source=%d "
            "destination=%d responsive=%d\n",
            finished, cancelled, static_cast<long long>(errors.size()),
            static_cast<int>(record.state), sourceFiles, destinationFiles, responsive);
        return 1;
    }

    QTemporaryDir cancellationFixture;
    const QString cancellationInput = cancellationFixture.filePath("Input");
    const QString cancellationShows = cancellationFixture.filePath("Shows");
    if (!cancellationFixture.isValid() || !QDir().mkpath(cancellationInput)
            || !QDir().mkpath(cancellationShows)) {
        std::fprintf(stderr, "Could not create the cancellation benchmark folders.\n");
        return 2;
    }
    QStringList cancellationEpisodes;
    cancellationEpisodes.reserve(CancellationFileCount);
    phase.restart();
    for (int index = 0; index < CancellationFileCount; ++index) {
        const int series = index / 999 + 1;
        const int episode = index % 999 + 1;
        const QString path = QDir(cancellationInput).filePath(
            QString("Cancellation Series %1 S01E%2.mkv")
                .arg(series).arg(episode, 3, 10, QLatin1Char('0')));
        if (!writeFixture(path)) {
            std::fprintf(stderr, "Could not populate the cancellation fixture.\n");
            return 2;
        }
        cancellationEpisodes.append(path);
    }
    const qint64 cancellationFixtureMs = phase.restart();
    const auto cancellationPlan = filemanager::files::planTvOrganization(
        {cancellationInput}, cancellationShows);
    const qint64 cancellationPlanningMs = phase.restart();
    const auto cancellationRequest = filemanager::files::createReviewedTvMove(
        cancellationPlan, cancellationEpisodes);
    const qint64 cancellationRequestMs = phase.restart();
    if (!cancellationRequest.valid()
            || cancellationRequest.request.reviewedMoves.size() != CancellationFileCount) {
        std::fprintf(stderr, "The maximum-size cancellation request was not valid.\n");
        return 1;
    }

    filemanager::files::TransferQueue cancellationQueue;
    QElapsedTimer cancellationQueueClock;
    QElapsedTimer cancellationRequestClock;
    QElapsedTimer cancellationHeartbeatClock;
    int cancellationHeartbeatTicks = 0;
    qint64 cancellationMaximumGapMs = 0;
    qint64 cancellationLatencyMs = -1;
    bool cancellationFinished = false;
    bool cancellationReported = false;
    QStringList cancellationErrors;
    QTimer cancellationHeartbeat;
    cancellationHeartbeat.setInterval(10);
    QObject::connect(&cancellationHeartbeat, &QTimer::timeout, [&] {
        cancellationMaximumGapMs = qMax(
            cancellationMaximumGapMs, cancellationHeartbeatClock.restart());
        ++cancellationHeartbeatTicks;
    });
    QObject::connect(&cancellationQueue, &filemanager::files::TransferQueue::jobFinished,
        &application, [&](quint64, bool wasCancelled, const QStringList& jobErrors) {
            cancellationFinished = true;
            cancellationReported = wasCancelled;
            cancellationErrors = jobErrors;
            cancellationLatencyMs = cancellationRequestClock.isValid()
                ? cancellationRequestClock.elapsed() : -1;
            application.quit();
        });
    QTimer cancellationTimeout;
    cancellationTimeout.setSingleShot(true);
    QObject::connect(&cancellationTimeout, &QTimer::timeout,
        &application, &QCoreApplication::quit);
    cancellationHeartbeatClock.start();
    cancellationHeartbeat.start();
    cancellationQueueClock.start();
    const quint64 cancellationId = cancellationQueue.enqueue(cancellationRequest.request);
    QTimer::singleShot(50, &application, [&] {
        cancellationRequestClock.start();
        cancellationQueue.cancel(cancellationId);
    });
    cancellationTimeout.start(120'000);
    application.exec();
    const qint64 cancellationQueueMs = cancellationQueueClock.elapsed();
    cancellationHeartbeat.stop();

    const auto cancellationHistory = cancellationQueue.history();
    const auto cancellationRecord = cancellationHistory.isEmpty()
        ? filemanager::files::TransferRecord{} : cancellationHistory.first();
    const int remainingSources = countFiles(cancellationInput);
    const int committedDestinations = countFiles(cancellationShows);
    std::printf("reviewed_transfer_cancellation files=%d fixture_ms=%lld planning_ms=%lld "
                "request_ms=%lld queue_ms=%lld cancellation_latency_ms=%lld heartbeat_ticks=%d "
                "max_heartbeat_gap_ms=%lld committed_files=%d remaining_sources=%d\n",
        CancellationFileCount, static_cast<long long>(cancellationFixtureMs),
        static_cast<long long>(cancellationPlanningMs),
        static_cast<long long>(cancellationRequestMs),
        static_cast<long long>(cancellationQueueMs),
        static_cast<long long>(cancellationLatencyMs), cancellationHeartbeatTicks,
        static_cast<long long>(cancellationMaximumGapMs), committedDestinations, remainingSources);
    const bool cancellationResponsive = cancellationHeartbeatTicks > 0
        && cancellationMaximumGapMs < 1'000 && cancellationLatencyMs >= 0
        && cancellationLatencyMs < 2'000;
    const bool cancellationCorrect = cancellationFinished && cancellationReported
        && cancellationRecord.id == cancellationId
        && cancellationRecord.state == filemanager::files::TransferJobState::Cancelled
        && remainingSources + committedDestinations == CancellationFileCount;
    if (!cancellationCorrect || !cancellationResponsive) {
        std::fprintf(stderr,
            "Cancellation benchmark failed: finished=%d reported=%d errors=%lld state=%d "
            "source=%d destination=%d responsive=%d\n",
            cancellationFinished, cancellationReported,
            static_cast<long long>(cancellationErrors.size()),
            static_cast<int>(cancellationRecord.state), remainingSources,
            committedDestinations, cancellationResponsive);
        return 1;
    }
    return 0;
}
