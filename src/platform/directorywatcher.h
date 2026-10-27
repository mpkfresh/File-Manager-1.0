#pragma once

#include <QObject>
#include <QSet>
#include <QStringList>

namespace filemanager::platform {
class DirectoryWatchService;

// Shared recursive root watches; only displayed-directory refresh ids leave the helper.
class DirectoryWatcher : public QObject
{
    Q_OBJECT
public:
    explicit DirectoryWatcher(QObject* parent = nullptr);
    ~DirectoryWatcher() override;
    void setRoot(const QString& path);
    void addPath(const QString& path);
    void removePaths(const QStringList& paths);
    QStringList directories() const;
    bool isWatching(const QString& path) const;

signals:
    void directoryChanged(const QString& path);
    // Native notification detail was lost; reconcile this displayed listing.
    void resynchronizationRequired(const QString& path);
    void monitoringReady(const QString& path);
    void monitoringFailed(const QString& path);

private:
    friend class DirectoryWatchService;
    DirectoryWatchService* service_;
    QSet<QString> paths_;
    QString root_;
};

// Entry point used only by FileManagerShellHost --watch-directories.
int runDirectoryWatchHost();
}
