#include "savedsearchstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

namespace filemanager::search {
namespace {

QString defaultStoragePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("saved-searches.dat"));
}

QByteArray protect(const QByteArray& plain, QString* error)
{
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(plain.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
    const QByteArray entropyBytes("FileManager.SavedSearches.v1");
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(entropyBytes.constData()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"File Manager saved searches", &entropy, nullptr,
            nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        if (error)
            *error = QStringLiteral("Windows could not encrypt saved searches (%1).")
                .arg(GetLastError());
        return {};
    }
    const QByteArray encrypted(reinterpret_cast<const char*>(output.pbData),
        static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return encrypted;
#else
    Q_UNUSED(plain);
    if (error)
        *error = QStringLiteral("Per-user saved-search encryption is unavailable.");
    return {};
#endif
}

QByteArray unprotect(const QByteArray& encrypted, QString* error)
{
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.constData()))};
    const QByteArray entropyBytes("FileManager.SavedSearches.v1");
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(entropyBytes.constData()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        if (error)
            *error = QStringLiteral("Windows could not decrypt saved searches (%1).")
                .arg(GetLastError());
        return {};
    }
    const QByteArray plain(reinterpret_cast<const char*>(output.pbData),
        static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return plain;
#else
    Q_UNUSED(encrypted);
    if (error)
        *error = QStringLiteral("Per-user saved-search encryption is unavailable.");
    return {};
#endif
}

bool valid(const SavedSearch& search)
{
    const QFileInfo root(search.root);
    return !search.id.trimmed().isEmpty() && search.id.size() <= 128
        && !search.name.trimmed().isEmpty() && search.name.size() <= 80
        && root.isAbsolute() && search.root.size() <= 32767
        && search.filters.name.size() <= 512 && search.filters.extension.size() <= 64
        && search.sizePreset >= 0 && search.sizePreset <= 5
        && search.datePreset >= 0 && search.datePreset <= 4
        && static_cast<int>(search.filters.itemType) >= static_cast<int>(SearchItemType::Any)
        && static_cast<int>(search.filters.itemType) <= static_cast<int>(SearchItemType::Folders);
}

QJsonObject toJson(const SavedSearch& search)
{
    return {{"id", search.id}, {"name", search.name}, {"root", search.root},
        {"query", search.filters.name}, {"extension", search.filters.extension},
        {"itemType", static_cast<int>(search.filters.itemType)},
        {"sizePreset", search.sizePreset}, {"datePreset", search.datePreset},
        {"minimumSize", search.filters.minimumSize},
        {"maximumSize", search.filters.maximumSize},
        {"modifiedAfter", search.filters.modifiedAfter.isValid()
                ? search.filters.modifiedAfter.toMSecsSinceEpoch() : -1},
        {"modifiedBefore", search.filters.modifiedBefore.isValid()
                ? search.filters.modifiedBefore.toMSecsSinceEpoch() : -1}};
}

SavedSearch fromJson(const QJsonObject& object)
{
    SavedSearch search;
    search.id = object.value("id").toString();
    search.name = object.value("name").toString();
    search.root = QDir::cleanPath(object.value("root").toString());
    search.filters.name = object.value("query").toString();
    search.filters.extension = object.value("extension").toString();
    search.filters.itemType = static_cast<SearchItemType>(object.value("itemType").toInt());
    search.sizePreset = object.value("sizePreset").toInt();
    search.datePreset = object.value("datePreset").toInt();
    search.filters.minimumSize = object.value("minimumSize").toInteger(-1);
    search.filters.maximumSize = object.value("maximumSize").toInteger(-1);
    const qint64 modifiedAfter = object.value("modifiedAfter").toInteger(-1);
    const qint64 modifiedBefore = object.value("modifiedBefore").toInteger(-1);
    if (modifiedAfter >= 0)
        search.filters.modifiedAfter = QDateTime::fromMSecsSinceEpoch(modifiedAfter);
    if (modifiedBefore >= 0)
        search.filters.modifiedBefore = QDateTime::fromMSecsSinceEpoch(modifiedBefore);
    return search;
}

} // namespace

