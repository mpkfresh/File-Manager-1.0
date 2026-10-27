#include "platform/windowsfolderintegration.h"

#include <QDir>
#include <QFileInfo>
#include <QUuid>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#endif

namespace filemanager::platform {

namespace {

constexpr auto productionClassesRoot = "Software\\Classes";
constexpr auto productionAppPathsRoot =
    "Software\\Microsoft\\Windows\\CurrentVersion\\App Paths";
constexpr auto verbName = "FileManager.Open";
constexpr auto ownerValue = "Kiatami.FileManager";

QString effectiveClassesRoot(const QString& requested)
{
    return requested.isEmpty() ? QString::fromLatin1(productionClassesRoot)
                               : QDir::toNativeSeparators(requested);
}

QString effectiveAppPathsRoot(const QString& requested)
{
    return requested.isEmpty() ? QString::fromLatin1(productionAppPathsRoot)
                               : QDir::toNativeSeparators(requested);
}

bool validClassesRoot(const QString& root)
{
    const QString normalized = QDir::toNativeSeparators(root);
    return normalized.startsWith("Software\\", Qt::CaseInsensitive)
        && !normalized.contains("..") && !normalized.endsWith(QLatin1Char('\\'))
        && normalized.size() <= 512;
}

QString verbKey(const QString& root, const QString& kind)
{
    return root + QLatin1Char('\\') + kind + "\\shell\\" + QString::fromLatin1(verbName);
}

#ifdef Q_OS_WIN
class RegistryKey final
{
public:
    RegistryKey() = default;
    ~RegistryKey() { if (key_) RegCloseKey(key_); }
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    HKEY* output() { return &key_; }
    HKEY get() const { return key_; }

private:
    HKEY key_ = nullptr;
};

QString systemError(LSTATUS status)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(status), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const QString message = length && buffer
        ? QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed()
        : QString("Windows registry error %1").arg(status);
    if (buffer)
        LocalFree(buffer);
    return message;
}

enum class KeyPresence { Missing, Present, Failed };

KeyPresence openKey(const QString& path, REGSAM access, RegistryKey& key, QString* error)
{
    const LSTATUS status = RegOpenKeyExW(
        HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(path.utf16()), 0, access, key.output());
    if (status == ERROR_FILE_NOT_FOUND)
        return KeyPresence::Missing;
    if (status != ERROR_SUCCESS) {
        if (error)
            *error = systemError(status);
        return KeyPresence::Failed;
    }
    return KeyPresence::Present;
}

bool readString(HKEY key, const wchar_t* name, QString* value)
{
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || type != REG_SZ
            || bytes < sizeof(wchar_t) || bytes > 64 * 1024
            || bytes % sizeof(wchar_t) != 0) {
        return false;
    }
    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    status = RegQueryValueExW(key, name, nullptr, &type,
        reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    // The value may shrink or change type between the size query and the read.
    // Only inspect returned bytes, and never accept a malformed ownership marker.
    if (status != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)
            || bytes % sizeof(wchar_t) != 0 || bytes > buffer.size() * sizeof(wchar_t))
        return false;
    buffer.resize(bytes / sizeof(wchar_t));
    if (buffer.back() != L'\0')
        return false;
    buffer.pop_back();
    *value = QString::fromStdWString(buffer);
    return true;
}

