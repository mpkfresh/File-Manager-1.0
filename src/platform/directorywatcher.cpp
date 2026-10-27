#include "directorywatcher.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <algorithm>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace filemanager::platform {
namespace {
constexpr qsizetype MaximumPendingBytes = 1024 * 1024;
QString keyFor(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}
}

class DirectoryWatchService : public QObject
{
public:
    struct Entry {
        QString path;
        QString id;
        QSet<DirectoryWatcher*> owners;
        bool active = false;
        bool failed = false;
    };
    bool isRoot(const Entry& entry) const
    {
        for (auto* owner : entry.owners)
            if (owner->root_.isEmpty() || keyFor(owner->root_) == keyFor(entry.path))
                return true;
        return false;
    }
    explicit DirectoryWatchService(QObject* parent) : QObject(parent)
    {
#ifdef Q_OS_WIN
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job_ && !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(job_);
            job_ = nullptr;
        }
#endif
        timeout_.setInterval(1000);
        connect(&timeout_, &QTimer::timeout, this, [this] {
            if (process_ && lastReply_.elapsed() > 8000)
                fail();
        });
    }
    ~DirectoryWatchService() override
    {
        stop();
#ifdef Q_OS_WIN
        if (job_)
            CloseHandle(job_);
#endif
    }

    void add(DirectoryWatcher* owner, const QString& path)
    {
        const QString key = keyFor(path);
        auto found = entries_.find(key);
        if (found != entries_.end()) {
            const bool previousRoot = isRoot(*found);
            found->owners.insert(owner);
            // A manual refresh/add is the retry boundary after a helper failure.
            if (!process_)
                start();
            else if (found->failed && process_->state() == QProcess::Running) {
                QList<Entry> retry;
                for (auto& entry : entries_) {
                    if (entry.failed) {
                        entry.failed = false;
                        entry.id = QString::number(++serial_);
                        retry.append(entry);
                    }
                }
                std::stable_sort(retry.begin(), retry.end(), [this](const Entry& a, const Entry& b) {
                    return isRoot(a) && !isRoot(b);
                });
                for (const auto& entry : retry)
                    sendAdd(entry);
            }
            else if (previousRoot != isRoot(*found) && process_->state() == QProcess::Running)
                send({{"op", "root"}, {"id", found->id}, {"root", isRoot(*found)}});
            return;
        }
        if (entries_.size() >= 512 || !QDir::isAbsolutePath(path)
                || path.size() > 8192 || path.contains(QChar::Null)) {
            QTimer::singleShot(0, owner, [owner, path] { emit owner->monitoringFailed(path); });
            return;
        }
        Entry entry{QDir::cleanPath(QDir::fromNativeSeparators(path)),
            QString::number(++serial_), {owner}, false};
        entries_.insert(key, entry);
        if (!process_)
            start();
        else if (process_->state() == QProcess::Running)
            sendAdd(entry);
    }

    void remove(DirectoryWatcher* owner, const QString& key)
    {
        auto found = entries_.find(key);
        if (found == entries_.end())
            return;
        const bool previousRoot = isRoot(*found);
        found->owners.remove(owner);
        if (!found->owners.isEmpty()) {
            if (process_ && process_->state() == QProcess::Running && previousRoot != isRoot(*found))
                send({{"op", "root"}, {"id", found->id}, {"root", isRoot(*found)}});
            return;
        }
        const QString id = found->id;
        entries_.erase(found);
        if (process_ && process_->state() == QProcess::Running) {
            send({{"op", "remove"}, {"id", id}});
        }
        if (entries_.isEmpty()) {
            // Navigation commonly replaces the last subscription in this event
            // turn. Keep the same helper for that remove/add sequence.
            QTimer::singleShot(0, this, [this] {
                if (entries_.isEmpty())
                    stop(false);
            });
        }
    }

    bool active(const QString& key) const
    {
        const auto found = entries_.constFind(key);
        return found != entries_.cend() && found->active;
    }
    QString pathFor(const QString& key) const
    {
        const auto found = entries_.constFind(key);
        return found == entries_.cend() ? key : found->path;
    }

private:
    void start()
    {
        if (entries_.isEmpty() || process_)
            return;
        auto* process = new QProcess(this);
        process_ = process;
        input_.clear();
        for (auto& entry : entries_) {
            entry.active = false;
            entry.failed = false;
            // New ids also invalidate replies from an earlier helper lifetime.
            entry.id = QString::number(++serial_);
        }
        process->setProgram(QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("FileManagerShellHost.exe")));
        process->setArguments({QStringLiteral("--watch-directories")});
        process->setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
            args->flags |= CREATE_NO_WINDOW;
        });
