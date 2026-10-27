#include "diagnostics/asyncdiagnosticlog.h"
#include "platform/helperinput.h"
#include "platform/helperoutput.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

using filemanager::diagnostics::AsyncDiagnosticLog;
namespace {
bool passed = true;
void expect(bool ok, const char* description)
{
    std::fprintf(ok ? stdout : stderr, "%s: %s\n", ok ? "PASS" : "FAIL", description);
    std::fflush(ok ? stdout : stderr);
    passed &= ok;
}
bool waitFor(const std::function<bool()>& condition, int milliseconds = 20000)
{
    QElapsedTimer time;
    time.start();
    while (!condition() && time.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}
QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
bool write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
void drain(AsyncDiagnosticLog& log)
{
    bool finished = false;
    const auto connection = QObject::connect(&log, &AsyncDiagnosticLog::shutdownFinished,
        &log, [&] { finished = true; });
    log.shutdown();
    expect(waitFor([&] { return finished; }, 7000), "Shutdown completes without a blocking join");
    QObject::disconnect(connection);
}

// Deterministic hostile/slow transport; never enabled in the production helper.
int fixture(const QString& mode)
{
    QByteArray line;
    QJsonArray events;
    filemanager::platform::HelperInputReader input;
    for (;;) {
        const int next = input.next();
        if (next < 0)
            return next == -1 ? 0 : 3;
        if (next != '\n') { line.append(static_cast<char>(next)); continue; }
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        line.clear();
        const QString op = request.value("op").toString();
        if (op == "record" && mode == "stall")
            QThread::msleep(30000);
        if (op == "record" && mode == "slow")
            QThread::msleep(600);
        bool success = !(op == "clear" && mode == "clear-failure");
        if (op == "record") events.append(request.value("code"));
        if (op == "clear" && success) events = {};
        QByteArray reply = QJsonDocument(QJsonObject{{"id", request.value("id")},
            {"success", success}, {"healthy", true}, {"events", events},
            {"error", success ? "" : "Synthetic clear failure"}}).toJson(QJsonDocument::Compact) + '\n';
        if (mode == "malformed") reply = "invalid reply\n";
        if (!filemanager::platform::writeHelperReply(reply))
            return 2;
    }
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments().at(1) == "--fixture")
        return fixture(app.arguments().at(2));
    app.setApplicationName("AsyncDiagnosticTests");
    QTemporaryDir root;
    expect(root.isValid(), "Owned temporary fixture is available");
    if (!root.isValid()) return 1;
    const QString self = QCoreApplication::applicationFilePath();
    const QString history = root.filePath("events.json");
    {
        AsyncDiagnosticLog log(history, nullptr, self, {"--fixture", "slow"});
        for (int i = 0; i < AsyncDiagnosticLog::maximumPendingRecords; ++i)
            expect(log.record(QString("queued.%1").arg(i)), "A bounded record request is accepted before startup");
        expect(!log.record("overflow.event") && log.pendingCount() == 64
            && log.errorString().contains("full"), "Queue overflow explicitly rejects the new event");
        expect(log.clear() && log.pendingCount() == 0 && log.isClearing(),
            "Clear discards older queued records before loading finishes");
        expect(!log.record("during.clear"), "New records are rejected during clear");
        bool cleared = false;
        QObject::connect(&log, &AsyncDiagnosticLog::clearFinished, &log,
            [&](bool ok, const QString&) { cleared = ok; });
        expect(waitFor([&] { return cleared; }) && log.eventCount() == 0,
            "Clear completion confirms an empty committed snapshot");
        int heartbeats = 0;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, &log, [&] { ++heartbeats; });
        heartbeat.start(10);
        expect(log.record("first.event") && log.record("second.event"), "FIFO records accepted");
        expect(log.eventCount() == 0, "Pending records are not reported as persisted");
        expect(waitFor([&] { return log.pendingCount() == 0; })
            && log.eventCodes() == QStringList{"first.event", "second.event"} && heartbeats >= 20,
            "FIFO persistence preserves event-loop heartbeats during slow writes");
        expect(log.record("stale.event"), "Start a slow in-flight record");
        const int countBeforeClear = log.eventCount();
        cleared = false;
        expect(log.clear(), "Clear queues behind an in-flight write");
        bool stalePublished = false;
        QObject::connect(&log, &AsyncDiagnosticLog::stateChanged, &log, [&] {
            if (log.isClearing() && log.eventCount() != countBeforeClear) stalePublished = true;
        });
        expect(waitFor([&] { return cleared; }) && !stalePublished && log.eventCount() == 0,
            "Stale write acknowledgement never resurrects cleared history");
        expect(log.record("drain.first") && log.record("drain.second"), "Queue writes immediately before graceful shutdown");
        drain(log);
        expect(log.eventCodes() == QStringList{"drain.first", "drain.second"},
            "Graceful shutdown drains accepted FIFO writes before signalling completion");
    }
    {
        AsyncDiagnosticLog log(history, nullptr, self, {"--fixture", "clear-failure"});
        expect(log.record("preserved.event") && waitFor([&] { return log.pendingCount() == 0 && !log.isLoading(); }),
            "Seed history for failed clear");
        bool replied = false, succeeded = true;
        QObject::connect(&log, &AsyncDiagnosticLog::clearFinished, &log,
            [&](bool ok, const QString&) { replied = true; succeeded = ok; });
        expect(log.clear() && waitFor([&] { return replied; }) && !succeeded
            && log.eventCodes() == QStringList{"preserved.event"} && !log.errorString().isEmpty(),
            "Failed clear retains history and reports failure");
        drain(log);
    }
    {
        AsyncDiagnosticLog log(history, nullptr, self, {"--fixture", "stall"});
        expect(waitFor([&] { return !log.isLoading(); }) && log.record("blocked.event"),
            "Start a stalled write for shutdown");
        bool finished = false;
        QObject::connect(&log, &AsyncDiagnosticLog::shutdownFinished, &log, [&] { finished = true; });
        QElapsedTimer elapsed;
        elapsed.start();
        log.shutdown();
        expect(!log.record("after.shutdown"), "Shutdown rejects further diagnostic records");
        expect(waitFor([&] { return finished; }, 7000) && elapsed.elapsed() >= 4500
            && elapsed.elapsed() < 6500 && !log.errorString().isEmpty(),
            "Stalled writer is stopped after the bounded five-second asynchronous drain");
    }
    {
        QElapsedTimer elapsed;
        auto* log = new AsyncDiagnosticLog(history, nullptr, self, {"--fixture", "stall"});
        expect(waitFor([&] { return !log->isLoading(); }) && log->record("disposed.event"),
            "Start a disposable blocked writer");
#ifdef Q_OS_WIN
        const auto* child = log->findChild<QProcess*>();
        HANDLE ownedProcess = child ? OpenProcess(SYNCHRONIZE, FALSE,
            static_cast<DWORD>(child->processId())) : nullptr;
        expect(ownedProcess != nullptr, "Capture the owned helper lifetime before disposal");
#endif
        elapsed.start();
        delete log;
        expect(elapsed.elapsed() < 100, "Controller disposal does not join the blocked writer");
#ifdef Q_OS_WIN
        if (ownedProcess) {
            expect(waitFor([&] { return WaitForSingleObject(ownedProcess, 0) == WAIT_OBJECT_0; }, 3000),
                "Disposal terminates the owned helper without leaving a live writer");
            CloseHandle(ownedProcess);
        }
#endif
        QCoreApplication::processEvents();
    }
    {
        AsyncDiagnosticLog log(history, nullptr, self, {"--fixture", "malformed"});
        expect(waitFor([&] { return !log.isLoading(); }) && !log.isAvailable()
            && !log.errorString().isEmpty(), "Malformed helper reply produces an explicit unavailable state");
        drain(log);
    }
    {
        AsyncDiagnosticLog log(history);
        expect(waitFor([&] { return !log.isLoading(); }) && log.isAvailable(), "Production helper loads empty history");
        expect(!log.record("invalid/path"), "Unsafe codes are rejected before queueing");
        expect(log.record("real.first") && log.record("real.second")
            && waitFor([&] { return log.pendingCount() == 0; }, 60000)
            && log.eventCodes() == QStringList{"real.first", "real.second"},
            "Production helper commits ordered records using atomic storage");
        QByteArray saved = read(history);
        expect(!saved.isEmpty(), "Committed history exists on disk");
#ifdef Q_OS_WIN
        HANDLE locked = CreateFileW(reinterpret_cast<LPCWSTR>(history.utf16()), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        expect(locked != INVALID_HANDLE_VALUE, "Lock original history against replacement");
        if (locked != INVALID_HANDLE_VALUE) {
            expect(log.record("must.fail") && waitFor([&] { return log.pendingCount() == 0; })
                && log.eventCount() == 2 && read(history) == saved && !log.errorString().isEmpty(),
                "Failed atomic replacement preserves committed bytes and snapshot");
            CloseHandle(locked);
        }
#endif
        filemanager::diagnostics::DiagnosticBundleInput input;
        input.productVersion = "test";
        input.openPaths = {root.filePath("private-folder")};
        bool exported = false, exportOk = false;
        QObject::connect(&log, &AsyncDiagnosticLog::exportFinished, &log,
            [&](bool ok, const QString&) { exported = true; exportOk = ok; });
        const QString destination = root.filePath("bundle.json");
        expect(log.record("real.third"), "Queue a record immediately before export");
        expect(log.exportBundle(destination, input, false)
            && !log.exportBundle(destination, input, false), "Only one export can be pending");
        expect(waitFor([&] { return exported; }) && exportOk, "Export acknowledges successful destination persistence");
        const auto bundle = QJsonDocument::fromJson(read(destination)).object();
        expect(!bundle.contains("openPaths") && bundle.value("openPathCount").toInt() == 1
            && bundle.value("eventCodes").toArray() == QJsonArray{"real.first", "real.second", "real.third"},
            "Ordered redacted export waits for preceding record persistence and includes no paths");
        saved = read(history);
        exported = false;
        expect(log.exportBundle(destination, input, true) && waitFor([&] { return exported; }) && exportOk
            && QJsonDocument::fromJson(read(destination)).object().value("openPaths").toArray().size() == 1,
            "Explicit path inclusion survives background export");
        exported = false;
        expect(log.exportBundle(history, input, false) && waitFor([&] { return exported; }) && !exportOk
            && read(history) == saved, "Export cannot overwrite retained diagnostic history");
        exported = false;
        expect(log.exportBundle(root.path(), input, false) && waitFor([&] { return exported; }) && !exportOk,
            "Destination write failure is not reported as an exported bundle");
        bool cleared = false;
        QObject::connect(&log, &AsyncDiagnosticLog::clearFinished, &log,
            [&](bool ok, const QString&) { cleared = ok; });
        expect(log.clear() && waitFor([&] { return cleared; }) && log.eventCount() == 0
            && !QFile::exists(history), "Production clear acknowledges actual removal");
        drain(log);
    }
    {
        expect(write(history, "broken JSON"), "Create unreadable-format history fixture");
        AsyncDiagnosticLog log(history);
        expect(waitFor([&] { return !log.isLoading(); }) && !log.isAvailable()
            && !log.record("do.not.overwrite") && read(history) == "broken JSON",
            "Load failure is distinguished from empty history and does not overwrite it");
        bool cleared = false;
        QObject::connect(&log, &AsyncDiagnosticLog::clearFinished, &log,
            [&](bool ok, const QString&) { cleared = ok; });
        expect(log.clear() && waitFor([&] { return cleared; }) && log.isAvailable(),
            "Explicit confirmed clear recovers unavailable history");
        drain(log);
    }
    if (!passed) {
        root.setAutoRemove(false);
        std::fprintf(stderr, "Retained failed fixture: %s\n", qPrintable(root.path()));
    }
    return passed ? 0 : 1;
}
