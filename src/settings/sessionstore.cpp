#include "sessionstore.h"
#include "workspace/layoutmodel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSet>
#include <QUuid>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

namespace filemanager::settings {
namespace {

constexpr int MaximumNamedWorkspaces = 20;
constexpr qsizetype MaximumNamedWorkspaceBytes = 2 * 1024 * 1024;
constexpr qsizetype MaximumWorkspaceJsonBytes = 1024 * 1024;
constexpr qsizetype MaximumWindowStateBytes = 256 * 1024;

void setError(QString* error, const QString& message)
{
    if (error)
        *error = message;
}

void recordRecovery(QSettings& settings, const QString& code)
{
    settings.setValue("recovery/workspace/lastResetUtc",
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    settings.setValue("recovery/workspace/reason", code);
}

void recoverCorruptWorkspace(QSettings& settings, const QString& code,
    const QString& message, QString* error)
{
    settings.remove("layout/workspaceProtected");
    settings.remove("layout/workspaceJson");
    settings.remove("layout/geometry");
    settings.remove("layout/shellSizes");
    settings.remove("layout/inspectorVisible");
    settings.remove("schemaVersion");
    settings.setValue("privacy/restorePreviousWorkspace", false);
    recordRecovery(settings, code);
    settings.sync();
    setError(error, message + " The invalid startup workspace was removed and restoration was disabled.");
}

void recoverCorruptNamedWorkspaces(const QString& code, const QString& message, QString* error)
{
    QSettings settings;
    settings.remove("layout/namedWorkspacesProtected");
    settings.remove("layout/activeNamedWorkspaceId");
    recordRecovery(settings, code);
    settings.sync();
    setError(error, message + " The invalid named-layout store was removed.");
}

bool validateSession(const Session& session, QString* detail = nullptr)
{
    if (session.version != Session::CurrentVersion) {
        setError(detail, "The workspace settings schema is unsupported.");
        return false;
    }
    if (session.workspaceLayoutJson.isEmpty()
        || session.workspaceLayoutJson.toUtf8().size() > MaximumWorkspaceJsonBytes
        || session.geometry.size() > MaximumWindowStateBytes
        || session.splitterState.size() > MaximumWindowStateBytes) {
        setError(detail, "The workspace state exceeds its safety limits.");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(session.workspaceLayoutJson.toUtf8(), &parseError);
    QString layoutError;
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !filemanager::workspace::LayoutNode::fromJson(document.object(), &layoutError)) {
        setError(detail, "The workspace layout is malformed or unsupported.");
        return false;
    }
    return true;
}

QByteArray protect(const QByteArray& plain, QString* error)
{
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(plain.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
    const QByteArray entropyBytes("FileManager.WorkspaceSession.v1");
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(entropyBytes.constData()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"File Manager workspace session", &entropy, nullptr,
            nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        if (error)
            *error = QStringLiteral("Windows could not encrypt the workspace state (%1).")
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
        *error = QStringLiteral("Per-user workspace encryption is unavailable.");
    return {};
#endif
}

QByteArray unprotect(const QByteArray& encrypted, QString* error)
{
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.constData()))};
    const QByteArray entropyBytes("FileManager.WorkspaceSession.v1");
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(entropyBytes.constData()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        if (error)
            *error = QStringLiteral("Windows could not decrypt the workspace state (%1).")
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
        *error = QStringLiteral("Per-user workspace encryption is unavailable.");
    return {};
#endif
}

QJsonObject sessionToJson(const Session& session)
{
    return {
        {"version", session.version},
        {"workspace", session.workspaceLayoutJson},
        {"geometry", QString::fromLatin1(session.geometry.toBase64())},
        {"splitter", QString::fromLatin1(session.splitterState.toBase64())},
        {"inspectorVisible", session.inspectorVisible}
    };
}

bool decodeState(const QJsonValue& value, QByteArray* output)
{
    if (!value.isString())
        return false;
    const QByteArray encoded = value.toString().toLatin1();
    if (encoded.size() > MaximumWindowStateBytes * 2)
        return false;
    const QByteArray decoded = QByteArray::fromBase64(encoded);
    if (decoded.size() > MaximumWindowStateBytes || decoded.toBase64() != encoded)
        return false;
    *output = decoded;
    return true;
}

bool sessionFromJson(const QJsonValue& value, Session* session)
{
    if (!value.isObject())
        return false;
    const QJsonObject object = value.toObject();
    if (object.value("version").toInt(-1) != Session::CurrentVersion
        || !object.value("workspace").isString()
        || !object.value("inspectorVisible").isBool())
        return false;
    Session decoded;
    decoded.workspaceLayoutJson = object.value("workspace").toString();
    if (decoded.workspaceLayoutJson.isEmpty()
        || decoded.workspaceLayoutJson.toUtf8().size() > MaximumWorkspaceJsonBytes
        || !decodeState(object.value("geometry"), &decoded.geometry)
        || !decodeState(object.value("splitter"), &decoded.splitterState))
        return false;
    decoded.inspectorVisible = object.value("inspectorVisible").toBool();
    if (!validateSession(decoded))
        return false;
    *session = std::move(decoded);
    return true;
}

QList<NamedWorkspace> loadNamedWorkspaces(QString* error)
{
    const QByteArray encrypted = QSettings().value("layout/namedWorkspacesProtected").toByteArray();
    if (encrypted.isEmpty())
        return {};
    if (encrypted.size() > MaximumNamedWorkspaceBytes * 2) {
        recoverCorruptNamedWorkspaces("named_store_oversized",
            "The encrypted named-layout store exceeds its safety limit.", error);
        return {};
    }
    const QByteArray plain = unprotect(encrypted, error);
    if (plain.isEmpty()) {
        recoverCorruptNamedWorkspaces("named_store_decryption_failed",
            "The encrypted named-layout store could not be decrypted.", error);
        return {};
    }
    if (plain.size() > MaximumNamedWorkspaceBytes) {
        recoverCorruptNamedWorkspaces("named_store_oversized",
            "The named-layout store exceeds its safety limit.", error);
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(plain, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        recoverCorruptNamedWorkspaces("named_store_malformed",
            "The named-layout store is malformed.", error);
        return {};
    }
    const QJsonObject root = document.object();
    const QJsonArray records = root.value("records").toArray();
    if (root.value("version").toInt(-1) != 1
        || !root.value("records").isArray() || records.size() > MaximumNamedWorkspaces) {
        recoverCorruptNamedWorkspaces("named_store_schema_invalid",
            "The named-layout store has an unsupported or invalid schema.", error);
        return {};
    }
    QList<NamedWorkspace> result;
    QSet<QString> ids;
    QSet<QString> names;
    for (const QJsonValue& value : records) {
        if (!value.isObject()) {
            recoverCorruptNamedWorkspaces("named_store_record_invalid",
                "The named-layout store contains an invalid record.", error);
            return {};
        }
        const QJsonObject object = value.toObject();
        NamedWorkspace workspace;
        workspace.id = object.value("id").toString();
        workspace.name = object.value("name").toString().trimmed();
        workspace.updatedAt = QDateTime::fromString(
            object.value("updatedAt").toString(), Qt::ISODateWithMs);
        const QString foldedName = workspace.name.toCaseFolded();
        if (workspace.id.isEmpty() || workspace.id.size() > 64
            || workspace.name.isEmpty() || workspace.name.size() > 64
            || ids.contains(workspace.id) || names.contains(foldedName)
            || !workspace.updatedAt.isValid()
            || !sessionFromJson(object.value("session"), &workspace.session)) {
            recoverCorruptNamedWorkspaces("named_store_record_invalid",
                "The named-layout store contains an invalid record.", error);
            return {};
        }
        ids.insert(workspace.id);
        names.insert(foldedName);
        result.append(std::move(workspace));
    }
    return result;
}

bool writeNamedWorkspaces(const QList<NamedWorkspace>& records, QString* error)
{
    if (records.isEmpty()) {
        QSettings settings;
        settings.remove("layout/namedWorkspacesProtected");
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            setError(error, "The encrypted named layouts could not be removed.");
            return false;
        }
        return true;
    }
    QJsonArray array;
    for (const NamedWorkspace& workspace : records) {
        array.append(QJsonObject{
            {"id", workspace.id}, {"name", workspace.name},
            {"updatedAt", workspace.updatedAt.toUTC().toString(Qt::ISODateWithMs)},
            {"session", sessionToJson(workspace.session)}
        });
    }
    const QByteArray plain = QJsonDocument(QJsonObject{
        {"version", 1}, {"records", array}
    }).toJson(QJsonDocument::Compact);
    if (plain.size() > MaximumNamedWorkspaceBytes) {
        setError(error, "The named-layout store exceeds its 2 MiB safety limit.");
        return false;
    }
    const QByteArray encrypted = protect(plain, error);
    if (encrypted.isEmpty())
        return false;
    QSettings settings;
    settings.setValue("layout/namedWorkspacesProtected", encrypted);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        setError(error, "The encrypted named layouts could not be saved.");
        return false;
    }
    return true;
}

} // namespace

Session SessionStore::load(QString* error) const
{
    QSettings settings;
    const int version = settings.value("schemaVersion", Session::CurrentVersion).toInt();
    QString workspaceJson;
    const QByteArray protectedWorkspace = settings.value("layout/workspaceProtected").toByteArray();
    if (!protectedWorkspace.isEmpty()) {
        if (protectedWorkspace.size() > MaximumWorkspaceJsonBytes * 2) {
            recoverCorruptWorkspace(settings, "workspace_encrypted_oversized",
                "The encrypted workspace state exceeds its safety limit.", error);
            return {};
        }
        const QByteArray plain = unprotect(protectedWorkspace, error);
        if (plain.isEmpty()) {
            recoverCorruptWorkspace(settings, "workspace_decryption_failed",
                "The encrypted workspace state could not be decrypted.", error);
            return {};
        }
        workspaceJson = QString::fromUtf8(plain);
    } else {
        workspaceJson = settings.value("layout/workspaceJson").toString();
        if (!workspaceJson.isEmpty()) {
            Session legacy{version, workspaceJson,
                settings.value("layout/geometry").toByteArray(),
                settings.value("layout/shellSizes").toByteArray(),
                settings.value("layout/inspectorVisible", false).toBool()};
            QString validationError;
            if (!validateSession(legacy, &validationError)) {
                recoverCorruptWorkspace(settings, "legacy_workspace_invalid",
                    validationError, error);
                return {};
            }
            const QByteArray migrated = protect(workspaceJson.toUtf8(), error);
            if (!migrated.isEmpty()) {
                settings.setValue("layout/workspaceProtected", migrated);
                settings.remove("layout/workspaceJson");
                settings.sync();
            }
        }
    }
    Session session {
        version,
        workspaceJson,
        settings.value("layout/geometry").toByteArray(),
        settings.value("layout/shellSizes").toByteArray(),
        settings.value("layout/inspectorVisible", false).toBool()
    };
    if (!session.workspaceLayoutJson.isEmpty()) {
        QString validationError;
        if (!validateSession(session, &validationError)) {
            recoverCorruptWorkspace(settings, "workspace_payload_invalid",
                validationError, error);
            return {};
        }
    }
    return session;
}

bool SessionStore::save(const Session& session, QString* error) const
{
    QString validationError;
    if (!validateSession(session, &validationError)) {
        setError(error, validationError);
        return false;
    }
    const QByteArray protectedWorkspace = protect(session.workspaceLayoutJson.toUtf8(), error);
    if (protectedWorkspace.isEmpty())
        return false;
    QSettings settings;
    settings.setValue("schemaVersion", Session::CurrentVersion);
    settings.setValue("layout/workspaceProtected", protectedWorkspace);
    settings.remove("layout/workspaceJson");
    settings.setValue("layout/geometry", session.geometry);
    settings.setValue("layout/shellSizes", session.splitterState);
    settings.setValue("layout/inspectorVisible", session.inspectorVisible);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("The encrypted workspace state could not be saved.");
        return false;
    }
    return true;
}

