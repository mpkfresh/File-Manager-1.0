#include "ui/backgroundfileicons.h"
#include "ui/presentationtiming.h"
#include "ui/localdirectorymodel.h"
#include "iconimagecomparison.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileIconProvider>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <cstdio>

namespace {
using Icons = filemanager::ui::BackgroundFileIcons;
using Result = filemanager::platform::FileIconResult;
using Status = filemanager::platform::FileIconStatus;
bool expect(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "Background icon failure: %s\n", message);
    return condition;
}
void describeVisibleDifference(const QImage& expected, const QImage& actual)
{
    if (expected.isNull() || actual.isNull() || expected.size() != actual.size()) {
        std::fprintf(stderr, "Model icon pixels: expected=%dx%d actual=%dx%d\n",
            expected.width(), expected.height(), actual.width(), actual.height());
        return;
    }
    const QImage left = expected.convertToFormat(QImage::Format_RGBA8888);
    const QImage right = actual.convertToFormat(QImage::Format_RGBA8888);
    int maxRgbDifference = 0;
    int rgbPixelsBeyondTolerance = 0;
    int alphaMismatchPixels = 0;
    for (int y = 0; y < left.height(); ++y) {
        const uchar* a = left.constScanLine(y);
        const uchar* b = right.constScanLine(y);
        for (int x = 0; x < left.width(); ++x) {
            const int offset = x * 4;
            if (a[offset + 3] != b[offset + 3])
                ++alphaMismatchPixels;
            if (a[offset + 3] == 0)
                continue;
            int pixelDifference = 0;
            for (int channel = 0; channel < 3; ++channel)
                pixelDifference = qMax(pixelDifference,
                    std::abs(int(a[offset + channel]) - int(b[offset + channel])));
            maxRgbDifference = qMax(maxRgbDifference, pixelDifference);
            if (pixelDifference > 2)
                ++rgbPixelsBeyondTolerance;
        }
    }
    std::fprintf(stderr, "Model icon pixels: %dx%d max_rgb_difference=%d "
        "rgb_pixels_beyond_tolerance=%d alpha_mismatch_pixels=%d\n",
        left.width(), left.height(), maxRgbDifference, rgbPixelsBeyondTolerance,
        alphaMismatchPixels);
}
bool waitUntil(const std::function<bool()>& done)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return done();
}
QString path(const QString& name)
{ return QDir::current().absoluteFilePath(QStringLiteral("injected-icons/") + name); }
Result ready(int extent, int marker)
{
    Result result{Status::Ready};
    result.image = QImage(extent, extent, QImage::Format_RGBA8888);
    result.image.fill(QColor(marker % 256, 35, 79));
    return result;
}
struct Gates {
    QSemaphore started;
    QSemaphore release;
    std::atomic_int calls{0};
    std::atomic_int exits{0};
    std::atomic_int cancelled{0};
    std::atomic_bool usedGuiThread{false};
};
auto gated(const std::shared_ptr<Gates>& gates)
{
    QThread* gui = qApp->thread();
    return [gates, gui](const QString&, int extent, const filemanager::platform::ShellCancellation& cancel) {
        const int marker = ++gates->calls;
        if (QThread::currentThread() == gui) gates->usedGuiThread = true;
        gates->started.release();
        const bool released = gates->release.tryAcquire(1, 5000);
        if (cancel->load()) ++gates->cancelled;
        ++gates->exits;
        return released ? ready(extent, marker) : Result{Status::TimedOut};
    };
}

