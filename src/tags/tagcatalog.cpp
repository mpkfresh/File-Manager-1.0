#include "tagcatalog.h"
#include "tagrules.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QMultiMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <limits>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#endif

namespace filemanager::tags {
namespace {

constexpr qsizetype MaximumEncryptedBytes = TagCatalog::MaximumPlainBytes + 1024 * 1024;
constexpr quint32 ReparsePointAttribute = 0x400;
constexpr quint32 DirectoryAttribute = 0x10;

QString defaultStoragePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("tag-catalog.dat"));
}

bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

bool acquireWriteLock(const QString& storagePath, QLockFile* lock, QString* error)
{
    if (!QDir().mkpath(QFileInfo(storagePath).absolutePath()))
        return fail(error, QStringLiteral("The tag catalog folder could not be created."));
    if (!lock->tryLock(0))
        return fail(error, QStringLiteral("Another File Manager window is changing tags; try again."));
    return true;
}

QString normalizedPath(const QString& path)
{
    if (!QDir::isAbsolutePath(path))
        return {};
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool validTag(const QString& tag)
{
    if (tag.isEmpty() || tag.size() > 80 || tag != tag.trimmed())
        return false;
    for (const QChar character : tag) {
        const ushort code = character.unicode();
        if (code < 0x20 || code == 0x7f)
            return false;
    }
    return true;
}

bool normalizeSelection(SelectedFile* file)
{
    file->path = normalizedPath(file->path);
    return !file->path.isEmpty() && file->path.size() <= 32767
        && file->snapshot.valid && file->snapshot.fileIndex != 0
        && !(file->snapshot.attributes & (ReparsePointAttribute | DirectoryAttribute));
}

bool normalizeTags(QStringList* tags)
{
    if (tags->isEmpty() || tags->size() > TagCatalog::MaximumTagsPerFile)
        return false;
    QStringList normalized;
    QSet<QString> seen;
    for (QString tag : *tags) {
        tag = tag.trimmed().normalized(QString::NormalizationForm_C);
        if (!validTag(tag))
            return false;
        const QString key = tag.toCaseFolded();
        if (!seen.contains(key)) {
            normalized.append(tag);
            seen.insert(key);
        }
    }
    if (normalized.isEmpty())
        return false;
    *tags = std::move(normalized);
    return true;
}

bool normalize(TaggedFile* file)
{
    SelectedFile selected{file->path, file->snapshot};
    if (!normalizeSelection(&selected) || !normalizeTags(&file->tags))
        return false;
    file->path = std::move(selected.path);
    return true;
}

QString hex(quint64 value)
{
    return QString::number(value, 16);
}

bool parseHex(const QJsonValue& value, quint64* number)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{1,16}$"));
    if (!value.isString() || !pattern.match(value.toString()).hasMatch())
        return false;
    bool ok = false;
    *number = value.toString().toULongLong(&ok, 16);
    return ok;
}

QJsonObject toJson(const TaggedFile& file)
{
    const auto& snapshot = file.snapshot;
    QJsonArray tags;
    for (const QString& tag : file.tags)
        tags.append(tag);
    return {{"path", file.path},
        {"volume", hex(snapshot.volumeSerial)},
        {"fileIndex", hex(snapshot.fileIndex)},
        {"size", hex(snapshot.size)},
        {"modified", hex(snapshot.lastWriteTime)},
        {"attributes", hex(snapshot.attributes)},
        {"tags", tags}};
}

QJsonObject toJson(const DestinationRule& rule)
{
    QJsonArray tags;
    for (const QString& tag : rule.requiredTags)
        tags.append(tag);
    return {{"id", rule.id}, {"tags", tags},
        {"destination", rule.destinationDirectory}};
}

bool fromJson(const QJsonValue& value, DestinationRule* rule)
{
    if (!value.isObject())
        return false;
    const QJsonObject object = value.toObject();
    if (object.size() != 3 || !object.value("id").isString()
            || !object.value("tags").isArray()
            || !object.value("destination").isString())
        return false;
    DestinationRule decoded;
    decoded.id = object.value("id").toString();
    decoded.destinationDirectory = object.value("destination").toString();
    for (const QJsonValue& tag : object.value("tags").toArray()) {
        if (!tag.isString())
            return false;
        decoded.requiredTags.append(tag.toString());
    }
    *rule = std::move(decoded);
    return true;
}

