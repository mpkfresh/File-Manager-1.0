#include "guardedfilerename.h"

#include "files/filenamevalidator.h"
#include "nativefilecopy.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QUuid>

#include <cstddef>
#include <bit>
#include <cstring>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winternl.h>
#endif

namespace filemanager::platform {
namespace {

QString pathKey(const QString& path)
{
    // Windows directories can opt into case sensitivity. Do not merge distinct guarded objects.
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

#ifdef Q_OS_WIN
class Handle
{
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value_; }
    void reset(HANDLE value = INVALID_HANDLE_VALUE)
    {
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_;
};

files::SourceSnapshot snapshot(HANDLE handle)
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

bool sameObject(const files::SourceSnapshot& left, const files::SourceSnapshot& right)
{
    return left.valid && right.valid && left.volumeSerial == right.volumeSerial
        && left.fileIndex == right.fileIndex;
}

bool regular(const files::SourceSnapshot& value, bool directory)
{
    constexpr DWORD unavailable = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE
        | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    return value.valid && !(value.attributes & unavailable)
        && ((value.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}

using CreateNativeFile = NTSTATUS (NTAPI*)(HANDLE*, ACCESS_MASK, OBJECT_ATTRIBUTES*,
    IO_STATUS_BLOCK*, LARGE_INTEGER*, ULONG, ULONG, ULONG, ULONG, void*, ULONG);
using SetNativeInformation = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
using NativeStatusToError = ULONG (NTAPI*)(NTSTATUS);

template <typename Function>
Function nativeFunction(const char* name)
{
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const FARPROC address = module ? GetProcAddress(module, name) : nullptr;
    return address ? std::bit_cast<Function>(address) : nullptr;
}

DWORD windowsError(NTSTATUS status)
{
    const auto translate = nativeFunction<NativeStatusToError>("RtlNtStatusToDosError");
    return translate ? translate(status) : ERROR_GEN_FAILURE;
}
#endif

} // namespace

struct GuardedCrossVolumeMove::State
{
    QString failure;
    QString source;
    QString destination;
    QString reviewedRoot;
    files::SourceSnapshot expectedSource;
    files::SourceSnapshot expectedRoot;
    bool attempted = false;
    quint32 nativeCode = 0;
#ifdef Q_OS_WIN
    std::vector<Handle> directories;
    QHash<QString, HANDLE> pinned;
    std::unique_ptr<Handle> sourceHandle;
    std::unique_ptr<Handle> destinationHandle;
    std::unique_ptr<Handle> anchorHandle;
    HANDLE targetDirectory = INVALID_HANDLE_VALUE;

    bool fail(const QString& message, DWORD code = ERROR_SUCCESS)
    {
        nativeCode = code;
        failure = code ? message + QStringLiteral(" (Windows error %1)").arg(code) : message;
        return false;
    }

    bool pinChain(QString path)
    {
        QStringList chain;
        for (;;) {
            if (pinned.contains(pathKey(path)))
                break;
            chain.prepend(path);
            const QString parent = QFileInfo(path).absolutePath();
            if (pathKey(parent) == pathKey(path))
                break;
            if (!files::validateWindowsName(QFileInfo(path).fileName()).valid)
                return fail("Unsupported directory name in the reviewed path.");
            path = parent;
        }
        for (const QString& entry : chain) {
            const HANDLE handle = CreateFileW(
                reinterpret_cast<LPCWSTR>(extendedLengthPath(entry).utf16()),
                FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL,
                nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return fail("Could not guard directory: " + entry, GetLastError());
            directories.emplace_back(handle);
            if (!regular(snapshot(handle), true))
                return fail("A directory is unavailable or a reparse point: " + entry);
            pinned.insert(pathKey(entry), handle);
        }
        return true;
    }

    bool removeAnchor(DWORD* failureCode = nullptr)
    {
        if (!anchorHandle)
            return true;
        FILE_DISPOSITION_INFO disposition{TRUE};
        const bool removed = SetFileInformationByHandle(anchorHandle->get(), FileDispositionInfo,
            &disposition, sizeof(disposition)) != FALSE;
        const DWORD code = removed ? ERROR_SUCCESS : GetLastError();
        anchorHandle.reset();
        if (failureCode)
            *failureCode = code;
        return removed;
    }
#endif
};

GuardedCrossVolumeMove::GuardedCrossVolumeMove(const QString& source,
    const files::SourceSnapshot& expectedSource, const QString& destination,
    const QString& reviewedRoot, const files::SourceSnapshot& expectedRoot)
    : state_(std::make_unique<State>())
{
    auto& state = *state_;
    state.source = source;
    state.destination = destination;
    state.reviewedRoot = reviewedRoot;
    state.expectedSource = expectedSource;
    state.expectedRoot = expectedRoot;
#ifdef Q_OS_WIN
    if (!QDir::isAbsolutePath(source) || !QDir::isAbsolutePath(destination)
        || !QDir::isAbsolutePath(reviewedRoot) || !files::validateWindowsPath(source).valid
        || !files::validateWindowsPath(destination).valid
        || QFileInfo(source).fileName() != QFileInfo(destination).fileName()
        || files::isPathInside(source, reviewedRoot) || !files::isPathInside(destination, reviewedRoot)
        || !regular(expectedSource, false) || !regular(expectedRoot, true)) {
        state.fail("Invalid reviewed cross-volume move request.");
        return;
    }
    if (!state.pinChain(reviewedRoot)
        || !sameObject(snapshot(state.pinned.value(pathKey(reviewedRoot))), expectedRoot)) {
        if (state.failure.isEmpty())
            state.fail("The reviewed destination folder was replaced.");
        return;
    }
    if (!state.pinChain(QFileInfo(source).absolutePath())
        || !state.pinChain(QFileInfo(destination).absolutePath()))
        return;
    state.targetDirectory = state.pinned.value(pathKey(QFileInfo(destination).absolutePath()));
    bool insideRetainedRoot = false;
    QString ancestor = QFileInfo(destination).absolutePath();
    for (;;) {
        if (sameObject(snapshot(state.pinned.value(pathKey(ancestor), INVALID_HANDLE_VALUE)), expectedRoot)) {
            insideRetainedRoot = true;
            break;
        }
        const QString parent = QFileInfo(ancestor).absolutePath();
        if (pathKey(parent) == pathKey(ancestor))
            break;
        ancestor = parent;
    }
    if (!insideRetainedRoot) {
        state.fail("The destination ancestry does not contain the reviewed folder identity.");
        return;
    }
    if (snapshot(state.targetDirectory).volumeSerial == expectedSource.volumeSerial) {
        state.fail("Same-volume files must use the guarded rename path.");
        return;
    }
    const HANDLE sourceHandle = CreateFileW(
        reinterpret_cast<LPCWSTR>(extendedLengthPath(source).utf16()),
        GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (sourceHandle == INVALID_HANDLE_VALUE) {
        state.fail("Could not guard the reviewed source for copying.", GetLastError());
        return;
    }
    state.sourceHandle = std::make_unique<Handle>(sourceHandle);
    if (snapshot(sourceHandle) != expectedSource) {
        state.fail("The source changed since the preview.");
        return;
    }
    if (nativePathKind(destination) != NativePathKind::Missing) {
        state.fail("The reviewed destination already exists or cannot be inspected.");
        return;
    }
    const auto createFile = nativeFunction<CreateNativeFile>("NtCreateFile");
    if (!createFile) {
        state.fail("Windows directory-relative creation is unavailable.", ERROR_NOT_SUPPORTED);
        return;
    }
    const QString anchor = QStringLiteral(".file-manager-guard-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".tmp");
    UNICODE_STRING anchorName{};
    anchorName.Buffer = const_cast<PWSTR>(reinterpret_cast<PCWSTR>(anchor.utf16()));
    anchorName.Length = static_cast<USHORT>(anchor.size() * sizeof(wchar_t));
    anchorName.MaximumLength = anchorName.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = state.targetDirectory;
    attributes.ObjectName = &anchorName;
    attributes.Attributes = 0x1000 | 0x40; // OBJ_DONT_REPARSE | OBJ_CASE_INSENSITIVE
    HANDLE anchorHandle = INVALID_HANDLE_VALUE;
    IO_STATUS_BLOCK io{};
    constexpr ULONG fileCreate = 2;
    constexpr ULONG fileNonDirectory = 0x40;
    constexpr ULONG fileSynchronousIoNonAlert = 0x20;
    constexpr ULONG fileOpenReparsePoint = 0x00200000;
    const NTSTATUS anchorStatus = createFile(&anchorHandle,
        FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE, &attributes, &io, nullptr,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, fileCreate,
        fileNonDirectory | fileSynchronousIoNonAlert | fileOpenReparsePoint, nullptr, 0);
    if (anchorStatus < 0 || anchorHandle == INVALID_HANDLE_VALUE) {
        if (anchorHandle != INVALID_HANDLE_VALUE) CloseHandle(anchorHandle);
        state.fail("Could not retain the destination folder against redirection.",
            windowsError(anchorStatus));
        return;
    }
    state.anchorHandle = std::make_unique<Handle>(anchorHandle);
    if (!regular(snapshot(state.targetDirectory), true))
        state.fail("The destination folder changed while its guard was created.");
#else
    Q_UNUSED(expectedRoot);
    state.failure = "Guarded cross-volume moves require Windows.";
#endif
}

GuardedCrossVolumeMove::~GuardedCrossVolumeMove()
{
#ifdef Q_OS_WIN
    state_->removeAnchor();
#endif
}

bool GuardedCrossVolumeMove::ready() const
{
    return state_->failure.isEmpty() && !state_->attempted;
}

QString GuardedCrossVolumeMove::error() const { return state_->failure; }

GuardedCrossVolumeResult GuardedCrossVolumeMove::commit(
    const std::function<bool(qint64, qint64)>& progress, const std::atomic_bool* cancelled,
    bool allowMetadataDegradation, const QStringList* approvedMetadataDegradations,
    const NativeMetadataProbe& additionalMetadataProbe)
{
    auto& state = *state_;
    GuardedCrossVolumeResult result;
    if (!ready()) {
        result.error = state.failure.isEmpty() ? "This move was already attempted." : state.failure;
        result.nativeCode = state.nativeCode;
        return result;
    }
    state.attempted = true;
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        result.cancelled = true;
        result.error = "Move cancelled before copying.";
        return result;
    }
#ifdef Q_OS_WIN
    const auto active = [&] {
        if (snapshot(state.sourceHandle->get()) != state.expectedSource)
            return false;
        for (const auto& directory : state.directories)
            if (!regular(snapshot(directory.get()), true)) return false;
        return regular(snapshot(state.targetDirectory), true);
    };
    if (!active()) {
        result.error = "A guarded source or destination changed before copying.";
        return result;
    }
    const auto effectiveProgress = [&](qint64 done, qint64 total) {
        return !(cancelled && cancelled->load(std::memory_order_relaxed))
            && (!progress || progress(done, total));
    };
    const auto commitStaging = [&](const QString& staging, const files::SourceSnapshot& expectedStaging) {
        NativeCopyResult commitResult;
        const HANDLE stagingHandle = CreateFileW(
            reinterpret_cast<LPCWSTR>(extendedLengthPath(staging).utf16()),
            FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr);
        if (stagingHandle == INVALID_HANDLE_VALUE) {
            commitResult.error = "Could not retain the copied staging object.";
            commitResult.nativeCode = GetLastError();
            return commitResult;
        }
        state.destinationHandle = std::make_unique<Handle>(stagingHandle);
        if (!sameObject(snapshot(stagingHandle), expectedStaging) || !active()
                || nativePathKind(state.destination) != NativePathKind::Missing) {
            commitResult.error = "The copied object or destination changed before commit.";
            commitResult.nativeCode = ERROR_INVALID_DATA;
        } else {
            const QString name = QFileInfo(state.destination).fileName();
            const size_t nameBytes = static_cast<size_t>(name.size()) * sizeof(wchar_t);
            const size_t bufferBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
            std::vector<std::max_align_t> storage((bufferBytes + sizeof(std::max_align_t) - 1)
                / sizeof(std::max_align_t));
            auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
            info->ReplaceIfExists = FALSE;
            info->RootDirectory = state.targetDirectory;
            info->FileNameLength = static_cast<DWORD>(nameBytes);
            std::memcpy(info->FileName, name.utf16(), nameBytes);
            IO_STATUS_BLOCK io{};
            const auto rename = nativeFunction<SetNativeInformation>("NtSetInformationFile");
            const NTSTATUS status = rename ? rename(stagingHandle, &io, info,
                static_cast<ULONG>(bufferBytes), 10) : static_cast<NTSTATUS>(0xC00000BBL);
            if (status < 0) {
                commitResult.error = QStringLiteral("Could not commit the copied destination (Windows error %1).")
                    .arg(windowsError(status));
                commitResult.nativeCode = windowsError(status);
            } else {
                commitResult.ok = true;
                commitResult.committedSnapshot = snapshot(stagingHandle);
            }
        }
        if (!commitResult.ok) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(stagingHandle, FileDispositionInfo,
                &disposition, sizeof(disposition));
            state.destinationHandle.reset();
        }
        return commitResult;
    };
    const auto copied = copyFileAtomically(state.source, state.destination, false,
        effectiveProgress, allowMetadataDegradation, commitStaging,
        approvedMetadataDegradations, additionalMetadataProbe);
    DWORD anchorError = ERROR_SUCCESS;
    const bool anchorRemoved = state.removeAnchor(&anchorError);
    if (!copied.ok) {
        result.cancelled = copied.cancelled || (cancelled && cancelled->load());
        result.error = copied.error;
        result.nativeCode = copied.nativeCode;
        result.metadataConfirmationRequired = copied.metadataConfirmationRequired;
        result.metadataDegradations = copied.metadataDegradations;
        if (!anchorRemoved) {
            result.error += QStringLiteral(" Destination guard cleanup also failed (Windows error %1).")
                .arg(anchorError);
        }
        return result;
    }
    result.destinationCommitted = true;
    result.destinationSnapshot = copied.committedSnapshot;
    if (!anchorRemoved) {
        result.error = "Copy committed, but the destination guard could not be removed.";
        result.nativeCode = anchorError;
        return result;
    }
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        result.cancelled = true;
        result.error = "Copy committed, but source removal was cancelled.";
        return result;
    }
    state.sourceHandle.reset();
    const HANDLE removalHandle = CreateFileW(
        reinterpret_cast<LPCWSTR>(extendedLengthPath(state.source).utf16()),
        FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (removalHandle == INVALID_HANDLE_VALUE) {
        result.error = "Copy committed, but the source could not be retained for removal.";
        result.nativeCode = GetLastError();
        return result;
    }
    state.sourceHandle = std::make_unique<Handle>(removalHandle);
    if (snapshot(removalHandle) != state.expectedSource
            || !sameObject(snapshot(state.destinationHandle->get()), copied.committedSnapshot)) {
        result.error = "Copy committed, but source or destination identity changed before removal.";
        result.nativeCode = ERROR_INVALID_DATA;
        return result;
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(removalHandle, FileDispositionInfo,
            &disposition, sizeof(disposition))) {
        result.error = "Copy committed, but Windows refused source removal.";
        result.nativeCode = GetLastError();
        return result;
    }
    state.sourceHandle.reset();
    result.sourceRemoved = true;
    result.moved = true;
#else
    Q_UNUSED(progress);
    Q_UNUSED(allowMetadataDegradation);
    Q_UNUSED(approvedMetadataDegradations);
    Q_UNUSED(additionalMetadataProbe);
    result.error = state.failure;
#endif
    return result;
}

struct GuardedDirectoryTree::State
{
    QString root;
    QString failure;
    quint32 nativeCode = 0;
#ifdef Q_OS_WIN
    std::vector<Handle> handles;
    HANDLE rootHandle = INVALID_HANDLE_VALUE;

    bool fail(const QString& message, DWORD code = ERROR_SUCCESS)
    {
        nativeCode = code;
        failure = code ? message + QStringLiteral(" (Windows error %1)").arg(code) : message;
        return false;
    }
#endif
};

GuardedDirectoryTree::GuardedDirectoryTree(const QString& reviewedRoot,
    const files::SourceSnapshot& expectedRoot)
    : state_(std::make_unique<State>())
{
    auto& state = *state_;
    state.root = QDir::cleanPath(QFileInfo(reviewedRoot).absoluteFilePath());
#ifdef Q_OS_WIN
    const QString portableRoot = QDir::fromNativeSeparators(reviewedRoot);
    if (!QDir::isAbsolutePath(reviewedRoot) || portableRoot.startsWith("//?/")
        || portableRoot.startsWith("//./") || !regular(expectedRoot, true)) {
        state.fail("Invalid reviewed destination root.");
        return;
    }
    const HANDLE rootHandle = CreateFileW(
        reinterpret_cast<LPCWSTR>(extendedLengthPath(state.root).utf16()),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL,
        nullptr);
    if (rootHandle == INVALID_HANDLE_VALUE) {
        state.fail("Could not guard the reviewed destination root.", GetLastError());
        return;
    }
    state.handles.emplace_back(rootHandle);
    state.rootHandle = rootHandle;
    if (!regular(snapshot(rootHandle), true) || !sameObject(snapshot(rootHandle), expectedRoot))
        state.fail("The reviewed destination root was replaced or became unsafe.");
#else
    Q_UNUSED(expectedRoot);
    state.failure = "Guarded directory creation requires Windows.";
#endif
}

GuardedDirectoryTree::~GuardedDirectoryTree() = default;

bool GuardedDirectoryTree::ready() const { return state_->failure.isEmpty(); }

QString GuardedDirectoryTree::error() const { return state_->failure; }

GuardedDirectoryResult GuardedDirectoryTree::ensure(const QString& relativeDirectory,
    const std::atomic_bool* cancelled)
{
    auto& state = *state_;
    GuardedDirectoryResult result;
    if (!ready()) {
        result.error = state.failure;
        result.nativeCode = state.nativeCode;
        return result;
    }
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        result.cancelled = true;
        result.error = "Folder creation cancelled before it began.";
        return result;
    }
    const QString normalized = QDir::fromNativeSeparators(relativeDirectory);
    const QStringList components = normalized.split('/', Qt::KeepEmptyParts);
    if (QDir::isAbsolutePath(normalized) || components.isEmpty()) {
        result.error = "A relative destination directory is required.";
        return result;
    }
    for (const QString& component : components) {
        if (!files::validateWindowsName(component).valid) {
            result.error = "The relative destination contains an unsafe directory name.";
            return result;
        }
    }
#ifdef Q_OS_WIN
    const auto createFile = nativeFunction<CreateNativeFile>("NtCreateFile");
    if (!createFile) {
        result.error = "Windows directory-relative creation is unavailable.";
        result.nativeCode = ERROR_NOT_SUPPORTED;
        return result;
    }
    HANDLE parent = state.rootHandle;
    QString visiblePath = state.root;
    for (const QString& component : components) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.error = "Folder creation cancelled; already-created folders were retained.";
            return result;
        }
        if (!regular(snapshot(parent), true)) {
            result.error = "A guarded destination directory changed before creation.";
            return result;
        }
        FILE_CASE_SENSITIVE_INFO caseInfo{};
        const bool caseSensitive = GetFileInformationByHandleEx(parent, FileCaseSensitiveInfo,
            &caseInfo, sizeof(caseInfo)) && (caseInfo.Flags & 1U) != 0;
        UNICODE_STRING name{};
        name.Buffer = const_cast<PWSTR>(reinterpret_cast<PCWSTR>(component.utf16()));
        name.Length = static_cast<USHORT>(component.size() * sizeof(wchar_t));
        name.MaximumLength = name.Length;
        OBJECT_ATTRIBUTES attributes{};
        attributes.Length = sizeof(attributes);
        attributes.RootDirectory = parent;
        attributes.ObjectName = &name;
        constexpr ULONG objectCaseInsensitive = 0x40;
        constexpr ULONG objectDoNotReparse = 0x1000;
        attributes.Attributes = objectDoNotReparse | (caseSensitive ? 0 : objectCaseInsensitive);
        HANDLE child = INVALID_HANDLE_VALUE;
        IO_STATUS_BLOCK io{};
        constexpr ULONG fileOpenIf = 3;
        constexpr ULONG fileDirectory = 0x1;
        constexpr ULONG fileSynchronousIoNonAlert = 0x20;
        constexpr ULONG fileOpenReparsePoint = 0x00200000;
        const NTSTATUS status = createFile(&child,
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE, fileOpenIf,
            fileDirectory | fileSynchronousIoNonAlert | fileOpenReparsePoint,
            nullptr, 0);
        if (status < 0 || child == INVALID_HANDLE_VALUE) {
            if (child != INVALID_HANDLE_VALUE)
                CloseHandle(child);
            result.nativeCode = windowsError(status);
            result.error = QStringLiteral("Could not create or guard destination folder '%1' (Windows error %2).")
                .arg(component).arg(result.nativeCode);
            return result;
        }
        state.handles.emplace_back(child);
        const auto identity = snapshot(child);
        if (!regular(identity, true)) {
            result.error = "A destination component is not a regular local directory: " + component;
            return result;
        }
        parent = child;
        visiblePath = QDir(visiblePath).filePath(component);
        constexpr quintptr fileCreated = 2;
        if (io.Information == fileCreated) {
            result.createdDirectories.append(visiblePath);
            result.createdSnapshots.append(identity);
        }
        if (!sameObject(files::captureSourceSnapshot(visiblePath), identity)) {
            result.error = "A destination directory's visible path does not match its retained object: "
                + visiblePath;
            return result;
        }
    }
    result.ok = true;
#else
    result.error = state.failure;
#endif
    return result;
}

GuardedBatchRenameResult guardedBatchRename(
    const QList<GuardedBatchRenameItem>& items, const GuardedBatchRenameHooks& hooks)
{
    GuardedBatchRenameResult result;
    if (items.isEmpty()) {
        result.errors.append("A guarded batch rename requires at least one changed item.");
        return result;
    }
#ifdef Q_OS_WIN
    enum class Location { Source, Staging, Destination };
    struct HeldItem
    {
        GuardedBatchRenameItem item;
        Handle handle;
        HANDLE parent = INVALID_HANDLE_VALUE;
        QString staging;
        Location location = Location::Source;

        HeldItem(GuardedBatchRenameItem value, HANDLE nativeHandle, HANDLE parentHandle)
            : item(std::move(value)), handle(nativeHandle), parent(parentHandle) {}
        HeldItem(HeldItem&&) noexcept = default;
        HeldItem(const HeldItem&) = delete;
    };

    const auto renameRelative = [](HANDLE object, HANDLE parent, const QString& name,
                                    DWORD* nativeCode) {
        const auto setInformation = nativeFunction<SetNativeInformation>("NtSetInformationFile");
        if (!setInformation) {
            if (nativeCode) *nativeCode = ERROR_NOT_SUPPORTED;
            return false;
        }
        const size_t nameBytes = static_cast<size_t>(name.size()) * sizeof(wchar_t);
        const size_t bufferBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
        std::vector<std::max_align_t> storage((bufferBytes + sizeof(std::max_align_t) - 1)
            / sizeof(std::max_align_t));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        info->ReplaceIfExists = FALSE;
        info->RootDirectory = parent;
        info->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(info->FileName, name.utf16(), nameBytes);
        IO_STATUS_BLOCK io{};
        const NTSTATUS status = setInformation(object, &io, info,
            static_cast<ULONG>(bufferBytes), 10);
        if (nativeCode)
            *nativeCode = status < 0 ? windowsError(status) : ERROR_SUCCESS;
        return status >= 0;
    };
    const auto identityAt = [](const QString& path, const files::SourceSnapshot& expected) {
        return sameObject(files::captureSourceSnapshot(path), expected);
    };

    QHash<QString, HANDLE> parentHandles;
    std::vector<Handle> retainedParents;
    retainedParents.reserve(static_cast<size_t>(items.size()));
    QSet<QString> sourceKeys;
    QSet<QString> destinationKeys;
    for (const auto& item : items) {
        const QFileInfo source(item.source);
        const QFileInfo destination(item.destination);
        const QString sourceParent = pathKey(source.absolutePath());
        const QString destinationParent = pathKey(destination.absolutePath());
        const QString sourceKey = (sourceParent + '/' + source.fileName()).toCaseFolded();
        const QString destinationKey = (destinationParent + '/' + destination.fileName()).toCaseFolded();
        if (!QDir::isAbsolutePath(item.source) || !QDir::isAbsolutePath(item.destination)
                || sourceParent != destinationParent || sourceKeys.contains(sourceKey)
                || destinationKeys.contains(destinationKey)
                || !files::validateWindowsName(source.fileName()).valid
                || !files::validateWindowsName(destination.fileName()).valid
                || !regular(item.expectedSource, item.directory)) {
            result.errors.append("The guarded batch mapping is invalid or duplicated: " + item.source);
            return result;
        }
        sourceKeys.insert(sourceKey);
        destinationKeys.insert(destinationKey);
        if (parentHandles.contains(sourceParent))
            continue;
        const HANDLE parent = CreateFileW(
            reinterpret_cast<LPCWSTR>(extendedLengthPath(source.absolutePath()).utf16()),
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL,
            nullptr);
        if (parent == INVALID_HANDLE_VALUE) {
            result.errors.append(QStringLiteral("Could not retain the rename folder (Windows error %1): %2")
                .arg(GetLastError()).arg(source.absolutePath()));
            return result;
        }
        retainedParents.emplace_back(parent);
        if (!regular(snapshot(parent), true)) {
            result.errors.append("A rename folder is unavailable or a reparse point: "
                + source.absolutePath());
            return result;
        }
        parentHandles.insert(sourceParent, parent);
    }
    for (const auto& item : items) {
        const QString destinationKey = (pathKey(QFileInfo(item.destination).absolutePath())
            + '/' + QFileInfo(item.destination).fileName()).toCaseFolded();
        if (!sourceKeys.contains(destinationKey)
                && nativePathKind(item.destination) != NativePathKind::Missing) {
            result.errors.append("A rename destination already exists or cannot be inspected: "
                + item.destination);
            return result;
        }
    }

    std::vector<HeldItem> held;
    held.reserve(static_cast<size_t>(items.size()));
    for (const auto& item : items) {
        DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL;
        if (item.directory)
            flags |= FILE_FLAG_BACKUP_SEMANTICS;
        const HANDLE object = CreateFileW(
            reinterpret_cast<LPCWSTR>(extendedLengthPath(item.source).utf16()),
            DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, flags, nullptr);
        if (object == INVALID_HANDLE_VALUE) {
            result.errors.append(QStringLiteral("Could not retain a rename source (Windows error %1): %2")
                .arg(GetLastError()).arg(item.source));
            return result;
        }
        const auto actual = snapshot(object);
        if (actual != item.expectedSource || !regular(actual, item.directory)) {
            CloseHandle(object);
            result.errors.append("A rename source changed before it could be retained: " + item.source);
            return result;
        }
        held.emplace_back(item, object,
            parentHandles.value(pathKey(QFileInfo(item.source).absolutePath()), INVALID_HANDLE_VALUE));
    }
    if (hooks.afterPrepared)
        hooks.afterPrepared();

    const auto recoveryPath = [](const HeldItem& item) {
        switch (item.location) {
        case Location::Source: return item.item.source;
        case Location::Staging: return item.staging;
        case Location::Destination: return item.item.destination;
        }
        return item.item.source;
    };
    const auto finishFailure = [&] {
        for (const HeldItem& item : held) {
            if (item.location == Location::Destination)
                ++result.renamed;
            if (item.location != Location::Source) {
                result.rollbackComplete = false;
                const QString path = recoveryPath(item);
                if (!result.manualRecoveryPaths.contains(path))
                    result.manualRecoveryPaths.append(path);
            }
        }
        for (const QString& path : result.manualRecoveryPaths)
            result.errors.append("Manual recovery required for: " + path);
        return result;
    };

    for (qsizetype index = 0; index < static_cast<qsizetype>(held.size()); ++index) {
        HeldItem& entry = held[static_cast<size_t>(index)];
        const QFileInfo source(entry.item.source);
        for (int attempt = 0; attempt < 100 && entry.staging.isEmpty(); ++attempt) {
            const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
            const QString candidate = source.dir().filePath(
                ".file-manager-rename-" + token + ".tmp");
            if (nativePathKind(candidate) == NativePathKind::Missing)
                entry.staging = candidate;
        }
        DWORD code = ERROR_SUCCESS;
        if (entry.staging.isEmpty() || snapshot(entry.handle.get()) != entry.item.expectedSource
                || !renameRelative(entry.handle.get(), entry.parent,
                    QFileInfo(entry.staging).fileName(), &code)) {
            result.errors.append(QStringLiteral("Could not stage retained source (Windows error %1): %2")
                .arg(code).arg(entry.item.source));
            for (qsizetype rollback = index - 1; rollback >= 0; --rollback) {
                HeldItem& prior = held[static_cast<size_t>(rollback)];
                if (renameRelative(prior.handle.get(), prior.parent,
                        QFileInfo(prior.item.source).fileName(), nullptr))
                    prior.location = Location::Source;
            }
            return finishFailure();
        }
        entry.location = Location::Staging;
    }

    for (qsizetype index = 0; index < static_cast<qsizetype>(held.size()); ++index) {
        HeldItem& entry = held[static_cast<size_t>(index)];
        if (hooks.beforeCommit)
            hooks.beforeCommit(index);
        DWORD code = ERROR_SUCCESS;
        if (snapshot(entry.handle.get()) != entry.item.expectedSource
                || !renameRelative(entry.handle.get(), entry.parent,
                    QFileInfo(entry.item.destination).fileName(), &code)
                || !identityAt(entry.item.destination, entry.item.expectedSource)) {
            result.errors.append(QStringLiteral("Could not commit retained source (Windows error %1): %2")
                .arg(code).arg(entry.item.destination));
            for (qsizetype committed = index - 1; committed >= 0; --committed) {
                HeldItem& prior = held[static_cast<size_t>(committed)];
                if (prior.location == Location::Destination
                        && renameRelative(prior.handle.get(), prior.parent,
                            QFileInfo(prior.staging).fileName(), nullptr))
                    prior.location = Location::Staging;
            }
            for (HeldItem& rollback : held) {
                if (rollback.location == Location::Staging
                        && renameRelative(rollback.handle.get(), rollback.parent,
                            QFileInfo(rollback.item.source).fileName(), nullptr))
                    rollback.location = Location::Source;
            }
            return finishFailure();
        }
        entry.location = Location::Destination;
    }
    result.ok = true;
    result.renamed = static_cast<int>(held.size());
#else
    Q_UNUSED(hooks);
    result.errors.append("Guarded batch renaming requires Windows.");
#endif
    return result;
}

struct GuardedFileRename::State
{
    QString failure;
    QString source;
    QString destination;
    files::SourceSnapshot expected;
    bool attempted = false;
    quint32 nativeCode = 0;
#ifdef Q_OS_WIN
    std::vector<Handle> directories;
    QHash<QString, HANDLE> pinned;
    std::unique_ptr<Handle> file;
    HANDLE targetDirectory = INVALID_HANDLE_VALUE;

