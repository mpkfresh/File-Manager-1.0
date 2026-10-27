#include "platform/singleinstancecoordinator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QtEndian>

#include <algorithm>

namespace filemanager::platform {
namespace {

constexpr qsizetype HeaderBytes = sizeof(quint32);
constexpr qsizetype MaximumPayloadBytes = 256 * 1024;
constexpr qsizetype MaximumForwardedPaths = 512;
constexpr char Acknowledged = '\x06';

QByteArray encodePaths(const QStringList& paths, bool foldersOnly)
{
    QJsonArray array;
    for (const QString& path : paths) {
        // Interpret relative launch arguments in the sending process. The primary
        // may have been started from a different directory. Do not canonicalize:
        // missing targets must survive forwarding for the UI to report them.
        const QFileInfo info(path);
        array.append(!path.isEmpty() && info.isRelative() ? info.absoluteFilePath() : path);
    }
    if (foldersOnly) {
        return QJsonDocument(QJsonObject{{"version", 1}, {"kind", "folders"}, {"paths", array}})
            .toJson(QJsonDocument::Compact);
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

bool decodePaths(const QByteArray& encoded, QStringList* paths, bool* foldersOnly)
{
    const QJsonDocument document = QJsonDocument::fromJson(encoded);
    QJsonArray array;
    *foldersOnly = false;
    if (document.isArray()) {
        array = document.array();
    } else if (document.isObject()) {
        const QJsonObject object = document.object();
        if (object.size() != 3 || object.value("version") != QJsonValue(1)
            || object.value("kind") != QJsonValue("folders") || !object.value("paths").isArray())
            return false;
        *foldersOnly = true;
        array = object.value("paths").toArray();
        if (array.isEmpty()) return false;
    } else {
        return false;
    }
    if (array.size() > MaximumForwardedPaths)
        return false;
    QStringList decoded;
    for (const QJsonValue& value : array) {
        if (!value.isString())
            return false;
        decoded.append(value.toString());
    }
    *paths = decoded;
    return true;
}

QString lockPath(const QString& serverName)
{
    const QByteArray digest = QCryptographicHash::hash(
        serverName.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath("filemanager-" + QString::fromLatin1(digest) + ".lock");
}

} // namespace

SingleInstanceCoordinator::SingleInstanceCoordinator(QString serverName, QObject* parent)
    : QObject(parent), serverName_(std::move(serverName)), instanceLock_(lockPath(serverName_)),
      server_(new QLocalServer(this))
{
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    connect(server_, &QLocalServer::newConnection,
        this, &SingleInstanceCoordinator::acceptConnection);
}

SingleInstanceCoordinator::Role SingleInstanceCoordinator::startOrForward(
    const QStringList& paths, QString* error, bool foldersOnly)
{
    if (instanceLock_.tryLock(0)) {
        QLocalServer::removeServer(serverName_);
        if (server_->listen(serverName_))
            return Role::Primary;
        instanceLock_.unlock();
        if (error)
            *error = server_->errorString();
        return Role::Error;
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (forwardToPrimary(paths, foldersOnly))
            return Role::Secondary;
        QThread::msleep(50);
    }
    if (error)
        *error = QStringLiteral("The primary instance is locked but its IPC endpoint is unavailable.");
    return Role::Error;
}

bool SingleInstanceCoordinator::forwardToPrimary(const QStringList& paths, bool foldersOnly)
{
    const QByteArray payload = encodePaths(paths, foldersOnly);
    if (payload.size() > MaximumPayloadBytes)
        return false;

    QByteArray frame(HeaderBytes + payload.size(), Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()),
        reinterpret_cast<uchar*>(frame.data()));
    std::copy(payload.cbegin(), payload.cend(), frame.begin() + HeaderBytes);

    QLocalSocket socket;
    socket.connectToServer(serverName_, QIODevice::ReadWrite);
    if (!socket.waitForConnected(750))
        return false;
    if (socket.write(frame) != frame.size())
        return false;
    socket.flush();
    const bool written = socket.waitForBytesWritten(750);
    if (!written && socket.bytesToWrite() != 0)
        return false;
    if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(1500))
        return false;
    if (socket.read(1) != QByteArray(1, Acknowledged))
        return false;
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        socket.waitForDisconnected(750);
    return true;
}

void SingleInstanceCoordinator::acceptConnection()
{
    while (QLocalSocket* socket = server_->nextPendingConnection()) {
        socket->setReadBufferSize(MaximumPayloadBytes + HeaderBytes + 1);
        pendingPayloads_.insert(socket, {});
        connect(socket, &QLocalSocket::readyRead, this,
            [this, socket] { readConnection(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            pendingPayloads_.remove(socket);
            socket->deleteLater();
        });
        readConnection(socket);
    }
}

void SingleInstanceCoordinator::readConnection(QLocalSocket* socket)
{
    auto found = pendingPayloads_.find(socket);
    if (found == pendingPayloads_.end())
        return;
    found.value().append(socket->readAll());
    const QByteArray& frame = found.value();
    if (frame.size() < HeaderBytes)
        return;

    const quint32 payloadSize = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(frame.constData()));
    if (payloadSize > MaximumPayloadBytes) {
        pendingPayloads_.remove(socket);
        socket->abort();
        return;
    }
    if (frame.size() < HeaderBytes + payloadSize)
        return;

    QStringList paths;
    bool foldersOnly = false;
    if (frame.size() != HeaderBytes + payloadSize
            || !decodePaths(frame.sliced(HeaderBytes, payloadSize), &paths, &foldersOnly)) {
        pendingPayloads_.remove(socket);
        socket->abort();
        return;
    }

    pendingPayloads_.remove(socket);
    socket->write(QByteArray(1, Acknowledged));
    socket->flush();
    // On Windows a server-side disconnect discards unread pipe data, even after
    // the write completed. Let the client consume the ACK and close its end.
    // A stalled client must not retain this completed connection indefinitely.
    QTimer::singleShot(2000, socket, [socket] { socket->abort(); });
    if (foldersOnly)
        emit foldersReceived(paths);
    else
        emit pathsReceived(paths);
}

} // namespace filemanager::platform