bool SessionStore::restorationEnabled() const
{
    return QSettings().value("privacy/restorePreviousWorkspace", false).toBool();
}

void SessionStore::setRestorationEnabled(bool enabled) const
{
    QSettings settings;
    settings.setValue("privacy/restorePreviousWorkspace", enabled);
    settings.sync();
}

bool SessionStore::hasSavedSession() const
{
    QSettings settings;
    return !settings.value("layout/workspaceProtected").toByteArray().isEmpty()
        || !settings.value("layout/workspaceJson").toString().isEmpty()
        || !settings.value("layout/namedWorkspacesProtected").toByteArray().isEmpty();
}

QList<NamedWorkspace> SessionStore::namedWorkspaces(QString* error) const
{
    return loadNamedWorkspaces(error);
}

bool SessionStore::saveNamedWorkspace(const QString& rawName, const Session& session,
    QString* savedId, QString* error) const
{
    const QString name = rawName.trimmed();
    if (name.isEmpty() || name.size() > 64
        || std::any_of(name.cbegin(), name.cend(), [](QChar character) {
            return character.unicode() < 0x20;
        })) {
        setError(error, "Layout names must contain 1 to 64 printable characters.");
        return false;
    }
    QString validationError;
    if (!validateSession(session, &validationError)) {
        setError(error, validationError);
        return false;
    }
    QString loadError;
    QList<NamedWorkspace> records = loadNamedWorkspaces(&loadError);
    if (!loadError.isEmpty()) {
        setError(error, loadError);
        return false;
    }
    auto existing = std::find_if(records.begin(), records.end(), [&](const NamedWorkspace& record) {
        return record.name.compare(name, Qt::CaseInsensitive) == 0;
    });
    if (existing == records.end()) {
        if (records.size() >= MaximumNamedWorkspaces) {
            setError(error, "No more than 20 named layouts can be saved.");
            return false;
        }
        records.append({QUuid::createUuid().toString(QUuid::WithoutBraces), name, session,
            QDateTime::currentDateTimeUtc()});
        existing = std::prev(records.end());
    } else {
        existing->name = name;
        existing->session = session;
        existing->updatedAt = QDateTime::currentDateTimeUtc();
    }
    if (!writeNamedWorkspaces(records, error))
        return false;
    if (savedId)
        *savedId = existing->id;
    return setActiveNamedWorkspaceId(existing->id, error);
}

