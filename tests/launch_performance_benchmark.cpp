#include "mainwindow.h"
#include "settings/settingshealth.h"
#include "ui/applicationtheme.h"
#include "ui/brandidentity.h"
#include "ui/filepane.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <cstdio>

namespace {

constexpr int EntryCount = 100;
constexpr int WarmSampleCount = 30;
constexpr qint64 LaunchTimeoutMs = 10000;
constexpr auto ReadyMarker = "FILE_MANAGER_FIRST_CONTENT_READY";
constexpr auto StageMarker = "startup_stages_us ";

bool createFixture(const QString& path)
{
    if (!QDir().mkpath(path))
        return false;
    for (int index = 0; index < EntryCount; ++index) {
        QFile item(QDir(path).filePath(
            QStringLiteral("entry-%1.txt").arg(index, 4, 10, QLatin1Char('0'))));
        if (!item.open(QIODevice::WriteOnly) || item.write("x") != 1)
            return false;
    }
    return true;
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

void printDistribution(const char* label, const QList<qint64>& values)
{
    std::printf("%s samples=%lld median_us=%lld p95_us=%lld min_us=%lld max_us=%lld\n",
        label, static_cast<long long>(values.size()),
        static_cast<long long>(percentile(values, 0.5)),
        static_cast<long long>(percentile(values, 0.95)),
        static_cast<long long>(percentile(values, 0.0)),
        static_cast<long long>(percentile(values, 1.0)));
}

int runFixture(QApplication& application, const QString& settingsPath, const QString& folder,
    bool directStartup)
{
    QElapsedTimer childTimer;
    childTimer.start();
    if (!QFileInfo(settingsPath).isDir() || !QFileInfo(folder).isDir())
        return 20;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsPath);
    application.setApplicationName(filemanager::ui::BrandIdentity::productName());
    application.setApplicationDisplayName(filemanager::ui::BrandIdentity::productName());
    application.setApplicationVersion(FILE_MANAGER_VERSION);
    application.setOrganizationName(QStringLiteral("FileManagerLaunchBenchmark"));
    application.setWindowIcon(filemanager::ui::BrandIdentity::applicationIcon());
    const auto settingsHealth = filemanager::settings::repairSettings();
    application.setProperty("settingsRecovered", settingsHealth.repaired());
    filemanager::ui::applyApplicationTheme(application);
    if (directStartup) {
        QSettings startupSettings;
        startupSettings.setValue(QStringLiteral("navigation/openTo"),
            QStringLiteral("last-folder"));
        startupSettings.setValue(QStringLiteral("navigation/recents"), QStringList{folder});
        startupSettings.sync();
        if (startupSettings.status() != QSettings::NoError)
            return 22;
    }

    const qint64 setupMicroseconds = childTimer.nsecsElapsed() / 1000;
    QElapsedTimer stageTimer;
    stageTimer.start();
    MainWindow::StartupTimings timings;
    MainWindow window(directStartup ? QString{} : folder, &timings);
    const qint64 windowMicroseconds = stageTimer.nsecsElapsed() / 1000;
    stageTimer.restart();
    window.openInitialPaths({folder});
    const qint64 openMicroseconds = stageTimer.nsecsElapsed() / 1000;
    stageTimer.restart();
    window.show();
    const qint64 showMicroseconds = stageTimer.nsecsElapsed() / 1000;
    stageTimer.restart();
    bool reported = false;
    QTimer readiness;
    readiness.setInterval(1);
    QObject::connect(&readiness, &QTimer::timeout, &application, [&] {
        if (reported || !window.isVisible() || !window.windowHandle()
                || !window.windowHandle()->isExposed())
            return;
        for (FilePane* pane : window.findChildren<FilePane*>()) {
            if (!pane->isVisible()
                    || QFileInfo(pane->currentPath()).absoluteFilePath()
                        != QFileInfo(folder).absoluteFilePath()
                    || pane->visibleItemCount() != EntryCount)
                continue;
            reported = true;
            timings.insert(QStringLiteral("show"), showMicroseconds);
            timings.insert(QStringLiteral("exposure_content_wait"), stageTimer.nsecsElapsed() / 1000);
            QJsonObject stages;
            for (auto it = timings.cbegin(); it != timings.cend(); ++it)
                stages.insert(it.key(), it.value());
            std::printf("startup_mode=%s\n", directStartup ? "settings" : "explicit");
            std::printf("%s%s\n", StageMarker,
                QJsonDocument(stages).toJson(QJsonDocument::Compact).constData());
            std::printf("child_phases_us setup=%lld window=%lld open=%lld ready=%lld\n",
                static_cast<long long>(setupMicroseconds),
                static_cast<long long>(windowMicroseconds),
                static_cast<long long>(openMicroseconds),
                static_cast<long long>(childTimer.nsecsElapsed() / 1000));
            std::fputs(ReadyMarker, stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
            QTimer::singleShot(0, &application, &QCoreApplication::quit);
            return;
        }
    });
    readiness.start();
    QTimer::singleShot(LaunchTimeoutMs, &application, [&] {
        if (!reported)
            application.exit(21);
    });
    return application.exec();
}

qint64 measureLaunch(const QString& executable, const QString& settingsPath,
    const QString& folder, bool directStartup, QString* error,
    QByteArray* evidence = nullptr)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    QElapsedTimer timer;
    timer.start();
    process.start(executable,
        {directStartup ? QStringLiteral("--fixture-direct") : QStringLiteral("--fixture"),
            settingsPath, folder},
        QIODevice::ReadOnly);
    if (!process.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("process start failed: %1").arg(process.errorString());
        return 0;
    }
    QByteArray output;
    qint64 readyMicroseconds = 0;
    while (timer.elapsed() < LaunchTimeoutMs && process.state() != QProcess::NotRunning) {
        if (process.waitForReadyRead(100)) {
            output.append(process.readAll());
            if (output.contains(ReadyMarker)) {
                readyMicroseconds = timer.nsecsElapsed() / 1000;
                break;
            }
        }
    }
    output.append(process.readAll());
    if (readyMicroseconds == 0 && output.contains(ReadyMarker))
        readyMicroseconds = timer.nsecsElapsed() / 1000;
    if (!process.waitForFinished(3000)) {
        process.terminate();
        if (!process.waitForFinished(1000)) {
            process.kill();
            process.waitForFinished(1000);
        }
    }
    const QByteArray expectedMode = directStartup ? "startup_mode=settings" : "startup_mode=explicit";
    if (readyMicroseconds == 0 || !output.contains(expectedMode)
            || process.exitStatus() != QProcess::NormalExit
            || process.exitCode() != 0) {
        if (error) {
            *error = QStringLiteral("ready=%1 exit=%2 output=%3")
                .arg(readyMicroseconds)
                .arg(process.exitCode())
                .arg(QString::fromLocal8Bit(output).trimmed());
        }
        return 0;
    }
    if (evidence)
        *evidence = output;
    return readyMicroseconds;
}

void printEvidence(const char* label, QByteArray evidence)
{
    evidence.replace('\r', ' ');
    evidence.replace('\n', ' ');
    std::printf("%s %s\n", label, evidence.trimmed().constData());
}

bool appendStages(const QByteArray& evidence, QMap<QString, QList<qint64>>& samples)
{
    for (const QByteArray& line : evidence.split('\n')) {
        if (!line.startsWith(StageMarker))
            continue;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line.mid(qstrlen(StageMarker)), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return false;
        const QJsonObject stages = document.object();
        if (stages.size() != 14)
            return false;
        for (auto it = stages.begin(); it != stages.end(); ++it) {
            if (!it.value().isDouble() || it.value().toInteger(-1) < 0)
                return false;
            samples[it.key()].append(it.value().toInteger());
        }
        return true;
    }
    return false;
}

void printStages(const char* mode, const QMap<QString, QList<qint64>>& stages)
{
    for (auto it = stages.cbegin(); it != stages.cend(); ++it) {
        const QByteArray label = QByteArray(mode) + '_' + it.key().toLatin1();
        printDistribution(label.constData(), it.value());
    }
}

int runBenchmark(const QString& executable)
{
    QTemporaryDir root;
    if (!root.isValid()) {
        std::fprintf(stderr, "Could not create the isolated launch benchmark root.\n");
        return 10;
    }
    const QString settingsPath = QDir(root.path()).filePath(QStringLiteral("settings"));
    const QString directSettingsPath = QDir(root.path()).filePath(
        QStringLiteral("direct-settings"));
    const QString folder = QDir(root.path()).filePath(QStringLiteral("folder"));
    if (!QDir().mkpath(settingsPath) || !QDir().mkpath(directSettingsPath)
            || !createFixture(folder)) {
        std::fprintf(stderr, "Could not create the launch benchmark fixture.\n");
        return 11;
    }

    QString error;
    QByteArray firstEvidence;
    const qint64 firstLaunch = measureLaunch(
        executable, settingsPath, folder, false, &error, &firstEvidence);
    if (firstLaunch == 0) {
        std::fprintf(stderr, "The first independent launch failed: %s\n",
            error.toUtf8().constData());
        return 12;
    }
    QList<qint64> warmLaunches;
    QMap<QString, QList<qint64>> warmStages;
    QByteArray warmEvidence;
    for (int sample = 0; sample < WarmSampleCount; ++sample) {
        const qint64 elapsed = measureLaunch(executable, settingsPath, folder, false, &error,
            &warmEvidence);
        if (elapsed == 0 || !appendStages(warmEvidence, warmStages)) {
            std::fprintf(stderr, "Warm launch %d failed: %s\n", sample + 1,
                error.toUtf8().constData());
            return 13;
        }
        warmLaunches.append(elapsed);
    }
    std::printf("first_independent_launch_us=%lld cache_state=uncontrolled\n",
        static_cast<long long>(firstLaunch));
    printDistribution("warm_launch_first_content", warmLaunches);
    printStages("explicit", warmStages);
    printEvidence("first_launch_child", firstEvidence);
    printEvidence("warm_launch_child", warmEvidence);

    QByteArray firstDirectEvidence;
    const qint64 firstDirectLaunch = measureLaunch(
        executable, directSettingsPath, folder, true, &error, &firstDirectEvidence);
    if (firstDirectLaunch == 0) {
        std::fprintf(stderr, "The first direct-folder launch failed: %s\n",
            error.toUtf8().constData());
        return 14;
    }
    QList<qint64> warmDirectLaunches;
    QMap<QString, QList<qint64>> warmDirectStages;
    QByteArray warmDirectEvidence;
    for (int sample = 0; sample < WarmSampleCount; ++sample) {
        const qint64 elapsed = measureLaunch(
            executable, directSettingsPath, folder, true, &error, &warmDirectEvidence);
        if (elapsed == 0 || !appendStages(warmDirectEvidence, warmDirectStages)) {
            std::fprintf(stderr, "Warm direct-folder launch %d failed: %s\n", sample + 1,
                error.toUtf8().constData());
            return 15;
        }
        warmDirectLaunches.append(elapsed);
    }
    std::printf("first_direct_folder_launch_us=%lld cache_state=warm\n",
        static_cast<long long>(firstDirectLaunch));
    printDistribution("warm_direct_folder_first_content", warmDirectLaunches);
    printStages("settings", warmDirectStages);
    printEvidence("first_direct_child", firstDirectEvidence);
    printEvidence("warm_direct_child", warmDirectEvidence);
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() == 4 && (arguments.at(1) == QStringLiteral("--fixture")
            || arguments.at(1) == QStringLiteral("--fixture-direct"))) {
        return runFixture(application, arguments.at(2), arguments.at(3),
            arguments.at(1) == QStringLiteral("--fixture-direct"));
    }
    if (arguments.size() != 1) {
        std::fprintf(stderr, "This benchmark takes no arguments.\n");
        return 1;
    }
    return runBenchmark(QCoreApplication::applicationFilePath());
}
