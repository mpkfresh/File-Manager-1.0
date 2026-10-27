#include "platform/directorywatcher.h"
#include "ui/localdirectorymodel.h"
#include "ui/presentationtiming.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTimeZone>
#include <cstdio>
#include <memory>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {
bool expect(bool condition, const char* message)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    std::fflush(stdout);
    return condition;
}
template<class Predicate> bool waitFor(Predicate ready, int milliseconds = 6000)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < milliseconds) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return ready();
}
void drain(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    waitFor([&] { return timer.elapsed() >= milliseconds; }, milliseconds + 100);
}
bool writeFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("payload") == 7;
}
#ifdef Q_OS_WIN
class PausedWatchHelper final {
public:
    bool pause(QProcess* process)
    {
        if (!process || process->state() != QProcess::Running
                || process->arguments() != QStringList{QStringLiteral("--watch-directories")})
            return false;
        const DWORD pid = static_cast<DWORD>(process->processId());
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        bool ok = Thread32First(snapshot, &entry) != FALSE;
        if (ok) do {
            if (entry.th32OwnerProcessID != pid)
                continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION,
                FALSE, entry.th32ThreadID);
            if (!thread || GetProcessIdOfThread(thread) != pid || SuspendThread(thread) == DWORD(-1)) {
                if (thread) CloseHandle(thread);
                ok = false;
                break;
            }
            threads_.push_back(thread);
        } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
        if (!ok || threads_.empty()) {
            resume();
            return false;
        }
        return true;
    }
    bool resume()
    {
        bool ok = true;
        for (HANDLE thread : threads_) {
            ok &= ResumeThread(thread) != DWORD(-1);
            CloseHandle(thread);
        }
        threads_.clear();
        return ok;
    }
    ~PausedWatchHelper() { resume(); }
private:
    std::vector<HANDLE> threads_;
};

