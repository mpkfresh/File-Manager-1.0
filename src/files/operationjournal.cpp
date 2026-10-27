#include "files/operationjournal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSet>

#include <algorithm>
#include <iterator>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

namespace filemanager::files {
namespace {

QString defaultStoragePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("operation-journal.dat"));
}

QString operationText(TransferOperation operation)
{
    return operation == TransferOperation::Move ? QStringLiteral("move") : QStringLiteral("copy");
}

TransferOperation operationFromText(const QString& value)
{
    return value == QStringLiteral("move") ? TransferOperation::Move : TransferOperation::Copy;
}

QString stateText(JournalState state)
{
    switch (state) {
    case JournalState::Queued: return QStringLiteral("queued");
    case JournalState::Running: return QStringLiteral("running");
    case JournalState::Paused: return QStringLiteral("paused");
    case JournalState::Succeeded: return QStringLiteral("succeeded");
    case JournalState::Failed: return QStringLiteral("failed");
    case JournalState::Cancelled: return QStringLiteral("cancelled");
    case JournalState::Interrupted: return QStringLiteral("interrupted");
    }
    return QStringLiteral("failed");
}

bool isKnownState(const QString& value)
{
    return value == "queued" || value == "running" || value == "paused"
        || value == "succeeded" || value == "failed" || value == "cancelled"
        || value == "interrupted";
}

JournalState stateFromText(const QString& value)
{
    if (value == QStringLiteral("queued")) return JournalState::Queued;
    if (value == QStringLiteral("running")) return JournalState::Running;
    if (value == QStringLiteral("paused")) return JournalState::Paused;
    if (value == QStringLiteral("succeeded")) return JournalState::Succeeded;
    if (value == QStringLiteral("cancelled")) return JournalState::Cancelled;
    if (value == QStringLiteral("interrupted")) return JournalState::Interrupted;
    return JournalState::Failed;
}

JournalState journalState(TransferJobState state)
{
    switch (state) {
    case TransferJobState::Queued: return JournalState::Queued;
    case TransferJobState::Running: return JournalState::Running;
    case TransferJobState::Paused: return JournalState::Paused;
    case TransferJobState::Succeeded: return JournalState::Succeeded;
    case TransferJobState::Failed: return JournalState::Failed;
    case TransferJobState::Cancelled: return JournalState::Cancelled;
    }
    return JournalState::Failed;
}

QByteArray protect(const QByteArray& plain, QString* error)
{
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(plain.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
    const QByteArray entropyBytes("FileManager.OperationJournal.v1");
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(entropyBytes.constData()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"File Manager operation journal", &entropy, nullptr,
            nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        if (error)
            *error = QStringLiteral("Windows could not encrypt the operation journal (%1).")
                .arg(GetLastError());
        return {};
    }
    const QByteArray encrypted(reinterpret_cast<const char*>(output.pbData),
        static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return encrypted;
#else
    if (error)
        *error = QStringLiteral("Per-user operation-journal encryption is unavailable.");
    Q_UNUSED(plain);
    return {};
#endif
}

QByteArray unprotect(const QByteArray& encrypted, QString* error)
{
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.constData()))};
    const QByteArray entropyBytes("FileManager.OperationJournal.v1");
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(entropyBytes.constData()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        if (error)
            *error = QStringLiteral("Windows could not decrypt the operation journal (%1).")
                .arg(GetLastError());
        return {};
    }
    const QByteArray plain(reinterpret_cast<const char*>(output.pbData),
        static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return plain;
#else
    if (error)
        *error = QStringLiteral("Per-user operation-journal encryption is unavailable.");
    Q_UNUSED(encrypted);
    return {};
#endif
}

