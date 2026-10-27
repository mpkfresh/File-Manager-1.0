#include "platform/guardedfilerename.h"
#include "platform/nativefilecopy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <iostream>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#endif

namespace {

bool writeFile(const QString& path, const QByteArray& bytes = "not a directory")
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

#ifdef Q_OS_WIN
HANDLE openNative(const QString& path, DWORD access, DWORD flags = 0)
{
    return CreateFileW(reinterpret_cast<LPCWSTR>(
        filemanager::platform::extendedLengthPath(path).utf16()), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        flags | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

bool renameNative(const QString& from, const QString& to)
{
    return MoveFileExW(reinterpret_cast<LPCWSTR>(filemanager::platform::extendedLengthPath(from).utf16()),
        reinterpret_cast<LPCWSTR>(filemanager::platform::extendedLengthPath(to).utf16()), 0) != FALSE;
}

bool createJunction(const QString& linkPath, const QString& targetPath)
{
    QProcess process;
    process.start("cmd.exe", {"/d", "/c", "mklink", "/J", QDir::toNativeSeparators(linkPath),
        QDir::toNativeSeparators(targetPath)});
    const bool finished = process.waitForFinished(30'000);
    if (!finished) {
        process.kill();
        process.waitForFinished(5'000);
    }
    return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
#endif

} // namespace

bool runGuardedDirectoryTests()
{
    using namespace filemanager::files;
    using namespace filemanager::platform;
    bool passed = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: guarded directory: " << message << '\n';
            passed = false;
        }
    };
    QTemporaryDir fixture;
    check(fixture.isValid(), "create disposable fixture");
    if (!fixture.isValid())
        return false;
    const QString shows = fixture.filePath("Shows");
    check(QDir().mkpath(shows), "create reviewed root");
    const auto rootIdentity = captureSourceSnapshot(shows);
#ifdef Q_OS_WIN
    {
        GuardedDirectoryTree tree(shows, rootIdentity);
        check(tree.ready(), "retain reviewed root identity");
        std::atomic_bool cancel{true};
        const auto cancelled = tree.ensure("Cancelled/Season 01", &cancel);
        check(cancelled.cancelled && !cancelled.ok && cancelled.createdDirectories.isEmpty()
            && !QFileInfo::exists(shows + "/Cancelled"), "pre-cancellation creates nothing");
        const auto traversal = tree.ensure("../Escape");
        const auto reserved = tree.ensure("CON/Season 01");
        const auto absolute = tree.ensure(fixture.filePath("Outside"));
        check(!traversal.ok && !reserved.ok && !absolute.ok
            && !QFileInfo::exists(fixture.filePath("Escape"))
            && !QFileInfo::exists(fixture.filePath("Outside")), "absolute, traversal and reserved paths fail closed");
        const auto created = tree.ensure(QString::fromUtf8("Série/Season 01"));
        check(created.ok && !created.cancelled && created.error.isEmpty()
            && created.createdDirectories.size() == 2 && created.createdSnapshots.size() == 2,
            "create complete Unicode series and season chain");
        for (qsizetype index = 0; index < created.createdDirectories.size(); ++index) {
            const auto visible = captureSourceSnapshot(created.createdDirectories.at(index));
            const auto retained = created.createdSnapshots.at(index);
            check(visible.valid && retained.valid
                    && visible.volumeSerial == retained.volumeSerial
                    && visible.fileIndex == retained.fileIndex,
                "return exact identity for every created folder");
        }
        const auto existing = tree.ensure(QString::fromUtf8("Série/Season 01"));
        check(existing.ok && existing.createdDirectories.isEmpty() && existing.createdSnapshots.isEmpty(),
            "reopening retained directories never reports them as newly created");
        const QString input = fixture.filePath("Input");
        const QString episode = input + "/Series.S01E01.mkv";
        const QString destination = shows + QString::fromUtf8("/Série/Season 01/Series.S01E01.mkv");
        check(QDir().mkpath(input) && writeFile(episode, "episode bytes"),
            "create disposable same-volume episode");
        const auto episodeIdentity = captureSourceSnapshot(episode);
        GuardedFileRename move(episode, episodeIdentity, destination, shows, rootIdentity);
        const auto moved = move.commit();
        check(moved.moved && moved.error.isEmpty() && !QFileInfo::exists(episode)
            && captureSourceSnapshot(destination).fileIndex == episodeIdentity.fileIndex,
            "new guarded folders interoperate with handle-bound same-volume rename");
        check(!renameNative(shows + QString::fromUtf8("/Série"), shows + "/ChangedSeries"),
            "created directory remains guarded against rename");
    }
    check(renameNative(shows + QString::fromUtf8("/Série"), shows + "/ChangedSeries")
        && renameNative(shows + "/ChangedSeries", shows + QString::fromUtf8("/Série")),
        "destroying session releases directory guards");
    {
        check(writeFile(shows + "/Blocked"), "create file/directory collision");
        GuardedDirectoryTree tree(shows, rootIdentity);
        const auto collision = tree.ensure("Blocked/Season 01");
        check(!collision.ok && collision.createdDirectories.isEmpty()
            && QFileInfo(shows + "/Blocked").isFile(), "ordinary file cannot be adopted as a directory");
    }
    check(QFile::remove(shows + "/Blocked"), "remove disposable collision");
    {
        auto changedIdentity = rootIdentity;
        ++changedIdentity.fileIndex;
        GuardedDirectoryTree tree(shows, changedIdentity);
        check(!tree.ready() && !tree.ensure("Series").ok && !QFileInfo::exists(shows + "/Series"),
            "wrong retained root identity creates nothing");
    }
    const QString junction = fixture.filePath("JunctionTemplate");
    const QString outside = fixture.filePath("OutsideTarget");
    check(QDir().mkpath(outside) && createJunction(junction, outside), "create disposable junction template");
    QByteArray reparseData(MAXIMUM_REPARSE_DATA_BUFFER_SIZE, '\0');
    DWORD reparseBytes = 0;
    const HANDLE junctionHandle = openNative(junction, FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS);
    const bool readReparse = junctionHandle != INVALID_HANDLE_VALUE
        && DeviceIoControl(junctionHandle, FSCTL_GET_REPARSE_POINT, nullptr, 0,
            reparseData.data(), static_cast<DWORD>(reparseData.size()), &reparseBytes, nullptr);
    if (junctionHandle != INVALID_HANDLE_VALUE) CloseHandle(junctionHandle);
    check(readReparse, "read fixture junction payload");
    bool converted = false;
    {
        GuardedDirectoryTree tree(shows, rootIdentity);
        const auto parent = tree.ensure("LateParent");
        check(parent.ok && parent.createdDirectories.size() == 1, "create empty parent for late conversion");
        const HANDLE attributes = openNative(shows + "/LateParent", FILE_WRITE_ATTRIBUTES,
            FILE_FLAG_BACKUP_SEMANTICS);
        DWORD returned = 0;
        converted = readReparse && attributes != INVALID_HANDLE_VALUE
            && DeviceIoControl(attributes, FSCTL_SET_REPARSE_POINT, reparseData.data(), reparseBytes,
                nullptr, 0, &returned, nullptr);
        if (attributes != INVALID_HANDLE_VALUE) CloseHandle(attributes);
        check(converted, "convert retained empty parent through attribute-only handle");
        const auto redirected = tree.ensure("LateParent/Escape");
        check(!redirected.ok && !QFileInfo::exists(outside + "/Escape"),
            "directory-relative creation refuses late reparse redirection");
    }
    if (converted)
        check(QDir().rmdir(shows + "/LateParent"), "remove only late fixture junction");
    check(QDir().rmdir(junction), "remove junction template itself");
    {
        const QString caseRoot = fixture.filePath("CaseRoot");
        check(QDir().mkpath(caseRoot), "create case-sensitive fixture root");
        const HANDLE handle = openNative(caseRoot, FILE_WRITE_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS);
        const FILE_CASE_SENSITIVE_INFO sensitivity{1};
        const bool enabled = handle != INVALID_HANDLE_VALUE && SetFileInformationByHandle(handle,
            FileCaseSensitiveInfo, const_cast<FILE_CASE_SENSITIVE_INFO*>(&sensitivity), sizeof(sensitivity));
        const DWORD enableError = enabled ? ERROR_SUCCESS : GetLastError();
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (enabled) {
            GuardedDirectoryTree tree(caseRoot, captureSourceSnapshot(caseRoot));
            const auto upper = tree.ensure("Series");
            const auto lower = tree.ensure("series");
            check(upper.ok && lower.ok && upper.createdDirectories.size() == 1
                && lower.createdDirectories.size() == 1
                && captureSourceSnapshot(caseRoot + "/Series").fileIndex
                    != captureSourceSnapshot(caseRoot + "/series").fileIndex,
                "case-sensitive directory creates and retains distinct spellings");
        } else {
            std::cerr << "SKIP: guarded directory case fixture unavailable (Windows error "
                << enableError << ").\n";
        }
    }
#else
    GuardedDirectoryTree tree(shows, rootIdentity);
    check(!tree.ready() && !tree.ensure("Series").ok, "unsupported platform fails closed");
#endif
    return passed;
}
