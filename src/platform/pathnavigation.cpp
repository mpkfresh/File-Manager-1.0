#include "platform/pathnavigation.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <pathcch.h>
#include <string>
#endif

namespace filemanager::platform {

QString fileSystemParentPath(const QString& rawPath)
{
    if (rawPath.isEmpty() || rawPath.contains(QChar::Null))
        return {};
#ifdef Q_OS_WIN
    if (rawPath.size() >= PATHCCH_MAX_CCH)
        return {};
    // fromNativeSeparators()/cleanPath() remove extended prefixes on Windows.
    // Callers supply normalized paths; keep the identity intact for native root parsing.
    std::wstring nativePath = QDir::toNativeSeparators(rawPath).toStdWString();
    PCWSTR rootEnd = nullptr;
    // Let Windows interpret drive, UNC, extended-drive and extended-UNC roots.
    if (PathCchSkipRoot(nativePath.c_str(), &rootEnd) != S_OK || !rootEnd || !*rootEnd)
        return {};
    // A fully qualified root ends with a separator and is longer than a current-drive slash.
    // In particular, C:relative must not be treated as C:\relative.
    if (rootEnd - nativePath.c_str() <= 1 || rootEnd[-1] != L'\\')
        return {};
    while (PathCchRemoveBackslash(nativePath.data(), nativePath.size() + 1) == S_OK) {
        // Trim trailing separators without removing a drive/share root.
    }
    if (PathCchRemoveFileSpec(nativePath.data(), nativePath.size() + 1) != S_OK)
        return {};
    return QString::fromWCharArray(nativePath.c_str());
#else
    if (!QDir::isAbsolutePath(rawPath))
        return {};
    const QString path = QDir::cleanPath(rawPath);
    const QString parent = QFileInfo(path).path();
    return parent == path ? QString{} : parent;
#endif
}

} // namespace filemanager::platform