QJsonObject toJson(const JournalRecord& record)
{
    QJsonArray sources;
    for (const QString& source : record.request.sources)
        sources.append(source);
    QJsonArray errors;
    for (const QString& error : record.errors)
        errors.append(error);
    QJsonArray manifest;
    for (const TransferManifestEntry& entry : record.manifest) {
        manifest.append(QJsonObject{{"source", entry.sourcePath},
            {"destination", entry.destinationPath}, {"directory", entry.directory},
            {"destinationCreated", entry.destinationCreated},
            {"sourceRemoved", entry.sourceRemoved}, {"size", entry.size},
            {"lastModifiedUtcMs", entry.lastModifiedUtcMs},
            {"volumeSerial", static_cast<qint64>(entry.volumeSerial)},
            {"fileIndex", QString::number(entry.fileIndex)},
            {"identityAvailable", entry.identityAvailable}, {"sourceSize", entry.sourceSize},
            {"sourceLastModifiedUtcMs", entry.sourceLastModifiedUtcMs},
            {"sourceVolumeSerial", static_cast<qint64>(entry.sourceVolumeSerial)},
            {"sourceFileIndex", QString::number(entry.sourceFileIndex)},
            {"sourceIdentityAvailable", entry.sourceIdentityAvailable}});
    }
    return {{"id", record.id}, {"operation", operationText(record.request.operation)},
        {"reviewedMove", record.request.hasReviewConstraints()},
        {"sources", sources}, {"destination", record.request.destinationDirectory},
        {"state", stateText(record.state)}, {"queuedAt", record.queuedAt.toString(Qt::ISODateWithMs)},
        {"startedAt", record.startedAt.toString(Qt::ISODateWithMs)},
        {"finishedAt", record.finishedAt.toString(Qt::ISODateWithMs)},
        {"completedBytes", record.completedBytes}, {"totalBytes", record.totalBytes},
        {"currentPath", record.currentPath}, {"errors", errors}, {"manifest", manifest}};
}

JournalRecord fromJson(const QJsonObject& object)
{
    JournalRecord record;
    record.id = object.value("id").toString();
    record.request.operation = operationFromText(object.value("operation").toString());
    for (const QJsonValue& source : object.value("sources").toArray())
        record.request.sources.append(source.toString());
    record.request.destinationDirectory = object.value("destination").toString();
    record.request.reviewedMove = object.value("reviewedMove").toBool(false);
    record.state = stateFromText(object.value("state").toString());
    record.queuedAt = QDateTime::fromString(object.value("queuedAt").toString(), Qt::ISODateWithMs);
    record.startedAt = QDateTime::fromString(object.value("startedAt").toString(), Qt::ISODateWithMs);
    record.finishedAt = QDateTime::fromString(object.value("finishedAt").toString(), Qt::ISODateWithMs);
    record.completedBytes = object.value("completedBytes").toInteger();
    record.totalBytes = object.value("totalBytes").toInteger();
    record.currentPath = object.value("currentPath").toString();
    for (const QJsonValue& error : object.value("errors").toArray())
        record.errors.append(error.toString());
    for (const QJsonValue& value : object.value("manifest").toArray()) {
        const QJsonObject item = value.toObject();
        TransferManifestEntry entry;
        entry.sourcePath = item.value("source").toString();
        entry.destinationPath = item.value("destination").toString();
        entry.directory = item.value("directory").toBool();
        entry.destinationCreated = item.value("destinationCreated").toBool();
        entry.sourceRemoved = item.value("sourceRemoved").toBool();
        entry.size = item.value("size").toInteger();
        entry.lastModifiedUtcMs = item.value("lastModifiedUtcMs").toInteger();
        entry.volumeSerial = static_cast<quint64>(item.value("volumeSerial").toInteger());
        entry.fileIndex = item.value("fileIndex").toString().toULongLong();
        entry.identityAvailable = item.value("identityAvailable").toBool();
        entry.sourceSize = item.value("sourceSize").toInteger();
        entry.sourceLastModifiedUtcMs = item.value("sourceLastModifiedUtcMs").toInteger();
        entry.sourceVolumeSerial =
            static_cast<quint64>(item.value("sourceVolumeSerial").toInteger());
        entry.sourceFileIndex = item.value("sourceFileIndex").toString().toULongLong();
        entry.sourceIdentityAvailable = item.value("sourceIdentityAvailable").toBool();
        record.manifest.append(std::move(entry));
    }
    return record;
}

bool validTimestamp(const QJsonValue& value, bool required)
{
    if (!value.isString())
        return false;
    if (value.toString().isEmpty())
        return !required;
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs).isValid();
}

bool validAbsolutePath(const QJsonValue& value)
{
    const QString path = value.toString();
    return value.isString() && !path.isEmpty() && path.size() <= 32767
        && QDir::isAbsolutePath(path);
}

