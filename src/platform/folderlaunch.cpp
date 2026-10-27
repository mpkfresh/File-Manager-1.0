#include "folderlaunch.h"

#include <QDir>
#include <QFileInfo>
#ifdef Q_OS_WIN
#include <windows.h>
#include <vector>
#endif

namespace filemanager::platform {

FolderLaunch resolveFolderLaunch(const QStringList& paths)
{
    if (paths.isEmpty() || paths.size() > 512)
        return {{}, QStringLiteral("Specify between 1 and 512 folders to open.")};
    QStringList folders;
    for (const QString& path : paths) {
        if (path.isEmpty() || path.contains(QChar::Null) || path.contains('"'))
            return {{}, QStringLiteral("The folder path is invalid.")};
        const QFileInfo info(QDir::fromNativeSeparators(path));
        if (!info.isDir()) {
            return {{}, QStringLiteral("The folder is missing or unavailable, or the path is not a folder:\n%1")
                .arg(path)};
        }
        QString absolute = info.absoluteFilePath();
#ifdef Q_OS_WIN
        // Shell dispatch can provide an 8.3 alias, especially for a long path.
        // Expand names without resolving a junction/symlink to a different location.
        QString native = QDir::toNativeSeparators(absolute);
        const bool wasExtended = native.startsWith(QStringLiteral("\\\\?\\"));
        if (!wasExtended) native = native.startsWith(QStringLiteral("\\\\"))
            ? QStringLiteral("\\\\?\\UNC\\") + native.mid(2) : QStringLiteral("\\\\?\\") + native;
        std::vector<wchar_t> longName(32768);
        const DWORD count = GetLongPathNameW(reinterpret_cast<LPCWSTR>(native.utf16()),
            longName.data(), static_cast<DWORD>(longName.size()));
        if (count > 0 && count < longName.size()) {
            QString expanded = QString::fromWCharArray(longName.data(), count);
            if (!wasExtended && expanded.startsWith(QStringLiteral("\\\\?\\UNC\\")))
                expanded = QStringLiteral("\\\\") + expanded.mid(8);
            else if (!wasExtended && expanded.startsWith(QStringLiteral("\\\\?\\")))
                expanded = expanded.mid(4);
            absolute = QDir::fromNativeSeparators(expanded);
        }
        const auto sensitivity = Qt::CaseInsensitive;
#else
        const auto sensitivity = Qt::CaseSensitive;
#endif
        if (!folders.contains(absolute, sensitivity))
            folders.append(absolute);
    }
    return {folders, {}};
}

} // namespace filemanager::platform
