#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

namespace filemanager::files {

struct NetworkResource
{
    QString remotePath;
    QString displayName;
    QString provider;
    bool container = false;
};

class NetworkDiscoveryService final : public QObject
{
    Q_OBJECT

public:
    static constexpr int MaximumResources = 256;
    static constexpr int MaximumDepth = 3;
    explicit NetworkDiscoveryService(QObject* parent = nullptr);
    ~NetworkDiscoveryService() override;

    void start();
    void cancel();
    bool isRunning() const { return running_; }

signals:
    void finished(const QVector<filemanager::files::NetworkResource>& resources,
        const QString& error);

private:
    std::shared_ptr<std::atomic_bool> cancellation_;
    bool running_ = false;
};

} // namespace filemanager::files

Q_DECLARE_METATYPE(filemanager::files::NetworkResource)