bool validManifestEntry(const QJsonValue& value)
{
    if (!value.isObject())
        return false;
    const QJsonObject item = value.toObject();
    const bool directory = item.value("directory").toBool(false);
    const bool sourceIdentity = item.value("sourceIdentityAvailable").toBool(true);
    const bool sourcePathValid = validAbsolutePath(item.value("source"))
        || (directory && !sourceIdentity && item.value("source").isString()
            && item.value("source").toString().isEmpty());
    return sourcePathValid
        && validAbsolutePath(item.value("destination"))
        && item.value("directory").isBool()
        && item.value("destinationCreated").isBool()
        && item.value("sourceRemoved").isBool()
        && item.value("size").isDouble() && item.value("size").toInteger(-1) >= 0
        && item.value("lastModifiedUtcMs").isDouble()
        && item.value("volumeSerial").isDouble()
        && item.value("fileIndex").isString()
        && item.value("fileIndex").toString().size() <= 20
        && item.value("identityAvailable").isBool()
        && item.value("sourceSize").isDouble()
        && item.value("sourceSize").toInteger(-1) >= 0
        && item.value("sourceLastModifiedUtcMs").isDouble()
        && item.value("sourceVolumeSerial").isDouble()
        && item.value("sourceFileIndex").isString()
        && item.value("sourceFileIndex").toString().size() <= 20
        && item.value("sourceIdentityAvailable").isBool();
}

bool validRecord(const QJsonValue& value, int schema)
{
    if (!value.isObject())
        return false;
    const QJsonObject object = value.toObject();
    const QString id = object.value("id").toString();
    const QString operation = object.value("operation").toString();
    const QString state = object.value("state").toString();
    const QJsonArray sources = object.value("sources").toArray();
    const QJsonArray errors = object.value("errors").toArray();
    const QJsonArray manifest = object.value("manifest").toArray();
    if ((schema >= 3 && !object.value("reviewedMove").isBool())
            || (schema < 3 && object.contains("reviewedMove")))
        return false;
    if (!object.value("id").isString() || id.isEmpty() || id.size() > 128
        || (operation != "copy" && operation != "move")
        || !object.value("sources").isArray() || sources.isEmpty() || sources.size() > 4096
        || !validAbsolutePath(object.value("destination"))
        || !object.value("state").isString() || !isKnownState(state)
        || !validTimestamp(object.value("queuedAt"), true)
        || !validTimestamp(object.value("startedAt"), false)
        || !validTimestamp(object.value("finishedAt"), false)
        || !object.value("completedBytes").isDouble()
        || !object.value("totalBytes").isDouble()
        || object.value("completedBytes").toInteger(-1) < 0
        || object.value("totalBytes").toInteger(-1) < 0
        || object.value("completedBytes").toInteger()
            > object.value("totalBytes").toInteger()
        || !object.value("currentPath").isString()
        || object.value("currentPath").toString().size() > 32767
        || (!object.value("currentPath").toString().isEmpty()
            && !QDir::isAbsolutePath(object.value("currentPath").toString()))
        || !object.value("errors").isArray() || errors.size() > 256
        || (schema >= 2 && !object.value("manifest").isArray())
        || manifest.size() > 10000) {
        return false;
    }
    for (const QJsonValue& source : sources) {
        if (!validAbsolutePath(source))
            return false;
    }
    for (const QJsonValue& itemError : errors) {
        if (!itemError.isString() || itemError.toString().size() > 4096)
            return false;
    }
    for (const QJsonValue& item : manifest) {
        if (!validManifestEntry(item))
            return false;
    }
    return true;
}

TransferManifestEntry currentManifestEntry(const QString& path)
{
    const QFileInfo info(path);
    TransferManifestEntry entry;
    entry.destinationPath = info.absoluteFilePath();
    entry.directory = info.isDir();
    entry.size = info.isFile() ? info.size() : 0;
    entry.lastModifiedUtcMs = info.lastModified().toUTC().toMSecsSinceEpoch();
#ifdef Q_OS_WIN
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(entry.destinationPath.utf16()),
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION identity{};
    if (handle != INVALID_HANDLE_VALUE) {
        if (GetFileInformationByHandle(handle, &identity)) {
            entry.volumeSerial = identity.dwVolumeSerialNumber;
            entry.fileIndex = (static_cast<quint64>(identity.nFileIndexHigh) << 32)
                | identity.nFileIndexLow;
            entry.identityAvailable = true;
        }
        CloseHandle(handle);
    }
#endif
    return entry;
}