bool resetAndDisposal()
{
    bool passed = true;
    const auto gates = std::make_shared<Gates>();
    auto icons = std::make_unique<Icons>(gated(gates));
    int repaint = 0;
    QObject::connect(icons.get(), &Icons::iconsChanged, icons.get(), [&] { ++repaint; });
    const QList<QSize> sizes{QSize(16, 16), QSize(32, 32), QSize(48, 48), QSize(256, 256)};
    const QIcon old = icons->icon(path("one"), sizes);
    passed &= expect(!old.isNull() && old.actualSize(QSize(24, 24)) == QSize(24, 24),
        "Pending icons must preserve their layout metrics.");
    passed &= expect(old.pixmap(QSize(16, 16), 1.0).isNull(), "First paint must return without waiting.");
    passed &= expect(gates->started.tryAcquire(1, 3000), "Worker must start after first request.");
    for (int i = 0; i < 10; ++i) old.pixmap(QSize(16, 16), 1.0);
    passed &= expect(gates->calls == 1 && !gates->usedGuiThread,
        "Repeated paints must deduplicate and never extract artwork on the GUI thread.");
    icons->clear();
    const QIcon current = icons->icon(path("one"), sizes);
    current.pixmap(QSize(16, 16), 1.0);
    passed &= expect(gates->started.tryAcquire(1, 3000), "New generation must start separately.");
    gates->release.release(2);
    passed &= expect(waitUntil([&] { return !current.pixmap(QSize(16, 16), 1.0).isNull() && repaint > 0; }),
        "Completed artwork must become paintable and request repaint.");
    passed &= expect(old.pixmap(QSize(16, 16), 1.0).isNull() && gates->calls == 2
            && current.pixmap(QSize(16, 16), 1.0).toImage().pixelColor(0, 0).red() == 2,
        "A late old result must not enter the current icon or resubmit the old path.");
    passed &= expect(current.pixmap(QSize(16, 16), 1.5).isNull()
            && gates->started.tryAcquire(1, 3000), "DPR changes must request new physical pixels.");
    gates->release.release();
    passed &= expect(waitUntil([&] { return !current.pixmap(QSize(16, 16), 1.5).isNull(); }),
        "New physical pixels must complete.");
    const QPixmap scaled = current.pixmap(QSize(16, 16), 1.5);
    passed &= expect(scaled.size() == QSize(24, 24) && scaled.devicePixelRatio() == 1.5,
        "The loader must receive the requested physical extent.");
    current.pixmap(QSize(48, 48), 1.0);
    passed &= expect(gates->started.tryAcquire(1, 3000), "Disposal fixture must have a live worker.");
    QElapsedTimer disposal;
    disposal.start();
    icons.reset();
    passed &= expect(disposal.elapsed() < 1000 && current.pixmap(QSize(16, 16), 1.0).isNull(),
        "Owner disposal must not wait, dereference the owner or retain usable stale pixels.");
    gates->release.release();
    passed &= expect(waitUntil([&] { return gates->exits == 4; }) && gates->cancelled >= 2,
        "Reset and disposal must cancel their live workers.");
    return passed;
}

bool overflowAndFailures()
{
    bool passed = true;
    const auto gates = std::make_shared<Gates>();
    Icons icons(gated(gates));
    QList<QIcon> requests;
    for (int i = 0; i < 65; ++i) {
        requests.append(icons.icon(path(QString::number(i)), {QSize(16, 16)}));
        requests.last().pixmap(QSize(16, 16), 1.0);
    }
    passed &= expect(gates->started.tryAcquire(2, 3000) && gates->calls == 2,
        "A full queue must start only two workers.");
    int repaints = 0;
    QObject::connect(&icons, &Icons::iconsChanged, &icons, [&] {
        ++repaints;
        for (const QIcon& icon : requests) icon.pixmap(QSize(16, 16), 1.0);
    });
    gates->release.release(65);
    passed &= expect(waitUntil([&] { return gates->exits == 65; }) && repaints > 0,
        "Capacity notifications must cause an overflowed paint request to be retried.");
    passed &= expect(waitUntil([&] {
        for (const QIcon& icon : requests) if (icon.pixmap(QSize(16, 16), 1.0).isNull()) return false;
        return true;
    }) && icons.pixmapCacheCost() <= Icons::PixmapCacheBudget,
        "Every accepted/overflowed icon must become visible within the pixmap cache budget.");

    const auto failures = std::make_shared<std::atomic_int>(0);
    Icons failing([failures](const QString&, int, const filemanager::platform::ShellCancellation&) {
        ++*failures;
        return Result{Status::TimedOut};
    });
    const QIcon failed = failing.icon(path("failed"), {QSize(16, 16)});
    int changes = 0;
    QObject::connect(&failing, &Icons::iconsChanged, &failing, [&] { ++changes; });
    failed.pixmap(QSize(16, 16), 1.0);
    passed &= expect(waitUntil([&] { return changes > 0; }), "Failed extraction must wake the view.");
    for (int i = 0; i < 10; ++i)
        passed &= expect(failed.pixmap(QSize(16, 16), 1.0).isNull(), "Failure must not substitute generic artwork.");
    passed &= expect(*failures == 1, "Repeated paint must not automatically retry a failed extraction.");
    failing.clear();
    const QIcon retry = failing.icon(path("failed"), {QSize(16, 16)});
    retry.pixmap(QSize(16, 16), 1.0);
    passed &= expect(waitUntil([&] { return *failures == 2; }), "Refresh must permit a new extraction.");
    return passed;
}

