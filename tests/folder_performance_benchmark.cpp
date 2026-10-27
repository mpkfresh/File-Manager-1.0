#include "files/scopeenumerator.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>

#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

constexpr int ProductionEntryCount = 100000;
constexpr qint64 EnumerationLimitMs = 30000;
constexpr SIZE_T WorkingSetLimit = 128ULL * 1024ULL * 1024ULL;

SIZE_T currentWorkingSet()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return 0;
    }
    return counters.WorkingSetSize;
}

bool createFixture(const QString& root)
{
    for (int index = 0; index < ProductionEntryCount; ++index) {
        const QString path = QDir(root).filePath(
            QStringLiteral("entry-%1.tmp").arg(index, 6, 10, QLatin1Char('0')));
        const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
        const HANDLE file = CreateFileW(nativePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        CloseHandle(file);
    }
    return true;
}

int runBenchmark()
{
    QTemporaryDir fixture;
    if (!fixture.isValid()) {
        std::fprintf(stderr, "Could not create the benchmark directory.\n");
        return 10;
    }

    QElapsedTimer fixtureTimer;
    fixtureTimer.start();
    if (!createFixture(fixture.path())) {
        std::fprintf(stderr, "Could not populate the %d-entry benchmark directory.\n",
            ProductionEntryCount);
        return 11;
    }
    const qint64 fixtureMs = fixtureTimer.elapsed();

    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
        static_cast<SIZE_T>(-1));
    SIZE_T sampledPeak = currentWorkingSet();
    QElapsedTimer enumerationTimer;
    enumerationTimer.start();
    int visited = 0;
    const auto result = filemanager::files::enumerateRegularTree(
        fixture.path(), false, nullptr, [&visited, &sampledPeak](const QFileInfo&) {
            ++visited;
            if ((visited % 256) == 0)
                sampledPeak = std::max(sampledPeak, currentWorkingSet());
            return true;
        });
    sampledPeak = std::max(sampledPeak, currentWorkingSet());
    const qint64 elapsedMs = enumerationTimer.elapsed();

    std::fprintf(stdout,
        "fixture_entries=%d fixture_ms=%lld enumeration_ms=%lld sampled_peak_mib=%.2f limit_mib=128\n",
        ProductionEntryCount, static_cast<long long>(fixtureMs),
        static_cast<long long>(elapsedMs),
        static_cast<double>(sampledPeak) / (1024.0 * 1024.0));
    if (!result.error.isEmpty() || result.cancelled || visited != ProductionEntryCount) {
        std::fprintf(stderr, "Enumeration failed: visited=%d error=%s cancelled=%d.\n", visited,
            result.error.toUtf8().constData(), result.cancelled);
        return 12;
    }
    if (elapsedMs > EnumerationLimitMs || sampledPeak == 0 || sampledPeak > WorkingSetLimit) {
        std::fprintf(stderr, "Benchmark exceeded its 30 s or 128 MiB release threshold.\n");
        return 13;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 1) {
        std::fprintf(stderr, "This benchmark takes no arguments.\n");
        return 1;
    }
    return runBenchmark();
}
