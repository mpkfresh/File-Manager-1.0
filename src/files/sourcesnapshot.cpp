#include "sourcesnapshot.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace filemanager::files {

SourceSnapshot captureSourceSnapshot(const QString& path)
{
    SourceSnapshot result;
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return result;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(handle, &info)) {
        result.volumeSerial = info.dwVolumeSerialNumber;
        result.fileIndex = (static_cast<quint64>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow;
        result.size = (static_cast<quint64>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
        result.lastWriteTime = (static_cast<quint64>(info.ftLastWriteTime.dwHighDateTime) << 32U)
            | info.ftLastWriteTime.dwLowDateTime;
        result.attributes = info.dwFileAttributes;
        result.valid = true;
    }
    CloseHandle(handle);
#else
    Q_UNUSED(path);
    // This Windows application must not fall back to path-only mutation authority.
#endif
    return result;
}

} // namespace filemanager::files
