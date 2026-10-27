#include "platform/guardedfilerename.h"
#include "platform/nativefilecopy.h"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QTemporaryDir>
#include <iostream>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#endif

namespace {

bool writeFile(const QString& path, const QByteArray& bytes = "episode bytes")
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
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
#endif

} // namespace

int runGuardedBatchRenameInterruptionFixture(const QString& root)
{
#ifdef Q_OS_WIN
    using namespace filemanager::files;
    using namespace filemanager::platform;
    const QString first = QDir(root).filePath("source-a.txt");
    const QString second = QDir(root).filePath("source-b.txt");
    const QList<GuardedBatchRenameItem> items{
        {first, QDir(root).filePath("renamed-a.txt"), false, captureSourceSnapshot(first)},
        {second, QDir(root).filePath("renamed-b.txt"), false, captureSourceSnapshot(second)}};
    if (!items[0].expectedSource.valid || !items[1].expectedSource.valid)
        return 74;
    GuardedBatchRenameHooks hooks;
    hooks.beforeCommit = [](qsizetype index) {
        if (index == 0)
            ExitProcess(77);
    };
    guardedBatchRename(items, hooks);
    return 75;
#else
    Q_UNUSED(root);
    return 76;
#endif
}

bool runGuardedFileRenameTests()
{
    using namespace filemanager::files;
    using namespace filemanager::platform;
    bool passed = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: guarded rename: " << message << '\n';
            passed = false;
        }
    };
    QTemporaryDir fixture;
    check(fixture.isValid(), "create disposable fixture");
    if (!fixture.isValid())
        return false;
    const QString input = fixture.filePath("Input");
    const QString shows = fixture.filePath("Shows");
    const QString target = shows + "/Series/Season 01";
    const QString source = input + QString::fromUtf8("/Série.S01E01.mkv");
    const QString destination = target + "/" + QFileInfo(source).fileName();
    check(QDir().mkpath(input) && QDir().mkpath(target) && writeFile(source), "create fixture files");
    const auto rootIdentity = captureSourceSnapshot(shows);
