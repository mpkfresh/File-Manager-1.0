#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTemporaryDir>
#include <QStringList>

#include <windows.h>
#include <array>
#include <cstdio>
#include <memory>
#include <vector>

struct NativeWatch {
    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED operation{};
    alignas(DWORD) std::array<char, 4096> buffer{};
    bool pending = false;

    bool open(const QString& path, bool notifications)
    {
        const QString native = QDir::toNativeSeparators(path);
        directory = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory == INVALID_HANDLE_VALUE)
            return false;
        if (!notifications)
            return true;
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event)
            return false;
        operation.hEvent = event;
        pending = ReadDirectoryChangesW(directory, buffer.data(), static_cast<DWORD>(buffer.size()),
            FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES,
            nullptr, &operation, nullptr) != FALSE;
        return pending;
    }

    ~NativeWatch()
    {
        if (pending) {
            CancelIoEx(directory, &operation);
            DWORD transferred = 0;
            // Keep the buffer and OVERLAPPED alive through cancellation completion.
            // The separately approved process deadline bounds a blocked local driver.
            GetOverlappedResult(directory, &operation, &transferred, TRUE);
        }
        if (directory != INVALID_HANDLE_VALUE)
            CloseHandle(directory);
        if (event)
            CloseHandle(event);
    }
};

// Explicitly approved diagnostic only; not an application startup check or CTest target.
// Each operation uses a fresh disposable tree. Fixed labels and error codes only.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("DirectoryWatchRenameProbe"));
    constexpr std::array modes{"none", "parent", "item", "nested", "all"};
    bool setupComplete = true;
    bool controlsPassed = true;
    constexpr std::array backends{"Qt", "Qt", "native_open", "native_changes"};
    for (size_t backend = 0; backend < backends.size(); ++backend) {
        const bool native = backend != 0;
        for (size_t mode = 0; mode < modes.size(); ++mode) {
            QTemporaryDir fixture;
            const QString parent = fixture.filePath(QStringLiteral("listing"));
            const QString source = parent + QStringLiteral("/item");
            const QString nested = source + QStringLiteral("/nested");
            const QString destination = parent + QStringLiteral("/renamed");
            bool ready = fixture.isValid() && QDir().mkpath(nested);
            QFile payload(nested + QStringLiteral("/payload.txt"));
            ready = ready && payload.open(QIODevice::WriteOnly) && payload.write("payload") == 7;
            payload.close();
            QFileSystemWatcher watcher;
            watcher.blockSignals(true);
            std::vector<std::unique_ptr<NativeWatch>> nativeWatches;
            QStringList paths;
            if (mode == 1 || mode == 4) paths.append(parent);
            if (mode == 2 || mode == 4) paths.append(source);
            if (mode == 3 || mode == 4) paths.append(nested);
            if (backend < 2) {
                if (!paths.isEmpty())
                    ready = ready && watcher.addPaths(paths).isEmpty();
            } else {
                for (const QString& path : paths) {
                    auto watch = std::make_unique<NativeWatch>();
                    ready = watch->open(path, backend == 3) && ready;
                    nativeWatches.push_back(std::move(watch));
                }
            }
            setupComplete &= ready;
            bool renamed = false;
            DWORD error = ERROR_SUCCESS;
            if (ready) {
                if (native) {
                    const QString from = QDir::toNativeSeparators(source);
                    const QString to = QDir::toNativeSeparators(destination);
                    renamed = MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()),
                        reinterpret_cast<LPCWSTR>(to.utf16()), 0) != FALSE;
                    if (!renamed)
                        error = GetLastError();
                } else {
                    renamed = QDir().rename(source, destination);
                }
            }
            QFile retained((renamed ? destination : source) + QStringLiteral("/nested/payload.txt"));
            const bool retainedBytes = retained.open(QIODevice::ReadOnly) && retained.readAll() == "payload";
            retained.close();
            const bool sourceExists = QFileInfo::exists(source);
            const bool destinationExists = QFileInfo::exists(destination);
            const bool consistent = ready && retainedBytes && sourceExists != renamed
                && destinationExists == renamed;
            setupComplete &= consistent;
            if (mode == 0)
                controlsPassed &= renamed && consistent;
            std::printf("watch_rename backend=%s api=%s mode=%s setup=%d watched=%lld renamed=%d "
                "native_error=%lu source_exists=%d destination_exists=%d retained_bytes=%d consistent=%d\n",
                backends[backend], native ? "MoveFileExW" : "QDir", modes[mode], ready,
                static_cast<long long>(backend < 2 ? watcher.directories().size() : nativeWatches.size()), renamed,
                static_cast<unsigned long>(error), sourceExists, destinationExists, retainedBytes, consistent);
        }
    }
    std::printf("watch_rename_summary cases=20 setup_and_contents=%d unwatched_controls=%d\n",
        setupComplete, controlsPassed);
    return setupComplete && controlsPassed ? 0 : 1;
}