bool createJunction(const QString& path, const QString& target)
{
    QProcess process;
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    process.start("cmd.exe", {"/d", "/c", "mklink", "/J", QDir::toNativeSeparators(path),
        QDir::toNativeSeparators(target)});
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
#endif
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    using filemanager::platform::DirectoryWatcher;
    using filemanager::ui::LocalDirectoryModel;
    bool passed = true;
    QPointer<QProcess> finalSubscriberHelper;
    const QString selectedParent = qEnvironmentVariable("FILE_MANAGER_WATCH_FIXTURE_PARENT");
    const QString fixtureParent = selectedParent.isEmpty() ? QDir::tempPath() : selectedParent;
    if (!expect(QDir::isAbsolutePath(fixtureParent) && QFileInfo(fixtureParent).isDir()
            && !QFileInfo(fixtureParent).isSymLink(), "fixture parent is an existing absolute directory"))
        return 1;
    QTemporaryDir fixture(QDir(fixtureParent).filePath("watch-fixture-XXXXXX"));
    const QString parentCanonical = QFileInfo(fixtureParent).canonicalFilePath();
    const QString fixtureCanonical = QFileInfo(fixture.path()).canonicalFilePath();
    const bool contained = fixture.isValid() && !parentCanonical.isEmpty()
        && fixtureCanonical.startsWith(parentCanonical.endsWith('/') ? parentCanonical : parentCanonical + '/',
            Qt::CaseInsensitive);
    if (!expect(contained, "fixture resolves inside the selected parent"))
        return 1;
    std::printf("fixture_contained=1\n");
    const QString root = fixture.filePath("listing");
    const QString item = root + "/item";
    const QString nested = item + "/nested";
    const QString unopened = root + "/unopened/deep";
    if (!expect(fixture.isValid() && QDir().mkpath(nested) && QDir().mkpath(unopened)
            && writeFile(nested + "/original.txt"), "create disposable watcher fixture"))
        return 1;
#ifdef Q_OS_WIN
    {
        const QString watchedRoot = fixture.filePath("overflow-root");
        const QString expanded = watchedRoot + "/expanded";
        const QString quiet = watchedRoot + "/quiet";
        const QString privateDeep = watchedRoot + "/unopened/deep";
        const QString otherRoot = fixture.filePath("unaffected-root");
        passed &= expect(QDir().mkpath(expanded) && QDir().mkpath(quiet)
                && QDir().mkpath(privateDeep) && QDir().mkpath(otherRoot)
                && writeFile(expanded + "/old.txt"), "create contained overflow fixture");
        LocalDirectoryModel model;
        DirectoryWatcher unrelated;
        unrelated.setRoot(otherRoot);
        model.setDirectory(watchedRoot);
        auto* watcher = model.findChild<DirectoryWatcher*>();
        passed &= expect(waitFor([&] { return model.indexForPath(expanded).isValid()
                && model.indexForPath(quiet).isValid(); }), "load overflow root");
        model.fetchMore(model.indexForPath(expanded));
        model.fetchMore(model.indexForPath(quiet));
        passed &= expect(waitFor([&] { return watcher && watcher->isWatching(expanded)
                && watcher->isWatching(quiet) && unrelated.isWatching(otherRoot)
                && model.indexForPath(expanded + "/old.txt").isValid(); }), "overflow subscriptions ready");
        drain(250);
        QPersistentModelIndex oldFile = model.indexForPath(expanded + "/old.txt");
        QSet<QString> resynced;
        int unrelatedResyncs = 0;
        QObject::connect(watcher, &DirectoryWatcher::resynchronizationRequired, &model,
            [&](const QString& path) { resynced.insert(path); });
        QObject::connect(&unrelated, &DirectoryWatcher::resynchronizationRequired, &unrelated,
            [&](const QString&) { ++unrelatedResyncs; });
        QProcess* helper = nullptr;
        for (auto* process : app.findChildren<QProcess*>())
            if (process->arguments() == QStringList{QStringLiteral("--watch-directories")}
                    && process->state() == QProcess::Running)
                helper = process;
        PausedWatchHelper paused;
        const bool held = paused.pause(helper);
        passed &= expect(held, "pause only the owned watcher helper for native overflow");
        bool burst = held;
        int completedWrites = 0;
        bool oldRemoved = false;
        bool childCreated = false;
        bool rootChildCreated = false;
        QElapsedTimer burstTime;
        burstTime.start();
        if (held) {
            // More than 32 KiB of names accumulate behind a completed request.
            // Fail if the bounded stimulus does not produce a native resync notice.
            for (int i = 0; i < 512 && burst; ++i) {
                if (burstTime.elapsed() > 4000) { burst = false; break; }
                burst = writeFile(privateDeep + '/' + QString(80, 'x') + QString::number(i) + ".txt");
                if (burst)
                    ++completedWrites;
            }
            if (burst) {
                oldRemoved = QFile::remove(expanded + "/old.txt");
                childCreated = oldRemoved && writeFile(expanded + "/new.txt");
                rootChildCreated = childCreated && writeFile(watchedRoot + "/new-root.txt");
                burst = rootChildCreated;
            }
        }
        const bool resumed = paused.resume();
        const qint64 burstMilliseconds = burstTime.elapsed();
        std::printf("overflow_stimulus writes=%d requested=512 old_removed=%d child_created=%d "
            "root_child_created=%d resumed=%d within_deadline=%d burst_ms=%lld\n",
            completedWrites, oldRemoved, childCreated, rootChildCreated, resumed,
            burstMilliseconds <= 4000, static_cast<long long>(burstMilliseconds));
        // A final write/resume can cross the guard after the last loop check.
        passed &= expect(burst && resumed && burstMilliseconds <= 4000,
            "bounded synthetic burst completes and helper resumes");
        const bool recovered = waitFor([&] {
            return resynced == QSet<QString>{watchedRoot, expanded, quiet}
                && !oldFile.isValid() && model.indexForPath(expanded + "/new.txt").isValid()
                && model.indexForPath(watchedRoot + "/new-root.txt").isValid();
        });
        std::printf("overflow_recovery resync_listings=%lld unrelated_resyncs=%d burst_ms=%lld "
            "resync_scope_exact=%d old_index_valid=%d child_index_valid=%d root_child_index_valid=%d recovered=%d\n",
            static_cast<long long>(resynced.size()), unrelatedResyncs,
            static_cast<long long>(burstMilliseconds), resynced == QSet<QString>{watchedRoot, expanded, quiet},
            oldFile.isValid(), model.indexForPath(expanded + "/new.txt").isValid(),
            model.indexForPath(watchedRoot + "/new-root.txt").isValid(), recovered);
        passed &= expect(recovered && unrelatedResyncs == 0,
            "native loss reconciles exactly displayed listings on the affected root");
        passed &= expect(!watcher->directories().contains(privateDeep)
                && !model.indexForPath(privateDeep).isValid(), "overflow adds no unopened descendants to the model or subscriptions");
        passed &= expect(writeFile(expanded + "/after-overflow.txt") && waitFor([&] {
            return model.indexForPath(expanded + "/after-overflow.txt").isValid();
        }), "normal notifications continue after overflow recovery");
    }
    {
        const QString anchorPath = fixture.filePath("registration-anchor");
        const QString deletedRoot = fixture.filePath("deleted-before-registration");
        passed &= expect(QDir().mkpath(anchorPath) && QDir().mkpath(deletedRoot)
                && writeFile(deletedRoot + "/stale.txt"), "create registration-race fixture");
        DirectoryWatcher anchor;
        anchor.setRoot(anchorPath);
        passed &= expect(waitFor([&] { return anchor.isWatching(anchorPath); }), "registration anchor ready");
        QProcess* helper = nullptr;
        for (auto* process : app.findChildren<QProcess*>())
            if (process->arguments() == QStringList{QStringLiteral("--watch-directories")}
                    && process->state() == QProcess::Running)
                helper = process;
        PausedWatchHelper paused;
        const bool held = paused.pause(helper);
        passed &= expect(held, "pause owned helper before registering a new root");
        LocalDirectoryModel model;
        int failures = 0;
        QObject::connect(&model, &LocalDirectoryModel::directoryMonitoringChanged, &model,
            [&](const QString&, bool available) { if (!available) ++failures; });
        model.setDirectory(deletedRoot);
        passed &= expect(waitFor([&] { return !model.loading()
                && model.indexForPath(deletedRoot + "/stale.txt").isValid(); }),
            "publish rows before watch registration completes");
        QPersistentModelIndex stale = model.indexForPath(deletedRoot + "/stale.txt");
        const bool removed = QFile::remove(deletedRoot + "/stale.txt") && QDir().rmdir(deletedRoot);
        const bool resumed = paused.resume();
        passed &= expect(held && removed && resumed, "delete root before releasing its queued registration");
        passed &= expect(waitFor([&] { return failures > 0 && !stale.isValid()
                && model.rowCount() == 0 && !model.loading(); }),
            "failed registration reconciles missing-root rows once");
        const int observedFailures = failures;
        drain(250);
        passed &= expect(failures == observedFailures && anchor.isWatching(anchorPath),
            "registration failure does not loop or disrupt another root");
    }
#endif
    {
        DirectoryWatcher first;
        DirectoryWatcher second;
        int firstChanges = 0;
        int secondChanges = 0;
        QObject::connect(&first, &DirectoryWatcher::directoryChanged, &first,
            [&](const QString&) { ++firstChanges; });
        QObject::connect(&second, &DirectoryWatcher::directoryChanged, &second,
            [&](const QString&) { ++secondChanges; });
        first.addPath(nested);
        second.addPath(nested);
        passed &= expect(waitFor([&] { return first.isWatching(nested) && second.isWatching(nested); }),
            "shared subscription becomes active");
        drain(250);
        firstChanges = secondChanges = 0;
        passed &= expect(writeFile(nested + "/canary.txt")
                && waitFor([&] { return firstChanges > 0 && secondChanges > 0; }),
            "native file creation refreshes both subscribers");
        first.removePaths({nested});
        drain(250);
        firstChanges = secondChanges = 0;
        passed &= expect(writeFile(nested + "/second.txt")
                && waitFor([&] { return secondChanges > 0; }) && firstChanges == 0,
            "detaching one subscriber preserves the other and drops stale replies");
    }
    {
        DirectoryWatcher watcher;
        int changes = 0;
        int monitoringFailures = 0;
        QObject::connect(&watcher, &DirectoryWatcher::directoryChanged, &watcher,
            [&](const QString&) { ++changes; });
        QObject::connect(&watcher, &DirectoryWatcher::monitoringFailed, &watcher,
            [&](const QString&) { ++monitoringFailures; });
        watcher.addPath(root);
        passed &= expect(waitFor([&] { return watcher.isWatching(root); }), "root watch active");
        drain(400);
        changes = 0;
        passed &= expect(writeFile(unopened + "/private-canary.txt"), "mutate unopened grandchild fixture");
        drain(1800);
        passed &= expect(changes == 0, "helper filters unopened grandchild creation from displayed-folder refreshes");
        const bool activeBefore = watcher.isWatching(root);
        QElapsedTimer visibleTimer;
        visibleTimer.start();
        const bool visibleCreated = writeFile(root + "/visible.txt");
        const qint64 visibleWriteMs = visibleTimer.restart();
        const bool visibleNotified = visibleCreated && waitFor([&] { return changes > 0; });
        std::printf("direct_child_notice created=%d changes=%d active_before=%d active_after=%d "
            "monitoring_failures=%d write_ms=%lld wait_ms=%lld notified=%d\n", visibleCreated, changes,
            activeBefore, watcher.isWatching(root), monitoringFailures,
            static_cast<long long>(visibleWriteMs), static_cast<long long>(visibleTimer.elapsed()), visibleNotified);
        passed &= expect(visibleNotified,
            "same registration still reports a direct child creation");
    }
    {
        DirectoryWatcher outer;
        DirectoryWatcher inner;
        outer.setRoot(root);
        outer.addPath(nested);
        inner.setRoot(nested);
        passed &= expect(waitFor([&] { return outer.isWatching(root) && outer.isWatching(nested)
                && inner.isWatching(nested); }), "overlapping opened roots become active");
        const QString moved = root + "/overlap-renamed";
        passed &= expect(QDir().rename(item, moved), "overlapping roots share coverage without a descendant lock");
        passed &= expect(waitFor([&] { return !inner.isWatching(nested); }), "moving a covered root retires its old subscription");
        passed &= expect(QDir().rename(moved, item), "restore overlap fixture ancestor");
        inner.addPath(nested);
        passed &= expect(waitFor([&] { return inner.isWatching(nested); }), "explicit retry binds restored inner root");
        outer.removePaths({root, nested});
        drain(150);
        int changes = 0;
        QObject::connect(&inner, &DirectoryWatcher::directoryChanged, &inner,
            [&](const QString&) { ++changes; });
        passed &= expect(writeFile(nested + "/inner-only.txt") && waitFor([&] { return changes > 0; }),
            "closing outer coverage preserves the separately opened inner root");
    }
    {
        const QString moving = fixture.filePath("moving-root");
        const QString moved = fixture.filePath("moved-root");
        DirectoryWatcher watcher;
        passed &= expect(QDir().mkpath(moving) && writeFile(moving + "/original.txt"), "create root movement fixture");
        watcher.setRoot(moving);
        passed &= expect(waitFor([&] { return watcher.isWatching(moving); }) && QDir().rename(moving, moved),
            "rename the opened root itself with delete sharing");
        passed &= expect(waitFor([&] { return !watcher.isWatching(moving); }), "root identity check retires a moved root");
        watcher.setRoot(moved);
        passed &= expect(waitFor([&] { return watcher.isWatching(moved); }), "explicit navigation binds the moved root");
    }
#ifdef Q_OS_WIN
    {
        const QString target = fixture.filePath("junction-target");
        const QString replacement = fixture.filePath("junction-replacement");
        const QString link = root + "/junction";
        passed &= expect(QDir().mkpath(target) && QDir().mkpath(replacement)
                && createJunction(link, target), "create disposable junction boundary");
        DirectoryWatcher outer;
        DirectoryWatcher opened;
        outer.setRoot(root);
        int rejected = 0;
        QObject::connect(&outer, &DirectoryWatcher::monitoringFailed, &outer,
            [&](const QString& path) { if (path == link) ++rejected; });
        outer.addPath(link);
        passed &= expect(waitFor([&] { return rejected > 0 && outer.isWatching(root); })
                && !outer.isWatching(link), "expanded junction does not inherit recursive coverage");
        outer.removePaths({link});
        opened.setRoot(link);
        passed &= expect(waitFor([&] { return opened.isWatching(link); }), "explicitly opened junction obtains its own coverage");
        drain(200);
        int changes = 0;
        QObject::connect(&opened, &DirectoryWatcher::directoryChanged, &opened,
            [&](const QString&) { ++changes; });
        passed &= expect(writeFile(target + "/before.txt") && waitFor([&] { return changes > 0; }),
            "opened junction reports changes from its selected target");
        const bool retargeted = QDir().rmdir(link) && createJunction(link, replacement);
        passed &= expect(retargeted && waitFor([&] { return !opened.isWatching(link); }),
            "retargeted junction retires the old target subscription");
        drain(200);
        changes = 0;
        passed &= expect(writeFile(target + "/after.txt"), "mutate former junction target fixture");
        drain(1200);
        passed &= expect(changes == 0, "retired junction does not forward former-target changes");
        opened.addPath(link);
        passed &= expect(waitFor([&] { return opened.isWatching(link); }), "explicit refresh binds the replacement junction target");
        drain(200);
        changes = 0;
        passed &= expect(writeFile(replacement + "/new.txt") && waitFor([&] { return changes > 0; }),
            "replacement junction target receives fresh notifications");
        opened.removePaths({link});
        outer.removePaths({root});
        passed &= expect(QDir().rmdir(link), "remove only the disposable junction");
    }
#endif
    {
        const QString moving = fixture.filePath("model-moving-root");
        const QString moved = fixture.filePath("model-moved-root");
        LocalDirectoryModel model;
        passed &= expect(QDir().mkpath(moving) && writeFile(moving + "/original.txt"), "create model root movement fixture");
        model.setDirectory(moving);
        auto* watcher = model.findChild<DirectoryWatcher*>();
        passed &= expect(waitFor([&] { return watcher && watcher->isWatching(moving)
                && model.indexForPath(moving + "/original.txt").isValid(); }), "model root movement fixture ready");
        QPersistentModelIndex original = model.indexForPath(moving + "/original.txt");
        passed &= expect(QDir().rename(moving, moved) && waitFor([&] { return !original.isValid()
                && model.rowCount() == 0 && !model.loading(); }), "moved model root retires stale rows");
        model.setDirectory(moved);
        passed &= expect(waitFor([&] { return model.indexForPath(moved + "/original.txt").isValid(); }),
            "explicit navigation loads the moved model root");
        const bool removedFile = QFile::remove(moved + "/original.txt");
        const bool removedRoot = removedFile && QDir().rmdir(moved);
#ifdef Q_OS_WIN
        const DWORD rootRemovalError = removedRoot ? ERROR_SUCCESS : GetLastError();
#else
        const unsigned long rootRemovalError = 0;
#endif
        const bool retiredRoot = waitFor([&] { return model.rowCount() == 0 && !watcher->isWatching(moved); });
        std::printf("opened_root_removal file_removed=%d root_removed=%d native_error=%lu retired=%d "
            "root_exists=%d watch_active=%d rows=%d loading=%d\n", removedFile, removedRoot,
            static_cast<unsigned long>(rootRemovalError), retiredRoot, QFileInfo::exists(moved),
            watcher->isWatching(moved), model.rowCount(), model.loading());
        passed &= expect(removedFile && removedRoot && retiredRoot,
            "deleting the opened root retires monitoring and model rows");
    }
    {
        LocalDirectoryModel model;
        auto* watcher = model.findChild<DirectoryWatcher*>();
        int rootHints = 0;
        if (watcher)
            QObject::connect(watcher, &DirectoryWatcher::directoryChanged, &model,
                [&](const QString& path) { if (path == root) ++rootHints; });
        model.setDirectory(root);
        passed &= expect(waitFor([&] { return !model.loading() && model.indexForPath(item).isValid(); }),
            "model root loads");
        QPersistentModelIndex oldItem = model.indexForPath(item);
        model.fetchMore(oldItem);
        passed &= expect(waitFor([&] { return model.indexForPath(nested).isValid(); }), "expand item");
        QPersistentModelIndex oldNested = model.indexForPath(nested);
        model.fetchMore(oldNested);
        passed &= expect(waitFor([&] { return watcher && watcher->isWatching(nested)
                && model.indexForPath(nested + "/original.txt").isValid(); }), "nested watch and rows ready");
        const QString renamed = root + "/renamed";
        passed &= expect(QDir().rename(item, renamed), "rename ancestor while descendant watch is active");
        passed &= expect(waitFor([&] { return model.indexForPath(renamed).isValid() && !oldItem.isValid()
                && !oldNested.isValid(); }), "model automatically replaces renamed ancestor and stale indexes");
        model.fetchMore(model.indexForPath(renamed));
        const QString newNested = renamed + "/nested";
        passed &= expect(waitFor([&] { return model.indexForPath(newNested).isValid(); }), "expand renamed item");
        model.fetchMore(model.indexForPath(newNested));
        passed &= expect(waitFor([&] { return watcher && watcher->isWatching(newNested); }),
            "rebind nested watch after rename");
        passed &= expect(writeFile(newNested + "/after-rename.txt")
                && waitFor([&] { return model.indexForPath(newNested + "/after-rename.txt").isValid(); }),
            "native changes after rename reach the model");
        const QString empty = root + "/empty";
        passed &= expect(QDir().mkpath(empty)
                && waitFor([&] { return model.indexForPath(empty).isValid(); }), "discover empty folder");
        model.fetchMore(model.indexForPath(empty));
        passed &= expect(waitFor([&] { return watcher && watcher->isWatching(empty); }), "watch empty expanded folder");
        const bool removedEmpty = QDir().rmdir(empty);
        const bool removedRows = waitFor([&] { return !model.indexForPath(empty).isValid(); });
        std::printf("empty_removal filesystem=%d rows_retired=%d path_exists=%d watch_active=%d\n",
            removedEmpty, removedRows, QFileInfo::exists(empty), watcher && watcher->isWatching(empty));
        passed &= expect(removedEmpty && removedRows, "remove watched empty folder");
        const int hintsBeforeRecreation = rootHints;
        const bool recreatedEmpty = QDir().mkpath(empty);
#ifdef Q_OS_WIN
        const DWORD recreationError = recreatedEmpty ? ERROR_SUCCESS : GetLastError();
#else
        const unsigned long recreationError = 0;
#endif
        const bool recreatedRow = waitFor([&] { return model.indexForPath(empty).isValid(); });
        std::printf("empty_recreation filesystem=%d native_error=%lu row_present=%d path_exists=%d "
            "root_watch_active=%d root_hints=%d model_loading=%d rows=%d\n", recreatedEmpty,
            static_cast<unsigned long>(recreationError), recreatedRow, QFileInfo::exists(empty),
            watcher && watcher->isWatching(root), rootHints - hintsBeforeRecreation, model.loading(), model.rowCount());
        passed &= expect(recreatedEmpty && recreatedRow, "recreate folder at same path");
        model.fetchMore(model.indexForPath(empty));
        passed &= expect(waitFor([&] { return watcher && watcher->isWatching(empty); })
                && writeFile(empty + "/new.txt")
                && waitFor([&] { return model.indexForPath(empty + "/new.txt").isValid(); }),
            "recreated empty folder receives fresh changes");
        int failures = 0;
        QObject::connect(&model, &LocalDirectoryModel::directoryMonitoringChanged, &model,
            [&](const QString&, bool available) { if (!available) ++failures; });
        QProcess* helper = nullptr;
        for (auto* process : app.findChildren<QProcess*>()) {
            if (process->arguments() == QStringList{QStringLiteral("--watch-directories")}
                    && process->state() == QProcess::Running)
                helper = process;
        }
        passed &= expect(helper != nullptr, "locate owned monitoring helper for crash recovery");
        if (helper) {
            helper->kill();
            passed &= expect(waitFor([&] { return failures > 0; }), "helper failure surfaces without losing rows");
            passed &= expect(model.indexForPath(empty + "/new.txt").isValid(), "helper crash preserves current model contents");
            drain(250);
            bool restarted = false;
            for (auto* process : app.findChildren<QProcess*>())
                restarted |= process->arguments() == QStringList{QStringLiteral("--watch-directories")}
                    && process->state() != QProcess::NotRunning;
            passed &= expect(!restarted && model.indexForPath(empty + "/new.txt").isValid(),
                "failure reconciliation preserves readable rows without restarting the helper");
            model.refresh();
            passed &= expect(waitFor([&] { return watcher->isWatching(root); })
                    && writeFile(root + "/recovered.txt")
                    && waitFor([&] { return model.indexForPath(root + "/recovered.txt").isValid(); }),
                "explicit refresh restarts monitoring after helper crash");
        }
        for (auto* process : app.findChildren<QProcess*>())
            if (process->arguments() == QStringList{QStringLiteral("--watch-directories")}
                    && process->state() == QProcess::Running)
                finalSubscriberHelper = process;
    }
    passed &= expect(waitFor([&] {
        return !finalSubscriberHelper || finalSubscriberHelper->state() == QProcess::NotRunning;
    }), "last subscriber releases its helper process");
#ifdef Q_OS_WIN
    {
        const QString timeRoot = fixture.filePath("timestamp-refresh");
        const QString timeFile = timeRoot + "/unchanged-size.txt";
        passed &= expect(QDir().mkpath(timeRoot) && writeFile(timeFile),
            "create timestamp-only refresh fixture");
        LocalDirectoryModel model;
        model.setDirectory(timeRoot);
        auto* watcher = model.findChild<DirectoryWatcher*>();
        passed &= expect(waitFor([&] { return model.indexForPath(timeFile).isValid()
                && watcher && watcher->isWatching(timeRoot); }), "timestamp fixture monitoring ready");
        const QPersistentModelIndex original(model.indexForPath(timeFile));
        int metadataUpdates = 0;
        QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
            [&](const QModelIndex&, const QModelIndex&) { ++metadataUpdates; });
        // These distinct instants share the same local clock hour during the
        // US Eastern autumn transition. Comparison must retain instant identity.
        for (int hour : {5, 6}) {
            const QDateTime expected(QDate(2024, 11, 3), QTime(hour, 30), QTimeZone::UTC);
            const int before = metadataUpdates;
            QFile file(timeFile);
            const bool changed = file.open(QIODevice::ReadWrite)
                && file.setFileTime(expected, QFileDevice::FileModificationTime);
            file.close();
            passed &= expect(changed && waitFor([&] {
                return metadataUpdates > before && original.isValid()
                    && model.fileInfo(original).lastModified(QTimeZone::UTC) == expected;
            }), "timestamp-only changes update the existing row at both UTC instants");
            passed &= expect(original == model.indexForPath(timeFile)
                    && model.fileInfo(original).size() == 7,
                "timestamp refresh preserves row identity and file size");
        }
    }
    {
        const QString permissionRoot = fixture.filePath("permission-refresh");
        const QString permissionFile = permissionRoot + "/unchanged-content.txt";
        passed &= expect(QDir().mkpath(permissionRoot) && writeFile(permissionFile),
            "create permission-only refresh fixture");
        LocalDirectoryModel model;
        model.setDirectory(permissionRoot);
        auto* watcher = model.findChild<DirectoryWatcher*>();
        passed &= expect(waitFor([&] { return model.indexForPath(permissionFile).isValid()
                && !model.loading() && watcher && watcher->isWatching(permissionRoot); }),
            "permission fixture monitoring ready");
        const QPersistentModelIndex original(model.indexForPath(permissionFile));
        const QDateTime modified = QFileInfo(permissionFile).lastModified(QTimeZone::UTC);
        const std::wstring nativePath = QDir::toNativeSeparators(permissionFile).toStdWString();
        const DWORD originalAttributes = GetFileAttributesW(nativePath.c_str());
        passed &= expect(originalAttributes != INVALID_FILE_ATTRIBUTES,
            "retain disposable fixture attributes for restoration");
        int metadataUpdates = 0;
        QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
            [&](const QModelIndex&, const QModelIndex&) { ++metadataUpdates; });
        using namespace filemanager::ui::diagnostics;
        static int guiMetadataComparisons = 0;
        static int guiListingCompletions = 0;
        guiMetadataComparisons = 0;
        guiListingCompletions = 0;
        const auto previousObserver = presentationObserver;
        presentationObserver = [](PresentationStage stage, int, int, qint64) {
            if (stage >= PresentationStage::MetadataSize && stage <= PresentationStage::MetadataType)
                ++guiMetadataComparisons;
            if (stage == PresentationStage::ListingCompletion)
                ++guiListingCompletions;
        };
        if (originalAttributes != INVALID_FILE_ATTRIBUTES) {
            for (bool readOnly : {true, false}) {
                // Populate the old cached permissions before changing the file.
                // This retained copy must remain unchanged while the worker reads.
                const QFileInfo beforeInfo = model.fileInfo(original);
                const auto beforePermissions = beforeInfo.permissions();
                const int beforeUpdates = metadataUpdates;
                const DWORD attributes = readOnly ? originalAttributes | FILE_ATTRIBUTE_READONLY
                    : originalAttributes & ~FILE_ATTRIBUTE_READONLY;
                const bool changed = SetFileAttributesW(nativePath.c_str(), attributes) != FALSE;
                const auto expected = QFileInfo(permissionFile).permissions();
                passed &= expect(changed && expected != beforePermissions && waitFor([&] {
                    return metadataUpdates > beforeUpdates && original.isValid()
                        && model.fileInfo(original).permissions() == expected;
                }), "read-only and writable changes update the existing model row");
                passed &= expect(original == model.indexForPath(permissionFile)
                        && model.fileInfo(original).size() == 7
                        && model.fileInfo(original).lastModified(QTimeZone::UTC) == modified
                        && beforeInfo.permissions() == beforePermissions,
                    "permission refresh preserves identity, content size, mtime and old snapshot");
            }
            passed &= expect(SetFileAttributesW(nativePath.c_str(), originalAttributes) != FALSE,
                "restore original disposable fixture attributes");
        }
        const int completionsBefore = guiListingCompletions;
        model.refresh();
        passed &= expect(waitFor([&] { return !model.loading()
                && guiListingCompletions > completionsBefore; }), "unchanged refresh completes");
        presentationObserver = previousObserver;
        passed &= expect(guiListingCompletions > 0 && guiMetadataComparisons == 0,
            "listing callbacks perform no metadata comparisons on the GUI thread");
        std::printf("permission_refresh gui_completions=%d gui_metadata_comparisons=%d\n",
            guiListingCompletions, guiMetadataComparisons);
    }
    {
        const QString hydrationRoot = fixture.filePath("ordered-cache-hydration");
        const QString firstPath = hydrationRoot + "/z-original.txt";
        const QString addedPath = hydrationRoot + "/a-added.txt";
        passed &= expect(QDir().mkpath(hydrationRoot) && writeFile(firstPath),
            "create ordered cache-hydration fixture");
        LocalDirectoryModel model;
        model.setDirectory(hydrationRoot);
        auto* watcher = model.findChild<DirectoryWatcher*>();
        passed &= expect(waitFor([&] { return !model.loading()
                && model.indexForPath(firstPath).isValid()
                && watcher && watcher->isWatching(hydrationRoot); }),
            "ordered cache-hydration monitoring ready");
        const QPersistentModelIndex first(model.indexForPath(firstPath));
        QFile addedFile(addedPath);
        const bool added = addedFile.open(QIODevice::WriteOnly) && addedFile.write("new") == 3;
        addedFile.close();
        passed &= expect(added && waitFor([&] { return !model.loading()
                && model.indexForPath(addedPath).isValid(); }),
            "watcher appends a differently sized file without replacing existing rows");
        const QPersistentModelIndex second(model.indexForPath(addedPath));
        const int firstRow = first.row();
        const int secondRow = second.row();
        const auto enumerated = QDir(hydrationRoot).entryInfoList(model.filters(), QDir::NoSort);
        std::printf("cache_hydration enumeration_order_differs=%d\n", !enumerated.isEmpty()
            && enumerated.first().absoluteFilePath() != model.filePath(model.index(0, 0)));
        int updates = 0;
        QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
            [&](const QModelIndex&, const QModelIndex&) { ++updates; });
        using namespace filemanager::ui::diagnostics;
        static int unchangedHydrations = 0;
        unchangedHydrations = 0;
        const auto previousObserver = presentationObserver;
        presentationObserver = [](PresentationStage stage, int, int, qint64) {
            if (stage == PresentationStage::ListingUnchangedCheck)
                ++unchangedHydrations;
        };
        for (int repeat = 0; repeat < 2; ++repeat) {
            const int before = unchangedHydrations;
            if (watcher)
                emit watcher->directoryChanged(hydrationRoot);
            passed &= expect(waitFor([&] { return unchangedHydrations > before && !model.loading(); }),
                "unchanged watcher hint hydrates the existing rows");
            passed &= expect(first.isValid() && second.isValid() && model.rowCount() == 2
                    && first.row() == firstRow && second.row() == secondRow
                    && model.filePath(first) == firstPath && model.filePath(second) == addedPath
                    && model.fileInfo(first).size() == 7 && model.fileInfo(second).size() == 3
                    && model.indexForPath(firstPath) == first && model.indexForPath(addedPath) == second
                    && updates == 0,
                "unchanged cache hydration preserves each file's identity, row and metadata without notifications");
        }
        presentationObserver = previousObserver;
    }
    {
        auto watcher = std::make_unique<DirectoryWatcher>();
        watcher->setRoot(root);
        passed &= expect(waitFor([&] { return watcher->isWatching(root); }),
            "shutdown regression starts a real watcher helper");
        QPointer<QProcess> helper;
        for (auto* process : app.findChildren<QProcess*>())
            if (process->arguments() == QStringList{QStringLiteral("--watch-directories")}
                    && process->state() == QProcess::Running)
                helper = process;
        HANDLE nativeProcess = helper ? OpenProcess(SYNCHRONIZE, FALSE,
            static_cast<DWORD>(helper->processId())) : nullptr;
        passed &= expect(helper && nativeProcess, "retain exact helper identity before service shutdown");
        if (helper && nativeProcess) {
            QObject* service = helper->parent();
            watcher.reset();
            // Do not pump events: this reproduces application destruction before
            // the queued last-subscriber callback or process completion is delivered.
            QElapsedTimer destruction;
            destruction.start();
            delete service;
            const qint64 destructionMs = destruction.elapsed();
            const bool detached = helper && !helper->parent();
            passed &= expect(detached && destructionMs < 100,
                "service destruction retains pending QProcess without blocking the UI");
            const bool stopped = waitFor([&] {
                return WaitForSingleObject(nativeProcess, 0) == WAIT_OBJECT_0;
            }, 5000);
            passed &= expect(stopped, "service job terminates the exact owned helper");
            passed &= expect(waitFor([&] {
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                return helper.isNull();
            }),
                "finished helper releases its detached process wrapper");
            std::printf("watcher_service_shutdown destruction_ms=%lld detached=%d native_stopped=%d\n",
                static_cast<long long>(destructionMs), detached, stopped);
        }
        if (nativeProcess)
            CloseHandle(nativeProcess);
    }
#endif
    std::printf("directory_watch_tests passed=%d\n", passed);
    return passed ? 0 : 1;
}