bool valueExists(HKEY key, const wchar_t* name)
{
    return RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

struct KeyStatus final
{
    bool exists = false;
    bool owned = false;
    bool current = false;
    QString error;
};

KeyStatus inspectVerbKey(const QString& path, const QString& expectedCommand,
    const QString& expectedExecutable)
{
    RegistryKey key;
    KeyStatus result;
    const KeyPresence presence = openKey(path, KEY_READ, key, &result.error);
    if (presence == KeyPresence::Missing)
        return result;
    if (presence == KeyPresence::Failed)
        return result;
    result.exists = true;
    QString owner;
    result.owned = readString(key.get(), L"FileManagerOwner", &owner)
        && owner == QString::fromLatin1(ownerValue);
    if (!result.owned)
        return result;

    QString icon;
    RegistryKey command;
    QString commandText;
    const KeyPresence commandPresence = openKey(path + "\\command", KEY_READ, command, nullptr);
    result.current = readString(key.get(), L"Icon", &icon)
        && icon.compare(expectedExecutable, Qt::CaseInsensitive) == 0
        && commandPresence == KeyPresence::Present
        && readString(command.get(), nullptr, &commandText)
        && commandText.compare(expectedCommand, Qt::CaseSensitive) == 0;
    return result;
}

bool writeString(HKEY key, const wchar_t* name, const QString& value, QString* error)
{
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = RegSetValueExW(key, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.utf16()), bytes);
    if (status == ERROR_SUCCESS)
        return true;
    if (error)
        *error = systemError(status);
    return false;
}

bool createVerbKey(const QString& path, const QString& executablePath,
    const QString& commandLine, bool* newlyCreated, QString* error)
{
    RegistryKey key;
    DWORD disposition = 0;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER,
        reinterpret_cast<const wchar_t*>(path.utf16()), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, key.output(), &disposition);
    Q_UNUSED(disposition);
    if (status != ERROR_SUCCESS) {
        if (error)
            *error = systemError(status);
        return false;
    }
    *newlyCreated = disposition == REG_CREATED_NEW_KEY;
    if (!*newlyCreated) {
        QString owner;
        if (!readString(key.get(), L"FileManagerOwner", &owner)
                || owner != QString::fromLatin1(ownerValue)) {
            if (error)
                *error = "The existing folder command is not owned by File Manager.";
            return false;
        }
    }
    if (!writeString(key.get(), L"FileManagerOwner", QString::fromLatin1(ownerValue), error)
            || !writeString(key.get(), nullptr, "Open in File Manager", error)
            || !writeString(key.get(), L"Icon", executablePath, error)
            || !writeString(key.get(), L"MultiSelectModel", "Single", error)) {
        return false;
    }

    RegistryKey command;
    status = RegCreateKeyExW(key.get(), L"command", 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_WRITE, nullptr, command.output(), &disposition);
    if (status != ERROR_SUCCESS) {
        if (error)
            *error = systemError(status);
        return false;
    }
    return writeString(command.get(), nullptr, commandLine, error);
}

KeyStatus inspectAppPathsKey(const QString& path, const QString& expectedExecutable)
{
    RegistryKey key;
    KeyStatus result;
    const KeyPresence presence = openKey(path, KEY_READ, key, &result.error);
    if (presence == KeyPresence::Missing)
        return result;
    if (presence == KeyPresence::Failed)
        return result;
    result.exists = true;
    QString owner;
    result.owned = readString(key.get(), L"FileManagerOwner", &owner)
        && owner == QString::fromLatin1(ownerValue);
    if (!result.owned)
        return result;
    QString registeredExecutable;
    result.current = readString(key.get(), nullptr, &registeredExecutable)
        && registeredExecutable.compare(expectedExecutable, Qt::CaseInsensitive) == 0
        && !valueExists(key.get(), L"Path");
    return result;
}

bool createAppPathsKey(const QString& path, const QString& executablePath,
    bool* newlyCreated, QString* error)
{
    RegistryKey key;
    DWORD disposition = 0;
    const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER,
        reinterpret_cast<const wchar_t*>(path.utf16()), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, key.output(), &disposition);
    if (status != ERROR_SUCCESS) {
        if (error)
            *error = systemError(status);
        return false;
    }
    *newlyCreated = disposition == REG_CREATED_NEW_KEY;
    if (!*newlyCreated) {
        QString owner;
        if (!readString(key.get(), L"FileManagerOwner", &owner)
                || owner != QString::fromLatin1(ownerValue)) {
            if (error)
                *error = "The existing App Paths entry is not owned by File Manager.";
            return false;
        }
    }
    if (!writeString(key.get(), L"FileManagerOwner", QString::fromLatin1(ownerValue), error)
            || !writeString(key.get(), nullptr, executablePath, error)) {
        return false;
    }
    const LSTATUS removePath = RegDeleteValueW(key.get(), L"Path");
    if (removePath != ERROR_SUCCESS && removePath != ERROR_FILE_NOT_FOUND) {
        if (error)
            *error = systemError(removePath);
        return false;
    }
    return true;
}

