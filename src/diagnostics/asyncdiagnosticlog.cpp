#include "diagnostics/asyncdiagnosticlog.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace filemanager::diagnostics {
AsyncDiagnosticLog::AsyncDiagnosticLog(QString storagePath, QObject* parent,
    QString helperProgram, QStringList helperArguments) : QObject(parent), storagePath_(std::move(storagePath))
{
    if (storagePath_.isEmpty())
        storagePath_ = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .filePath(QStringLiteral("diagnostics/events.json"));
    if (helperProgram.isEmpty()) {
        helperProgram = QDir(QCoreApplication::applicationDirPath()).filePath("FileManagerShellHost.exe");
        helperArguments = {QStringLiteral("--diagnostic-storage")};
    }
    shutdownTimer_.setSingleShot(true);
    shutdownTimer_.setInterval(5000);
    connect(&shutdownTimer_, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("Shutdown ended before all pending diagnostics were saved."));
    });
    // Defer process launch and all disk work until after construction and signal wiring.
    QTimer::singleShot(0, this, [this, helperProgram, helperArguments] {
        if (!stopped_)
            start(helperProgram, helperArguments);
    });
}

AsyncDiagnosticLog::~AsyncDiagnosticLog()
{
    stopProcess();
}

void AsyncDiagnosticLog::start(QString program, QStringList arguments)
{
    auto* process = new QProcess(this);
    process_ = process;
    process->setProgram(program);
    process->setArguments(arguments);
    process->setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        fail(QStringLiteral("Diagnostic storage could not be isolated."));
        return;
    }
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    connect(process, &QProcess::started, this, [this, process] {
#ifdef Q_OS_WIN
        HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
            static_cast<DWORD>(process->processId()));
        const bool assigned = child && AssignProcessToJobObject(job_, child);
        if (child)
            CloseHandle(child);
        if (!assigned) {
            fail(QStringLiteral("Diagnostic storage could not be isolated."));
            return;
        }
#endif
        started_ = true;
        QJsonObject init{{"op", "init"}, {"path", storagePath_}, {"id", QString::number(++serial_)}};
        queue_.prepend({init, generation_});
        pump();
    });
    connect(process, &QProcess::readyReadStandardOutput, this, &AsyncDiagnosticLog::readReplies);
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        fail(QStringLiteral("The diagnostic storage process is unavailable."));
    });
    connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (stopping_ && !busy_ && queue_.isEmpty() && code == 0 && status == QProcess::NormalExit) {
            stopProcess();
            finishShutdown();
        } else {
            fail(QStringLiteral("The diagnostic storage process stopped unexpectedly."));
        }
    });
    process->start();
}

bool AsyncDiagnosticLog::reject(const QString& error)
{
    error_ = error;
    emit stateChanged();
    return false;
}

bool AsyncDiagnosticLog::record(const QString& code)
{
    if (!isSafeDiagnosticEventCode(code))
        return reject(QStringLiteral("The diagnostic event code is not path-safe."));
    if (stopping_ || stopped_ || clearing_ || (!loading_ && !available_))
        return reject(QStringLiteral("The diagnostic event was not queued: storage is unavailable or clearing."));
    if (pendingRecords_ >= maximumPendingRecords)
        return reject(QStringLiteral("The diagnostic queue is full; the new event was not queued."));
    ++pendingRecords_;
    enqueue({{"op", "record"}, {"code", code}});
    return true;
}

bool AsyncDiagnosticLog::clear()
{
    if (stopping_ || stopped_ || clearing_ || (!loading_ && !process_))
        return reject(QStringLiteral("Diagnostic history cannot be cleared while storage is unavailable."));
    ++generation_;
    clearing_ = true;
    // Only records are discardable. An already accepted export keeps its FIFO position.
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->message.value("op") == "record") {
            --pendingRecords_;
            it = queue_.erase(it);
        } else {
            ++it;
        }
    }
    enqueue({{"op", "clear"}});
    return true;
}

bool AsyncDiagnosticLog::exportBundle(const QString& destination,
    const DiagnosticBundleInput& input, bool includePaths)
{
    if (stopping_ || stopped_ || clearing_ || exporting_ || (!loading_ && !available_))
        return reject(QStringLiteral("Diagnostic export is unavailable or already pending."));
    DiagnosticBundleInput snapshot = input;
    snapshot.eventCodes.clear();
    const auto bundle = QJsonDocument::fromJson(createDiagnosticBundle(snapshot, includePaths)).object();
    QJsonObject request{{"op", "export"}, {"destination", destination}, {"bundle", bundle}};
    if (QJsonDocument(request).toJson(QJsonDocument::Compact).size() > 1024 * 1024 - 256)
        return reject(QStringLiteral("The diagnostic export exceeds the request size limit."));
    exporting_ = true;
    enqueue(request);
    return true;
}

