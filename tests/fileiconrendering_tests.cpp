#include "platform/fileicon.h"
#include "ui/applicationtheme.h"
#include "ui/fileiconimage.h"
#include "iconimagecomparison.h"

#include <QApplication>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QMap>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>

bool verifyBackgroundFileIcons();
using filemanager::tests::sameVisibleImage;

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    QImage comparisonFixture(1, 1, QImage::Format_RGBA8888);
    comparisonFixture.setPixelColor(0, 0, QColor(100, 120, 140, 200));
    QImage comparisonVariant = comparisonFixture;
    comparisonVariant.setPixelColor(0, 0, QColor(102, 118, 141, 200));
    if (!sameVisibleImage(comparisonFixture, comparisonVariant))
        return 6;
    comparisonVariant.setPixelColor(0, 0, QColor(103, 120, 140, 200));
    if (sameVisibleImage(comparisonFixture, comparisonVariant))
        return 6;
    comparisonVariant.setPixelColor(0, 0, QColor(100, 120, 140, 201));
    if (sameVisibleImage(comparisonFixture, comparisonVariant))
        return 6;
    application.setOrganizationName("FileManagerTests");
    application.setApplicationName("IconRenderingTests");
    QTemporaryDir fixture;
    if (!fixture.isValid())
        return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, fixture.path());
    filemanager::ui::applyApplicationTheme(application);
    if (!verifyBackgroundFileIcons())
        return 5;
    const QString file = fixture.filePath("artwork.png");
    QImage content(37, 23, QImage::Format_RGBA8888);
    content.fill(Qt::magenta);
    if (!content.save(file))
        return 3;
    QFileIconProvider provider;
    int comparisons = 0;
    int failures = 0;
    bool savedFailure = false;
    for (const QString& path : {file, fixture.path()}) {
        const QIcon original = provider.icon(QFileInfo(path));
        QMap<int, filemanager::platform::FileIconResult> responses;
        for (int logical : {16, 17, 24, 32, 48, 96}) {
            for (qreal dpr : {1.0, 1.5, 2.0}) {
                const int physical = qRound(logical * dpr);
                if (!responses.contains(physical))
                    responses.insert(physical, filemanager::platform::readFileIconIsolated(path, physical));
                const auto& response = responses[physical];
                if (response.status != filemanager::platform::FileIconStatus::Ready
                        || response.image.isNull() || !response.processStarted || !response.processStopped) {
                    std::fprintf(stderr, "Native input failed: kind=%s physical=%d status=%s exit=%d elapsed_ms=%lld\n",
                        path == file ? "file" : "folder", physical,
                        filemanager::platform::fileIconStatusName(response.status), response.helperExitCode,
                        static_cast<long long>(response.elapsedMilliseconds));
                    return 4;
                }
                const QIcon reconstructed = filemanager::ui::fileIconFromImage(response.image,
                    original.availableSizes());
                for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active, QIcon::Selected}) {
                    for (QIcon::State state : {QIcon::Off, QIcon::On}) {
                        ++comparisons;
                        const QSize requested(logical, logical);
                        const QPixmap expected = original.pixmap(requested, dpr, mode, state);
                        const QPixmap actual = reconstructed.pixmap(requested, dpr, mode, state);
                        const QImage expectedPixels = expected.toImage().convertToFormat(QImage::Format_RGBA8888);
                        const QImage actualPixels = actual.toImage().convertToFormat(QImage::Format_RGBA8888);
                        QImage expectedPaint(QSize(120, 120) * dpr, QImage::Format_ARGB32_Premultiplied);
                        expectedPaint.setDevicePixelRatio(dpr);
                        expectedPaint.fill(Qt::transparent);
                        QImage actualPaint = expectedPaint;
                        { QPainter painter(&expectedPaint); original.paint(&painter,
                            QRect(QPoint(3, 5), requested), Qt::AlignCenter, mode, state); }
                        { QPainter painter(&actualPaint); reconstructed.paint(&painter,
                            QRect(QPoint(3, 5), requested), Qt::AlignCenter, mode, state); }
                        const bool samePixels = sameVisibleImage(expectedPixels, actualPixels);
                        const bool sameDpr = qFuzzyCompare(expected.devicePixelRatio(), actual.devicePixelRatio());
                        const bool samePaint = sameVisibleImage(expectedPaint, actualPaint);
                        const bool sameSize = original.actualSize(requested, mode, state)
                            == reconstructed.actualSize(requested, mode, state);
                        if (!samePixels || !sameDpr || !samePaint || !sameSize) {
                            ++failures;
                            if (failures <= 12)
                                std::fprintf(stderr, "Icon rendering mismatch: kind=%s logical=%d dpr=%.2f mode=%d state=%d "
                                    "source=%dx%d expected=%dx%d actual=%dx%d expected_dpr=%.4f actual_dpr=%.4f "
                                    "pixels=%d paint=%d metrics=%d\n", path == file ? "file" : "folder", logical, dpr,
                                    int(mode), int(state), response.image.width(), response.image.height(), expected.width(),
                                    expected.height(), actual.width(), actual.height(), expected.devicePixelRatio(),
                                    actual.devicePixelRatio(), samePixels, samePaint, sameSize);
                            if (!savedFailure) {
                                fixture.setAutoRemove(false);
                                expectedPaint.save(fixture.filePath("expected-paint.png"));
                                actualPaint.save(fixture.filePath("actual-paint.png"));
                                expectedPixels.save(fixture.filePath("expected-pixels.png"));
                                actualPixels.save(fixture.filePath("actual-pixels.png"));
                                std::fprintf(stderr, "First rendering failure artifacts: %s\n", qPrintable(fixture.path()));
                                savedFailure = true;
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("Native icon rendering comparisons=%d failures=%d theme=file_manager "
        "fixtures=file,folder modes=4 states=2 logical_sizes=6 dpr=1,1.5,2 "
        "visible_rgb_tolerance=2 alpha=exact\n", comparisons, failures);
    return failures ? 1 : 0;
}