bool removeVerbKey(const QString& path, QString* error)
{
    const LSTATUS status = RegDeleteTreeW(
        HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(path.utf16()));
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND)
        return true;
    if (error)
        *error = systemError(status);
    return false;
}

enum class CleanupOwnership { Preserve, Owned, Failed };

// Missing command values are allowed after an interrupted cleanup, but a
// malformed or replaced command is never treated as absence.
bool readOptionalString(HKEY key, const wchar_t* name, QString* value,
    bool* missing, QString* error)
{
    const LSTATUS status = RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr);
    *missing = status == ERROR_FILE_NOT_FOUND;
    if (*missing)
        return true;
    if (status != ERROR_SUCCESS) {
        *error = systemError(status);
        return false;
    }
    return readString(key, name, value);
}

CleanupOwnership cleanupOwnership(const QString& path, bool appPaths,
    const QString& executable, QString* error)
{
    RegistryKey key;
    const auto presence = openKey(path, KEY_READ, key, error);
    if (presence == KeyPresence::Failed)
        return CleanupOwnership::Failed;
    if (presence == KeyPresence::Missing)
        return CleanupOwnership::Preserve;
    QString owner;
    if (!readString(key.get(), L"FileManagerOwner", &owner)
        || owner != QString::fromLatin1(ownerValue))
        return CleanupOwnership::Preserve;

    QString target;
    if (appPaths) {
        return readString(key.get(), nullptr, &target)
                && target.compare(executable, Qt::CaseInsensitive) == 0
            ? CleanupOwnership::Owned : CleanupOwnership::Preserve;
    }

    QString icon;
    bool iconMissing = false;
    if (!readOptionalString(key.get(), L"Icon", &icon, &iconMissing, error))
        return error->isEmpty() ? CleanupOwnership::Preserve : CleanupOwnership::Failed;
    // A conflicting installation path is preserved even if the command matches.
    if (!iconMissing && icon.compare(executable, Qt::CaseInsensitive) != 0)
        return CleanupOwnership::Preserve;
    RegistryKey command;
    const auto commandPresence = openKey(path + "\\command", KEY_READ, command, error);
    if (commandPresence == KeyPresence::Failed)
        return CleanupOwnership::Failed;
    bool commandMissing = commandPresence == KeyPresence::Missing;
    if (!commandMissing
        && !readOptionalString(command.get(), nullptr, &target, &commandMissing, error))
        return error->isEmpty() ? CleanupOwnership::Preserve : CleanupOwnership::Failed;
    if (!commandMissing) {
        return target.compare(windowsFolderCommandLine(executable), Qt::CaseInsensitive) == 0
            ? CleanupOwnership::Owned : CleanupOwnership::Preserve;
    }
    return !iconMissing ? CleanupOwnership::Owned : CleanupOwnership::Preserve;
}

bool selectedAsDefault(const QString& classesRoot, const QString& kind,
    bool production, bool* selected, QString* error)
{
    RegistryKey shell;
    const QString path = (production ? kind : classesRoot + '\\' + kind) + "\\shell";
    const LSTATUS status = RegOpenKeyExW(production ? HKEY_CLASSES_ROOT : HKEY_CURRENT_USER,
        reinterpret_cast<const wchar_t*>(path.utf16()), 0, KEY_READ, shell.output());
    *selected = false;
    if (status == ERROR_FILE_NOT_FOUND)
        return true;
    if (status != ERROR_SUCCESS) {
        *error = systemError(status);
        return false;
    }
    QString value;
    bool missing = false;
    if (!readOptionalString(shell.get(), nullptr, &value, &missing, error)) {
        if (error->isEmpty())
            *error = "The folder's default verb is malformed; restore it before uninstalling.";
        return false;
    }
    for (const QString& verb : value.split(',')) {
        if (verb.trimmed().compare(QString::fromLatin1(verbName), Qt::CaseInsensitive) == 0)
            *selected = true;
    }
    return true;
}

