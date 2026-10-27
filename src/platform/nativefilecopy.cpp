#include "nativefilecopy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QUuid>

#include <array>
#include <cstddef>
#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#endif

namespace filemanager::platform {
namespace {

#ifdef Q_OS_WIN
QString systemError(DWORD code)
{
    wchar_t* message = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    const QString result = size > 0 && message
        ? QString::fromWCharArray(message, static_cast<qsizetype>(size)).trimmed()
        : QStringLiteral("Windows error %1").arg(code);
    if (message)
        LocalFree(message);
    return result;
}

bool cloudTag(DWORD tag)
{
    if (tag == IO_REPARSE_TAG_CLOUD)
        return true;
#ifdef IO_REPARSE_TAG_CLOUD_1
    return tag >= IO_REPARSE_TAG_CLOUD_1 && tag <= IO_REPARSE_TAG_CLOUD_F;
#else
    return false;
#endif
}

bool removeNativeObject(const QString& path)
{
    const QString native = extendedLengthPath(path);
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return true;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        ? RemoveDirectoryW(reinterpret_cast<LPCWSTR>(native.utf16())) != FALSE
        : DeleteFileW(reinterpret_cast<LPCWSTR>(native.utf16())) != FALSE;
}

bool restoreSparseLayout(const QString& source, const QString& destination, DWORD* nativeError)
{
    const QString nativeSource = extendedLengthPath(source);
    const QString nativeDestination = extendedLengthPath(destination);
    const HANDLE sourceHandle = CreateFileW(reinterpret_cast<LPCWSTR>(nativeSource.utf16()),
        FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (sourceHandle == INVALID_HANDLE_VALUE) {
        *nativeError = GetLastError();
        return false;
    }
    const HANDLE destinationHandle = CreateFileW(
        reinterpret_cast<LPCWSTR>(nativeDestination.utf16()), FILE_WRITE_DATA,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (destinationHandle == INVALID_HANDLE_VALUE) {
        *nativeError = GetLastError();
        CloseHandle(sourceHandle);
        return false;
    }
    LARGE_INTEGER size{};
    DWORD returned = 0;
    bool ok = GetFileSizeEx(sourceHandle, &size) != FALSE
        && DeviceIoControl(destinationHandle, FSCTL_SET_SPARSE, nullptr, 0,
            nullptr, 0, &returned, nullptr) != FALSE;
    qint64 cursor = 0;
    FILE_ALLOCATED_RANGE_BUFFER query{};
    std::array<FILE_ALLOCATED_RANGE_BUFFER, 256> ranges{};
    while (ok && cursor < size.QuadPart) {
        query.FileOffset.QuadPart = cursor;
        query.Length.QuadPart = size.QuadPart - cursor;
        returned = 0;
        const BOOL queried = DeviceIoControl(sourceHandle, FSCTL_QUERY_ALLOCATED_RANGES,
            &query, sizeof(query), ranges.data(), static_cast<DWORD>(sizeof(ranges)),
            &returned, nullptr);
        const DWORD queryError = queried ? ERROR_SUCCESS : GetLastError();
        if (!queried && queryError != ERROR_MORE_DATA) {
            *nativeError = queryError;
            ok = false;
            break;
        }
        const DWORD count = returned / sizeof(FILE_ALLOCATED_RANGE_BUFFER);
        for (DWORD index = 0; ok && index < count; ++index) {
            const qint64 rangeStart = ranges[index].FileOffset.QuadPart;
            const qint64 rangeEnd = rangeStart + ranges[index].Length.QuadPart;
            if (rangeStart > cursor) {
                FILE_ZERO_DATA_INFORMATION hole{};
                hole.FileOffset.QuadPart = cursor;
                hole.BeyondFinalZero.QuadPart = rangeStart;
                ok = DeviceIoControl(destinationHandle, FSCTL_SET_ZERO_DATA,
                    &hole, sizeof(hole), nullptr, 0, &returned, nullptr) != FALSE;
                if (!ok)
                    *nativeError = GetLastError();
            }
            cursor = qMax(cursor, rangeEnd);
        }
        if (ok && queried)
            break;
        if (ok && count == 0) {
            *nativeError = ERROR_INVALID_DATA;
            ok = false;
        }
    }
    if (ok && cursor < size.QuadPart) {
        FILE_ZERO_DATA_INFORMATION hole{};
        hole.FileOffset.QuadPart = cursor;
        hole.BeyondFinalZero.QuadPart = size.QuadPart;
        ok = DeviceIoControl(destinationHandle, FSCTL_SET_ZERO_DATA,
            &hole, sizeof(hole), nullptr, 0, &returned, nullptr) != FALSE;
        if (!ok)
            *nativeError = GetLastError();
    }
    CloseHandle(destinationHandle);
    CloseHandle(sourceHandle);
    return ok;
}

bool restoreFileTimes(const QString& source, const QString& destination, DWORD* nativeError)
{
    WIN32_FILE_ATTRIBUTE_DATA sourceData{};
    const QString nativeSource = extendedLengthPath(source);
    const QString nativeDestination = extendedLengthPath(destination);
    if (!GetFileAttributesExW(reinterpret_cast<LPCWSTR>(nativeSource.utf16()),
            GetFileExInfoStandard, &sourceData)) {
        *nativeError = GetLastError();
        return false;
    }
    const HANDLE destinationHandle = CreateFileW(
        reinterpret_cast<LPCWSTR>(nativeDestination.utf16()), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (destinationHandle == INVALID_HANDLE_VALUE) {
        *nativeError = GetLastError();
        return false;
    }
    const bool ok = SetFileTime(destinationHandle, &sourceData.ftCreationTime,
        &sourceData.ftLastAccessTime, &sourceData.ftLastWriteTime) != FALSE;
    if (!ok)
        *nativeError = GetLastError();
    CloseHandle(destinationHandle);
    return ok;
}

DWORD reparseTag(const QString& path)
{
    const QString nativePath = extendedLengthPath(path);
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> buffer{};
    DWORD returned = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0,
        buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr) != FALSE;
    CloseHandle(handle);
    if (!ok || returned < sizeof(DWORD))
        return 0;
    return *reinterpret_cast<const DWORD*>(buffer.data());
}

struct ProgressContext
{
    const std::function<bool(qint64, qint64)>* progress = nullptr;
    bool cancelled = false;
    files::SourceSnapshot stagingSnapshot;
};

files::SourceSnapshot handleSnapshot(HANDLE handle)
{
    BY_HANDLE_FILE_INFORMATION info{};
    files::SourceSnapshot result;
    if (!GetFileInformationByHandle(handle, &info))
        return result;
    result.valid = true;
    result.volumeSerial = info.dwVolumeSerialNumber;
    result.fileIndex = (static_cast<quint64>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow;
    result.size = (static_cast<quint64>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
    result.lastWriteTime = (static_cast<quint64>(info.ftLastWriteTime.dwHighDateTime) << 32U)
        | info.ftLastWriteTime.dwLowDateTime;
    result.attributes = info.dwFileAttributes;
    return result;
}

DWORD CALLBACK copyProgress(LARGE_INTEGER totalFileSize, LARGE_INTEGER totalBytesTransferred,
    LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE destination, LPVOID data)
{
    auto* context = static_cast<ProgressContext*>(data);
    if (!context->stagingSnapshot.valid)
        context->stagingSnapshot = handleSnapshot(destination);
    if (context->progress && *context->progress
            && !(*context->progress)(totalBytesTransferred.QuadPart, totalFileSize.QuadPart)) {
        context->cancelled = true;
        return PROGRESS_CANCEL;
    }
    return PROGRESS_CONTINUE;
}

std::optional<QMap<QString, quint64>> streamMap(const QString& path, DWORD* nativeError)
{
    WIN32_FIND_STREAM_DATA data{};
    const QString nativePath = extendedLengthPath(path);
    HANDLE search = FindFirstStreamW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        FindStreamInfoStandard, &data, 0);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_HANDLE_EOF || error == ERROR_INVALID_PARAMETER
                || error == ERROR_NOT_SUPPORTED) {
            return QMap<QString, quint64>{};
        }
        if (nativeError)
            *nativeError = error;
        return std::nullopt;
    }
    QMap<QString, quint64> streams;
    do {
        const QString name = QString::fromWCharArray(data.cStreamName).toCaseFolded();
        streams.insert(name, static_cast<quint64>(data.StreamSize.QuadPart));
    } while (FindNextStreamW(search, &data));
    const DWORD finalError = GetLastError();
    FindClose(search);
    if (finalError != ERROR_HANDLE_EOF) {
        if (nativeError)
            *nativeError = finalError;
        return std::nullopt;
    }
    return streams;
}

bool hasNamedStreams(const QMap<QString, quint64>& streams)
{
    for (auto iterator = streams.cbegin(); iterator != streams.cend(); ++iterator)
        if (iterator.key() != QStringLiteral("::$data"))
            return true;
    return false;
}

QString verifyPreservedMetadata(const QString& source, const QString& staging,
    bool verifyStreams, DWORD* nativeError)
{
    WIN32_FILE_ATTRIBUTE_DATA sourceData{};
    WIN32_FILE_ATTRIBUTE_DATA stagingData{};
    const QString nativeSource = extendedLengthPath(source);
    const QString nativeStaging = extendedLengthPath(staging);
    if (!GetFileAttributesExW(reinterpret_cast<LPCWSTR>(nativeSource.utf16()),
            GetFileExInfoStandard, &sourceData)
        || !GetFileAttributesExW(reinterpret_cast<LPCWSTR>(nativeStaging.utf16()),
            GetFileExInfoStandard, &stagingData)) {
        *nativeError = GetLastError();
        return QStringLiteral("Could not verify copied file metadata: ") + systemError(*nativeError);
    }
    constexpr DWORD preservedAttributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN
        | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_READONLY
        | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_TEMPORARY;
    if ((sourceData.dwFileAttributes & preservedAttributes)
            != (stagingData.dwFileAttributes & preservedAttributes)) {
        *nativeError = ERROR_INVALID_DATA;
        return QStringLiteral("Windows did not preserve the source file attributes.");
    }
    if (sourceData.ftLastWriteTime.dwHighDateTime != stagingData.ftLastWriteTime.dwHighDateTime
        || sourceData.ftLastWriteTime.dwLowDateTime != stagingData.ftLastWriteTime.dwLowDateTime) {
        *nativeError = ERROR_INVALID_DATA;
        return QStringLiteral("Windows did not preserve the source modification timestamp.");
    }
    if (verifyStreams) {
        DWORD streamError = ERROR_SUCCESS;
        const auto sourceStreams = streamMap(source, &streamError);
        const auto stagingStreams = streamMap(staging, &streamError);
        if (!sourceStreams || !stagingStreams) {
            *nativeError = streamError;
            return QStringLiteral("Could not verify alternate data streams: ")
                + systemError(streamError);
        }
        if (*sourceStreams != *stagingStreams) {
            *nativeError = ERROR_INVALID_DATA;
            return QStringLiteral("Windows did not preserve every alternate data stream.");
        }
    }
    return {};
}
#endif

} // namespace

NativePathKind nativePathKind(const QString& path)
{
#ifdef Q_OS_WIN
    const QString nativePath = extendedLengthPath(path);
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return NativePathKind::Missing;
    if ((attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
            | FILE_ATTRIBUTE_RECALL_ON_OPEN)) != 0) {
        return NativePathKind::CloudPlaceholder;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
        return NativePathKind::Regular;
    const DWORD tag = reparseTag(path);
    if (tag == IO_REPARSE_TAG_SYMLINK)
        return NativePathKind::SymbolicLink;
    if (cloudTag(tag))
        return NativePathKind::CloudPlaceholder;
    return NativePathKind::UnsupportedReparse;
#else
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymbolicLink())
        return NativePathKind::Missing;
    return info.isSymbolicLink() ? NativePathKind::SymbolicLink : NativePathKind::Regular;
#endif
}

QString extendedLengthPath(const QString& path)
{
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    if (native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    return QStringLiteral("\\\\?\\") + native;
#else
    return native;
#endif
}

NativeVolumeCapabilities nativeVolumeCapabilities(const QString& path)
{
    NativeVolumeCapabilities result;
#ifdef Q_OS_WIN
    const QString nativePath = extendedLengthPath(path);
    std::array<wchar_t, 32768> root{};
    if (!GetVolumePathNameW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), root.data(),
            static_cast<DWORD>(root.size())))
        return result;
    std::array<wchar_t, 256> fileSystem{};
    DWORD flags = 0;
    if (!GetVolumeInformationW(root.data(), nullptr, 0, nullptr, nullptr, &flags,
            fileSystem.data(), static_cast<DWORD>(fileSystem.size())))
        return result;
    result.known = true;
    result.namedStreams = (flags & FILE_NAMED_STREAMS) != 0;
    result.persistentAcls = (flags & FILE_PERSISTENT_ACLS) != 0;
    result.sparseFiles = (flags & FILE_SUPPORTS_SPARSE_FILES) != 0;
    result.encryption = (flags & FILE_SUPPORTS_ENCRYPTION) != 0;
    result.fileSystemName = QString::fromWCharArray(fileSystem.data());
#else
    Q_UNUSED(path);
#endif
    return result;
}

