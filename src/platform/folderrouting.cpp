#include "folderrouting.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QStandardPaths>
#include <algorithm>
#include <stdexcept>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#endif

namespace filemanager::platform {
QString folderRoutingCommand(const QString& executable)
{
    if (!QFileInfo(executable).isAbsolute() || executable.contains('"')
        || executable.contains(QChar::Null)) return {};
    return '"' + QDir::toNativeSeparators(QFileInfo(executable).absoluteFilePath())
        + "\" --open-folder -- \"%1\\.\"";
}
#ifdef Q_OS_WIN
namespace {
constexpr auto verb = "FileManager.DefaultFolder";
constexpr qsizetype maxBytes = 65536;
constexpr qsizetype maxJournal = 1024 * 1024;
struct Raw {
    bool exists = false;
    DWORD type = 0;
    QByteArray bytes;
    bool operator==(const Raw&) const = default;
};
struct Value { QString key; QString name; Raw before; Raw applied; };
struct Key { HKEY handle = nullptr; ~Key() { if (handle) RegCloseKey(handle); } };
[[noreturn]] void fail(const QString& error) { throw std::runtime_error(error.toUtf8().constData()); }
LPCWSTR wide(const QString& text) { return reinterpret_cast<LPCWSTR>(text.utf16()); }
QString native(const QString& path) { return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()); }
void check(LSTATUS status, const QString& operation)
{
    if (status != ERROR_SUCCESS) fail(operation + QStringLiteral(" (Windows error %1).").arg(status));
}
struct Scope {
    QString classes;
    QString journal;
    QString installer;
    bool fixture = false;
};
Scope scope(const QString& root)
{
    if (root.isEmpty()) return {"Software\\Classes", "Software\\Kiatami\\File Manager\\FolderRouting",
        "Software\\Kiatami\\FileManager\\Installer", false};
    const QString prefix = "Software\\FileManagerTests\\FolderRouting_";
    const QByteArray suffix = root.mid(prefix.size()).toLatin1();
    if (!root.startsWith(prefix) || suffix.size() != 32
        || !std::all_of(suffix.cbegin(), suffix.cend(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) fail("Invalid isolated folder-routing fixture scope.");
    return {root + "\\Classes", root + "\\Journal", root + "\\Installer", true};
}
bool exists(const QString& path)
{
    Key key;
    const auto result = RegOpenKeyExW(HKEY_CURRENT_USER, wide(path), 0, KEY_READ, &key.handle);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return false;
    check(result, "Cannot inspect folder-routing key");
    return true;
}
Raw read(const QString& path, const QString& name = {}, qsizetype limit = maxBytes)
{
    Key key;
    auto result = RegOpenKeyExW(HKEY_CURRENT_USER, wide(path), 0, KEY_QUERY_VALUE, &key.handle);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return {};
    check(result, "Cannot read folder-routing state");
    Raw value;
    DWORD size = 0;
    result = RegQueryValueExW(key.handle, wide(name), nullptr, &value.type, nullptr, &size);
    if (result == ERROR_FILE_NOT_FOUND) return {};
    check(result, "Cannot read folder-routing value");
    if (size > limit) fail("Folder-routing registry data exceeds the recovery limit.");
    value.bytes.resize(size);
    check(RegQueryValueExW(key.handle, wide(name), nullptr, &value.type,
        reinterpret_cast<BYTE*>(value.bytes.data()), &size), "Folder-routing value changed during reading");
    value.bytes.resize(size);
    value.exists = true;
    return value;
}
Raw text(const QString& string)
{
    return {true, REG_SZ, QByteArray(reinterpret_cast<const char*>(string.utf16()),
        (string.size() + 1) * static_cast<qsizetype>(sizeof(wchar_t)))};
}
void write(const QString& path, const QString& name, const Raw& value)
{
    if (!value.exists && !exists(path)) return;
    Key key;
    check(RegCreateKeyExW(HKEY_CURRENT_USER, wide(path), 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, nullptr, &key.handle, nullptr), "Cannot open folder-routing key");
    const auto result = value.exists ? RegSetValueExW(key.handle, wide(name), 0, value.type,
        reinterpret_cast<const BYTE*>(value.bytes.constData()), static_cast<DWORD>(value.bytes.size()))
        : RegDeleteValueW(key.handle, wide(name));
    if (result != ERROR_FILE_NOT_FOUND || value.exists) check(result, "Cannot update folder-routing value");
    check(RegFlushKey(key.handle), "Cannot persist folder-routing state");
}
bool removeEmptyKey(const QString& path)
{
    if (!exists(path)) return true;
    Key key;
    check(RegOpenKeyExW(HKEY_CURRENT_USER, wide(path), 0, KEY_READ, &key.handle), "Cannot inspect recovery key");
    DWORD subkeys = 0, values = 0;
    check(RegQueryInfoKeyW(key.handle, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
        &values, nullptr, nullptr, nullptr, nullptr), "Cannot inspect recovery key contents");
    if (subkeys || values) return false;
    RegCloseKey(key.handle); key.handle = nullptr;
    const auto result = RegDeleteKeyW(HKEY_CURRENT_USER, wide(path));
    if (result == ERROR_FILE_NOT_FOUND) return true;
    check(result, "Cannot remove empty recovery key");
    return true;
}
QString shell(const Scope& s, const QString& kind) { return s.classes + '\\' + kind + "\\shell"; }
std::vector<Value> plan(const Scope& s, const QString& executable)
{
    std::vector<Value> result;
    for (const QString& kind : {QStringLiteral("Directory"), QStringLiteral("Drive")}) {
        const QString handler = shell(s, kind) + '\\' + verb;
        result.push_back({handler, {}, {}, text("Open with File Manager")});
        result.push_back({handler, "FileManagerOwner", {}, text(native(executable))});
        result.push_back({handler, "Icon", {}, text(native(executable))});
        result.push_back({handler, "MultiSelectModel", {}, text("Single")});
        result.push_back({handler + "\\command", {}, {}, text(folderRoutingCommand(executable))});
    }
    for (const QString& kind : {QStringLiteral("Directory"), QStringLiteral("Drive")})
        result.push_back({shell(s, kind), {}, {}, text(verb)});
    return result;
}
QStringList keys(const Scope& s)
{
    QStringList result;
    for (const QString& kind : {QStringLiteral("Directory"), QStringLiteral("Drive")})
        result << s.classes + '\\' + kind << shell(s, kind) << shell(s, kind) + '\\' + verb
               << shell(s, kind) + '\\' + verb + "\\command";
    return result;
}
QJsonObject encode(const Raw& value)
{
    return {{"exists", value.exists}, {"type", static_cast<qint64>(value.type)},
        {"bytes", QString::fromLatin1(value.bytes.toBase64())}};
}
Raw decode(const QJsonValue& value)
{
    const auto object = value.toObject();
    const qint64 type = object.value("type").toInteger(-1);
    const auto encoded = object.value("bytes").toString().toLatin1();
    const auto bytes = QByteArray::fromBase64(encoded);
    if (object.size() != 3 || !object.value("exists").isBool() || !object.value("bytes").isString()
        || type < 0 || type > MAXDWORD || bytes.size() > maxBytes || bytes.toBase64() != encoded)
        fail("Invalid raw value in folder-routing recovery record.");
    if (!object.value("exists").toBool() && (type != 0 || !bytes.isEmpty()))
        fail("Invalid absent value in folder-routing recovery record.");
    return {object.value("exists").toBool(), static_cast<DWORD>(type), bytes};
}
struct Journal {
    QString phase;
    std::vector<Value> entries;
    QStringList createdKeys;
};
void store(const Scope& s, const QString& executable, const Journal& journal)
{
    QJsonArray values;
    for (const auto& entry : journal.entries)
        values.append(QJsonObject{{"key", entry.key}, {"name", entry.name},
            {"before", encode(entry.before)}, {"applied", encode(entry.applied)}});
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"owner", native(executable)},
        {"phase", journal.phase}, {"values", values},
        {"createdKeys", QJsonArray::fromStringList(journal.createdKeys)}}).toJson(QJsonDocument::Compact);
    if (bytes.size() > maxJournal) fail("Folder-routing recovery record exceeds its size limit.");
    write(s.journal, "Journal", {true, REG_BINARY, bytes});
}
QJsonObject journalObject(const Raw& value)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(value.bytes, &error);
    if (value.type != REG_BINARY || error.error != QJsonParseError::NoError || !document.isObject())
        fail("The folder-routing recovery record is corrupt. It has been preserved.");
    return document.object();
}
Journal load(const Scope& s, const QString& executable, const Raw& value)
{
    const auto object = journalObject(value);
    if (object.value("schema").toInt() != 1 || object.value("owner").toString().compare(native(executable), Qt::CaseInsensitive) != 0)
        fail("Folder routing belongs to another installation or an unsupported recovery format.");
    Journal journal{object.value("phase").toString(), plan(s, executable), {}};
    if (journal.phase != "applying" && journal.phase != "enabled" && journal.phase != "restoring")
        fail("Unknown folder-routing recovery phase.");
    const auto values = object.value("values").toArray();
    if (values.size() != static_cast<qsizetype>(journal.entries.size())) fail("Invalid folder-routing recovery count.");
    for (qsizetype i = 0; i < values.size(); ++i) {
        auto& expected = journal.entries[static_cast<size_t>(i)];
        const auto item = values[i].toObject();
        if (item.value("key").toString() != expected.key || item.value("name").toString() != expected.name
            || !(decode(item.value("applied")) == expected.applied)) fail("Recovery entry is outside the owned registration scope.");
        expected.before = decode(item.value("before"));
        if (i < 10 && expected.before.exists) fail("The recovery handler was not originally empty.");
    }
    const auto permitted = keys(s);
    if (!object.value("createdKeys").isArray()) fail("Missing created-key recovery scope.");
    int lastIndex = -1;
    for (const auto item : object.value("createdKeys").toArray()) {
        const int index = static_cast<int>(permitted.indexOf(item.toString()));
        if (!item.isString() || index <= lastIndex) fail("Invalid created-key recovery order.");
        journal.createdKeys << item.toString();
        lastIndex = index;
    }
    return journal;
}
bool installed(const Scope& s, const QString& executable)
{
    if (!QFileInfo(executable).isFile()) return false;
    if (s.fixture) return true;
    const Raw marker = read(s.installer, "InstallPath");
    if (!marker.exists || marker.type != REG_SZ || marker.bytes.size() < 2
        || marker.bytes.size() % 2 != 0 || !marker.bytes.endsWith(QByteArray(2, '\0'))) return false;
    const QString path = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(marker.bytes.constData()),
        marker.bytes.size() / 2 - 1);
    return !path.contains(QChar::Null) && QFileInfo(path).isAbsolute()
        && QDir::cleanPath(QDir::fromNativeSeparators(path)).compare(
            QFileInfo(executable).absolutePath(), Qt::CaseInsensitive) == 0;
}
bool orphan(const Scope& s)
{
    for (const QString& kind : {QStringLiteral("Directory"), QStringLiteral("Drive")})
        if (exists(shell(s, kind) + '\\' + verb) || read(shell(s, kind)) == text(verb)) return true;
    return false;
}
bool restore(const Scope& s, const QString& executable, Journal& journal)
{
    journal.phase = "restoring";
    store(s, executable, journal);
    bool clean = true;
    for (auto it = journal.entries.rbegin(); it != journal.entries.rend(); ++it) {
        const bool isDefault = it->key == shell(s, "Directory") || it->key == shell(s, "Drive");
        if (!clean && !isDefault) break; // Keep handlers while a default is unresolved.
        const Raw current = read(it->key, it->name);
        if (current == it->before) continue;
        if (!(current == it->applied)) { clean = false; continue; }
        write(it->key, it->name, it->before);
        if (!(read(it->key, it->name) == it->before)) clean = false;
    }
    if (clean) {
        for (auto it = journal.createdKeys.crbegin(); it != journal.createdKeys.crend(); ++it)
            if (!removeEmptyKey(*it)) clean = false;
    }
    if (clean) write(s.journal, "Journal", {});
    return clean;
}
struct Notify {
    bool production;
    ~Notify() { if (production) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }
};
}
#endif