#endif
        connect(process, &QProcess::started, this, [this, process] {
            if (process != process_)
                return;
#ifdef Q_OS_WIN
            HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                static_cast<DWORD>(process->processId()));
            const bool contained = child && job_ && AssignProcessToJobObject(job_, child);
            if (child)
                CloseHandle(child);
            if (!contained) {
                fail();
                return;
            }
#endif
            auto snapshot = entries_.values();
            std::stable_sort(snapshot.begin(), snapshot.end(), [this](const Entry& a, const Entry& b) {
                return isRoot(a) && !isRoot(b);
            });
            for (const auto& entry : snapshot)
                sendAdd(entry);
        });
        connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
            if (process != process_)
                return;
            input_ += process->read(MaximumPendingBytes + 1);
            if (input_.size() > MaximumPendingBytes) {
                fail();
                return;
            }
            qsizetype newline;
            while ((newline = input_.indexOf('\n')) >= 0) {
                const QByteArray line = input_.first(newline);
                input_.remove(0, newline + 1);
                const QList<QByteArray> words = line.split(' ');
                if (line == "heartbeat") {
                    lastReply_.restart();
                    continue;
                }
                if (words.size() != 2 || (words[0] != "ready" && words[0] != "changed"
                        && words[0] != "failed" && words[0] != "retired" && words[0] != "resync")) {
                    fail();
                    return;
                }
                lastReply_.restart();
                const QString id = QString::fromLatin1(words[1]);
                for (auto& entry : entries_) {
                    if (entry.id != id)
                        continue;
                    if (words[0] == "ready") {
                        entry.active = true;
                        entry.failed = false;
                    }
                    if (words[0] == "failed" || words[0] == "retired") {
                        entry.active = false;
                        entry.failed = true;
                    }
                    const QString path = entry.path;
                    const auto owners = entry.owners.values();
                    // Defer delivery: model changes must not invalidate this table iterator.
                    for (auto* owner : owners) {
                        const QString key = keyFor(path);
                        const bool failed = words[0] == "failed";
                        const bool ready = words[0] == "ready";
                        const bool resync = words[0] == "resync";
                        QTimer::singleShot(0, owner, [this, owner, key, id, path, failed, ready, resync] {
                            const auto current = entries_.constFind(key);
                            if (current == entries_.cend() || current->id != id
                                    || !current->owners.contains(owner))
                                return;
                            if (failed)
                                emit owner->monitoringFailed(path);
                            else {
                                const QPointer<DirectoryWatcher> liveOwner(owner);
                                if (ready)
                                    emit owner->monitoringReady(path);
                                if (liveOwner && resync)
                                    emit liveOwner->resynchronizationRequired(path);
                                if (liveOwner)
                                    emit liveOwner->directoryChanged(path);
                            }
                        });
                    }
                    break;
                }
            }
        });
        connect(process, &QProcess::errorOccurred, this, [this, process] {
            if (process == process_)
                fail();
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process] { if (process == process_) fail(); });
        lastReply_.start();
        timeout_.start();
        process->start();
    }

    void sendAdd(const Entry& entry)
    {
        send({{"op", "add"}, {"id", entry.id}, {"path", entry.path}, {"root", isRoot(entry)}});
    }
    void send(const QJsonObject& message)
    {
        if (!process_)
            return;
        const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        if (process_->bytesToWrite() + bytes.size() > MaximumPendingBytes
                || process_->write(bytes) != bytes.size())
            fail();
    }
    void stop(bool abort = true)
    {
        timeout_.stop();
        auto* process = std::exchange(process_, nullptr);
        input_.clear();
        if (!process)
            return;
        process->disconnect(this);
        if (process->state() == QProcess::NotRunning) {
            process->deleteLater();
        } else {
            // The service may be destroyed before EOF/kill is acknowledged.
            // Keep QProcess alive until finished instead of invoking its blocking
            // destructor through QObject's child cleanup. The native job still
            // owns the helper and kills it when the service is destroyed.
            process->setParent(nullptr);
            // EOF lets the helper cancel its native directory requests.
            // Kill only failed/stuck helpers; never wait on the GUI thread.
            connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                process, &QObject::deleteLater);
            if (abort) {
                process->kill();
            } else {
                process->closeWriteChannel();
                QTimer::singleShot(1500, process, [process] {
                    if (process->state() != QProcess::NotRunning)
                        process->kill();
                });
            }
        }
    }
    void fail()
    {
        stop();
        for (auto& entry : entries_) {
            entry.active = false;
            entry.id = QString::number(++serial_);
            const QString path = entry.path;
            const QString id = entry.id;
            for (auto* owner : entry.owners)
                QTimer::singleShot(0, owner, [this, owner, path, id] {
                    const auto current = entries_.constFind(keyFor(path));
                    if (current != entries_.cend() && current->id == id
                            && current->owners.contains(owner))
                        emit owner->monitoringFailed(path);
                });
        }
    }
    QHash<QString, Entry> entries_;
    QProcess* process_ = nullptr;
    QTimer timeout_;
    QElapsedTimer lastReply_;
    QByteArray input_;
    quint64 serial_ = 0;
#ifdef Q_OS_WIN
    HANDLE job_ = nullptr;
#endif
};

DirectoryWatcher::DirectoryWatcher(QObject* parent) : QObject(parent)
{
    static QPointer<DirectoryWatchService> shared;
    if (!shared)
        shared = new DirectoryWatchService(QCoreApplication::instance());
    service_ = shared;
}
DirectoryWatcher::~DirectoryWatcher()
{
    removePaths(directories());
}
void DirectoryWatcher::addPath(const QString& path)
{
    const QString key = keyFor(path);
    const QString rootKey = keyFor(root_);
    if (!root_.isEmpty() && key != rootKey
            && !key.startsWith(rootKey.endsWith('/') ? rootKey : rootKey + '/')) {
        emit monitoringFailed(path);
        return;
    }
    paths_.insert(key);
    service_->add(this, path);
}
void DirectoryWatcher::setRoot(const QString& path)
{
    removePaths(directories());
    root_ = path;
    addPath(path);
}
void DirectoryWatcher::removePaths(const QStringList& paths)
{
    for (const auto& path : paths) {
        const QString key = keyFor(path);
        paths_.remove(key);
        service_->remove(this, key);
    }
}
QStringList DirectoryWatcher::directories() const
{
    QStringList result;
    for (const auto& key : paths_)
        result.append(service_->pathFor(key));
    return result;
}
bool DirectoryWatcher::isWatching(const QString& path) const
{
    const QString key = keyFor(path);
    return paths_.contains(key) && service_->active(key);
}
}