bool fromJson(const QJsonValue& value, TaggedFile* file)
{
    if (!value.isObject())
        return false;
    const QJsonObject object = value.toObject();
    if (object.size() != 7 || !object.value("path").isString()
            || !object.value("tags").isArray()) {
        return false;
    }
    TaggedFile decoded;
    decoded.path = object.value("path").toString();
    decoded.snapshot.valid = true;
    quint64 attributes = 0;
    if (!parseHex(object.value("volume"), &decoded.snapshot.volumeSerial)
            || !parseHex(object.value("fileIndex"), &decoded.snapshot.fileIndex)
            || !parseHex(object.value("size"), &decoded.snapshot.size)
            || !parseHex(object.value("modified"), &decoded.snapshot.lastWriteTime)
            || !parseHex(object.value("attributes"), &attributes)
            || attributes > std::numeric_limits<quint32>::max()) {
        return false;
    }
    decoded.snapshot.attributes = static_cast<quint32>(attributes);
    for (const QJsonValue& tag : object.value("tags").toArray()) {
        if (!tag.isString())
            return false;
        decoded.tags.append(tag.toString());
    }
    if (!normalize(&decoded))
        return false;
    *file = std::move(decoded);
    return true;
}

QByteArray protect(const QByteArray& plain, QString* error)
{
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(plain.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
    const QByteArray entropyBytes("FileManager.TagCatalog.v1");
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(entropyBytes.constData()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"File Manager tag catalog", &entropy, nullptr,
            nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        fail(error, QStringLiteral("Windows could not encrypt the tag catalog (%1).")
                .arg(GetLastError()));
        return {};
    }
    const QByteArray encrypted(reinterpret_cast<const char*>(output.pbData),
        static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return encrypted;
#else
    Q_UNUSED(plain);
    fail(error, QStringLiteral("Per-user tag-catalog encryption is unavailable."));
    return {};
#endif
}

QByteArray unprotect(const QByteArray& encrypted, QString* error)
{
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.constData()))};
    const QByteArray entropyBytes("FileManager.TagCatalog.v1");
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(entropyBytes.constData()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        fail(error, QStringLiteral("Windows could not decrypt the tag catalog (%1).")
                .arg(GetLastError()));
        return {};
    }
    const QByteArray plain(reinterpret_cast<const char*>(output.pbData),
        static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return plain;
#else
    Q_UNUSED(encrypted);
    fail(error, QStringLiteral("Per-user tag-catalog encryption is unavailable."));
    return {};
#endif
}

} // namespace

TagCatalog::TagCatalog(QString storagePath)
    : storagePath_(storagePath.isEmpty() ? defaultStoragePath() : std::move(storagePath))
{
}

bool TagCatalog::load(QString* error)
{
    loaded_ = false;
    entries_.clear();
    rules_.clear();
    if (!QFile::exists(storagePath_)) {
        loaded_ = true;
        return true;
    }
    QFile file(storagePath_);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0
            || file.size() > MaximumEncryptedBytes) {
        return fail(error, QStringLiteral("The encrypted tag catalog is unavailable or oversized."));
    }
    const QByteArray plain = unprotect(file.readAll(), error);
    if (plain.isEmpty() || plain.size() > MaximumPlainBytes)
        return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(plain, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("The encrypted tag catalog is not valid JSON."));
    const QJsonObject root = document.object();
    const int schema = root.value("schema").toInt(-1);
    if (((schema == 1 && root.size() != 3)
            || (schema == 2 && (root.size() != 4 || !root.value("rules").isArray()))
            || (schema != 1 && schema != 2))
            || root.value("product").toString() != QStringLiteral("FileManager")
            || !root.value("records").isArray()) {
        return fail(error, QStringLiteral("The encrypted tag catalog has an unsupported format."));
    }
    const QJsonArray records = root.value("records").toArray();
    if (records.size() > MaximumRecords)
        return fail(error, QStringLiteral("The encrypted tag catalog exceeds its record limit."));
    QMap<QString, TaggedFile> loaded;
    for (const QJsonValue& value : records) {
        TaggedFile fileRecord;
        if (!fromJson(value, &fileRecord) || loaded.contains(fileRecord.path))
            return fail(error, QStringLiteral("The encrypted tag catalog contains invalid records."));
        const QString path = fileRecord.path;
        loaded.insert(path, std::move(fileRecord));
    }
    QList<DestinationRule> loadedRules;
    if (schema == 2) {
        const QJsonArray storedRules = root.value("rules").toArray();
        if (storedRules.size() > MaximumRules)
            return fail(error, QStringLiteral("The encrypted tag catalog exceeds its rule limit."));
        for (const QJsonValue& value : storedRules) {
            DestinationRule rule;
            if (!fromJson(value, &rule))
                return fail(error, QStringLiteral("The encrypted tag catalog contains invalid rules."));
            loadedRules.append(std::move(rule));
        }
        if (!validateDestinationRules(loadedRules, error))
            return false;
    }
    entries_ = std::move(loaded);
    rules_ = std::move(loadedRules);
    loaded_ = true;
    return true;
}