void AsyncDiagnosticLog::enqueue(QJsonObject message)
{
    message.insert("id", QString::number(++serial_));
    queue_.enqueue({std::move(message), generation_});
    emit stateChanged();
    pump();
}

void AsyncDiagnosticLog::pump()
{
    if (!started_ || !process_ || busy_)
        return;
    if (queue_.isEmpty()) {
        if (stopping_)
            process_->closeWriteChannel();
        return;
    }
    active_ = queue_.dequeue();
    busy_ = true;
    const QByteArray bytes = QJsonDocument(active_.message).toJson(QJsonDocument::Compact) + '\n';
    if (process_->write(bytes) != bytes.size())
        fail(QStringLiteral("The diagnostic storage request could not be sent."));
}

void AsyncDiagnosticLog::readReplies()
{
    if (!process_)
        return;
    replies_.append(process_->readAllStandardOutput());
    if (replies_.size() > 128 * 1024) {
        fail(QStringLiteral("The diagnostic storage reply exceeded its limit."));
        return;
    }
    const auto newline = replies_.indexOf('\n');
    if (newline < 0)
        return;
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(replies_.left(newline), &parse);
    replies_.remove(0, newline + 1);
    const QJsonObject reply = document.object();
    if (parse.error != QJsonParseError::NoError || !document.isObject() || !busy_
        || reply.value("id") != active_.message.value("id") || !replies_.isEmpty()
        || !reply.value("success").isBool() || !reply.value("healthy").isBool()
        || !reply.value("events").isArray() || reply.value("events").toArray().size() > 512) {
        fail(QStringLiteral("The diagnostic storage reply was invalid."));
        return;
    }
    QStringList committed;
    for (const auto& event : reply.value("events").toArray()) {
        if (!event.isString() || !isSafeDiagnosticEventCode(event.toString())) {
            fail(QStringLiteral("The diagnostic storage snapshot was invalid."));
            return;
        }
        committed.append(event.toString());
    }
    const QString op = active_.message.value("op").toString();
    const bool success = reply.value("success").toBool();
    const QString error = success ? QString{} : reply.value("error").toString();
    available_ = reply.value("healthy").toBool();
    if (op == "init")
        loading_ = false;
    if (op == "record")
        --pendingRecords_;
    // A pre-clear completion cannot republish the history being removed.
    if (active_.generation == generation_)
        events_ = committed;
    if (!success)
        error_ = error.isEmpty() ? QStringLiteral("Diagnostic storage did not complete the request.") : error;
    else if (op == "clear")
        error_.clear();
    busy_ = false;
    if (op == "clear") {
        clearing_ = false;
        emit clearFinished(success, error);
    }
    if (op == "export") {
        exporting_ = false;
        emit exportFinished(success, error);
    }
    emit stateChanged();
    pump();
}

void AsyncDiagnosticLog::shutdown()
{
    if (stopping_ || stopped_)
        return;
    stopping_ = true;
    shutdownTimer_.start();
    emit stateChanged();
    if (!loading_ && !process_)
        finishShutdown();
    else
        pump();
}

void AsyncDiagnosticLog::finishShutdown()
{
    if (stopped_)
        return;
    shutdownTimer_.stop();
    stopped_ = true;
    emit shutdownFinished();
}

void AsyncDiagnosticLog::fail(const QString& error)
{
    stopProcess();
    loading_ = false;
    available_ = false;
    busy_ = false;
    queue_.clear();
    pendingRecords_ = 0;
    error_ = error;
    const bool wasClearing = clearing_;
    const bool wasExporting = exporting_;
    clearing_ = exporting_ = false;
    if (wasClearing)
        emit clearFinished(false, error);
    if (wasExporting)
        emit exportFinished(false, error);
    emit stateChanged();
    if (stopping_)
        finishShutdown();
}

void AsyncDiagnosticLog::stopProcess()
{
    started_ = false;
    if (auto* process = process_.data()) {
        process_ = nullptr;
        process->disconnect(this);
        // Do not invoke QProcess's blocking destructor while a child is live.
        process->setParent(nullptr);
        if (process->state() == QProcess::NotRunning)
            process->deleteLater();
        else {
            connect(process, &QProcess::finished, process, &QObject::deleteLater);
            process->kill();
        }
    }
#ifdef Q_OS_WIN
    if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
    }
#endif
}
}