QStringList unsupportedNativeMetadata(bool encrypted, bool sourceHasNamedStreams,
    bool sparse, bool sourceHasPersistentAcls, const NativeVolumeCapabilities& destination)
{
    QStringList unsupported;
    const QString target = destination.fileSystemName.isEmpty()
        ? QStringLiteral("the destination filesystem") : destination.fileSystemName;
    if (!destination.known) {
        unsupported.append(QStringLiteral(
            "Destination metadata capabilities could not be verified."));
        return unsupported;
    }
    if (encrypted && !destination.encryption)
        unsupported.append(QStringLiteral("EFS encryption is not supported by %1.").arg(target));
    if (sourceHasNamedStreams && !destination.namedStreams) {
        unsupported.append(QStringLiteral(
            "Alternate data streams, including Mark-of-the-Web, are not supported by %1.")
            .arg(target));
    }
    if (sparse && !destination.sparseFiles)
        unsupported.append(QStringLiteral("Sparse allocation is not supported by %1.").arg(target));
    if (sourceHasPersistentAcls && !destination.persistentAcls) {
        unsupported.append(QStringLiteral(
            "Persistent Windows permissions are not supported by %1.").arg(target));
    }
    return unsupported;
}

NativeMetadataReview reviewNativeCopyMetadata(const QString& source,
    const QString& destinationLocation)
{
    NativeMetadataReview result;
#ifdef Q_OS_WIN
    if (nativePathKind(source) != NativePathKind::Regular) {
        result.error = QStringLiteral("Only locally available regular files can be reviewed for metadata preservation.");
        result.nativeCode = ERROR_INVALID_DATA;
        return result;
    }
    const DWORD sourceAttributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(extendedLengthPath(source).utf16()));
    if (sourceAttributes == INVALID_FILE_ATTRIBUTES) {
        result.nativeCode = GetLastError();
        result.error = systemError(result.nativeCode);
        return result;
    }
    DWORD streamError = ERROR_SUCCESS;
    const auto sourceStreams = streamMap(source, &streamError);
    if (!sourceStreams) {
        result.nativeCode = streamError;
        result.error = QStringLiteral("Could not inspect alternate data streams: ")
            + systemError(streamError);
        return result;
    }
    const auto sourceCapabilities = nativeVolumeCapabilities(source);
    const auto destinationCapabilities = nativeVolumeCapabilities(destinationLocation);
    result.degradations = unsupportedNativeMetadata(
        (sourceAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0,
        hasNamedStreams(*sourceStreams),
        (sourceAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0,
        sourceCapabilities.known && sourceCapabilities.persistentAcls,
        destinationCapabilities);
    result.inspected = true;
#else
    Q_UNUSED(source);
    Q_UNUSED(destinationLocation);
    result.error = QStringLiteral("Native metadata review requires Windows.");
#endif
    return result;
}

NativeCopyResult copyFileAtomically(const QString& source, const QString& destination,
    bool replaceExisting, const std::function<bool(qint64, qint64)>& progress,
    bool allowMetadataDegradation,
    const std::function<NativeCopyResult(const QString&, const files::SourceSnapshot&)>& stagingCommit,
    const QStringList* approvedMetadataDegradations,
    const NativeMetadataProbe& additionalMetadataProbe)
{
    const NativePathKind kind = nativePathKind(source);
    if (kind == NativePathKind::Missing)
        return {false, false, QStringLiteral("Source no longer exists: ") + source};
    if (kind == NativePathKind::CloudPlaceholder) {
        return {false, false, QStringLiteral(
            "Cloud item is not locally available. Make it available offline and retry: ") + source};
    }
    if (kind == NativePathKind::UnsupportedReparse) {
        return {false, false,
            QStringLiteral("Unsupported reparse point was not followed: ") + source};
    }

#ifdef Q_OS_WIN
    const QFileInfo destinationInfo(destination);
    const QString destinationParent = destinationInfo.absolutePath();
    const DWORD sourceAttributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(extendedLengthPath(source).utf16()));
    if (sourceAttributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return {false, false, systemError(error), error};
    }
    DWORD streamError = ERROR_SUCCESS;
    std::optional<QMap<QString, quint64>> sourceStreams = QMap<QString, quint64>{};
    if (kind == NativePathKind::Regular) {
        sourceStreams = streamMap(source, &streamError);
        if (!sourceStreams)
            return {false, false, QStringLiteral("Could not inspect alternate data streams: ")
                + systemError(streamError), streamError};
    }
    const auto sourceCapabilities = nativeVolumeCapabilities(source);
    const auto destinationCapabilities = nativeVolumeCapabilities(destinationParent);
    QStringList degradations = unsupportedNativeMetadata(
        (sourceAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0,
        kind == NativePathKind::Regular && hasNamedStreams(*sourceStreams),
        (sourceAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0,
        sourceCapabilities.known && sourceCapabilities.persistentAcls,
        destinationCapabilities);
    if (additionalMetadataProbe) {
        // A test probe can add warnings, never remove the native warnings.
        const auto additional = additionalMetadataProbe(source, destinationParent);
        if (!additional.inspected || !additional.error.isEmpty()) {
            return {false, false, additional.error.isEmpty()
                ? QStringLiteral("Additional metadata inspection failed.")
                : additional.error, additional.nativeCode};
        }
        for (const QString& detail : additional.degradations) {
            if (!degradations.contains(detail))
                degradations.append(detail);
        }
    }
    bool unapprovedDegradation = !allowMetadataDegradation;
    if (approvedMetadataDegradations) {
        for (const QString& detail : degradations) {
            if (!approvedMetadataDegradations->contains(detail)) {
                unapprovedDegradation = true;
                break;
            }
        }
    }
    if (!degradations.isEmpty() && unapprovedDegradation) {
        NativeCopyResult confirmation;
        confirmation.error = QStringLiteral("Destination metadata confirmation is required.");
        confirmation.metadataConfirmationRequired = true;
        confirmation.metadataDegradations = degradations;
        return confirmation;
    }
    QString staging;
    for (int attempt = 0; attempt < 100 && staging.isEmpty(); ++attempt) {
        const QString candidate = destinationInfo.dir().filePath(
            QStringLiteral(".file-manager-copy-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12)
            + QStringLiteral(".tmp"));
        const QString nativeCandidate = extendedLengthPath(candidate);
        if (GetFileAttributesW(reinterpret_cast<LPCWSTR>(nativeCandidate.utf16()))
                == INVALID_FILE_ATTRIBUTES) {
            staging = candidate;
        }
    }
    if (staging.isEmpty())
        return {false, false, QStringLiteral("Could not reserve an atomic destination name.")};

    ProgressContext context{&progress};
    BOOL cancel = FALSE;
    DWORD flags = COPY_FILE_FAIL_IF_EXISTS;
    if (kind == NativePathKind::SymbolicLink)
        flags |= COPY_FILE_COPY_SYMLINK;
    if (allowMetadataDegradation
            && (sourceAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0
            && !destinationCapabilities.encryption) {
        flags |= COPY_FILE_ALLOW_DECRYPTED_DESTINATION;
    }
    const QString nativeSource = extendedLengthPath(source);
    const QString nativeStaging = extendedLengthPath(staging);
    if (!CopyFileExW(reinterpret_cast<LPCWSTR>(nativeSource.utf16()),
            reinterpret_cast<LPCWSTR>(nativeStaging.utf16()), copyProgress, &context, &cancel, flags)) {
        const DWORD error = GetLastError();
        removeNativeObject(staging);
        return {false, context.cancelled || error == ERROR_REQUEST_ABORTED,
            context.cancelled ? QStringLiteral("Transfer cancelled.") : systemError(error), error};
    }
    if ((sourceAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0
            && destinationCapabilities.sparseFiles) {
        DWORD sparseError = ERROR_SUCCESS;
        if (!restoreSparseLayout(source, staging, &sparseError)
                || !restoreFileTimes(source, staging, &sparseError)) {
            removeNativeObject(staging);
            return {false, false,
                QStringLiteral("Could not preserve sparse-file allocation and timestamps: ")
                    + systemError(sparseError), sparseError};
        }
    }
    DWORD verificationError = ERROR_SUCCESS;
    const QString verificationFailure = kind == NativePathKind::Regular
            && degradations.isEmpty()
        ? verifyPreservedMetadata(source, staging,
            destinationCapabilities.namedStreams, &verificationError)
        : QString{};
    if (!verificationFailure.isEmpty()) {
        removeNativeObject(staging);
        return {false, false, verificationFailure, verificationError};
    }
    if (stagingCommit) {
        const auto currentStaging = files::captureSourceSnapshot(staging);
        if (!context.stagingSnapshot.valid || !currentStaging.valid
                || context.stagingSnapshot.volumeSerial != currentStaging.volumeSerial
                || context.stagingSnapshot.fileIndex != currentStaging.fileIndex) {
            removeNativeObject(staging);
            return {false, false, QStringLiteral("The copied staging object changed before commit."),
                ERROR_INVALID_DATA};
        }
        return stagingCommit(staging, context.stagingSnapshot);
    }
    const DWORD moveFlags = MOVEFILE_WRITE_THROUGH
        | (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0);
    const QString nativeDestination = extendedLengthPath(destination);
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(nativeStaging.utf16()),
            reinterpret_cast<LPCWSTR>(nativeDestination.utf16()), moveFlags)) {
        const DWORD nativeError = GetLastError();
        const QString error = systemError(nativeError);
        removeNativeObject(staging);
        return {false, false, QStringLiteral("Could not commit destination: ") + error,
            nativeError};
    }
    NativeCopyResult committed{true, false, {}};
    committed.committedSnapshot = files::captureSourceSnapshot(destination);
    return committed;
#else
    Q_UNUSED(replaceExisting);
    Q_UNUSED(allowMetadataDegradation);
    Q_UNUSED(stagingCommit);
    Q_UNUSED(approvedMetadataDegradations);
    Q_UNUSED(additionalMetadataProbe);
    if (kind == NativePathKind::SymbolicLink)
        return {false, false, QStringLiteral("Symbolic-link copying is not supported here.")};
    if (QFile::copy(source, destination)) {
        if (progress)
            progress(QFileInfo(source).size(), QFileInfo(source).size());
        return {true, false, {}};
    }
    return {false, false, QStringLiteral("Could not copy file: ") + source};
#endif
}

} // namespace filemanager::platform
