#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QStringList>
#include <windows.h>
#include <shlobj.h>
#include <array>
#include <cstdio>
#include <vector>

// Standalone diagnostic, never run by application startup or CTest.
// Register only disposable local fixtures; output fixed labels and counters, not paths.
namespace {
constexpr UINT notificationMessage = WM_APP + 71;
struct Events {
    QString canary;
    QString directory;
    unsigned total = 0;
    unsigned canaryEvents = 0;
    unsigned directoryEvents = 0;
    unsigned lockFailures = 0;
    unsigned pathFailures = 0;
};

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* events = reinterpret_cast<Events*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == notificationMessage && events) {
        PIDLIST_ABSOLUTE* items = nullptr;
        LONG kind = 0;
        HANDLE locked = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wParam),
            static_cast<DWORD>(lParam), &items, &kind);
        if (!locked) {
            ++events->lockFailures;
            return 0;
        }
        ++events->total;
        // Only the subscribed path-bearing events are accepted. The second PIDL is
        // a path only for rename events; never interpret other event payloads as paths.
        const int count = (kind & (SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER)) ? 2 : 1;
        for (int index = 0; items && index < count; ++index) {
            if (!items[index])
                continue;
            std::array<wchar_t, 32768> buffer{};
            if (!SHGetPathFromIDListEx(items[index], buffer.data(),
                    static_cast<DWORD>(buffer.size()), GPFIDL_DEFAULT)) {
                ++events->pathFailures;
                continue;
            }
            const QString path = QDir::fromNativeSeparators(QString::fromWCharArray(buffer.data()));
            if (path.compare(events->canary, Qt::CaseInsensitive) == 0)
                ++events->canaryEvents;
            if (path.compare(events->directory, Qt::CaseInsensitive) == 0)
                ++events->directoryEvents;
        }
        SHChangeNotification_Unlock(locked);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void pump(DWORD milliseconds)
{
    const ULONGLONG end = GetTickCount64() + milliseconds;
    while (GetTickCount64() < end) {
        MSG message{};
        // Bound a dispatch batch as well as elapsed time under notification load.
        for (int count = 0; count < 128 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const ULONGLONG now = GetTickCount64();
        if (now < end)
            MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>((end - now) < 20 ? end - now : 20),
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

bool writePayload(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("payload") == 7;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
        return 2;
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"FileManagerShellWatchProbe";
    if (!RegisterClassW(&windowClass)) {
        CoUninitialize();
        return 3;
    }
    const QString fixtureRoot = QDir::current().absoluteFilePath(QStringLiteral("out/test-temp"));
    bool integrity = QDir().mkpath(fixtureRoot);
    constexpr std::array modes{"none", "parent", "nested", "all"};
    constexpr std::array variants{"shell_only", "combined_nonrecursive", "combined_recursive_entry"};
    for (size_t variant = 0; variant < variants.size(); ++variant) {
        for (size_t mode = 0; mode < modes.size(); ++mode) {
            QTemporaryDir fixture(fixtureRoot + QStringLiteral("/shell-watch-XXXXXX"));
            const QString parent = fixture.filePath(QStringLiteral("listing"));
            const QString source = parent + QStringLiteral("/item");
            const QString nested = source + QStringLiteral("/nested");
            const QString destination = parent + QStringLiteral("/renamed");
            bool ready = fixture.isValid() && QDir().mkpath(nested)
                && writePayload(nested + QStringLiteral("/payload.txt"));
            Events events;
            events.directory = mode == 1 ? parent : nested;
            events.canary = events.directory + QStringLiteral("/canary.txt");
            HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPED,
                0, 0, 0, 0, nullptr, nullptr, windowClass.hInstance, &events);
            ready = ready && window;
            QStringList paths;
            if (mode == 1 || mode == 3) paths.append(parent);
            if (mode == 3) paths.append(source);
            if (mode == 2 || mode == 3) paths.append(nested);
            std::vector<ULONG> registrations;
            for (const QString& path : paths) {
                const QString native = QDir::toNativeSeparators(path);
                PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(reinterpret_cast<LPCWSTR>(native.utf16()));
                const SHChangeNotifyEntry entry{pidl, variant == 2 ? TRUE : FALSE};
                const int flags = SHCNRF_ShellLevel | SHCNRF_NewDelivery
                    | (variant ? SHCNRF_InterruptLevel : 0);
                // Deliberately omit SHCNRF_RecursiveInterrupt. Variant 2 tests Shell
                // delivery filtering separately from recursive filesystem monitoring.
                const LONG kinds = SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR
                    | SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEDIR | SHCNE_UPDATEITEM;
                const ULONG registration = pidl && window
                    ? SHChangeNotifyRegister(window, flags, kinds, notificationMessage, 1, &entry) : 0;
                CoTaskMemFree(pidl);
                ready = ready && registration;
                if (registration)
                    registrations.push_back(registration);
            }
            pump(300);
            events.total = events.canaryEvents = events.directoryEvents = 0;
            ready = ready && writePayload(events.canary);
            // Native filesystem mutation only: no explicit Shell notification yet.
            pump(1800);
            const Events nativeEvents = events;
            events.total = events.canaryEvents = events.directoryEvents = 0;
            const QString nativeCanary = QDir::toNativeSeparators(events.canary);
            if (ready)
                SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                    nativeCanary.utf16(), nullptr);
            pump(700);
            const Events explicitEvents = events;
            const QString from = QDir::toNativeSeparators(source);
            const QString to = QDir::toNativeSeparators(destination);
            const bool renamed = ready && MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()),
                reinterpret_cast<LPCWSTR>(to.utf16()), 0);
            const DWORD error = renamed ? ERROR_SUCCESS : GetLastError();
            pump(300);
            QFile payload((renamed ? destination : source) + QStringLiteral("/nested/payload.txt"));
            const bool retained = payload.open(QIODevice::ReadOnly) && payload.readAll() == "payload";
            payload.close();
            const bool consistent = retained && QFileInfo::exists(source) != renamed
                && QFileInfo::exists(destination) == renamed;
            bool deregistered = true;
            for (ULONG registration : registrations)
                deregistered = (SHChangeNotifyDeregister(registration) != FALSE) && deregistered;
            pump(100);
            if (window)
                DestroyWindow(window);
            integrity = ready && consistent && deregistered && !events.lockFailures
                && (mode != 0 || renamed) && integrity;
            std::printf("shell_watch variant=%s mode=%s setup=%d registrations=%zu "
                "native_events=%u native_canary=%u native_directory=%u "
                "explicit_events=%u explicit_canary=%u explicit_directory=%u "
                "renamed=%d native_error=%lu retained=%d consistent=%d deregistered=%d "
                "lock_failures=%u path_failures=%u\n",
                variants[variant], modes[mode], ready, registrations.size(),
                nativeEvents.total, nativeEvents.canaryEvents, nativeEvents.directoryEvents,
                explicitEvents.total, explicitEvents.canaryEvents, explicitEvents.directoryEvents,
                renamed, static_cast<unsigned long>(error), retained, consistent, deregistered,
                events.lockFailures, events.pathFailures);
            std::fflush(stdout);
        }
    }
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    CoUninitialize();
    std::printf("shell_watch_summary cases=12 fixture_integrity=%d\n", integrity);
    // Event delivery and rename outcomes are measurements, not assumed success criteria.
    return integrity ? 0 : 1;
}
