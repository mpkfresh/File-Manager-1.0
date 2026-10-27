#include "platform/cloudfilestatus.h"

#include "platform/nativefilecopy.h"

#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <cfapi.h>
#endif

namespace filemanager::platform {

CloudFileStatus inspectCloudFileStatus(const QString& path)
{
    CloudFileStatus result;
#ifdef Q_OS_WIN
    const QString nativePath = extendedLengthPath(path);
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS
            | FILE_FLAG_OPEN_NO_RECALL, nullptr);

    FILE_ATTRIBUTE_TAG_INFO metadata{};
    if (handle != INVALID_HANDLE_VALUE) {
        result.inspected = GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
            &metadata, sizeof(metadata)) != FALSE;
        CloseHandle(handle);
    }
    if (!result.inspected) {
        const DWORD attributes = GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(nativePath.utf16()));
        // Attributes alone cannot identify a reparse-backed cloud placeholder.
        if (attributes == INVALID_FILE_ATTRIBUTES
            || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return result;
        metadata.FileAttributes = attributes;
        metadata.ReparseTag = 0;
        result.inspected = true;
    }

    const DWORD attributes = metadata.FileAttributes;
    const auto placeholderState = CfGetPlaceholderStateFromAttributeTag(
        attributes, metadata.ReparseTag);
    const DWORD state = static_cast<DWORD>(placeholderState);
    result.cloudFilesPlaceholder = placeholderState != CF_PLACEHOLDER_STATE_INVALID
        && (state & CF_PLACEHOLDER_STATE_PLACEHOLDER) != 0;
    result.providerBacked = result.cloudFilesPlaceholder
        || (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN
            | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) != 0;
    result.syncRoot = result.cloudFilesPlaceholder
        && (state & CF_PLACEHOLDER_STATE_SYNC_ROOT) != 0;
    result.inSync = result.cloudFilesPlaceholder
        && (state & CF_PLACEHOLDER_STATE_IN_SYNC) != 0;
    const bool partial = result.cloudFilesPlaceholder
        && (state & CF_PLACEHOLDER_STATE_PARTIAL) != 0;
    const bool recallOrOffline = (attributes & (FILE_ATTRIBUTE_OFFLINE
        | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) != 0;
    result.partiallyOnDisk = partial
        && (state & CF_PLACEHOLDER_STATE_PARTIALLY_ON_DISK) != 0;
    result.contentReady = result.cloudFilesPlaceholder && !partial && !recallOrOffline;
    result.pinned = (attributes & FILE_ATTRIBUTE_PINNED) != 0;
    result.unpinned = (attributes & FILE_ATTRIBUTE_UNPINNED) != 0;
#else
    Q_UNUSED(path);
#endif
    return result;
}

QString cloudFileStatusText(const CloudFileStatus& status, bool includeOrdinaryItem)
{
    if (!status.inspected)
        return includeOrdinaryItem ? QStringLiteral("Status unavailable") : QString{};
    if (!status.providerBacked)
        return includeOrdinaryItem ? QStringLiteral("Local item") : QString{};

    QString availability;
    if (status.syncRoot)
        availability = QStringLiteral("Sync root");
    else if (status.partiallyOnDisk)
        availability = QStringLiteral("Partly on this device");
    else if (status.cloudFilesPlaceholder && !status.contentReady)
        availability = QStringLiteral("Content not ready");
    else if (status.cloudFilesPlaceholder && status.contentReady && status.pinned)
        availability = QStringLiteral("Always available on this device");
    else if (status.cloudFilesPlaceholder && status.contentReady)
        availability = QStringLiteral("Available on this device");
    else
        availability = QStringLiteral("Offline provider item");

    if (!status.cloudFilesPlaceholder)
        return availability + QStringLiteral(" · Sync state unavailable");
    return availability + (status.inSync ? QStringLiteral(" · In sync")
                                         : QStringLiteral(" · Not reported in sync"));
}

bool isLocallyPreviewableFile(const QString& path)
{
#ifdef Q_OS_WIN
    const QString nativePath = extendedLengthPath(path);
    const DWORD attributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES
            || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
#else
    if (!QFileInfo(path).isFile())
        return false;
#endif
    const NativePathKind kind = nativePathKind(path);
    if (kind == NativePathKind::Regular)
        return true;
    if (kind != NativePathKind::CloudPlaceholder
            && kind != NativePathKind::UnsupportedReparse)
        return false;
    const CloudFileStatus status = inspectCloudFileStatus(path);
    return status.inspected && status.cloudFilesPlaceholder && status.contentReady;
}

} // namespace filemanager::platform