bool removeMatchingValue(HKEY key, const wchar_t* name, const QString& expected,
    QString* error, Qt::CaseSensitivity sensitivity = Qt::CaseSensitive)
{
    QString current;
    bool missing = false;
    if (!readOptionalString(key, name, &current, &missing, error))
        return error->isEmpty(); // Preserve malformed later-added data.
    if (missing || current.compare(expected, sensitivity) != 0)
        return true;
    const LSTATUS status = RegDeleteValueW(key, name);
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND)
        return true;
    *error = systemError(status);
    return false;
}

bool removeEmptyKey(const QString& path, QString* error)
{
    RegistryKey key;
    const auto presence = openKey(path, KEY_READ, key, error);
    if (presence == KeyPresence::Missing)
        return true;
    if (presence == KeyPresence::Failed)
        return false;
    DWORD children = 0;
    DWORD values = 0;
    LSTATUS status = RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr,
        &children, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_SUCCESS && (children || values))
        return true;
    if (status == ERROR_SUCCESS)
        status = RegDeleteKeyW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(path.utf16()));
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND)
        return true;
    *error = systemError(status);
    return false;
}

bool cleanupRegistration(const QString& path, bool appPaths,
    const QString& executable, QString* error)
{
    // Recheck ownership immediately before mutation. Registry writes by other
    // programs are not covered by our process; this is not an atomic transaction.
    const auto ownership = cleanupOwnership(path, appPaths, executable, error);
    if (ownership != CleanupOwnership::Owned)
        return ownership != CleanupOwnership::Failed;
    {
        RegistryKey key;
        if (openKey(path, KEY_READ | KEY_SET_VALUE, key, error) != KeyPresence::Present)
            return false;
        if (appPaths) {
            if (!removeMatchingValue(key.get(), nullptr, executable, error, Qt::CaseInsensitive))
                return false;
        } else {
            const QString commandPath = path + "\\command";
            {
                RegistryKey command;
                const auto presence = openKey(commandPath, KEY_READ | KEY_SET_VALUE, command, error);
                if (presence == KeyPresence::Failed)
                    return false;
                if (presence == KeyPresence::Present
                    && !removeMatchingValue(command.get(), nullptr,
                        windowsFolderCommandLine(executable), error, Qt::CaseInsensitive))
                    return false;
            }
            if (!removeEmptyKey(commandPath, error)
                || !removeMatchingValue(key.get(), nullptr, "Open in File Manager", error)
                || !removeMatchingValue(key.get(), L"MultiSelectModel", "Single", error)
                || !removeMatchingValue(key.get(), L"Icon", executable, error, Qt::CaseInsensitive))
                return false;
        }
        if (!removeMatchingValue(key.get(), L"FileManagerOwner", QString::fromLatin1(ownerValue), error))
            return false;
    }
    return removeEmptyKey(path, error);
}
#endif

} // namespace

QString windowsFolderCommandLine(const QString& executablePath)
{
    if (executablePath.contains('"'))
        return {};
    return '"' + QDir::toNativeSeparators(executablePath) + "\" \"%1\"";
}