#ifdef Q_OS_WIN
    check(writeFile(source + ":subtitle", "retained stream"), "create NTFS stream fixture");
    const auto original = captureSourceSnapshot(source);
    {
        GuardedFileRename move(source, original, destination, shows, rootIdentity);
        if (!move.ready())
            std::cerr << "Guard preparation: " << move.error().toStdString() << '\n';
        check(move.ready(), "prepare read-only same-volume move with Unicode paths");
        check(QFileInfo::exists(source) && !QFileInfo::exists(destination), "preparation does not move files");
        const HANDLE writer = openNative(source, GENERIC_WRITE);
        check(writer == INVALID_HANDLE_VALUE, "source data writes are blocked while guarded");
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        check(!renameNative(source, source + ".changed"), "source rename is blocked while guarded");
        check(!renameNative(input, fixture.filePath("ChangedInput")), "source parent is pinned");
        check(!renameNative(shows, fixture.filePath("ChangedShows")), "reviewed root is pinned");
        check(!renameNative(target, shows + "/ChangedSeason"), "target parent is pinned");
        const auto result = move.commit();
        if (!result.moved)
            std::cerr << "Guard commit: " << result.error.toStdString() << '\n';
        check(result.moved && !result.cancelled && result.error.isEmpty(), "native handle-bound rename succeeds");
        check(!QFileInfo::exists(source) && QFileInfo::exists(destination), "only the reviewed name moved");
        check(result.destinationSnapshot.valid && result.destinationSnapshot.fileIndex == original.fileIndex
            && result.destinationSnapshot.volumeSerial == original.volumeSerial
            && result.destinationSnapshot.size == original.size
            && result.destinationSnapshot.lastWriteTime == original.lastWriteTime,
            "committed identity, size and write timestamp survive native rename");
        check(!renameNative(destination, destination + ".changed"), "committed file remains guarded until result is recorded");
        check(!move.commit().moved, "session commits at most once");
    }
    check(readFile(destination) == "episode bytes" && readFile(destination + ":subtitle") == "retained stream",
        "default bytes and alternate stream survive unchanged");
    check(renameNative(destination, source), "closing the session releases the file guards");

    auto current = captureSourceSnapshot(source);
    {
        GuardedFileRename move(source, current, destination, shows, rootIdentity);
        std::atomic_bool cancelled{true};
        const auto result = move.commit(&cancelled);
        check(move.error().isEmpty() && result.cancelled && !result.moved && QFileInfo::exists(source)
            && !QFileInfo::exists(destination), "pre-commit cancellation leaves names untouched");
        check(!move.commit().moved, "cancelled sessions cannot be reused");
    }
    {
        GuardedFileRename move(source, current, destination, shows, rootIdentity);
        check(move.ready() && writeFile(destination, "collision"), "introduce collision after preparation");
        const auto result = move.commit();
        check(!result.moved && !result.error.isEmpty() && QFileInfo::exists(source)
            && readFile(destination) == "collision", "late destination collision is never overwritten");
    }
    check(QFile::remove(destination), "remove only disposable collision");
    {
        const HANDLE writer = openNative(source, GENERIC_WRITE);
        check(writer != INVALID_HANDLE_VALUE, "hold pre-existing source writer");
        GuardedFileRename move(source, current, destination, shows, rootIdentity);
        check(!move.ready() && !move.commit().moved, "pre-existing writer prevents preparation");
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    }
    {
        GuardedFileRename move(source, current, destination, shows, rootIdentity);
        const HANDLE attributes = openNative(source, FILE_WRITE_ATTRIBUTES);
        check(attributes != INVALID_HANDLE_VALUE, "attribute access is independent of share flags");
        if (attributes != INVALID_HANDLE_VALUE) {
            ULARGE_INTEGER changed{};
            changed.QuadPart = current.lastWriteTime + 10'000'000;
            const FILETIME time{changed.LowPart, changed.HighPart};
            check(SetFileTime(attributes, nullptr, nullptr, &time) != FALSE, "change held source timestamp");
            CloseHandle(attributes);
        }
        check(!move.commit().moved && QFileInfo::exists(source), "metadata changes are checked again before commit");
    }
    current = captureSourceSnapshot(source);
    {
        const QString retained = input + "/retained.mkv";
        check(renameNative(source, retained) && writeFile(source), "replace source after its snapshot");
        GuardedFileRename move(source, current, destination, shows, rootIdentity);
        check(!move.ready() && !move.commit().moved, "same-path replacement is not adopted");
    }
    current = captureSourceSnapshot(source);
    {
        auto wrongRoot = rootIdentity;
        ++wrongRoot.fileIndex;
        GuardedFileRename move(source, current, destination, shows, wrongRoot);
        check(!move.ready(), "reviewed root identity must match");
    }
    {
        GuardedFileRename move(source, current, target + "/renamed.mkv", shows, rootIdentity);
        check(!move.ready(), "organizer move cannot change basename");
    }
    {
        GuardedFileRename move(source, current, fixture.filePath(QFileInfo(source).fileName()), shows, rootIdentity);
        check(!move.ready(), "destination outside reviewed root is refused");
    }
    {
        GuardedFileRename move(source, current, shows + "/Missing/" + QFileInfo(source).fileName(), shows, rootIdentity);
        check(!move.ready() && !QFileInfo::exists(shows + "/Missing"), "preparation never creates missing folders");
    }
    {
        auto otherVolume = current;
        ++otherVolume.volumeSerial;
        GuardedFileRename move(source, otherVolume, destination, shows, rootIdentity);
        const auto result = move.commit();
        check(!move.ready() && result.nativeCode == ERROR_NOT_SAME_DEVICE && QFileInfo::exists(source),
            "cross-volume request cannot fall back to copy/delete");
    }
    const QString junction = shows + "/Link";
    QProcess link;
    link.start("cmd.exe", {"/d", "/c", "mklink", "/J", QDir::toNativeSeparators(junction),
        QDir::toNativeSeparators(input)});
    const bool linkFinished = link.waitForFinished(30'000);
    if (!linkFinished) { link.kill(); link.waitForFinished(5'000); }
    const bool linked = linkFinished && link.exitStatus() == QProcess::NormalExit && link.exitCode() == 0;
    check(linked, "create disposable junction boundary");
    if (linked) {
        {
            GuardedFileRename move(source, current, junction + "/" + QFileInfo(source).fileName(), shows, rootIdentity);
            check(!move.ready() && !move.commit().moved, "junction destination ancestor is rejected");
        }
        QByteArray reparseData(MAXIMUM_REPARSE_DATA_BUFFER_SIZE, '\0');
        DWORD reparseBytes = 0;
        const HANDLE junctionHandle = openNative(junction, FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS);
        const bool readReparse = junctionHandle != INVALID_HANDLE_VALUE
            && DeviceIoControl(junctionHandle, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                reparseData.data(), static_cast<DWORD>(reparseData.size()), &reparseBytes, nullptr);
        check(readReparse, "read disposable junction data for a late attribute-only change");
        if (junctionHandle != INVALID_HANDLE_VALUE) CloseHandle(junctionHandle);
        bool converted = false;
        if (readReparse) {
            GuardedFileRename move(source, current, destination, shows, rootIdentity);
            check(move.ready(), "prepare before destination is converted to a junction");
            const HANDLE attributes = openNative(target, FILE_WRITE_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS);
            DWORD returned = 0;
            converted = attributes != INVALID_HANDLE_VALUE
                && DeviceIoControl(attributes, FSCTL_SET_REPARSE_POINT, reparseData.data(), reparseBytes,
                    nullptr, 0, &returned, nullptr);
            check(converted, "attribute-only handle can change a guarded empty directory into a junction");
            if (attributes != INVALID_HANDLE_VALUE) CloseHandle(attributes);
            check(!move.commit().moved && QFileInfo::exists(source), "late directory reparse change is refused");
        }
        if (converted)
            check(QDir().rmdir(target) && QDir().mkpath(target), "remove only late fixture junction and restore empty target");
        check(QDir().rmdir(junction), "remove junction itself, never its target");
    }
    {
        const QString caseRoot = fixture.filePath("CaseSensitive");
        check(QDir().mkpath(caseRoot), "create case-sensitivity fixture");
        const HANDLE directory = openNative(caseRoot, FILE_WRITE_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS);
        const FILE_CASE_SENSITIVE_INFO sensitivity{1}; // FILE_CS_FLAG_CASE_SENSITIVE_DIR
        const bool enabled = directory != INVALID_HANDLE_VALUE && SetFileInformationByHandle(directory,
            FileCaseSensitiveInfo, const_cast<FILE_CASE_SENSITIVE_INFO*>(&sensitivity), sizeof(sensitivity));
        const DWORD enableError = enabled ? ERROR_SUCCESS : GetLastError();
        if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
        if (enabled) {
            const QString retainedRoot = caseRoot + "/Shows";
            const QString differentRoot = caseRoot + "/shows";
            check(QDir().mkpath(retainedRoot) && QDir().mkpath(differentRoot), "create distinct case-sensitive folders");
            const auto retainedIdentity = captureSourceSnapshot(retainedRoot);
            check(retainedIdentity.fileIndex != captureSourceSnapshot(differentRoot).fileIndex,
                "case-sensitive folders really are distinct objects");
            GuardedFileRename move(source, current, differentRoot + "/" + QFileInfo(source).fileName(),
                retainedRoot, retainedIdentity);
            check(!move.ready() && !move.commit().moved, "case-folded prefix cannot escape retained folder identity");
        } else {
            std::cerr << "SKIP: case-sensitive folder fixture unavailable (Windows error " << enableError << ").\n";
        }
    }
    check(readFile(source) == "episode bytes" && !QFileInfo::exists(destination), "rejected moves preserve fixture source");

    const QString interruptedRoot = fixture.filePath("InterruptedBatch");
    const QString interruptedFirst = QDir(interruptedRoot).filePath("source-a.txt");
    const QString interruptedSecond = QDir(interruptedRoot).filePath("source-b.txt");
    const QString interruptedFirstDestination = QDir(interruptedRoot).filePath("renamed-a.txt");
    const QString interruptedSecondDestination = QDir(interruptedRoot).filePath("renamed-b.txt");
    check(QDir().mkpath(interruptedRoot)
            && writeFile(interruptedFirst, "first interrupted bytes")
            && writeFile(interruptedSecond, "second interrupted bytes"),
        "create process-interruption fixtures");
    QProcess interrupted;
    interrupted.start(QCoreApplication::applicationFilePath(),
        {"--guarded-batch-rename-interruption", interruptedRoot});
    const bool interruptionFinished = interrupted.waitForFinished(30'000);
    if (!interruptionFinished) {
        interrupted.kill();
        interrupted.waitForFinished(5'000);
    }
    check(interruptionFinished && interrupted.exitCode() == 77,
        "terminate the helper only after every source is staged and before commit");
    const QStringList stagedNames = QDir(interruptedRoot).entryList(
        {".file-manager-rename-*.tmp"}, QDir::Files | QDir::Hidden, QDir::Name);
    check(!QFileInfo::exists(interruptedFirst) && !QFileInfo::exists(interruptedSecond)
            && !QFileInfo::exists(interruptedFirstDestination)
            && !QFileInfo::exists(interruptedSecondDestination)
            && stagedNames.size() == 2,
        "an interrupted no-journal batch leaves only two discoverable staging objects");
    QHash<QByteArray, QString> recoveryTargets{{"first interrupted bytes", interruptedFirst},
        {"second interrupted bytes", interruptedSecond}};
    bool recoveredEveryObject = stagedNames.size() == 2;
    for (const QString& stagedName : stagedNames) {
        const QString stagedPath = QDir(interruptedRoot).filePath(stagedName);
        const QString recoveryTarget = recoveryTargets.take(readFile(stagedPath));
        recoveredEveryObject = recoveredEveryObject && !recoveryTarget.isEmpty()
            && QFile::rename(stagedPath, recoveryTarget);
    }
    check(recoveredEveryObject && recoveryTargets.isEmpty()
            && readFile(interruptedFirst) == "first interrupted bytes"
            && readFile(interruptedSecond) == "second interrupted bytes",
        "manual inspection can recover every staged object with exact bytes intact");
#else
    GuardedFileRename move(source, captureSourceSnapshot(source), destination, shows, rootIdentity);
    check(!move.ready() && !move.commit().moved, "unsupported platform fails closed");
#endif
    return passed;
}