SavedSearchStore::SavedSearchStore(QString storagePath)
    : storagePath_(storagePath.isEmpty() ? defaultStoragePath() : std::move(storagePath))
{
}

bool SavedSearchStore::load(QString* error)
{
    if (!QFile::exists(storagePath_)) {
        entries_.clear();
        return true;
    }
    QFile file(storagePath_);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > MaximumStorageBytes) {
        if (error)
            *error = QStringLiteral("The encrypted saved-search file is unavailable or oversized.");
        return false;
    }
    const QByteArray plain = unprotect(file.readAll(), error);
    if (plain.isEmpty() || plain.size() > MaximumStorageBytes)
        return false;
    const QJsonDocument document = QJsonDocument::fromJson(plain);
    const QJsonObject root = document.object();
    const QJsonArray records = root.value("searches").toArray();
    if (!document.isObject() || root.value("schema").toInt() != 1
            || records.size() > MaximumEntries) {
        if (error)
            *error = QStringLiteral("The encrypted saved-search file has an unsupported format.");
        return false;
    }
    QList<SavedSearch> loaded;
    for (const QJsonValue& value : records) {
        const SavedSearch search = fromJson(value.toObject());
        if (!value.isObject() || !valid(search)) {
            if (error)
                *error = QStringLiteral("The encrypted saved-search file contains invalid data.");
            return false;
        }
        loaded.append(search);
    }
    entries_ = std::move(loaded);
    return true;
}

bool SavedSearchStore::save(const SavedSearch& search, QString* error)
{
    SavedSearch normalized = search;
    normalized.name = normalized.name.trimmed();
    normalized.root = QDir::cleanPath(normalized.root);
    normalized.filters.name = normalized.filters.name.trimmed();
    normalized.filters.extension = normalized.filters.extension.trimmed().remove('.');
    if (!valid(normalized)) {
        if (error)
            *error = QStringLiteral("The saved-search name, scope or filters are invalid.");
        return false;
    }
    QList<SavedSearch> updated = entries_;
    auto existing = std::find_if(updated.begin(), updated.end(), [&](const SavedSearch& entry) {
        return entry.id == normalized.id;
    });
    if (existing != updated.end())
        *existing = normalized;
    else if (updated.size() >= MaximumEntries) {
        if (error)
            *error = QStringLiteral("The saved-search limit has been reached.");
        return false;
    } else {
        updated.append(normalized);
    }
    if (!write(updated, error))
        return false;
    entries_ = std::move(updated);
    return true;
}

bool SavedSearchStore::remove(const QString& id, QString* error)
{
    QList<SavedSearch> updated = entries_;
    updated.erase(std::remove_if(updated.begin(), updated.end(),
        [&](const SavedSearch& entry) { return entry.id == id; }), updated.end());
    if (!write(updated, error))
        return false;
    entries_ = std::move(updated);
    return true;
}

bool SavedSearchStore::clear(QString* error)
{
    if (QFile::exists(storagePath_) && !QFile::remove(storagePath_)) {
        if (error)
            *error = QStringLiteral("Could not remove the encrypted saved-search file.");
        return false;
    }
    entries_.clear();
    return true;
}

const QList<SavedSearch>& SavedSearchStore::entries() const
{
    return entries_;
}

QString SavedSearchStore::storagePath() const
{
    return storagePath_;
}

bool SavedSearchStore::write(const QList<SavedSearch>& entries, QString* error) const
{
    QJsonArray records;
    for (const SavedSearch& search : entries)
        records.append(toJson(search));
    const QByteArray plain = QJsonDocument(QJsonObject{{"schema", 1}, {"searches", records}})
        .toJson(QJsonDocument::Compact);
    if (plain.size() > MaximumStorageBytes) {
        if (error)
            *error = QStringLiteral("The saved-search data exceeds its storage limit.");
        return false;
    }
    const QByteArray encrypted = protect(plain, error);
    if (encrypted.isEmpty() || encrypted.size() > MaximumStorageBytes)
        return false;
    if (!QDir().mkpath(QFileInfo(storagePath_).absolutePath())) {
        if (error)
            *error = QStringLiteral("Could not create the saved-search folder.");
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

} // namespace filemanager::search