bool matchesCommittedItem(const TransferManifestEntry& recorded)
{
    if (!recorded.destinationCreated || !recorded.identityAvailable
            || !QFileInfo::exists(recorded.destinationPath))
        return false;
    const TransferManifestEntry current = currentManifestEntry(recorded.destinationPath);
    if (!current.identityAvailable || current.directory != recorded.directory
            || current.volumeSerial != recorded.volumeSerial
            || current.fileIndex != recorded.fileIndex)
        return false;
    return recorded.directory || (current.size == recorded.size
        && current.lastModifiedUtcMs == recorded.lastModifiedUtcMs);
}

bool matchesOriginalSource(const TransferManifestEntry& recorded)
{
    if (recorded.sourceRemoved || !recorded.sourceIdentityAvailable
            || !QFileInfo::exists(recorded.sourcePath))
        return false;
    const TransferManifestEntry current = currentManifestEntry(recorded.sourcePath);
    if (!current.identityAvailable || current.directory != recorded.directory
            || current.volumeSerial != recorded.sourceVolumeSerial
            || current.fileIndex != recorded.sourceFileIndex)
        return false;
    return recorded.directory || (current.size == recorded.sourceSize
        && current.lastModifiedUtcMs == recorded.sourceLastModifiedUtcMs);
}

} // namespace

QByteArray serializeOperationJournal(const QList<JournalRecord>& journalRecords, int schema)
{
    for (const auto& record : journalRecords) {
        if (record.request.hasReviewConstraints())
            schema = qMax(schema, 3); // Never encode guarded requests as replayable legacy records.
    }
    QJsonArray records;
    for (const JournalRecord& record : journalRecords) {
        QJsonObject object = toJson(record);
        if (schema == 1)
            object.remove("manifest");
        if (schema < 3)
            object.remove("reviewedMove");
        records.append(object);
    }
    return QJsonDocument(QJsonObject{{"schema", schema}, {"records", records}})
        .toJson(QJsonDocument::Compact);
}

