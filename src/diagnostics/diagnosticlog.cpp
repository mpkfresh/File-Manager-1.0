#include "diagnostics/diagnosticlog.h"

#include "diagnostics/diagnosticbundle.h"

#include <algorithm>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace filemanager::diagnostics {
namespace {

QString defaultStoragePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("diagnostics/events.json"));
}

} // namespace

DiagnosticLog::DiagnosticLog(QString storagePath)
    : storagePath_(storagePath.isEmpty() ? defaultStoragePath() : std::move(storagePath))
{
    load();
    const int previousCount = events_.size();
    prune(QDateTime::currentDateTimeUtc());
    if (events_.size() != previousCount)
        save(&initializationError_);
}

bool DiagnosticLog::record(const QString& eventCode, QString* error)
{
    if (!isSafeDiagnosticEventCode(eventCode)) {
        if (error)
            *error = QStringLiteral("The diagnostic event code is not path-safe.");
        return false;
    }
    const QVector<Event> previousEvents = events_;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    prune(now);
    events_.append({now, eventCode});
    while (events_.size() > maximumEventCount)
        events_.removeFirst();
    if (save(error))
        return true;
    events_ = previousEvents;
    return false;
}

bool DiagnosticLog::clear(QString* error)
{
    if (QFileInfo::exists(storagePath_) && !QFile::remove(storagePath_)) {
        if (error)
            *error = QStringLiteral("The local diagnostic history could not be removed.");
        return false;
    }
    events_.clear();
    return true;
}

QStringList DiagnosticLog::eventCodes() const
{
    QStringList result;
    result.reserve(events_.size());
    for (const Event& event : events_)
        result.append(event.code);
    return result;
}

int DiagnosticLog::eventCount() const
{
    return events_.size();
}

QString DiagnosticLog::storagePath() const
{
    return storagePath_;
}

void DiagnosticLog::load()
{
    QFile file(storagePath_);
    if (!file.open(QIODevice::ReadOnly)) {
        if (QFileInfo::exists(storagePath_))
            initializationError_ = QStringLiteral("The local diagnostic history could not be read.");
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        initializationError_ = QStringLiteral("The local diagnostic history could not be decoded.");
        return;
    }
    const QJsonArray serializedEvents = document.object().value(QStringLiteral("events")).toArray();
    for (const QJsonValue& value : serializedEvents) {
        const QJsonObject object = value.toObject();
        const QString code = object.value(QStringLiteral("code")).toString();
        const QDateTime timestamp = QDateTime::fromString(
            object.value(QStringLiteral("timestampUtc")).toString(), Qt::ISODateWithMs);
        if (timestamp.isValid() && isSafeDiagnosticEventCode(code))
            events_.append({timestamp.toUTC(), code});
    }
}

void DiagnosticLog::prune(const QDateTime& nowUtc)
{
    const QDateTime cutoff = nowUtc.addDays(-retentionDays);
    events_.erase(std::remove_if(events_.begin(), events_.end(),
                      [&cutoff](const Event& event) { return event.timestampUtc < cutoff; }),
        events_.end());
    while (events_.size() > maximumEventCount)
        events_.removeFirst();
}

bool DiagnosticLog::save(QString* error)
{
    const QFileInfo info(storagePath_);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error)
            *error = QStringLiteral("The diagnostic history folder could not be created.");
        return false;
    }
    QJsonArray serializedEvents;
    for (const Event& event : std::as_const(events_)) {
        serializedEvents.append(QJsonObject{
            {QStringLiteral("timestampUtc"), event.timestampUtc.toString(Qt::ISODateWithMs)},
            {QStringLiteral("code"), event.code}});
    }
    const QByteArray content = QJsonDocument(QJsonObject{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("events"), serializedEvents}}).toJson(QJsonDocument::Compact);
    QSaveFile file(storagePath_);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(content) != content.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace filemanager::diagnostics
