#include "platform/systemopen.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace filemanager::platform {

SystemOpenResult openWithSystemShell(const QString& absolutePath)
{
    const QFileInfo info(absolutePath);
    if (!info.isAbsolute() || !info.exists())
        return {false, 0, QStringLiteral("The item is unavailable or is not an absolute path.")};
#ifdef Q_OS_WIN
    // Keep the UTF-16 storage alive until ShellExecuteExW has consumed the request.
    const QString nativePath = QDir::toNativeSeparators(info.absoluteFilePath());
    SHELLEXECUTEINFOW request{};
    request.cbSize = sizeof(request);
    request.fMask = SEE_MASK_ASYNCOK;
    request.lpVerb = L"open";
    request.lpFile = reinterpret_cast<LPCWSTR>(nativePath.utf16());
    request.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&request)) {
        const DWORD error = GetLastError();
        return {false, error, QStringLiteral("Windows could not open this item (%1).").arg(error)};
    }
    return {true, reinterpret_cast<quintptr>(request.hInstApp), {}};
#else
    const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
    return {opened, 0, opened ? QString{} : QStringLiteral("The system could not open this item.")};
#endif
}

} // namespace filemanager::platform