bool navigationTypeReuse()
{
    bool passed = true;
    const auto gates = std::make_shared<Gates>();
    Icons icons(gated(gates));
    const QList<QSize> sizes{QSize(16, 16), QSize(32, 32)};
    const QIcon oldType = icons.icon(path("first.txt"), sizes, "extension:txt");
    oldType.pixmap(QSize(16, 16), 1.0);
    passed &= expect(gates->started.tryAcquire(1, 3000), "Type fixture loader should start.");
    gates->release.release();
    passed &= expect(waitUntil([&] { return !oldType.pixmap(QSize(16, 16), 1.0).isNull(); }),
        "Type fixture should become ready.");
    const QIcon oldSpecific = icons.icon(path("artwork.ico"), sizes);
    oldSpecific.pixmap(QSize(16, 16), 1.0);
    passed &= expect(gates->started.tryAcquire(1, 3000), "Path-specific fixture loader should start.");
    gates->release.release();
    passed &= expect(waitUntil([&] { return !oldSpecific.pixmap(QSize(16, 16), 1.0).isNull(); }),
        "Path-specific fixture should become ready.");
    icons.navigate();
    const QIcon newType = icons.icon(path("elsewhere/second.txt"), sizes, "extension:txt");
    passed &= expect(!newType.pixmap(QSize(16, 16), 1.0).isNull() && gates->calls == 2
            && newType.pixmap(QSize(16, 16), 1.0).toImage().pixelColor(0, 0).red() == 1,
        "Navigation must reuse a completed equivalent type without another loader call.");
    passed &= expect(oldType.pixmap(QSize(16, 16), 1.0).isNull()
            && oldSpecific.pixmap(QSize(16, 16), 1.0).isNull(),
        "Retaining pixels must not keep old-generation handles usable.");
    const QIcon newSpecific = icons.icon(path("artwork.ico"), sizes);
    const QIcon differentType = icons.icon(path("picture.png"), sizes, "extension:png");
    passed &= expect(newSpecific.pixmap(QSize(16, 16), 1.0).isNull()
            && differentType.pixmap(QSize(16, 16), 1.0).isNull(),
        "Path-specific and other-type artwork must not reuse the retained type raster.");
    passed &= expect(gates->started.tryAcquire(2, 3000), "Both fresh requests should start.");
    icons.navigate();
    gates->release.release(2);
    const QIcon currentPng = icons.icon(path("picture.png"), sizes, "extension:png");
    passed &= expect(currentPng.pixmap(QSize(16, 16), 1.0).isNull(),
        "Cancelled pending type work must not become a reusable result.");
    passed &= expect(waitUntil([&] { return gates->calls == 5; }),
        "A new-generation request must start after old workers relinquish their slots.");
    gates->release.release();
    passed &= expect(waitUntil([&] { return !currentPng.pixmap(QSize(16, 16), 1.0).isNull(); })
            && currentPng.pixmap(QSize(16, 16), 1.0).toImage().pixelColor(0, 0).red() == 5
            && gates->cancelled >= 2,
        "Late cancelled results must not replace the fresh type image.");
    icons.clear();
    const QIcon refreshed = icons.icon(path("first.txt"), sizes, "extension:txt");
    passed &= expect(refreshed.pixmap(QSize(16, 16), 1.0).isNull()
            && gates->started.tryAcquire(1, 3000), "Explicit refresh must clear retained type pixels.");
    gates->release.release();
    passed &= expect(waitUntil([&] { return !refreshed.pixmap(QSize(16, 16), 1.0).isNull(); })
            && gates->calls == 6 && icons.pixmapCacheCost() <= Icons::PixmapCacheBudget,
        "Refreshed type artwork must be extracted again within the cache budget.");
    return passed;
}