    bool fail(const QString& message, DWORD code = ERROR_SUCCESS)
    {
        nativeCode = code;
        failure = code ? message + QStringLiteral(" (Windows error %1)").arg(code) : message;
        return false;
    }

    bool pinChain(QString path)
    {
        QStringList chain;
        for (;;) {
            if (pinned.contains(pathKey(path)))
                break;
            chain.prepend(path);
            const QString parent = QFileInfo(path).absolutePath();
            if (pathKey(parent) == pathKey(path))
                break;
            if (!files::validateWindowsName(QFileInfo(path).fileName()).valid)
                return fail("Unsupported directory name in the reviewed path.");
            path = parent;
        }
        // Open root-to-leaf so an already opened parent cannot be renamed while opening its child.
        for (const QString& entry : chain) {
            const HANDLE handle = CreateFileW(
                reinterpret_cast<LPCWSTR>(extendedLengthPath(entry).utf16()),
                FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL,
                nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return fail("Could not guard directory: " + entry, GetLastError());
            directories.emplace_back(handle);
            if (!regular(snapshot(handle), true))
                return fail("A directory is unavailable or a reparse point: " + entry);
            pinned.insert(pathKey(entry), handle);
        }
        return true;
    }
#endif
};

GuardedFileRename::GuardedFileRename(const QString& source, const files::SourceSnapshot& expectedSource,
    const QString& destination, const QString& reviewedRoot, const files::SourceSnapshot& expectedRoot)
    : state_(std::make_unique<State>())
{
    auto& state = *state_;
    state.source = source;
    state.destination = destination;
    state.expected = expectedSource;
#ifdef Q_OS_WIN
    if (!QDir::isAbsolutePath(source) || !QDir::isAbsolutePath(destination)
        || !QDir::isAbsolutePath(reviewedRoot) || !files::validateWindowsPath(source).valid
        || !files::validateWindowsPath(destination).valid
        || QFileInfo(source).fileName() != QFileInfo(destination).fileName()
        || files::isPathInside(source, reviewedRoot) || !files::isPathInside(destination, reviewedRoot)
        || !regular(expectedSource, false) || !regular(expectedRoot, true)) {
        state.fail("Invalid reviewed same-volume rename request.");
        return;
    }
    if (!state.pinChain(reviewedRoot)
        || !sameObject(snapshot(state.pinned.value(pathKey(reviewedRoot))), expectedRoot)) {
        if (state.failure.isEmpty())
            state.fail("The reviewed destination folder was replaced.");
        return;
    }
    if (!state.pinChain(QFileInfo(source).absolutePath())
        || !state.pinChain(QFileInfo(destination).absolutePath()))
        return;
    bool insideRetainedRoot = false;
    QString ancestor = QFileInfo(destination).absolutePath();
    for (;;) {
        if (sameObject(snapshot(state.pinned.value(pathKey(ancestor), INVALID_HANDLE_VALUE)), expectedRoot)) {
            insideRetainedRoot = true;
            break;
        }
        const QString parent = QFileInfo(ancestor).absolutePath();
        if (pathKey(parent) == pathKey(ancestor))
            break;
        ancestor = parent;
    }
    if (!insideRetainedRoot) {
        state.fail("The destination ancestry does not contain the reviewed folder identity.");
        return;
    }
    state.targetDirectory = state.pinned.value(pathKey(QFileInfo(destination).absolutePath()));
    if (snapshot(state.targetDirectory).volumeSerial != expectedSource.volumeSerial) {
        state.fail("Cross-volume moves require the guarded copy backend.", ERROR_NOT_SAME_DEVICE);
        return;
    }
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(extendedLengthPath(source).utf16()),
        FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        state.fail("Could not guard the reviewed source.", GetLastError());
        return;
    }
    state.file = std::make_unique<Handle>(handle);
    if (snapshot(handle) != expectedSource) {
        state.fail("The source changed since the preview.");
        return;
    }
    if (nativePathKind(destination) != NativePathKind::Missing)
        state.fail("The reviewed destination already exists or cannot be inspected.");
#else
    Q_UNUSED(expectedRoot);
    Q_UNUSED(reviewedRoot);
    state.failure = "Guarded file renaming requires Windows.";
#endif
}

GuardedFileRename::~GuardedFileRename() = default;

bool GuardedFileRename::ready() const
{
    return state_->failure.isEmpty() && !state_->attempted;
}

QString GuardedFileRename::error() const { return state_->failure; }

GuardedRenameResult GuardedFileRename::commit(const std::atomic_bool* cancelled)
{
    auto& state = *state_;
    if (!ready())
        return {false, false, state.failure.isEmpty() ? QStringLiteral("This rename was already attempted.")
            : state.failure, state.nativeCode, {}};
    state.attempted = true;
    if (cancelled && cancelled->load(std::memory_order_relaxed))
        return {false, true, "Rename cancelled before commit.", 0, {}};
#ifdef Q_OS_WIN
    if (snapshot(state.file->get()) != state.expected)
        return {false, false, "The held source metadata changed before commit.", 0, {}};
    for (const auto& directory : state.directories) {
        if (!regular(snapshot(directory.get()), true))
            return {false, false, "A guarded directory changed before commit.", 0, {}};
    }
    // Use the documented NT directory-relative rename contract. The Win32 wrapper rejects
    // a non-null RootDirectory on the tested host; resolving an absolute path again would
    // unnecessarily expose the destination to path redirection.
    const auto setInformation = nativeFunction<SetNativeInformation>("NtSetInformationFile");
    if (!setInformation)
        return {false, false, "Windows directory-relative rename is unavailable.", ERROR_NOT_SUPPORTED, {}};
    const QString name = QFileInfo(state.destination).fileName();
    const size_t nameBytes = static_cast<size_t>(name.size()) * sizeof(wchar_t);
    const size_t bufferBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
    // max_align_t storage keeps FILE_RENAME_INFO correctly aligned on every Windows target.
    std::vector<std::max_align_t> storage((bufferBytes + sizeof(std::max_align_t) - 1)
        / sizeof(std::max_align_t));
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = state.targetDirectory;
    info->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(info->FileName, name.utf16(), nameBytes);
    IO_STATUS_BLOCK io{};
    constexpr ULONG renameInformation = 10; // FILE_INFORMATION_CLASS::FileRenameInformation
    if (cancelled && cancelled->load(std::memory_order_relaxed))
        return {false, true, "Rename cancelled before commit.", 0, {}};
    const NTSTATUS status = setInformation(state.file->get(), &io, info,
        static_cast<ULONG>(bufferBytes), renameInformation);
    if (status < 0) {
        const DWORD code = windowsError(status);
        return {false, false, QStringLiteral("Windows refused the rename (error %1). No copy/delete fallback was used.")
            .arg(code), code, {}};
    }
    // Success means the namespace mutation happened, even if the metadata query now fails.
    // Never report this as an untouched source or attempt a path-based rollback.
    const auto committed = snapshot(state.file->get());
    const auto visible = files::captureSourceSnapshot(state.destination);
    const QString warning = sameObject(committed, visible) ? QString{}
        : QStringLiteral("The file moved, but its destination path could not be verified. Manual review is required.");
    return {true, false, warning, 0, committed};
#else
    return {false, false, state.failure, 0, {}};
#endif
}

} // namespace filemanager::platform