bool SessionStore::updateNamedWorkspace(
    const QString& id, const Session& session, QString* error) const
{
    QString validationError;
    if (!validateSession(session, &validationError)) {
        setError(error, validationError);
        return false;
    }
    QString loadError;
    QList<NamedWorkspace> records = loadNamedWorkspaces(&loadError);
    if (!loadError.isEmpty()) {
        setError(error, loadError);
        return false;
    }
    auto existing = std::find_if(records.begin(), records.end(),
        [&](const NamedWorkspace& record) { return record.id == id; });
    if (existing == records.end()) {
        setError(error, "The active named layout no longer exists.");
        return false;
    }
    existing->session = session;
    existing->updatedAt = QDateTime::currentDateTimeUtc();
    return writeNamedWorkspaces(records, error);
}

bool SessionStore::removeNamedWorkspace(const QString& id, QString* error) const
{
    QString loadError;
    QList<NamedWorkspace> records = loadNamedWorkspaces(&loadError);
    if (!loadError.isEmpty()) {
        setError(error, loadError);
        return false;
    }
    const qsizetype before = records.size();
    records.removeIf([&](const NamedWorkspace& record) { return record.id == id; });
    if (records.size() == before) {
        setError(error, "The named layout no longer exists.");
        return false;
    }
    if (!writeNamedWorkspaces(records, error))
        return false;
    if (activeNamedWorkspaceId() == id)
        return setActiveNamedWorkspaceId({}, error);
    return true;
}

QString SessionStore::activeNamedWorkspaceId() const
{
    return QSettings().value("layout/activeNamedWorkspaceId").toString();
}

bool SessionStore::setActiveNamedWorkspaceId(const QString& id, QString* error) const
{
    QSettings settings;
    if (id.isEmpty())
        settings.remove("layout/activeNamedWorkspaceId");
    else
        settings.setValue("layout/activeNamedWorkspaceId", id);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        setError(error, "The active named layout could not be updated.");
        return false;
    }
    return true;
}

bool SessionStore::clear(QString* error) const
{
    QSettings settings;
    settings.remove("layout");
    settings.remove("recovery/workspace");
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("The saved workspace state could not be removed.");
        return false;
    }
    return true;
}

} // namespace filemanager::settings