bool parseOperationJournal(const QByteArray& plain, QList<JournalRecord>* records,
    int* schema, QString* error)
{
    constexpr qsizetype maximumPlainBytes = 4 * 1024 * 1024;
    if (!records) {
        if (error)
            *error = QStringLiteral("No operation-journal destination was provided.");
        return false;
    }
    if (plain.isEmpty() || plain.size() > maximumPlainBytes) {
        if (error)
            *error = QStringLiteral("The operation journal exceeds its safety limit.");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(plain, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("The operation journal is malformed.");
        return false;
    }
    const QJsonObject root = document.object();
    const int decodedSchema = root.value("schema").toInt(-1);
    const QJsonArray values = root.value("records").toArray();
    if ((decodedSchema != 1 && decodedSchema != 2 && decodedSchema != 3)
        || !root.value("records").isArray() || values.size() > 1000) {
        if (error)
            *error = QStringLiteral("The operation journal has an unsupported or invalid schema.");
        return false;
    }
    QList<JournalRecord> decoded;
    QSet<QString> ids;
    for (const QJsonValue& value : values) {
        if (!validRecord(value, decodedSchema)) {
            if (error)
                *error = QStringLiteral("The operation journal contains an invalid record.");
            return false;
        }
        JournalRecord record = fromJson(value.toObject());
        if (ids.contains(record.id)) {
            if (error)
                *error = QStringLiteral("The operation journal contains duplicate record IDs.");
            return false;
        }
        ids.insert(record.id);
        decoded.append(std::move(record));
    }
    *records = std::move(decoded);
    if (schema)
        *schema = decodedSchema;
    return true;
}

OperationJournal::OperationJournal(QString storagePath, QObject* parent)
    : QObject(parent), storagePath_(storagePath.isEmpty() ? defaultStoragePath() : std::move(storagePath))
{
    flushTimer_.setSingleShot(true);
    flushTimer_.setInterval(350);
    connect(&flushTimer_, &QTimer::timeout, this, [this] { flush(); });
}

OperationJournal::~OperationJournal()
{
    if (flushTimer_.isActive())
        flush();
}

bool OperationJournal::load(QString* error)
{
    records_.clear();
    QFile file(storagePath_);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    constexpr qint64 maximumEncryptedBytes = 8 * 1024 * 1024;
    if (file.size() <= 0 || file.size() > maximumEncryptedBytes)
        return quarantineCorruptStorage(
            QStringLiteral("The encrypted operation journal exceeds its safety limit."), error);
    QString decodeError;
    const QByteArray plain = unprotect(file.readAll(), &decodeError);
    file.close();
    if (plain.isEmpty())
        return quarantineCorruptStorage(decodeError.isEmpty()
                ? QStringLiteral("The operation journal could not be decrypted.") : decodeError,
            error);
    int schema = 0;
    if (!parseOperationJournal(plain, &records_, &schema, &decodeError))
        return quarantineCorruptStorage(decodeError, error);
    bool interrupted = false;
    for (JournalRecord& record : records_) {
        if (record.state == JournalState::Queued || record.state == JournalState::Running
                || record.state == JournalState::Paused) {
            record.state = JournalState::Interrupted;
            record.finishedAt = QDateTime::currentDateTimeUtc();
            interrupted = true;
        }
    }
    const qsizetype countBeforePrune = records_.size();
    prune();
    if (schema == 1 || interrupted || records_.size() != countBeforePrune)
        scheduleFlush();
    emit changed();
    return true;
}

void OperationJournal::synchronize(const QList<TransferRecord>& transfers)
{
    for (const TransferRecord& transfer : transfers) {
        auto found = std::find_if(records_.begin(), records_.end(), [&](const JournalRecord& record) {
            return record.id == transfer.durableId;
        });
        if (found == records_.end()) {
            records_.append({transfer.durableId, transfer.request});
            found = std::prev(records_.end());
        }
        found->request = transfer.request;
        found->state = journalState(transfer.state);
        found->queuedAt = transfer.queuedAt;
        found->startedAt = transfer.startedAt;
        found->finishedAt = transfer.finishedAt;
        found->completedBytes = transfer.completedBytes;
        found->totalBytes = transfer.totalBytes;
        found->currentPath = transfer.currentPath;
        found->errors = transfer.errors;
        found->manifest = transfer.manifest;
    }
    prune();
    scheduleFlush();
    emit changed();
}

bool OperationJournal::flush(QString* error)
{
    flushTimer_.stop();
    const QByteArray plain = serializeOperationJournal(records_);
    const QByteArray encrypted = protect(plain, error);
    if (encrypted.isEmpty())
        return false;
    if (!QDir().mkpath(QFileInfo(storagePath_).absolutePath())) {
        if (error)
            *error = QStringLiteral("Could not create the operation-journal folder.");
        return false;
    }
    QSaveFile file(storagePath_);
    if (!file.open(QIODevice::WriteOnly) || file.write(encrypted) != encrypted.size()
            || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool OperationJournal::quarantineCorruptStorage(const QString& reason, QString* error)
{
    flushTimer_.stop();
    records_.clear();
    const QString suffix = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmsszzz");
    const QString quarantinePath = storagePath_ + ".corrupt-" + suffix;
    const bool moved = QFile::rename(storagePath_, quarantinePath);
    if (error) {
        *error = moved
            ? reason + " The encrypted file was moved aside and new history will start empty."
            : reason + " The invalid file could not be moved aside; clear it in Preferences.";
    }
    emit changed();
    return false;
}

bool OperationJournal::clear(QString* error)
{
    flushTimer_.stop();
    records_.clear();
    QStringList failures;
    if (QFile::exists(storagePath_) && !QFile::remove(storagePath_))
        failures.append(QFileInfo(storagePath_).fileName());
    const QFileInfo storageInfo(storagePath_);
    const QFileInfoList quarantined = QDir(storageInfo.absolutePath()).entryInfoList(
        {storageInfo.fileName() + ".corrupt-*"}, QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo& item : quarantined) {
        if (!QFile::remove(item.absoluteFilePath()))
            failures.append(item.fileName());
    }
    if (!failures.isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not remove encrypted operation history: %1")
                .arg(failures.join(", "));
        return false;
    }
    emit changed();
    return true;
}

bool OperationJournal::dismissInterrupted(const QStringList& recordIds, QString* error)
{
    if (recordIds.isEmpty())
        return true;
    const qsizetype previousSize = records_.size();
    records_.removeIf([&](const JournalRecord& record) {
        return record.state == JournalState::Interrupted && recordIds.contains(record.id);
    });
    if (records_.size() == previousSize)
        return true;
    if (!flush(error))
        return false;
    emit changed();
    return true;
}

QStringList OperationJournal::verifiedCleanupPaths(
    const QString& recordId, QStringList* refusedPaths) const
{
    QStringList verified;
    const auto found = std::find_if(records_.cbegin(), records_.cend(),
        [&](const JournalRecord& record) { return record.id == recordId; });
    if (found == records_.cend() || found->state != JournalState::Interrupted)
        return verified;
    for (const TransferManifestEntry& entry : found->manifest) {
        if (matchesOriginalSource(entry) && matchesCommittedItem(entry))
            verified.append(entry.destinationPath);
        else if (entry.destinationCreated && QFileInfo::exists(entry.destinationPath)
                && refusedPaths)
            refusedPaths->append(entry.destinationPath);
    }
    std::sort(verified.begin(), verified.end(), [](const QString& left, const QString& right) {
        return left.count(QDir::separator()) > right.count(QDir::separator());
    });
    return verified;
}

RecoveryCleanupResult OperationJournal::cleanupVerifiedCreatedItems(const QString& recordId) const
{
    RecoveryCleanupResult result;
    const auto found = std::find_if(records_.cbegin(), records_.cend(),
        [&](const JournalRecord& record) { return record.id == recordId; });
    if (found == records_.cend() || found->state != JournalState::Interrupted)
        return result;
    QList<TransferManifestEntry> entries = found->manifest;
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.destinationPath.count(QDir::separator())
            > right.destinationPath.count(QDir::separator());
    });
    for (const TransferManifestEntry& entry : entries) {
        if (!entry.destinationCreated || !QFileInfo::exists(entry.destinationPath))
            continue;
        if (!matchesOriginalSource(entry) || !matchesCommittedItem(entry)) {
            result.refusedPaths.append(entry.destinationPath);
            continue;
        }
        const QFileInfo info(entry.destinationPath);
        const bool removed = info.isDir()
            ? QDir(info.absolutePath()).rmdir(info.fileName())
            : QFile::remove(entry.destinationPath);
        if (removed)
            result.removedPaths.append(entry.destinationPath);
        else
            result.errors.append(QStringLiteral("Could not remove verified recovery item: %1")
                .arg(entry.destinationPath));
    }
    return result;
}

std::optional<TransferRequest> OperationJournal::retryRequest(
    const QString& recordId, QStringList* unavailableSources) const
{
    const auto found = std::find_if(records_.cbegin(), records_.cend(),
        [&](const JournalRecord& record) { return record.id == recordId; });
    if (found == records_.cend() || found->state != JournalState::Interrupted)
        return std::nullopt;
    TransferRequest request = found->request;
    if (request.hasReviewConstraints())
        return std::nullopt; // Stored paths cannot replace the discarded review/identity evidence.
    request.sources.clear();
    for (const QString& source : found->request.sources) {
        if (QFileInfo::exists(source))
            request.sources.append(source);
        else if (unavailableSources)
            unavailableSources->append(source);
    }
    if (request.sources.isEmpty())
        return std::nullopt;
    return request;
}

void OperationJournal::prune(const QDateTime& now)
{
    records_.removeIf([&](const JournalRecord& record) {
        if (!record.finishedAt.isValid() || record.state == JournalState::Interrupted)
            return false;
        return record.finishedAt.daysTo(now) > StandardRetentionDays;
    });
}

QList<JournalRecord> OperationJournal::records() const { return records_; }

QList<JournalRecord> OperationJournal::interruptedRecords() const
{
    QList<JournalRecord> interrupted;
    std::copy_if(records_.cbegin(), records_.cend(), std::back_inserter(interrupted),
        [](const JournalRecord& record) { return record.state == JournalState::Interrupted; });
    return interrupted;
}

QString OperationJournal::storagePath() const { return storagePath_; }

void OperationJournal::scheduleFlush() { flushTimer_.start(); }

} // namespace filemanager::files
