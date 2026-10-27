#include "preview/fileiconscheduler.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QSemaphore>
#include <QThread>
#include <QThreadPool>
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace {
using Scheduler = filemanager::preview::FileIconScheduler;
using Result = filemanager::platform::FileIconResult;
using Status = filemanager::platform::FileIconStatus;

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool waitUntil(const std::function<bool()>& done)
{
    QElapsedTimer deadline;
    deadline.start();
    while (!done() && deadline.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return done();
}

QString path(const QString& name)
{
    // Injected loaders never access these paths or launch the Shell helper.
    return QDir::current().absoluteFilePath(QStringLiteral("icon-scheduler-fixtures/") + name);
}

Result ready(int extent, int marker)
{
    Result result;
    result.status = Status::Ready;
    result.image = QImage(extent, extent, QImage::Format_RGBA8888);
    result.image.fill(QColor(marker % 256, 23, 45, 255));
    return result;
}

struct Gates {
    QSemaphore started;
    QSemaphore release;
    QSemaphore oldGenerationRelease;
    QSemaphore exited;
    bool separateOldGeneration = false;
    std::atomic_int calls{0};
    std::atomic_int active{0};
    std::atomic_int peak{0};
    std::atomic_int cancellations{0};
};

Scheduler::Loader gated(const std::shared_ptr<Gates>& gates)
{
    return [gates](const QString&, int extent, const filemanager::platform::ShellCancellation& cancel) {
        const int marker = ++gates->calls;
        const int active = ++gates->active;
        int peak = gates->peak.load();
        while (active > peak && !gates->peak.compare_exchange_weak(peak, active)) {}
        gates->started.release();
        // Deliberately return success after cancellation to exercise generation
        // rejection independently of the native loader's cooperative cancellation.
        const bool released = (gates->separateOldGeneration && marker <= 2
            ? gates->oldGenerationRelease : gates->release).tryAcquire(1, 5000);
        if (cancel->load())
            ++gates->cancellations;
        --gates->active;
        gates->exited.release();
        return released ? ready(extent, marker) : Result{Status::TimedOut};
    };
}

bool queueAndCache()
{
    bool passed = true;
    const auto gates = std::make_shared<Gates>();
    Scheduler scheduler(gated(gates));
    int completions = 0;
    int capacitySignals = 0;
    QObject::connect(&scheduler, &Scheduler::requestFinished, &scheduler,
        [&](const QString&, int) { ++completions; });
    QObject::connect(&scheduler, &Scheduler::capacityAvailable, &scheduler, [&] { ++capacitySignals; });
    for (int i = 0; i < Scheduler::MaximumPendingRequests; ++i)
        passed &= expect(scheduler.request(path(QString::number(i)), 16).state == Scheduler::State::Pending,
            "Requests within capacity must be accepted.");
    passed &= expect(gates->started.tryAcquire(2, 3000) && gates->calls == 2
            && scheduler.activeRequestCount() == 2 && scheduler.pendingRequestCount() == 64,
        "Only two workers may start while the first requests are held.");
    passed &= expect(scheduler.request(path("0"), 16).state == Scheduler::State::Pending
            && scheduler.pendingRequestCount() == 64,
        "Repeated requests must share pending work.");
    passed &= expect(scheduler.request(path("overflow"), 16).state == Scheduler::State::Busy
            && scheduler.pendingRequestCount() == 64,
        "Overflow must report busy without admitting or dropping accepted work.");
    gates->release.release(64);
    passed &= expect(waitUntil([&] { return completions == 64; })
            && gates->calls == 64 && gates->peak <= 2 && scheduler.pendingRequestCount() == 0,
        "All accepted work must drain exactly once within the worker bound.");
    const auto cached = scheduler.request(path("0"), 16);
    passed &= expect(cached.state == Scheduler::State::Cached && cached.result.status == Status::Ready
            && gates->calls == 64 && capacitySignals > 0,
        "Completed artwork must be reusable without a new worker and capacity must be signalled.");
    passed &= expect(scheduler.request(path("overflow"), 16).state == Scheduler::State::Pending,
        "An overflow request can be explicitly resubmitted after capacity becomes available.");
    gates->release.release();
    passed &= expect(waitUntil([&] { return completions == 65; }), "Resubmitted work must complete.");
    scheduler.request(path("0"), 32);
    scheduler.request(path("Case"), 16);
    scheduler.request(path("case"), 16);
    gates->release.release(3);
    passed &= expect(waitUntil([&] { return completions == 68; })
            && scheduler.request(path("0"), 32).result.image.size() == QSize(32, 32)
            && scheduler.request(path("Case"), 16).result.image
                != scheduler.request(path("case"), 16).result.image,
        "Requested size and case-sensitive path identity must remain separate cache entries.");
    return passed;
}

bool generationsAndDisposal()
{
    bool passed = true;
    const auto gates = std::make_shared<Gates>();
    // The replacement worker must not consume a permit meant for one of the
    // two old workers; otherwise this fixture races its own cancellation.
    gates->separateOldGeneration = true;
    Scheduler scheduler(gated(gates));
    int completions = 0;
    QObject::connect(&scheduler, &Scheduler::requestFinished, &scheduler,
        [&](const QString&, int) { ++completions; });
    scheduler.request(path("same"), 16);
    scheduler.request(path("old"), 16);
    scheduler.request(path("never-start"), 16);
    passed &= expect(gates->started.tryAcquire(2, 3000), "Old-generation requests must start for this check.");
    scheduler.clear();
    scheduler.request(path("same"), 16);
    passed &= expect(scheduler.activeRequestCount() == 2 && scheduler.pendingRequestCount() == 1
            && gates->calls == 2, "Cancelled jobs must retain their slots until they finish.");
    gates->oldGenerationRelease.release(2);
    // A replacement may start after the first cancelled worker retires, before
    // the second has observed cancellation. Check only after both old callbacks.
    const bool oldRetired = waitUntil([&] { return gates->calls == 3
        && gates->cancellations == 2 && scheduler.activeRequestCount() == 1; });
    passed &= expect(oldRetired && completions == 0
            && scheduler.request(path("same"), 16).state == Scheduler::State::Pending
            && scheduler.pendingRequestCount() == 1,
        "Late success must not publish or remove the replacement request for the same path.");
    gates->release.release();
    passed &= expect(waitUntil([&] { return completions == 1; }), "The replacement generation must complete.");
    const auto replacement = scheduler.request(path("same"), 16);
    passed &= expect(replacement.state == Scheduler::State::Cached
            && replacement.result.status == Status::Ready && !replacement.result.image.isNull()
            && replacement.result.image.pixelColor(0, 0).red() == 3 && gates->calls == 3,
        "Only replacement pixels may enter the new cache; old queued work must never start.");

    const auto disposal = std::make_shared<Gates>();
    auto disposable = std::make_unique<Scheduler>(gated(disposal));
    disposable->request(path("disposed"), 16);
    passed &= expect(disposal->started.tryAcquire(1, 3000), "Disposal fixture must have active work.");
    QElapsedTimer elapsed;
    elapsed.start();
    disposable.reset();
    passed &= expect(elapsed.elapsed() < 1000, "View-owner disposal must not wait for the blocked loader.");
    disposal->release.release();
    passed &= expect(disposal->exited.tryAcquire(1, 3000) && disposal->cancellations == 1,
        "Disposal must revoke the active token without a callback into the destroyed scheduler.");
    QCoreApplication::processEvents();
    return passed;
}

bool failuresAndBudget()
{
    bool passed = true;
    const auto calls = std::make_shared<std::atomic_int>(0);
    Scheduler scheduler([calls](const QString&, int extent, const auto&) {
        return ++*calls == 1 ? Result{Status::TimedOut} : ready(extent, 9);
    });
    int completions = 0;
    QObject::connect(&scheduler, &Scheduler::requestFinished, &scheduler,
        [&](const QString&, int) { ++completions; });
    scheduler.request(path("retry"), 16);
    passed &= expect(waitUntil([&] { return completions == 1; }), "Failed requests must complete explicitly.");
    for (int i = 0; i < 10; ++i) {
        const auto failure = scheduler.request(path("retry"), 16);
        passed &= expect(failure.state == Scheduler::State::Cached && failure.result.status == Status::TimedOut
                && failure.result.image.isNull() && *calls == 1,
            "Repeated paints must not automatically relaunch a failed request.");
    }
    scheduler.clear();
    scheduler.request(path("retry"), 16);
    passed &= expect(waitUntil([&] { return completions == 2; })
            && scheduler.request(path("retry"), 16).result.status == Status::Ready && *calls == 2,
        "Explicit invalidation must allow a fresh attempt after failure.");
    Scheduler budget([](const QString&, int extent, const auto&) { return ready(extent, 1); });
    int budgetCompletions = 0;
    QObject::connect(&budget, &Scheduler::requestFinished, &budget,
        [&](const QString&, int) { ++budgetCompletions; });
    for (int i = 0; i < 12; ++i)
        budget.request(path(QString::number(i)), 512);
    passed &= expect(waitUntil([&] { return budgetCompletions == 12; })
            && budget.cacheCost() <= Scheduler::CacheBudget && budget.cachedCount() < 12,
        "Large icon results must trigger cache eviction within the accounted byte budget.");
    Scheduler malformed([](const QString& target, int, const auto&) -> Result {
        if (target.endsWith("throw"))
            throw std::runtime_error("injected loader failure");
        return target.endsWith("large") ? ready(513, 1) : Result{Status::Ready};
    });
    int malformedCompletions = 0;
    QObject::connect(&malformed, &Scheduler::requestFinished, &malformed,
        [&](const QString&, int) { ++malformedCompletions; });
    malformed.request(path("throw"), 16);
    malformed.request(path("empty"), 16);
    malformed.request(path("large"), 16);
    passed &= expect(waitUntil([&] { return malformedCompletions == 3; })
            && malformed.request(path("throw"), 16).result.status == Status::HelperFailed
            && malformed.request(path("empty"), 16).result.status == Status::InvalidReply
            && malformed.request(path("large"), 16).result.status == Status::InvalidReply,
        "Exceptions and invalid successful results must complete as failures without escaping the worker.");
    return passed;
}

bool reentrantCompletion()
{
    bool passed = true;
    QPointer<Scheduler> scheduler = new Scheduler([](const QString&, int extent, const auto&) {
        return ready(extent, 1);
    });
    QObject::connect(scheduler, &Scheduler::requestFinished, QCoreApplication::instance(),
        [&] { delete scheduler.data(); });
    scheduler->request(path("delete-on-completion"), 16);
    passed &= expect(waitUntil([&] { return scheduler.isNull(); }),
        "A completion receiver may dispose the scheduler without a later signal dereferencing it.");
    return passed;
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    // Reproduce a one-vCPU guest: the icon scheduler must keep its own two
    // bounded worker slots when Qt's global pool is constrained.
    QThreadPool* globalPool = QThreadPool::globalInstance();
    const int originalGlobalMaximum = globalPool->maxThreadCount();
    globalPool->setMaxThreadCount(1);
    bool passed = queueAndCache();
    passed &= generationsAndDisposal();
    passed &= failuresAndBudget();
    passed &= reentrantCompletion();
    globalPool->setMaxThreadCount(originalGlobalMaximum);
    return passed ? 0 : 1;
}