FolderRoutingStatus folderRoutingStatus(const QString& executable, const QString& fixtureRoot)
{
#ifdef Q_OS_WIN
    try {
        const auto s = scope(fixtureRoot);
        if (folderRoutingCommand(executable).isEmpty()) fail("Invalid executable path for folder routing.");
        const auto saved = read(s.journal, "Journal", maxJournal);
        if (!saved.exists) {
            if (orphan(s)) return {FolderRoutingState::Conflict, false, false,
                "Folder routing exists without this installation's recovery record. Existing settings were preserved."};
            const bool eligible = installed(s, executable);
            return {FolderRoutingState::Off, false, eligible, eligible ? "Windows folder routing is unchanged."
                : "Install or repair File Manager with its installer to enable default-folder opening."};
        }
        const auto object = journalObject(saved);
        const QString owner = object.value("owner").toString();
        if (owner.isEmpty()) fail("The folder-routing recovery record has no owner.");
        if (folderRoutingCommand(owner).isEmpty()) fail("Invalid owner in the folder-routing recovery record.");
        if (owner.compare(native(executable), Qt::CaseInsensitive) != 0) {
            (void)load(s, owner, saved);
            return {FolderRoutingState::OtherInstallation, false, false, "Folder routing belongs to another File Manager installation."};
        }
        const auto journal = load(s, executable, saved);
        bool current = true;
        for (const auto& item : journal.entries) current &= read(item.key, item.name) == item.applied;
        if (current && journal.phase == "enabled")
            return {FolderRoutingState::Enabled, true, true, "Supported folder and drive launches use File Manager."};
        return {FolderRoutingState::RecoveryNeeded, true, false,
            "Folder routing needs recovery. Restore previous behavior before enabling it again; later changes will be preserved."};
    } catch (const std::exception& error) {
        return {FolderRoutingState::Unavailable, false, false, QString::fromUtf8(error.what())};
    }
#else
    Q_UNUSED(executable); Q_UNUSED(fixtureRoot);
    return {FolderRoutingState::Unavailable, false, false, "Folder routing is available only on Windows."};
#endif
}

