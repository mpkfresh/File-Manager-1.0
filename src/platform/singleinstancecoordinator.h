#pragma once

#include <QByteArray>
#include <QObject>
#include <QHash>
#include <QLockFile>
#include <QStringList>

class QLocalServer;
class QLocalSocket;

namespace filemanager::platform {

class SingleInstanceCoordinator final : public QObject
{
    Q_OBJECT

public:
    enum class Role { Primary, Secondary, Error };
    explicit SingleInstanceCoordinator(
        QString serverName = QStringLiteral("Kiatami.FileManager.Instance.v1"),
        QObject* parent = nullptr);

    Role startOrForward(const QStringList& paths, QString* error = nullptr,
        bool foldersOnly = false);

signals:
    void pathsReceived(const QStringList& paths);
    void foldersReceived(const QStringList& folders);

private:
    bool forwardToPrimary(const QStringList& paths, bool foldersOnly);
    void acceptConnection();
    void readConnection(QLocalSocket* socket);

    QString serverName_;
    QLockFile instanceLock_;
    QLocalServer* server_ = nullptr;
    QHash<QLocalSocket*, QByteArray> pendingPayloads_;
};

} // namespace filemanager::platform
