#include "networkdriveui.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <winnetwk.h>
#endif

namespace filemanager::platform {
namespace {

#ifdef Q_OS_WIN
QString windowsErrorMessage(DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    QString message = size && buffer ? QString::fromWCharArray(buffer, static_cast<int>(size)).trimmed()
                                     : QStringLiteral("Windows error %1").arg(code);
    if (buffer)
        LocalFree(buffer);
    return message;
}

NetworkDriveDialogResult translateResult(DWORD result, QString* error)
{
    if (error)
        error->clear();
    if (result == NO_ERROR)
        return NetworkDriveDialogResult::Completed;
    if (result == ERROR_CANCELLED)
        return NetworkDriveDialogResult::Cancelled;
    if (error)
        *error = windowsErrorMessage(result);
    return NetworkDriveDialogResult::Failed;
}
#endif

} // namespace

bool networkDriveDialogsSupported()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

NetworkDriveDialogResult showMapNetworkDriveDialog(quintptr owner, QString* error)
{
#ifdef Q_OS_WIN
    return translateResult(WNetConnectionDialog(
        reinterpret_cast<HWND>(owner), RESOURCETYPE_DISK), error);
#else
    Q_UNUSED(owner);
    if (error)
        *error = QStringLiteral("Network-drive dialogs are available only on Windows.");
    return NetworkDriveDialogResult::Unsupported;
#endif
}

NetworkDriveDialogResult showDisconnectNetworkDriveDialog(quintptr owner, QString* error)
{
#ifdef Q_OS_WIN
    return translateResult(WNetDisconnectDialog(
        reinterpret_cast<HWND>(owner), RESOURCETYPE_DISK), error);
#else
    Q_UNUSED(owner);
    if (error)
        *error = QStringLiteral("Network-drive dialogs are available only on Windows.");
    return NetworkDriveDialogResult::Unsupported;
#endif
}

} // namespace filemanager::platform