FolderCommandStatus windowsFolderCommandStatus(
    const QString& executablePath, const QString& classesRoot)
{
    const QString root = effectiveClassesRoot(classesRoot);
    if (!validClassesRoot(root))
        return {false, false, false, "The folder-command registry scope is invalid."};
    const QFileInfo executable(QDir::cleanPath(QDir::fromNativeSeparators(executablePath)));
    const QString command = windowsFolderCommandLine(executable.absoluteFilePath());
    if (!executable.isAbsolute() || command.isEmpty())
        return {false, false, false, "The File Manager executable path is invalid."};

#ifdef Q_OS_WIN
    const QString expectedExecutable = QDir::toNativeSeparators(executable.absoluteFilePath());
    const KeyStatus directory = inspectVerbKey(
        verbKey(root, "Directory"), command, expectedExecutable);
    const KeyStatus drive = inspectVerbKey(verbKey(root, "Drive"), command, expectedExecutable);
    if (!directory.error.isEmpty() || !drive.error.isEmpty())
        return {false, false, false,
            !directory.error.isEmpty() ? directory.error : drive.error};
    if ((directory.exists && !directory.owned) || (drive.exists && !drive.owned)) {
        return {directory.exists || drive.exists, false, false,
            "A folder command with this name exists but is not owned by File Manager."};
    }
    const bool registered = directory.exists || drive.exists;
    const bool current = directory.exists && drive.exists && directory.current && drive.current;
    return {registered, current, true,
        registered && !current ? "The File Manager folder command needs repair." : QString{}};
#else
    Q_UNUSED(root);
    return {false, false, false, "Folder-command integration is available only on Windows."};
#endif
}

FolderCommandResult setWindowsFolderCommandRegistered(bool enabled,
    const QString& executablePath, const QString& classesRoot)
{
    const QString root = effectiveClassesRoot(classesRoot);
    const FolderCommandStatus existing = windowsFolderCommandStatus(executablePath, root);
    if (!existing.manageable)
        return {false, existing.detail};
    const QFileInfo executable(QDir::cleanPath(QDir::fromNativeSeparators(executablePath)));
    if (enabled && (!executable.isAbsolute() || !executable.exists() || !executable.isFile()))
        return {false, "The File Manager executable is unavailable."};

#ifdef Q_OS_WIN
    QString error;
    const QString directoryKey = verbKey(root, "Directory");
    const QString driveKey = verbKey(root, "Drive");
    if (enabled) {
        const QString executableNative = QDir::toNativeSeparators(executable.absoluteFilePath());
        const QString command = windowsFolderCommandLine(executableNative);
        bool directoryCreated = false;
        bool driveCreated = false;
        if (!createVerbKey(directoryKey, executableNative, command, &directoryCreated, &error)) {
            if (directoryCreated)
                removeVerbKey(directoryKey, nullptr);
            return {false, "The Windows folder command could not be registered: " + error};
        }
        if (!createVerbKey(driveKey, executableNative, command, &driveCreated, &error)) {
            if (driveCreated)
                removeVerbKey(driveKey, nullptr);
            if (directoryCreated)
                removeVerbKey(directoryKey, nullptr);
            return {false, "The Windows folder command could not be registered: " + error};
        }
    } else if (!removeVerbKey(directoryKey, &error) || !removeVerbKey(driveKey, &error)) {
        return {false, "The Windows folder command could not be removed: " + error};
    }
    if (root.compare(QString::fromLatin1(productionClassesRoot), Qt::CaseInsensitive) == 0)
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return {true, {}};
#else
    Q_UNUSED(enabled);
    return {false, "Folder-command integration is available only on Windows."};
#endif
}

AppPathsStatus windowsAppPathsStatus(
    const QString& executablePath, const QString& appPathsRoot)
{
    const QString root = effectiveAppPathsRoot(appPathsRoot);
    if (!validClassesRoot(root))
        return {false, false, false, "The App Paths registry scope is invalid."};
    const QFileInfo executable(QDir::cleanPath(QDir::fromNativeSeparators(executablePath)));
    if (!executable.isAbsolute()
            || executable.suffix().compare("exe", Qt::CaseInsensitive) != 0) {
        return {false, false, false, "The File Manager executable path is invalid."};
    }
#ifdef Q_OS_WIN
    const QString nativeExecutable = QDir::toNativeSeparators(executable.absoluteFilePath());
    const QString keyPath = root + QLatin1Char('\\') + executable.fileName();
    const KeyStatus status = inspectAppPathsKey(keyPath, nativeExecutable);
    if (!status.error.isEmpty())
        return {false, false, false, status.error};
    if (status.exists && !status.owned) {
        return {true, false, false,
            "An App Paths entry with this executable name is not owned by File Manager."};
    }
    return {status.exists, status.current, true,
        status.exists && !status.current ? "The File Manager App Paths entry needs repair."
                                         : QString{}};
#else
    Q_UNUSED(root);
    return {false, false, false, "App Paths integration is available only on Windows."};
#endif
}