bool TagCatalog::assign(const QList<TaggedFile>& files, QString* error)
{
    if (!loaded_)
        return fail(error, QStringLiteral("Load the tag catalog before changing it."));
    if (files.isEmpty())
        return true;
    QLockFile lock(storagePath_ + QStringLiteral(".lock"));
    if (!acquireWriteLock(storagePath_, &lock, error) || !load(error))
        return false;
    QMap<QString, TaggedFile> updated = entries_;
    QSet<QString> incomingPaths;
    for (TaggedFile record : files) {
        if (!normalize(&record) || incomingPaths.contains(record.path))
            return fail(error, QStringLiteral("A selected file, identity or tag is invalid."));
        incomingPaths.insert(record.path);
        if (!updated.contains(record.path) && updated.size() >= MaximumRecords)
            return fail(error, QStringLiteral("The tag catalog record limit has been reached."));
        const QString path = record.path;
        updated.insert(path, std::move(record));
    }
    if (!write(updated, rules_, error))
        return false;
    entries_ = std::move(updated);
    return true;
}

bool TagCatalog::addTags(const QList<SelectedFile>& files, const QStringList& tags,
    QString* error)
{
    return changeTags(files, tags, false, error);
}

bool TagCatalog::removeTags(const QList<SelectedFile>& files, const QStringList& tags,
    QString* error)
{
    return changeTags(files, tags, true, error);
}

bool TagCatalog::changeTags(const QList<SelectedFile>& files, const QStringList& tags,
    bool remove, QString* error)
{
    if (!loaded_)
        return fail(error, QStringLiteral("Load the tag catalog before changing it."));
    if (files.isEmpty())
        return fail(error, QStringLiteral("Select at least one file to change tags."));
    QStringList requested = tags;
    if (!normalizeTags(&requested))
        return fail(error, QStringLiteral("Enter 1 to 32 valid tag labels."));
    QLockFile lock(storagePath_ + QStringLiteral(".lock"));
    if (!acquireWriteLock(storagePath_, &lock, error) || !load(error))
        return false;

    QSet<QString> requestedKeys;
    for (const QString& tag : requested)
        requestedKeys.insert(tag.toCaseFolded());
    QMap<QString, TaggedFile> updated = entries_;
    QSet<QString> selectedPaths;
    bool changed = false;
    for (SelectedFile selected : files) {
        if (!normalizeSelection(&selected) || selectedPaths.contains(selected.path))
            return fail(error, QStringLiteral("A selected file or native identity is invalid or duplicated."));
        selectedPaths.insert(selected.path);
        auto existing = updated.find(selected.path);
        if (existing == updated.end()) {
            if (remove)
                continue;
            if (updated.size() >= MaximumRecords)
                return fail(error, QStringLiteral("The tag catalog record limit has been reached."));
            const QString path = selected.path;
            updated.insert(path, TaggedFile{std::move(selected.path),
                selected.snapshot, requested});
            changed = true;
            continue;
        }
        if (match(selected.path, selected.snapshot).state != TagMatchState::Current)
            return fail(error, QStringLiteral("A selected file changed or was replaced; review its tags first."));

        QStringList merged;
        if (remove) {
            for (const QString& tag : existing.value().tags) {
                if (!requestedKeys.contains(tag.toCaseFolded()))
                    merged.append(tag);
            }
        } else {
            merged = existing.value().tags;
            QSet<QString> present;
            for (const QString& tag : merged)
                present.insert(tag.toCaseFolded());
            for (const QString& tag : requested) {
                if (!present.contains(tag.toCaseFolded())) {
                    merged.append(tag);
                    present.insert(tag.toCaseFolded());
                }
            }
            if (merged.size() > MaximumTagsPerFile)
                return fail(error, QStringLiteral("A selected file would exceed 32 tags."));
        }
        if (merged == existing.value().tags)
            continue;
        changed = true;
        if (merged.isEmpty())
            updated.erase(existing);
        else
            existing.value().tags = std::move(merged);
    }
    if (!changed)
        return true;
    if (!write(updated, rules_, error))
        return false;
    entries_ = std::move(updated);
    return true;
}