bool modelIntegration()
{
    QTemporaryDir fixture;
    if (!fixture.isValid()) return false;
    QFile one(fixture.filePath("one.txt"));
    QFile two(fixture.filePath("two.txt"));
    if (!one.open(QIODevice::WriteOnly) || !two.open(QIODevice::WriteOnly)) return false;
    one.close(); two.close();
    filemanager::ui::LocalDirectoryModel model;
    int repaint = 0;
    QObject::connect(&model, &filemanager::ui::LocalDirectoryModel::iconsChanged,
        &model, [&] { ++repaint; });
    model.setDirectory(fixture.path());
    if (!waitUntil([&] { return !model.loading() && model.rowCount() == 2; })) return false;
    const QIcon first = model.indexForPath(one.fileName()).data(Qt::DecorationRole).value<QIcon>();
    const QIcon second = model.indexForPath(two.fileName()).data(Qt::DecorationRole).value<QIcon>();
    bool passed = expect(first.cacheKey() == second.cacheKey(), "Equivalent file types must share an icon request.");
    const int before = repaint;
    first.pixmap(QSize(16, 16), 1.0);
    const bool firstReady = waitUntil([&] {
        return repaint > before && !first.pixmap(QSize(16, 16), 1.0).isNull();
    });
    if (!firstReady) {
        // Distinguish a Shell-helper failure from a model/scheduler publication
        // failure without adding work to the ordinary passing test path.
        const auto direct = filemanager::platform::readFileIconIsolated(one.fileName(), 16);
        std::fprintf(stderr, "Model icon diagnostic: repaint=%d first_null=%d direct_status=%s "
            "direct_started=%d direct_stopped=%d direct_exit=%d direct_ms=%lld\n",
            repaint, first.pixmap(QSize(16, 16), 1.0).isNull(),
            filemanager::platform::fileIconStatusName(direct.status), direct.processStarted,
            direct.processStopped, direct.helperExitCode,
            static_cast<long long>(direct.elapsedMilliseconds));
    }
    passed &= expect(firstReady,
        "The actual folder model must publish asynchronous native artwork and repaint notification.");
    if (!firstReady)
        return false; // A failed isolated helper must not lead into synchronous Shell comparisons.
    const QPixmap expected = QFileIconProvider().icon(QFileInfo(one.fileName())).pixmap(QSize(16, 16), 1.0);
    const QImage expectedPixels = expected.toImage();
    const QImage firstPixels = first.pixmap(QSize(16, 16), 1.0).toImage();
    const bool firstMatches = filemanager::tests::sameVisibleImage(expectedPixels, firstPixels);
    if (!firstMatches)
        describeVisibleDifference(expectedPixels, firstPixels);
    passed &= expect(firstMatches,
        "Model artwork must match the existing native file icon.");
    QTemporaryDir otherFolder;
    if (!otherFolder.isValid()) return false;
    QFile otherFile(otherFolder.filePath("other.txt"));
    if (!otherFile.open(QIODevice::WriteOnly)) return false;
    otherFile.close();
    model.setDirectory(otherFolder.path());
    passed &= expect(waitUntil([&] { return !model.loading() && model.rowCount() == 1; }),
        "A different folder should finish loading.");
    const QIcon otherIcon = model.indexForPath(otherFile.fileName()).data(Qt::DecorationRole).value<QIcon>();
    const QImage otherPixels = otherIcon.pixmap(QSize(16, 16), 1.0).toImage();
    const bool otherMatches = filemanager::tests::sameVisibleImage(expectedPixels, otherPixels);
    if (!otherMatches)
        describeVisibleDifference(expectedPixels, otherPixels);
    passed &= expect(otherMatches,
        "The real model must immediately reuse completed native type artwork across folders.");
    model.setDirectory(fixture.path());
    passed &= expect(waitUntil([&] { return !model.loading() && model.rowCount() == 2; }),
        "Returning to the first folder should finish loading.");
    const QIcon returned = model.indexForPath(one.fileName()).data(Qt::DecorationRole).value<QIcon>();
    passed &= expect(!returned.pixmap(QSize(16, 16), 1.0).isNull(),
        "Returning to a folder must retain completed type artwork.");
    model.refresh();
    passed &= expect(waitUntil([&] { return !model.loading(); })
            && returned.pixmap(QSize(16, 16), 1.0).isNull(),
        "A folder refresh must invalidate old icons even when file metadata is unchanged.");
    const QIcon fresh = model.indexForPath(one.fileName()).data(Qt::DecorationRole).value<QIcon>();
    passed &= expect(waitUntil([&] { return !fresh.pixmap(QSize(16, 16), 1.0).isNull(); }),
        "Refreshed rows must obtain fresh native icons.");
    model.setDirectory(fixture.filePath("missing"));
    passed &= expect(fresh.pixmap(QSize(16, 16), 1.0).isNull(),
        "Navigation must immediately revoke the previous folder's icon handles.");
    return passed;
}
}

bool verifyBackgroundFileIcons()
{
    const bool reset = resetAndDisposal();
    const bool overflow = overflowAndFailures();
    const bool reuse = navigationTypeReuse();
    const bool model = modelIntegration();
    std::printf("Background icon checks reset_disposal=%d overflow_failure=%d type_reuse=%d model_native=%d\n",
        reset, overflow, reuse, model);
    return reset && overflow && reuse && model;
}
