#include "settings/contextmenupermissions.h"
#include "settings/shellpermissions.h"
#include "ui/filepane.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLoggingCategory>
#include <QSettings>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {

constexpr int WarmSampleCount = 30;
constexpr qint64 LoadTimeoutMs = 10000;
enum class Measurement { SafePaths, Consent, Visit };
std::atomic_uint benchmarkWarnings{0};
QtMessageHandler previousMessageHandler = nullptr;

void recordBenchmarkMessage(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        benchmarkWarnings.fetch_add(1, std::memory_order_relaxed);
    if (previousMessageHandler)
        previousMessageHandler(type, context, message);
    else
        std::fprintf(stderr, "%s\n", qFormatLogMessage(type, context, message).toUtf8().constData());
}

struct ShellTiming
{
    qint64 callMicroseconds = 0;
    qint64 firstContentMicroseconds = 0;
    qint64 completeMicroseconds = 0;
    qint64 maxEventTurnMicroseconds = 0;
    int itemCount = 0;
    bool success = false;
    bool truncated = false;
    QString error;
    int providerRequests = 0;
};

void processOneTurn()
{
    QEventLoop events;
    // Allow native messages to progress between observations instead of spinning zero-time loops.
    // Report this sampling floor; it is included in event-turn and content latency measurements.
    QTimer::singleShot(1, Qt::PreciseTimer, &events, &QEventLoop::quit);
    events.exec();
}

void processFor(qint64 milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds)
        processOneTurn();
}

qint64 processMeasuredTurn()
{
    QElapsedTimer timer;
    timer.start();
    processOneTurn();
    return timer.nsecsElapsed() / 1000;
}

qint64 percentile(QList<qint64> values, double fraction)
{
    if (values.isEmpty())
        return 0;
    std::sort(values.begin(), values.end());
    const qsizetype index = qBound<qsizetype>(0,
        static_cast<qsizetype>((values.size() - 1) * fraction + 0.5), values.size() - 1);
    return values.at(index);
}

void printDistribution(const char* destination, const char* metric,
    const QList<qint64>& values)
{
    std::printf("%s_%s samples=%lld median_us=%lld p95_us=%lld min_us=%lld max_us=%lld\n",
        destination, metric, static_cast<long long>(values.size()),
        static_cast<long long>(percentile(values, 0.5)),
        static_cast<long long>(percentile(values, 0.95)),
        static_cast<long long>(percentile(values, 0.0)),
        static_cast<long long>(percentile(values, 1.0)));
}

ShellTiming measureDestination(FilePane& pane,
    filemanager::platform::BuiltInShellLocation location, Measurement mode)
{
    ShellTiming result;
    bool finished = false;
    const QMetaObject::Connection completion = QObject::connect(&pane,
        &FilePane::shellLoadFinished, &pane,
        [&result, &finished](bool success, bool truncated, const QString& error) {
            result.success = success;
            result.truncated = truncated;
            result.error = error;
            finished = true;
        });
    QElapsedTimer timer;
    const auto request = QObject::connect(&pane, &FilePane::shellProviderRequestStarted, &pane,
        [&result] { ++result.providerRequests; });
    timer.start();
    const bool accepted = pane.navigateBuiltInShell(location, false);
    result.callMicroseconds = timer.nsecsElapsed() / 1000;
    if (mode != Measurement::SafePaths) {
        auto* allow = pane.findChild<QPushButton*>("AllowShellVisit");
        const bool gated = accepted && finished && !result.success && allow && allow->isVisible()
            && result.providerRequests == 0 && pane.visibleItemCount() == 0
            && !pane.hasTemporaryShellVisit();
        if (!gated) {
            result.success = false;
            result.error = QStringLiteral("The destination did not stop at consent without provider work.");
            QObject::disconnect(completion);
            QObject::disconnect(request);
            return result;
        }
        if (mode == Measurement::Consent) {
            result.success = true;
            result.error.clear();
        } else {
            finished = false;
            result.error.clear();
            timer.restart(); // Measure provider latency from Allow, excluding human decision time.
            allow->click();
            result.callMicroseconds = timer.nsecsElapsed() / 1000;
        }
    }
    while (accepted && !finished && timer.elapsed() < LoadTimeoutMs) {
        result.maxEventTurnMicroseconds = qMax(
            result.maxEventTurnMicroseconds, processMeasuredTurn());
        if (pane.visibleItemCount() > 0 && result.firstContentMicroseconds == 0)
            result.firstContentMicroseconds = timer.nsecsElapsed() / 1000;
    }
    if (finished) {
        result.completeMicroseconds = timer.nsecsElapsed() / 1000;
        result.itemCount = pane.visibleItemCount();
        if (result.firstContentMicroseconds == 0)
            result.firstContentMicroseconds = result.completeMicroseconds;
    } else if (!accepted) {
        result.error = QStringLiteral("The built-in destination was not accepted.");
    } else {
        result.error = QStringLiteral("Timed out waiting for isolated Shell enumeration.");
    }
    QObject::disconnect(completion);
    QObject::disconnect(request);
    if (result.providerRequests != (mode == Measurement::Visit ? 1 : 0)
            || pane.hasTemporaryShellVisit() != (mode == Measurement::Visit)
            || filemanager::settings::loadShellPermissions().allowThirdPartyNamespaces) {
        result.success = false;
        result.error = QStringLiteral("Unexpected provider dispatch or permission state.");
    }
    return result;
}