bool TagCatalog::remove(const QString& path, QString* error)
{
    if (!loaded_)
        return fail(error, QStringLiteral("Load the tag catalog before changing it."));
    const QString key = normalizedPath(path);
    if (key.isEmpty())
        return fail(error, QStringLiteral("The selected tag path is invalid."));
    QLockFile lock(storagePath_ + QStringLiteral(".lock"));
    if (!acquireWriteLock(storagePath_, &lock, error) || !load(error))
        return false;
    if (!entries_.contains(key))
        return true;
    QMap<QString, TaggedFile> updated = entries_;
    updated.remove(key);
    if (!write(updated, rules_, error))
        return false;
    entries_ = std::move(updated);
    return true;
}

bool TagCatalog::relocate(const QList<TagPathChange>& changes, QString* error)
{
    if (!loaded_)
        return fail(error, QStringLiteral("Load the tag catalog before moving labels."));
    if (changes.isEmpty())
        return true;
    QLockFile lock(storagePath_ + QStringLiteral(".lock"));
    if (!acquireWriteLock(storagePath_, &lock, error) || !load(error))
        return false;
    if (entries_.isEmpty())
        return true;

    QHash<QString, QString> storedKeys;
    QSet<QString> ambiguous;
    QMultiMap<QString, QString> sortedKeys;
    for (auto it = entries_.cbegin(); it != entries_.cend(); ++it) {
        const QString folded = it.key().toCaseFolded();
        sortedKeys.insert(folded, it.key());
        if (storedKeys.contains(folded))
            ambiguous.insert(folded);
        else
            storedKeys.insert(folded, it.key());
    }

    struct FileChange {
        QString source;
        QString destination;
        filemanager::files::SourceSnapshot before;
        filemanager::files::SourceSnapshot after;
        bool verifiedByDirectory = false;
    };
    QList<FileChange> files;
    for (const TagPathChange& change : changes) {
        const QString source = normalizedPath(change.sourcePath);
        const QString destination = normalizedPath(change.destinationPath);
        if (!change.directory && !storedKeys.contains(source.toCaseFolded())
                && !storedKeys.contains(destination.toCaseFolded()))
            continue;
        const QString prefix = source + QLatin1Char('/');
        const QString foldedPrefix = prefix.toCaseFolded();
        if (change.directory) {
            const auto first = sortedKeys.lowerBound(foldedPrefix);
            if (first == sortedKeys.cend() || !first.key().startsWith(foldedPrefix))
                continue;
        }
        const auto& before = change.sourceSnapshot;
        const auto& after = change.destinationSnapshot;
        if (source.isEmpty() || destination.isEmpty() || source.size() > 32767
                || destination.size() > 32767 || !before.valid || !after.valid
                || before.fileIndex == 0 || after.fileIndex == 0
                || (before.attributes & ReparsePointAttribute)
                || (after.attributes & ReparsePointAttribute)
                || bool(before.attributes & DirectoryAttribute) != change.directory
                || bool(after.attributes & DirectoryAttribute) != change.directory) {
            return fail(error, QStringLiteral("A changed label path or native identity is invalid."));
        }
        const auto current = filemanager::files::captureSourceSnapshot(destination);
        if (!current.valid || current.volumeSerial != after.volumeSerial
                || current.fileIndex != after.fileIndex
                || bool(current.attributes & DirectoryAttribute) != change.directory
                || (current.attributes & ReparsePointAttribute)) {
            return fail(error, QStringLiteral("A changed file was replaced before labels could follow it."));
        }
        if (change.directory) {
            if (before.volumeSerial != after.volumeSerial
                    || before.fileIndex != after.fileIndex) {
                return fail(error, QStringLiteral(
                    "A folder move changed native identity without individual file results."));
            }
            for (auto it = sortedKeys.lowerBound(foldedPrefix);
                    it != sortedKeys.cend() && it.key().startsWith(foldedPrefix); ++it) {
                const QString storedPath = it.value();
                const TaggedFile& record = entries_.value(storedPath);
                files.append({storedPath, normalizedPath(destination
                    + storedPath.mid(source.size())), record.snapshot, record.snapshot, true});
            }
        } else {
            files.append({source, destination, before, after, false});
        }
    }
    if (files.isEmpty())
        return true;
    QSet<QString> sourceKeys;
    QSet<QString> destinationKeys;
    for (const FileChange& file : files) {
        if (file.source.isEmpty() || file.destination.isEmpty()
                || file.destination.size() > 32767)
            return fail(error, QStringLiteral("A relocated label path is invalid."));
        const QString sourceKey = file.source.toCaseFolded();
        const QString destinationKey = file.destination.toCaseFolded();
        if (sourceKeys.contains(sourceKey) || destinationKeys.contains(destinationKey)
                || ambiguous.contains(sourceKey) || ambiguous.contains(destinationKey)) {
            return fail(error, QStringLiteral("Changed label paths overlap or are ambiguous."));
        }
        sourceKeys.insert(sourceKey);
        destinationKeys.insert(destinationKey);
        if (!file.verifiedByDirectory) {
            const auto current = filemanager::files::captureSourceSnapshot(file.destination);
            if (!current.valid || current.volumeSerial != file.after.volumeSerial
                    || current.fileIndex != file.after.fileIndex
                    || (current.attributes & (ReparsePointAttribute | DirectoryAttribute))) {
                return fail(error, QStringLiteral(
                    "A moved file was replaced before labels could follow it."));
            }
        }
    }

    QMap<QString, TaggedFile> updated = entries_;
    QList<TaggedFile> carried;
    bool changed = false;
    for (const FileChange& file : files) {
        const QString sourceKey = storedKeys.value(file.source.toCaseFolded());
        const QString destinationKey = storedKeys.value(file.destination.toCaseFolded());
        TaggedFile moved;
        bool hasMoved = false;
        if (!sourceKey.isEmpty()) {
            const TaggedFile record = entries_.value(sourceKey);
            if (record.snapshot.volumeSerial == file.before.volumeSerial
                    && record.snapshot.fileIndex == file.before.fileIndex) {
                moved = record;
                hasMoved = true;
            }
            changed = updated.remove(sourceKey) > 0 || changed;
        }
        if (!destinationKey.isEmpty() && destinationKey != sourceKey) {
            const TaggedFile existing = entries_.value(destinationKey);
            if (existing.snapshot.volumeSerial == file.after.volumeSerial
                    && existing.snapshot.fileIndex == file.after.fileIndex) {
                if (!hasMoved) {
                    moved = existing;
                    hasMoved = true;
                } else {
                    QSet<QString> present;
                    for (const QString& tag : moved.tags)
                        present.insert(tag.toCaseFolded());
                    for (const QString& tag : existing.tags) {
                        if (!present.contains(tag.toCaseFolded())) {
                            moved.tags.append(tag);
                            present.insert(tag.toCaseFolded());
                        }
                    }
                    if (moved.tags.size() > MaximumTagsPerFile)
                        return fail(error, QStringLiteral("Combined labels exceed the per-file limit."));
                }
            }
            changed = updated.remove(destinationKey) > 0 || changed;
        }
        if (hasMoved) {
            moved.path = file.destination;
            moved.snapshot = file.after;
            carried.append(std::move(moved));
        }
    }
    for (TaggedFile& record : carried) {
        updated.insert(record.path, std::move(record));
        changed = true;
    }
    if (!changed)
        return true;
    if (!write(updated, rules_, error))
        return false;
    entries_ = std::move(updated);
    return true;
}

