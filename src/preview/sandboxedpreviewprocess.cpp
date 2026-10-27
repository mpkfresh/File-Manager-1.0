#include "sandboxedpreviewprocess.h"

#include "previewidentity.h"
#include "platform/cloudfilestatus.h"
#include "platform/nativefilecopy.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <cfapi.h>
#include <userenv.h>
#include <aclapi.h>

#include <array>
#include <vector>
#endif

namespace filemanager::preview {
namespace {

QString defaultHelperExecutable()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        "FileManagerPreviewHost.exe");
#else
        "FileManagerPreviewHost");
#endif
}

SandboxedPreviewProcessResult runCompatibilityHelper(const QString& sourcePath,
    const QString& mode, int timeoutMilliseconds, qsizetype maximumOutputBytes,
    const QString& executable, const std::atomic_bool* cancel)
{
    QProcess helper;
    helper.start(executable, {mode, QFileInfo(sourcePath).absoluteFilePath()}, QIODevice::ReadOnly);
    if (!helper.waitForStarted(qMin(timeoutMilliseconds, 1000)))
        return {false, false, false, -1, {}, "The isolated preview helper could not start."};
    int waited = 0;
    while (!helper.waitForFinished(50)) {
        waited += 50;
        if ((cancel && cancel->load()) || waited >= timeoutMilliseconds) {
            helper.kill();
            helper.waitForFinished(1000);
            return {false, false, false, -1, {}, cancel && cancel->load()
                ? "Preview cancelled." : "The isolated preview helper timed out."};
        }
        if (helper.bytesAvailable() > maximumOutputBytes) {
            helper.kill();
            helper.waitForFinished(1000);
            return {false, false, false, -1, {},
                "The isolated preview reply exceeded its limit."};
        }
    }
    const QByteArray output = helper.readAllStandardOutput();
    if (output.size() > maximumOutputBytes)
        return {false, false, false, helper.exitCode(), {},
            "The isolated preview reply exceeded its limit."};
    if (helper.exitStatus() != QProcess::NormalExit || helper.exitCode() != 0)
        return {false, false, false, helper.exitCode(), output,
            "The isolated preview helper failed or crashed."};
    return {true, false, false, helper.exitCode(), output, {}};
}

#ifdef Q_OS_WIN
constexpr SIZE_T PreviewMemoryLimit = 768ULL * 1024 * 1024;

bool contentReadyCloudPlaceholder(const FILE_ATTRIBUTE_TAG_INFO& info)
{
    if ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0
            || (info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE
                | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) != 0)
        return false;
    const auto placeholderState = CfGetPlaceholderStateFromAttributeTag(
        info.FileAttributes, info.ReparseTag);
    const DWORD state = static_cast<DWORD>(placeholderState);
    return placeholderState != CF_PLACEHOLDER_STATE_INVALID
        && (state & CF_PLACEHOLDER_STATE_PLACEHOLDER) != 0
        && (state & CF_PLACEHOLDER_STATE_PARTIAL) == 0;
}

QString windowsError(const QString& prefix, DWORD code = GetLastError())
{
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    const QString detail = length && message ? QString::fromWCharArray(message, length).trimmed()
                                             : QString::number(code);
    if (message)
        LocalFree(message);
    return prefix + ": " + detail;
}

QString quotedArgument(QString value)
{
    value.replace('"', "\\\"");
    return '"' + value + '"';
}

bool appContainerSid(PSID* sid, QString* error)
{
    *sid = nullptr;
    HRESULT result = CreateAppContainerProfile(AppContainerProfileName, L"File Manager Preview Host",
        L"Networkless, read-only file preview isolation", nullptr, 0, sid);
    if (SUCCEEDED(result))
        return true;
    const HRESULT creationResult = result;
    result = DeriveAppContainerSidFromAppContainerName(AppContainerProfileName, sid);
    if (FAILED(result)) {
        *error = QString("The AppContainer preview profile could not be created (0x%1).")
            .arg(static_cast<quint32>(creationResult), 8, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

bool copyRuntimeFile(const QString& source, const QString& destination, QString* error)
{
    const QFileInfo sourceInfo(source);
    const QFileInfo destinationInfo(destination);
    const QString nativeDestination = QDir::toNativeSeparators(destination);
    const DWORD attributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(nativeDestination.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        *error = "The private preview runtime refuses a redirected or directory component: "
            + destination;
        return false;
    }
    if (destinationInfo.isFile() && destinationInfo.size() == sourceInfo.size()) {
        QFile sourceFile(source);
        QFile destinationFile(destination);
        if (sourceFile.open(QIODevice::ReadOnly) && destinationFile.open(QIODevice::ReadOnly)
                && QCryptographicHash::hash(sourceFile.readAll(), QCryptographicHash::Sha256)
                    == QCryptographicHash::hash(destinationFile.readAll(),
                        QCryptographicHash::Sha256))
            return true;
    }
    // Optical test media and some installers copy files with the read-only
    // attribute intact. The next version must be able to replace its own
    // private runtime file after the previous helper has exited.
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_READONLY)
                && !SetFileAttributesW(reinterpret_cast<LPCWSTR>(nativeDestination.utf16()),
                    attributes & ~FILE_ATTRIBUTE_READONLY)) {
            *error = windowsError("The private preview runtime could not make its old file replaceable");
            return false;
        }
    }
    if (QFile::exists(destination) && !QFile::remove(destination)) {
        *error = "The private preview runtime could not replace " + destination;
        return false;
    }
    if (!QFile::copy(source, destination)) {
        *error = "The private preview runtime could not copy " + source;
        return false;
    }
    return true;
}