bool identityMatches(const FilePane& pane,
    filemanager::platform::BuiltInShellLocation location)
{
    return !pane.isFileSystemLocation()
        && pane.currentPath().isEmpty()
        && pane.builtInShellLocation() == location
        && pane.serializedLocation()
            == filemanager::platform::ShellItem::builtInIdentity(location);
}

int runBenchmark()
{
    QTemporaryDir settings;
    if (!settings.isValid()) {
        std::fprintf(stderr, "Could not create isolated benchmark settings.\n");
        return 10;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

    const auto namespacePermissions = filemanager::settings::loadShellPermissions();
    const auto contextPermissions = filemanager::settings::loadContextMenuPermissions();
    if (namespacePermissions.allowThirdPartyNamespaces
            || contextPermissions.allowWindowsExtensions) {
        std::fprintf(stderr, "Optional Windows Shell permissions were not off in the isolated profile.\n");
        return 11;
    }

    struct Destination {
        const char* label;
        filemanager::platform::BuiltInShellLocation location;
        Measurement mode;
    };
    const Destination destinations[] = {
        {"this_pc_safe_paths", filemanager::platform::BuiltInShellLocation::ThisPc, Measurement::SafePaths},
        {"libraries_consent", filemanager::platform::BuiltInShellLocation::Libraries, Measurement::Consent},
        {"libraries_visit", filemanager::platform::BuiltInShellLocation::Libraries, Measurement::Visit},
        {"recycle_bin_consent", filemanager::platform::BuiltInShellLocation::RecycleBin, Measurement::Consent},
        {"recycle_bin_visit", filemanager::platform::BuiltInShellLocation::RecycleBin, Measurement::Visit},
    };

    FilePane pane(FilePane::HomeIdentity);
    pane.resize(1000, 700);
    pane.show();
    processOneTurn();

    std::printf("shell_navigation_benchmark warm_samples=%d timeout_ms=%lld "
        "persistent_namespaces=off context_menu_extensions=off visit_mode=explicit_allow_each_sample "
        "event_turn_pacing_ms=1\n",
        WarmSampleCount, static_cast<long long>(LoadTimeoutMs));
    for (const Destination& destination : destinations) {
        const ShellTiming first = measureDestination(pane, destination.location, destination.mode);
        if (benchmarkWarnings.load(std::memory_order_relaxed) != 0)
            return 20;
        if (!first.success || first.completeMicroseconds == 0
                || !identityMatches(pane, destination.location)) {
            std::fprintf(stderr, "%s first load failed: %s\n", destination.label,
                first.error.toUtf8().constData());
            return 12;
        }
        std::printf("%s_first call_us=%lld ready_us=%lld complete_us=%lld "
            "max_event_turn_us=%lld items=%d truncated=%s provider_requests=%d\n",
            destination.label,
            static_cast<long long>(first.callMicroseconds),
            static_cast<long long>(first.firstContentMicroseconds),
            static_cast<long long>(first.completeMicroseconds),
            static_cast<long long>(first.maxEventTurnMicroseconds), first.itemCount,
            first.truncated ? "true" : "false", first.providerRequests);

        QList<qint64> calls;
        QList<qint64> firstContent;
        QList<qint64> completions;
        QList<qint64> eventTurns;
        for (int sample = 0; sample < WarmSampleCount; ++sample) {
            const ShellTiming timing = measureDestination(pane, destination.location, destination.mode);
            if (benchmarkWarnings.load(std::memory_order_relaxed) != 0)
                return 20;
            if (!timing.success || timing.completeMicroseconds == 0
                    || !identityMatches(pane, destination.location)) {
                std::fprintf(stderr, "%s warm load %d failed: %s\n", destination.label,
                    sample + 1, timing.error.toUtf8().constData());
                return 13;
            }
            calls.append(timing.callMicroseconds);
            firstContent.append(timing.firstContentMicroseconds);
            completions.append(timing.completeMicroseconds);
            eventTurns.append(timing.maxEventTurnMicroseconds);
        }
        printDistribution(destination.label, "call", calls);
        printDistribution(destination.label,
            destination.mode == Measurement::Consent ? "consent_ready" : "first_content", firstContent);
        printDistribution(destination.label, "complete", completions);
        printDistribution(destination.label, "max_event_turn", eventTurns);
    }

    pane.navigateBuiltInShell(filemanager::platform::BuiltInShellLocation::Libraries, false);
    auto* allow = pane.findChild<QPushButton*>("AllowShellVisit");
    if (!allow || !allow->isVisible() || pane.hasTemporaryShellVisit())
        return 14;
    int supersededCompletions = 0;
    filemanager::platform::BuiltInShellLocation completedLocation
        = filemanager::platform::BuiltInShellLocation::None;
    const QMetaObject::Connection supersededCompletion = QObject::connect(&pane,
        &FilePane::shellLoadFinished, &pane,
        [&pane, &supersededCompletions, &completedLocation](bool, bool, const QString&) {
            ++supersededCompletions;
            completedLocation = pane.builtInShellLocation();
        });
    QElapsedTimer supersededTimer;
    supersededTimer.start();
    allow->click();
    pane.navigateBuiltInShell(filemanager::platform::BuiltInShellLocation::ThisPc, false);
    processFor(5500);
    QObject::disconnect(supersededCompletion);
    if (supersededCompletions != 1
            || completedLocation != filemanager::platform::BuiltInShellLocation::ThisPc
            || pane.hasTemporaryShellVisit() || !pane.currentShellLocation().idList.isEmpty()
            || !identityMatches(pane,
                filemanager::platform::BuiltInShellLocation::ThisPc)) {
        std::fprintf(stderr,
            "A superseded built-in destination published stale completion state.\n");
        return 14;
    }
    std::printf("superseded_destinations observation_us=%lld completions=%d final=this_pc_safe_paths\n",
        static_cast<long long>(supersededTimer.nsecsElapsed() / 1000),
        supersededCompletions);
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    previousMessageHandler = qInstallMessageHandler(recordBenchmarkMessage);
    int result = 0;
    {
        QApplication application(argc, argv);
        application.setOrganizationName(QStringLiteral("FileManagerTests"));
        application.setApplicationName(QStringLiteral("ShellNavigationPerformanceBenchmark"));
        if (application.arguments().size() != 1) {
            std::fprintf(stderr, "This benchmark takes no arguments.\n");
            result = 1;
        } else {
            result = runBenchmark();
        }
    }
    qInstallMessageHandler(previousMessageHandler);
    const unsigned warnings = benchmarkWarnings.load(std::memory_order_relaxed);
    if (warnings != 0) {
        std::fprintf(stderr, "INVALID PERFORMANCE RUN: %u Qt warning/critical diagnostics. "
            "Do not accept emitted timings as a clean baseline.\n", warnings);
        return result == 0 ? 20 : result;
    }
    return result;
}