FolderRoutingResult setFolderRouting(bool enabled, const QString& executable, const QString& fixtureRoot)
{
#ifdef Q_OS_WIN
    try {
        const auto s = scope(fixtureRoot);
        if (folderRoutingCommand(executable).isEmpty()) fail("Invalid executable path for folder routing.");
        const QString locks = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + "/Kiatami/FileManager/IntegrationLocks";
        if (!QDir().mkpath(locks)) fail("Cannot create the per-user integration lock directory.");
        const QString digest = QString::fromLatin1(QCryptographicHash::hash(s.journal.toUtf8(), QCryptographicHash::Sha256).toHex());
        QLockFile lock(locks + '/' + digest + ".lock");
        if (!lock.tryLock(0)) fail("Another File Manager process is updating folder routing. Try again after it finishes.");
        const auto saved = read(s.journal, "Journal", maxJournal);
        Notify notify{!s.fixture};
        if (!enabled) {
            if (!saved.exists) {
                if (orphan(s)) fail("No recovery record exists for the current routing; existing values were preserved.");
                return {true, {}};
            }
            auto journal = load(s, executable, saved);
            if (!restore(s, executable, journal)) fail("Later registry changes prevent complete restoration. Conflicting values and the recovery record were preserved.");
            return {true, {}};
        }
        if (!installed(s, executable)) fail("Default-folder opening requires this installed copy of File Manager. Install or repair it first.");
        if (saved.exists) {
            const auto status = folderRoutingStatus(executable, fixtureRoot);
            if (status.state == FolderRoutingState::Enabled && status.owned) return {true, {}};
            fail(status.detail);
        }
        if (orphan(s)) fail("An existing folder handler or default has no matching recovery record. It was not overwritten.");
        Journal journal{"applying", plan(s, executable), {}};
        for (auto& item : journal.entries) item.before = read(item.key, item.name);
        for (size_t i = 0; i < 10; ++i)
            if (journal.entries[i].before.exists) fail("The folder handler changed before setup. It was preserved.");
        // Never source prior defaults from HKCR; preserve raw HKCU absence/type/bytes.
        store(s, executable, journal);
        try {
            for (const auto& path : keys(s)) {
                Key key;
                DWORD disposition = 0;
                check(RegCreateKeyExW(HKEY_CURRENT_USER, wide(path), 0, nullptr, REG_OPTION_NON_VOLATILE,
                    KEY_READ, nullptr, &key.handle, &disposition), "Cannot prepare folder-routing key");
                if (disposition == REG_CREATED_NEW_KEY) {
                    journal.createdKeys << path;
                    store(s, executable, journal);
                } else if (path.contains(QStringLiteral("\\") + verb)) {
                    fail("A folder handler appeared during setup. It was not adopted.");
                }
            }
            for (const auto& item : journal.entries) {
                if (item.key == shell(s, "Directory") || item.key == shell(s, "Drive")) {
                    for (size_t i = 0; i < 10; ++i) {
                        const auto& handler = journal.entries[i];
                        if (!(read(handler.key, handler.name) == handler.applied))
                            fail("The prepared folder handler changed before activation.");
                    }
                }
                if (!(read(item.key, item.name) == item.before)) fail("A registration value changed during setup.");
                write(item.key, item.name, item.applied);
            }
            for (const auto& item : journal.entries)
                if (!(read(item.key, item.name) == item.applied)) fail("Folder routing changed before setup completed.");
            journal.phase = "enabled";
            store(s, executable, journal);
        } catch (...) {
            try { restore(s, executable, journal); } catch (...) { /* Retain durable recovery record. */ }
            throw;
        }
        return {true, {}};
    } catch (const std::exception& error) { return {false, QString::fromUtf8(error.what())}; }
#else
    Q_UNUSED(enabled); Q_UNUSED(executable); Q_UNUSED(fixtureRoot);
    return {false, "Folder routing is available only on Windows."};
#endif
}
} // namespace filemanager::platform
