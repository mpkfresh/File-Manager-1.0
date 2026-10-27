#include "installcleanup.h"

#include "previewidentity.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <userenv.h>
#endif

namespace filemanager::preview {
namespace {

constexpr int MaximumCleanupEntries = 4096;
constexpr int MaximumCleanupDepth = 16;
constexpr int CleanupRetryCount = 6;

#ifdef Q_OS_WIN
constexpr DWORD CleanupRetryDelayMilliseconds = 25;
QString windowsError(const QString& action, DWORD code)
{
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    const QString detail = length && message ? QString::fromWCharArray(message, length).trimmed()
                                             : QString::number(code);
    if (message)
        LocalFree(message);
    return action + ": " + detail;
}

QString extendedPath(const QString& path)
{
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    return QStringLiteral("\\\\?\\") + native;
}

bool removeLeaf(const QString& path, DWORD attributes, bool directory, QString* error)
{
    const QString native = extendedPath(path);
    if (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM)) {
        const DWORD writableAttributes = attributes
            & ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM);
        if (!SetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()), writableAttributes)) {
            if (error)
                *error = windowsError("The preview runtime attributes could not be cleared",
                    GetLastError());
            return false;
        }
    }
    DWORD failure = ERROR_SUCCESS;
    for (int attempt = 0; attempt < CleanupRetryCount; ++attempt) {
        const BOOL removed = directory
            ? RemoveDirectoryW(reinterpret_cast<LPCWSTR>(native.utf16()))
            : DeleteFileW(reinterpret_cast<LPCWSTR>(native.utf16()));
        if (removed)
            return true;
        failure = GetLastError();
        if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND)
            return true;
        const bool transient = failure == ERROR_ACCESS_DENIED
            || failure == ERROR_BUSY
            || failure == ERROR_DIR_NOT_EMPTY
            || failure == ERROR_LOCK_VIOLATION
            || failure == ERROR_SHARING_VIOLATION;
        if (!transient || attempt + 1 == CleanupRetryCount)
            break;
        Sleep(CleanupRetryDelayMilliseconds);
    }
    if (error)
        *error = windowsError("The preview runtime item could not be removed", failure);
    return false;
}

bool profileDeleteMissing(HRESULT result)
{
    return result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
        || result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

bool profileDeleteTransient(HRESULT result)
{
    return result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)
        || result == HRESULT_FROM_WIN32(ERROR_BUSY)
        || result == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION)
        || result == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
}

bool removeTree(const QString& path, int depth, int* visited, QString* error)
{
    if (depth > MaximumCleanupDepth || *visited >= MaximumCleanupEntries) {
        if (error)
            *error = "The preview runtime cleanup exceeded its safety bounds.";
        return false;
    }
    ++*visited;

    const QString native = extendedPath(path);
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return true;
        if (error)
            *error = windowsError("The preview runtime could not be inspected", code);
        return false;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (error)
            *error = "The preview runtime path is not a directory; cleanup was refused.";
        return false;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return removeLeaf(path, attributes, true, error);

    WIN32_FIND_DATAW item{};
    const QString pattern = native + QStringLiteral("\\*");
    HANDLE search = FindFirstFileW(reinterpret_cast<LPCWSTR>(pattern.utf16()), &item);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND) {
            if (error)
                *error = windowsError("The preview runtime could not be enumerated", code);
            return false;
        }
    } else {
        bool success = true;
        do {
            const QString name = QString::fromWCharArray(item.cFileName);
            if (name == QStringLiteral(".") || name == QStringLiteral(".."))
                continue;
            if (*visited >= MaximumCleanupEntries) {
                if (error)
                    *error = "The preview runtime cleanup exceeded its entry limit.";
                success = false;
                break;
            }
            const QString child = QDir(path).filePath(name);
            const bool directory = item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            const bool reparse = item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
            if (directory && !reparse) {
                if (!removeTree(child, depth + 1, visited, error)) {
                    success = false;
                    break;
                }
            } else {
                ++*visited;
                if (!removeLeaf(child, item.dwFileAttributes, directory, error)) {
                    success = false;
                    break;
                }
            }
        } while (FindNextFileW(search, &item));
        const DWORD enumerationCode = GetLastError();
        FindClose(search);
        if (!success)
            return false;
        if (enumerationCode != ERROR_NO_MORE_FILES) {
            if (error)
                *error = windowsError("The preview runtime enumeration failed", enumerationCode);
            return false;
        }
    }
    return removeLeaf(path, attributes, true, error);
}
#endif

} // namespace

QString installedPreviewRuntimePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QString::fromLatin1(PreviewRuntimeDirectoryName));
}

bool removePreviewRuntimeTree(const QString& runtimePath, QString* error)
{
    if (error)
        error->clear();
    const QFileInfo runtimeInfo(QDir::cleanPath(runtimePath));
    if (!runtimeInfo.isAbsolute()
            || runtimeInfo.fileName() != QString::fromLatin1(PreviewRuntimeDirectoryName)) {
        if (error)
            *error = "Cleanup is restricted to an absolute PreviewRuntime directory.";
        return false;
    }
#ifdef Q_OS_WIN
    int visited = 0;
    return removeTree(runtimeInfo.absoluteFilePath(), 0, &visited, error);
#else
    if (!runtimeInfo.exists())
        return true;
    const bool removed = QDir(runtimeInfo.absoluteFilePath()).removeRecursively();
    if (!removed && error)
        *error = "The preview runtime directory could not be removed.";
    return removed;
#endif
}

bool removeInstalledPreviewSandbox(QString* error)
{
    QStringList failures;
    QString runtimeError;
    if (!removePreviewRuntimeTree(installedPreviewRuntimePath(), &runtimeError))
        failures.append(runtimeError);
#ifdef Q_OS_WIN
    HRESULT profileResult = E_FAIL;
    for (int attempt = 0; attempt < CleanupRetryCount; ++attempt) {
        profileResult = DeleteAppContainerProfile(AppContainerProfileName);
        if (SUCCEEDED(profileResult) || profileDeleteMissing(profileResult)
                || !profileDeleteTransient(profileResult))
            break;
        Sleep(CleanupRetryDelayMilliseconds);
    }
    if (FAILED(profileResult) && !profileDeleteMissing(profileResult)) {
        failures.append(QString("The preview AppContainer profile could not be removed (0x%1).")
            .arg(static_cast<quint32>(profileResult), 8, 16, QLatin1Char('0')));
    }
#endif
    if (error)
        *error = failures.join(' ');
    return failures.isEmpty();
}

} // namespace filemanager::preview
