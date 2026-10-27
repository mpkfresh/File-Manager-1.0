#include "platform/shelllink.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#endif

namespace filemanager::platform {
namespace {

#ifdef Q_OS_WIN
QString systemMessage(HRESULT result)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const QString message = length && buffer
        ? QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed()
        : QStringLiteral("Windows Shell link operation failed (0x%1).")
            .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
    if (buffer)
        LocalFree(buffer);
    return message;
}

HRESULT saveShellLink(const QString& target, const QString& temporaryPath)
{
    if (QFileInfo(target).suffix().compare(QStringLiteral("lnk"), Qt::CaseInsensitive) == 0) {
        return CopyFileW(reinterpret_cast<LPCWSTR>(target.utf16()),
                   reinterpret_cast<LPCWSTR>(temporaryPath.utf16()), TRUE)
            ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    IShellLinkW* link = nullptr;
    HRESULT status = CoCreateInstance(
        CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(status))
        status = link->SetPath(reinterpret_cast<LPCWSTR>(target.utf16()));
    const QFileInfo targetInfo(target);
    if (SUCCEEDED(status)) {
        const QString description = QStringLiteral("Shortcut to %1").arg(targetInfo.fileName());
        status = link->SetDescription(reinterpret_cast<LPCWSTR>(description.utf16()));
    }
    if (SUCCEEDED(status)) {
        const QString workingDirectory = targetInfo.absolutePath();
        status = link->SetWorkingDirectory(
            reinterpret_cast<LPCWSTR>(workingDirectory.utf16()));
    }
    IPersistFile* persistent = nullptr;
    if (SUCCEEDED(status))
        status = link->QueryInterface(IID_PPV_ARGS(&persistent));
    if (SUCCEEDED(status)) {
        status = persistent->Save(
            reinterpret_cast<LPCWSTR>(temporaryPath.utf16()), TRUE);
    }
    if (persistent)
        persistent->Release();
    if (link)
        link->Release();
    return status;
}
#endif

QString linkName(const QString& target, int copyNumber)
{
    QString sourceName = QFileInfo(target).fileName();
    if (sourceName.isEmpty())
        sourceName = QStringLiteral("Shortcut");
    return copyNumber == 1
        ? QStringLiteral("%1 - Shortcut.lnk").arg(sourceName)
        : QStringLiteral("%1 - Shortcut (%2).lnk").arg(sourceName).arg(copyNumber);
}

} // namespace

ShellLinkBatchResult createShellLinks(
    const QStringList& absoluteTargets, const QString& destinationDirectory)
{
    ShellLinkBatchResult result;
    const QFileInfo destinationInfo(destinationDirectory);
    if (!destinationInfo.isAbsolute() || !destinationInfo.isDir()) {
        result.errors.append(QStringLiteral("The shortcut destination is unavailable."));
        return result;
    }
    if (absoluteTargets.isEmpty()) {
        result.errors.append(QStringLiteral("No shortcut targets were supplied."));
        return result;
    }
    for (const QString& target : absoluteTargets) {
        const QFileInfo targetInfo(target);
        if (!targetInfo.isAbsolute() || !targetInfo.exists()) {
            result.errors.append(QStringLiteral("The shortcut target is unavailable: %1")
                                     .arg(QDir::toNativeSeparators(target)));
        }
    }
    if (!result.errors.isEmpty())
        return result;

#ifdef Q_OS_WIN
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        result.errors.append(systemMessage(initialized));
        return result;
    }

    const QDir destination(destinationInfo.absoluteFilePath());
    for (const QString& target : absoluteTargets) {
        const QString temporaryPath = destination.filePath(
            QStringLiteral(".file-manager-%1.lnk")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        const HRESULT saved = saveShellLink(QFileInfo(target).absoluteFilePath(), temporaryPath);
        if (FAILED(saved)) {
            QFile::remove(temporaryPath);
            result.errors.append(QStringLiteral("Could not create a shortcut to %1: %2")
                                     .arg(QFileInfo(target).fileName(), systemMessage(saved)));
            continue;
        }

        bool moved = false;
        DWORD moveError = ERROR_FILE_EXISTS;
        for (int copyNumber = 1; copyNumber <= 10'000; ++copyNumber) {
            const QString finalPath = destination.filePath(linkName(target, copyNumber));
            if (MoveFileExW(reinterpret_cast<LPCWSTR>(temporaryPath.utf16()),
                    reinterpret_cast<LPCWSTR>(finalPath.utf16()), MOVEFILE_WRITE_THROUGH)) {
                result.createdPaths.append(finalPath);
                moved = true;
                break;
            }
            moveError = GetLastError();
            if (moveError != ERROR_ALREADY_EXISTS && moveError != ERROR_FILE_EXISTS)
                break;
        }
        if (!moved) {
            QFile::remove(temporaryPath);
            result.errors.append(QStringLiteral("Could not place the shortcut for %1: %2")
                                     .arg(QFileInfo(target).fileName(),
                                         systemMessage(HRESULT_FROM_WIN32(moveError))));
        }
    }
    CoUninitialize();
#else
    Q_UNUSED(destinationDirectory)
    result.errors.append(QStringLiteral("Windows Shell links are available only on Windows."));
#endif
    return result;
}

} // namespace filemanager::platform