bool grantReadExecute(const QString& path, PSID sid, bool directory, QString* error)
{
    PACL existingAcl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const QString nativePath = QDir::toNativeSeparators(path);
    DWORD status = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(nativePath.utf16())), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &existingAcl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        *error = windowsError("The private preview runtime permissions could not be inspected",
            status);
        return false;
    }
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    PACL updatedAcl = nullptr;
    status = SetEntriesInAclW(1, &access, existingAcl, &updatedAcl);
    if (status == ERROR_SUCCESS)
        status = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(nativePath.utf16())), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, updatedAcl, nullptr);
    if (updatedAcl) LocalFree(updatedAcl);
    if (descriptor) LocalFree(descriptor);
    if (status != ERROR_SUCCESS) {
        *error = windowsError("The private preview runtime permissions could not be applied",
            status);
        return false;
    }
    return true;
}

QString stageRuntime(const QString& executable, PSID sid, QString* error)
{
    const QString destinationRoot = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation)).filePath(
            QString::fromLatin1(PreviewRuntimeDirectoryName));
    if (!QDir().mkpath(destinationRoot)) {
        *error = "The private AppContainer runtime folder could not be created.";
        return {};
    }
    const QDir sourceRoot(QFileInfo(executable).absolutePath());
    QSet<QString> allowedRelativePaths;
    // The helper runs from this private folder, not beside the main executable.
    // Keep its direct Qt imports in sync with the package verifier; omitting an
    // imported DLL makes the restricted process exit before its entry point.
    const QStringList runtimeFiles = {QFileInfo(executable).fileName(), "Qt6Core.dll", "Qt6Gui.dll",
        "Qt6Multimedia.dll", "Qt6Network.dll", "Qt6Sql.dll", "Qt6Svg.dll",
        "Qt6Widgets.dll", "avcodec-61.dll",
        "avformat-61.dll", "avutil-59.dll", "swresample-5.dll", "swscale-8.dll",
        // Clean machines rely on the package's app-local CRT. The private
        // runtime must carry those dependencies without a global installation.
        "vcruntime140.dll", "vcruntime140_1.dll", "vcruntime140_threads.dll",
        "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
        "msvcp140_codecvt_ids.dll", "concrt140.dll", "vccorlib140.dll"};
    for (const QString& name : runtimeFiles) {
        const QString source = sourceRoot.filePath(name);
        if (QFileInfo::exists(source)) {
            allowedRelativePaths.insert(name);
            if (!copyRuntimeFile(source, QDir(destinationRoot).filePath(name), error))
                return {};
        }
    }
    const QStringList runtimeDirectories = {"imageformats", "multimedia", "networkinformation", "tls"};
    for (const QString& name : runtimeDirectories) {
        const QString sourceDirectory = sourceRoot.filePath(name);
        if (!QFileInfo(sourceDirectory).isDir())
            continue;
        const QString destinationDirectory = QDir(destinationRoot).filePath(name);
        if (!QDir().mkpath(destinationDirectory)) {
            *error = "The private preview plugin folder could not be created.";
            return {};
        }
        QDirIterator iterator(sourceDirectory, QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString source = iterator.next();
            const QString relative = QDir(sourceDirectory).relativeFilePath(source);
            const QString destination = QDir(destinationDirectory).filePath(relative);
            allowedRelativePaths.insert(name + '/' + relative);
            QDir().mkpath(QFileInfo(destination).absolutePath());
            if (!copyRuntimeFile(source, destination, error))
                return {};
        }
    }
    QDirIterator staleIterator(destinationRoot, QDir::Files | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (staleIterator.hasNext()) {
        const QString stalePath = staleIterator.next();
        const QString relative = QDir(destinationRoot).relativeFilePath(stalePath);
        if (!allowedRelativePaths.contains(relative) && !QFile::remove(stalePath)) {
            *error = "The private preview runtime could not remove a stale component.";
            return {};
        }
    }
    if (!grantReadExecute(destinationRoot, sid, true, error))
        return {};
    QDirIterator stagedIterator(destinationRoot,
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (stagedIterator.hasNext()) {
        stagedIterator.next();
        const QFileInfo info = stagedIterator.fileInfo();
        if (!info.isSymLink()
                && !grantReadExecute(info.absoluteFilePath(), sid, info.isDir(), error))
            return {};
    }
    return QDir(destinationRoot).filePath(QFileInfo(executable).fileName());
}

void closeHandle(HANDLE& handle)
{
    if (handle && handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    handle = nullptr;
}

SandboxedPreviewProcessResult runWindowsSandbox(const QString& sourcePath, const QString& mode,
    int timeoutMilliseconds, qsizetype maximumOutputBytes, const QString& executable,
    const std::atomic_bool* cancel)
{
    SandboxedPreviewProcessResult result;
    const auto sourceKind = filemanager::platform::nativePathKind(sourcePath);
    const bool cloudCandidate = sourceKind == filemanager::platform::NativePathKind::CloudPlaceholder
        || sourceKind == filemanager::platform::NativePathKind::UnsupportedReparse;
    if (sourceKind != filemanager::platform::NativePathKind::Regular
            && !cloudCandidate) {
        result.error = "The selected file is unavailable or a cloud placeholder. "
            "Make it available locally before previewing.";
        return result;
    }
    const QString nativePath = QDir::toNativeSeparators(sourcePath);
    HANDLE selected = nullptr;
    BY_HANDLE_FILE_INFORMATION selectedIdentity{};
    if (cloudCandidate) {
        // Keep an attribute-only, no-recall handle open across the data open.
        // This rejects partial placeholders before requesting read access.
        selected = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
            FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL,
            nullptr);
        FILE_ATTRIBUTE_TAG_INFO selectedTag{};
        const bool ready = selected != INVALID_HANDLE_VALUE
            && GetFileInformationByHandleEx(selected, FileAttributeTagInfo,
                &selectedTag, sizeof(selectedTag)) != FALSE
            && contentReadyCloudPlaceholder(selectedTag)
            && GetFileInformationByHandle(selected, &selectedIdentity) != FALSE;
        if (!ready) {
            if (selected == INVALID_HANDLE_VALUE)
                selected = nullptr;
            closeHandle(selected);
            result.error = "The selected cloud placeholder is not fully available locally.";
            return result;
        }
    }
    const DWORD dataOpenFlags = FILE_FLAG_OPEN_NO_RECALL
        | (sourceKind == filemanager::platform::NativePathKind::Regular
            ? FILE_FLAG_OPEN_REPARSE_POINT : 0);
    HANDLE source = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, dataOpenFlags, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        const DWORD openError = GetLastError();
        source = nullptr;
        closeHandle(selected);
        result.error = windowsError("The selected file could not be opened read-only", openError);
        return result;
    }
    // The path may have changed since the metadata-only preflight. Inspect the
    // opened object before handing its no-recall handle to the restricted host.
    FILE_ATTRIBUTE_TAG_INFO opened{};
    const bool inspected = GetFileInformationByHandleEx(source, FileAttributeTagInfo,
        &opened, sizeof(opened)) != FALSE;
    const DWORD blockedAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE
        | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    bool locallyReady = inspected && (opened.FileAttributes & blockedAttributes) == 0;
    if (locallyReady && cloudCandidate) {
        BY_HANDLE_FILE_INFORMATION openedIdentity{};
        // Cloud Files may hide its implementation reparse tag on a data handle.
        // The attribute handle above identified the ready placeholder; the
        // data handle must still name that same file and require no recall.
        const bool dataStateReady = (opened.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0
            || contentReadyCloudPlaceholder(opened);
        locallyReady = dataStateReady
            && GetFileInformationByHandle(source, &openedIdentity) != FALSE
            && openedIdentity.dwVolumeSerialNumber == selectedIdentity.dwVolumeSerialNumber
            && openedIdentity.nFileIndexHigh == selectedIdentity.nFileIndexHigh
            && openedIdentity.nFileIndexLow == selectedIdentity.nFileIndexLow;
    } else if (locallyReady) {
        locallyReady = (opened.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    }
    closeHandle(selected);
    if (!locallyReady) {
        closeHandle(source);
        result.error = "The selected file is unavailable or a cloud placeholder. "
            "Make it available locally before previewing.";
        return result;
    }
    SetHandleInformation(source, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    SECURITY_ATTRIBUTES pipeSecurity{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    HANDLE diagnosticReadPipe = nullptr;
    HANDLE diagnosticWritePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &pipeSecurity, 0)
            || !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)
            || !CreatePipe(&diagnosticReadPipe, &diagnosticWritePipe, &pipeSecurity, 0)
            || !SetHandleInformation(diagnosticReadPipe, HANDLE_FLAG_INHERIT, 0)) {
        result.error = windowsError("The preview result channels could not be created");
        closeHandle(source); closeHandle(readPipe); closeHandle(writePipe);
        closeHandle(diagnosticReadPipe); closeHandle(diagnosticWritePipe);
        return result;
    }

    PSID sid = nullptr;
    if (!appContainerSid(&sid, &result.error)) {
        closeHandle(source); closeHandle(readPipe); closeHandle(writePipe);
        closeHandle(diagnosticReadPipe); closeHandle(diagnosticWritePipe);
        return result;
    }
    static QMutex stagingMutex;
    static QString stagedExecutable;
    {
        const QMutexLocker lock(&stagingMutex);
        if (stagedExecutable.isEmpty() || !QFileInfo::exists(stagedExecutable)) {
            stagedExecutable = stageRuntime(executable, sid, &result.error);
            if (stagedExecutable.isEmpty() || !QFileInfo::exists(stagedExecutable)) {
                FreeSid(sid); closeHandle(source); closeHandle(readPipe); closeHandle(writePipe);
                closeHandle(diagnosticReadPipe); closeHandle(diagnosticWritePipe);
                return result;
            }
        }
    }
    SECURITY_CAPABILITIES capabilities{};
    capabilities.AppContainerSid = sid;
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attributeBytes);
    std::vector<std::byte> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 2, 0, &attributeBytes)) {
        result.error = windowsError("The preview security attributes could not be initialized");
        FreeSid(sid); closeHandle(source); closeHandle(readPipe); closeHandle(writePipe);
        closeHandle(diagnosticReadPipe); closeHandle(diagnosticWritePipe);
        return result;
    }
    std::array<HANDLE, 3> inherited{source, writePipe, diagnosticWritePipe};
    const bool attributesReady = UpdateProcThreadAttribute(attributes, 0,
            PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &capabilities, sizeof(capabilities),
            nullptr, nullptr)
        && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited.data(), sizeof(inherited), nullptr, nullptr);
    if (!attributesReady) {
        result.error = windowsError("The preview security attributes could not be applied");
        DeleteProcThreadAttributeList(attributes); FreeSid(sid);
        closeHandle(source); closeHandle(readPipe); closeHandle(writePipe);
        closeHandle(diagnosticReadPipe); closeHandle(diagnosticWritePipe);
        return result;
    }

    const QFileInfo info(sourcePath);
    const QString command = quotedArgument(stagedExecutable) + " " + quotedArgument(mode + "-handle")
        + " " + QString::number(reinterpret_cast<quintptr>(source)) + " "
        + quotedArgument(info.fileName()) + " " + QString::number(info.size()) + " "
        + QString::number(info.lastModified().toMSecsSinceEpoch());
    std::vector<wchar_t> commandBuffer(static_cast<size_t>(command.size()) + 1);
    command.toWCharArray(commandBuffer.data());
    commandBuffer[static_cast<size_t>(command.size())] = L'\0';

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullptr;
    startup.StartupInfo.hStdOutput = writePipe;
    startup.StartupInfo.hStdError = diagnosticWritePipe;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(reinterpret_cast<LPCWSTR>(stagedExecutable.utf16()),
        commandBuffer.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, reinterpret_cast<LPCWSTR>(QFileInfo(stagedExecutable).absolutePath().utf16()),
        &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    FreeSid(sid);
    closeHandle(writePipe);
    closeHandle(diagnosticWritePipe);
    if (!created) {
        const DWORD launchError = GetLastError();
        const bool stagedPresent = QFileInfo::exists(stagedExecutable);
        result.error = windowsError(
            stagedPresent
                ? "The AppContainer preview helper could not start (staged file present)"
                : "The AppContainer preview helper could not start (staged file missing)",
            launchError);
        closeHandle(source); closeHandle(readPipe); closeHandle(diagnosticReadPipe);
        return result;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION
        | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = PreviewMemoryLimit;
    const bool jobReady = job
        && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))
        && AssignProcessToJobObject(job, process.hProcess);
    if (!jobReady || ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        result.error = windowsError("The preview resource boundary could not be applied");
        TerminateProcess(process.hProcess, 1);
        closeHandle(process.hThread); closeHandle(process.hProcess); closeHandle(job);
        closeHandle(source); closeHandle(readPipe); closeHandle(diagnosticReadPipe);
        return result;
    }
    result.sandboxed = true;
    result.jobRestricted = true;
    closeHandle(process.hThread);
    closeHandle(source);

    QElapsedTimer timer;
    timer.start();
    bool exceeded = false;
    bool diagnosticsExceeded = false;
    bool stopped = false;
    quint64 diagnosticBytes = 0;
    const auto drain = [&](HANDLE pipe, QByteArray* output) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || !available)
            return false;
        std::array<char, 64 * 1024> buffer{};
        DWORD read = 0;
        const DWORD requested = qMin<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(pipe, buffer.data(), requested, &read, nullptr) || !read)
            return false;
        if (output) output->append(buffer.data(), static_cast<qsizetype>(read));
        else diagnosticBytes += read;
        return true;
    };
    for (;;) {
        drain(readPipe, &result.standardOutput);
        drain(diagnosticReadPipe, nullptr);
        if (result.standardOutput.size() > maximumOutputBytes) { exceeded = true; break; }
        if (diagnosticBytes > 1024 * 1024) {
            diagnosticsExceeded = true;
            exceeded = true;
            break;
        }
        if (cancel && cancel->load()) { stopped = true; break; }
        if (timer.elapsed() >= timeoutMilliseconds) break;
        if (WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0) {
            for (;;) {
                const bool replyRead = drain(readPipe, &result.standardOutput);
                const bool diagnosticRead = drain(diagnosticReadPipe, nullptr);
                if (result.standardOutput.size() > maximumOutputBytes
                        || diagnosticBytes > 1024 * 1024) {
                    exceeded = true;
                    diagnosticsExceeded = diagnosticBytes > 1024 * 1024;
                    break;
                }
                if (!replyRead && !diagnosticRead) break;
            }
            break;
        }
        QThread::msleep(10);
    }
    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exitCode);
    if (exitCode == STILL_ACTIVE)
        TerminateJobObject(job, 1);
    WaitForSingleObject(process.hProcess, 1000);
    GetExitCodeProcess(process.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
    closeHandle(process.hProcess); closeHandle(job); closeHandle(readPipe);
    closeHandle(diagnosticReadPipe);
    if (diagnosticBytes > 1024 * 1024) {
        exceeded = true;
        diagnosticsExceeded = true;
    }

    if (exceeded) result.error = diagnosticsExceeded
        ? "The isolated preview diagnostics exceeded their limit."
        : "The isolated preview reply exceeded its limit.";
    else if (stopped) result.error = "Preview cancelled.";
    else if (timer.elapsed() >= timeoutMilliseconds && exitCode != 0)
        result.error = "The isolated preview helper timed out.";
    else if (exitCode != 0) {
        const QString detail = QString::fromUtf8(result.standardOutput).trimmed().left(1024);
        result.error = QString("The isolated preview helper failed or crashed (0x%1)%2")
            .arg(exitCode, 8, 16, QLatin1Char('0'))
            .arg(detail.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + detail);
    }
    else if (result.standardOutput.size() > maximumOutputBytes)
        result.error = "The isolated preview reply exceeded its limit.";
    else result.success = true;
    return result;
}
#endif

} // namespace

SandboxedPreviewProcessResult runSandboxedPreviewHost(const QString& sourcePath,
    const QString& mode, int timeoutMilliseconds, qsizetype maximumOutputBytes,
    const QString& helperExecutable, const std::atomic_bool* cancel)
{
    if (cancel && cancel->load())
        return {false, false, false, -1, {}, "Preview cancelled."};
    const QString executable = helperExecutable.isEmpty() ? defaultHelperExecutable()
                                                           : helperExecutable;
#ifdef Q_OS_WIN
    if (helperExecutable.isEmpty())
        return runWindowsSandbox(sourcePath, mode, timeoutMilliseconds, maximumOutputBytes,
            executable, cancel);
#endif
    return runCompatibilityHelper(sourcePath, mode, timeoutMilliseconds, maximumOutputBytes,
        executable, cancel);
}

} // namespace filemanager::preview