bool TagCatalog::replaceRules(const QList<DestinationRule>& rules, QString* error,
    const QList<DestinationRule>* expectedRules)
{
    if (!loaded_)
        return fail(error, QStringLiteral("Load the tag catalog before changing rules."));
    if (!validateDestinationRules(rules, error))
        return false;
    QLockFile lock(storagePath_ + QStringLiteral(".lock"));
    if (!acquireWriteLock(storagePath_, &lock, error) || !load(error))
        return false;
    if (expectedRules && rules_ != *expectedRules)
        return fail(error, QStringLiteral("Destination rules changed in another File Manager window. Reload and review them before saving."));
    if (!write(entries_, rules, error))
        return false;
    rules_ = rules;
    return true;
}


bool TagCatalog::clear(QString* error)
{
    if (!loaded_)
        return fail(error, QStringLiteral("Load the tag catalog before changing it."));
    QLockFile lock(storagePath_ + QStringLiteral(".lock"));
    if (!acquireWriteLock(storagePath_, &lock, error))
        return false;
    if (QFile::exists(storagePath_) && !QFile::remove(storagePath_))
        return fail(error, QStringLiteral("The encrypted tag catalog could not be cleared."));
    entries_.clear();
    rules_.clear();
    return true;
}

TagMatch TagCatalog::match(const QString& path,
    const filemanager::files::SourceSnapshot& current) const
{
    if (!loaded_)
        return {TagMatchState::Unavailable, {}};
    const QString key = normalizedPath(path);
    if (key.isEmpty())
        return {};
    const auto it = entries_.constFind(key);
    if (it == entries_.cend())
        return {};
    const auto& stored = it.value();
    if (!current.valid || current.fileIndex == 0
            || (current.attributes & (ReparsePointAttribute | DirectoryAttribute))) {
        return {TagMatchState::Unavailable, {}};
    }
    if (current.volumeSerial != stored.snapshot.volumeSerial
            || current.fileIndex != stored.snapshot.fileIndex) {
        return {TagMatchState::Replaced, {}};
    }
    // User labels follow the same native file identity through content edits.
    // The old catalog timestamp never authorizes a file operation.
    return {TagMatchState::Current, stored.tags};
}