AppPathsResult setWindowsAppPathsRegistered(bool enabled,
    const QString& executablePath, const QString& appPathsRoot)
{
    const QString root = effectiveAppPathsRoot(appPathsRoot);
    const AppPathsStatus existing = windowsAppPathsStatus(executablePath, root);
    if (!existing.manageable)
        return {false, existing.detail};
    const QFileInfo executable(QDir::cleanPath(QDir::fromNativeSeparators(executablePath)));
    if (enabled && (!executable.exists() || !executable.isFile()))
        return {false, "The File Manager executable is unavailable."};
#ifdef Q_OS_WIN
    const QString keyPath = root + QLatin1Char('\\') + executable.fileName();
    QString error;
    if (enabled) {
        bool newlyCreated = false;
        if (!createAppPathsKey(keyPath,
                QDir::toNativeSeparators(executable.absoluteFilePath()),
                &newlyCreated, &error)) {
            if (newlyCreated)
                removeVerbKey(keyPath, nullptr);
            return {false, "The Windows App Paths entry could not be registered: " + error};
        }
    } else if (!removeVerbKey(keyPath, &error)) {
        return {false, "The Windows App Paths entry could not be removed: " + error};
    }
    if (root.compare(QString::fromLatin1(productionAppPathsRoot), Qt::CaseInsensitive) == 0)
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return {true, {}};
#else
    Q_UNUSED(enabled);
    return {false, "App Paths integration is available only on Windows."};
#endif
}

FolderCommandResult removeWindowsIntegrationForInstallation(
    const QString& executablePath, const QString& fixtureRoot)
{
    const QString prefix = "Software\\FileManagerTests\\FolderIntegration\\";
    const QString fixture = QDir::toNativeSeparators(fixtureRoot);
    const QString id = fixture.mid(prefix.size());
    if (!fixture.isEmpty() && (!fixture.startsWith(prefix)
            || QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id))
        return {false, "The uninstall registry fixture scope is invalid."};
    const QFileInfo executable(QDir::cleanPath(QDir::fromNativeSeparators(executablePath)));
    if (!executable.isAbsolute() || executablePath.contains('"')
        || executablePath.contains(QChar::Null)
        || executable.suffix().compare("exe", Qt::CaseInsensitive) != 0)
        return {false, "The File Manager executable path is invalid."};
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(executable.absoluteFilePath());
    const QString classes = fixture.isEmpty() ? effectiveClassesRoot({}) : fixture + "\\Classes";
    const QString appPaths = fixture.isEmpty() ? effectiveAppPathsRoot({}) : fixture + "\\App Paths";
    QString error;
    // Preflight both folder types before removing any registration.
    for (const auto* kind : {"Directory", "Drive"}) {
        const auto ownership = cleanupOwnership(verbKey(classes, kind), false, native, &error);
        if (ownership == CleanupOwnership::Failed)
            return {false, error};
        if (ownership == CleanupOwnership::Owned) {
            bool selected = false;
            if (!selectedAsDefault(classes, kind, fixture.isEmpty(), &selected, &error))
                return {false, error};
            if (selected)
                return {false, "Open in File Manager is selected as a folder default. "
                    "Restore a working default before uninstalling."};
        }
    }
    bool success = true;
    for (const auto* kind : {"Directory", "Drive"}) {
        if (!cleanupRegistration(verbKey(classes, kind), false, native, &error)) {
            success = false;
            break;
        }
    }
    if (success)
        success = cleanupRegistration(appPaths + '\\' + executable.fileName(), true, native, &error);
    if (fixture.isEmpty())
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return {success, error};
#else
    return {false, "Windows integration cleanup is available only on Windows."};
#endif
}

} // namespace filemanager::platform
