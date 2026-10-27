#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <windows.h>
#include <array>
#include <cstdio>

namespace {
struct RootWatch {
    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED operation{};
    alignas(DWORD) std::array<char, 16384> buffer{};
    bool pending = false;
    bool arm()
    {
        ResetEvent(event);
        operation = {};
        operation.hEvent = event;
        pending = ReadDirectoryChangesW(directory, buffer.data(), DWORD(buffer.size()), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
            nullptr, &operation, nullptr) != FALSE;
        return pending;
    }
    bool open(const QString& root)
    {
        const QString native = QDir::toNativeSeparators(root);
        directory = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return directory != INVALID_HANDLE_VALUE && event && arm();
    }
    bool observe(QSet<QString> expected, unsigned& events)
    {
        QElapsedTimer timer;
        timer.start();
        while (!expected.isEmpty() && timer.elapsed() < 5000) {
            const DWORD remaining = DWORD(qMax<qint64>(0, 5000 - timer.elapsed()));
            if (!pending || WaitForSingleObject(event, remaining) != WAIT_OBJECT_0)
                return false;
            DWORD bytes = 0;
            const bool completed = GetOverlappedResult(directory, &operation, &bytes, FALSE) != FALSE;
            pending = false;
            if (!completed || !bytes)
                return false;
            DWORD offset = 0;
            while (offset < bytes) {
                constexpr DWORD header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
                if (bytes - offset < header)
                    return false;
                const auto* record = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
                if ((record->FileNameLength % sizeof(wchar_t)) != 0 || record->FileNameLength > bytes - offset - header)
                    return false;
                const QString name = QDir::fromNativeSeparators(QString::fromWCharArray(record->FileName,
                    int(record->FileNameLength / sizeof(wchar_t))));
                expected.remove(QString::number(record->Action) + QLatin1Char('/') + name);
                ++events;
                if (!record->NextEntryOffset)
                    break;
                if (record->NextEntryOffset < header + record->FileNameLength
                        || record->NextEntryOffset >= bytes - offset || (record->NextEntryOffset % 4) != 0)
                    return false;
                offset += record->NextEntryOffset;
            }
            if (!arm())
                return false;
        }
        return expected.isEmpty();
    }
    ~RootWatch()
    {
        if (pending) {
            CancelIoEx(directory, &operation);
            DWORD bytes = 0;
            GetOverlappedResult(directory, &operation, &bytes, TRUE);
        }
        if (directory != INVALID_HANDLE_VALUE)
            CloseHandle(directory);
        if (event)
            CloseHandle(event);
    }
};
bool createFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("payload") == 7;
}
}

// Standalone diagnostic, not CTest or production monitoring. All names are fixture-only.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString fixtures = QDir::current().absoluteFilePath("out/test-temp");
    if (!QDir().mkpath(fixtures))
        return 2;
    bool passed = true;
    for (int iteration = 0; iteration < 3; ++iteration) {
        QTemporaryDir fixture(fixtures + "/recursive-root-XXXXXX");
        const QString root = fixture.filePath("listing");
        const QString source = root + "/item";
        const QString nested = source + "/nested";
        const QString unopened = root + "/unopened/deep";
        bool setup = fixture.isValid() && QDir().mkpath(nested) && QDir().mkpath(unopened)
            && createFile(nested + "/original.txt");
        RootWatch watch;
        setup = setup && watch.open(root);
        unsigned events = 0;
        const bool displayed = setup && createFile(nested + "/new.txt")
            && watch.observe({QStringLiteral("1/item/nested/new.txt")}, events);
        const bool broader = setup && createFile(unopened + "/scope-canary.txt")
            && watch.observe({QStringLiteral("1/unopened/deep/scope-canary.txt")}, events);
        const QString from = QDir::toNativeSeparators(source);
        const QString destination = root + "/renamed";
        const QString to = QDir::toNativeSeparators(destination);
        const bool renamed = setup && MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()),
            reinterpret_cast<LPCWSTR>(to.utf16()), 0);
        const DWORD renameError = renamed ? ERROR_SUCCESS : GetLastError();
        const bool renameEvents = renamed && watch.observe({QStringLiteral("4/item"), QStringLiteral("5/renamed")}, events);
        const bool afterRename = renamed && createFile(destination + "/nested/after.txt")
            && watch.observe({QStringLiteral("1/renamed/nested/after.txt")}, events);
        const QString empty = root + "/empty";
        const bool created = setup && QDir().mkdir(empty) && watch.observe({QStringLiteral("1/empty")}, events);
        const bool removed = created && QDir().rmdir(empty) && watch.observe({QStringLiteral("2/empty")}, events);
        const bool recreated = removed && QDir().mkdir(empty) && watch.observe({QStringLiteral("1/empty")}, events);
        QFile payload((renamed ? destination : source) + "/nested/original.txt");
        const bool retained = payload.open(QIODevice::ReadOnly) && payload.readAll() == "payload";
        payload.close();
        const bool consistent = QFileInfo::exists(source) != renamed && QFileInfo::exists(destination) == renamed;
        const bool complete = setup && displayed && broader && renamed && renameEvents && afterRename
            && created && removed && recreated && retained && consistent;
        passed &= complete;
        std::printf("recursive_root iteration=%d setup=%d displayed_change=%d unopened_change_received=%d "
            "renamed=%d rename_error=%lu rename_events=%d post_rename=%d create=%d remove=%d recreate=%d "
            "retained=%d consistent=%d events=%u complete=%d\n", iteration, setup, displayed, broader,
            renamed, static_cast<unsigned long>(renameError), renameEvents, afterRename, created, removed,
            recreated, retained, consistent, events, complete);
        std::fflush(stdout);
    }
    return passed ? 0 : 1;
}