QList<TaggedFile> TagCatalog::entries() const
{
    return loaded_ ? entries_.values() : QList<TaggedFile>{};
}

QList<DestinationRule> TagCatalog::rules() const
{
    return loaded_ ? rules_ : QList<DestinationRule>{};
}

QString TagCatalog::storagePath() const
{
    return storagePath_;
}

bool TagCatalog::ready() const
{
    return loaded_;
}

bool TagCatalog::write(const QMap<QString, TaggedFile>& files,
    const QList<DestinationRule>& rules, QString* error) const
{
    QJsonArray records;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        records.append(toJson(it.value()));
    QJsonArray storedRules;
    for (const DestinationRule& rule : rules)
        storedRules.append(toJson(rule));
    const QByteArray plain = QJsonDocument(QJsonObject{
        {"schema", 2}, {"product", "FileManager"}, {"records", records},
        {"rules", storedRules}})
        .toJson(QJsonDocument::Compact);
    if (plain.size() > MaximumPlainBytes)
        return fail(error, QStringLiteral("The tag catalog exceeds its storage limit."));
    const QByteArray encrypted = protect(plain, error);
    if (encrypted.isEmpty() || encrypted.size() > MaximumEncryptedBytes)
        return false;
    if (!QDir().mkpath(QFileInfo(storagePath_).absolutePath()))
        return fail(error, QStringLiteral("The tag catalog folder could not be created."));
    QSaveFile file(storagePath_);
    if (!file.open(QIODevice::WriteOnly) || file.write(encrypted) != encrypted.size()
            || !file.commit()) {
        return fail(error, file.errorString());
    }
    return true;
}

} // namespace filemanager::tags
