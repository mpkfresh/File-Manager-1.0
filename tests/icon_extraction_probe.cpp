// Standalone, explicitly approved diagnostic. No automatic retries or production changes.
#include "platform/fileicon.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <cstdio>
#include <memory>
#include <float.h>
#include <xmmintrin.h>

namespace {
constexpr int Extent = 256;
constexpr int OutputLimit = 8 * 1024 * 1024;

void trace(const char* stage)
{
    std::fprintf(stderr, "stage=%s\n", stage);
    std::fflush(stderr);
}

QByteArray packed(const QImage& image)
{
    QByteArray bytes;
    for (int y = 0; y < image.height(); ++y)
        bytes.append(reinterpret_cast<const char*>(image.constScanLine(y)), image.width() * 4);
    return bytes;
}

QJsonObject raster(const QImage& image)
{
    if (image.isNull() || image.depth() != 32 || image.width() > Extent || image.height() > Extent)
        return {};
    const auto bytes = packed(image);
    return {{"width", image.width()}, {"height", image.height()}, {"format", int(image.format())},
        {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
        {"pixels", QString::fromLatin1(bytes.toBase64())}};
}

QJsonObject compare(const QJsonObject& left, const QJsonObject& right)
{
    const auto a = QByteArray::fromBase64(left["pixels"].toString().toLatin1());
    const auto b = QByteArray::fromBase64(right["pixels"].toString().toLatin1());
    const bool shape = left["width"] == right["width"] && left["height"] == right["height"];
    QJsonObject result{{"valid", !a.isEmpty() && !b.isEmpty()}, {"same_shape", shape},
        {"same_format", left["format"] == right["format"]},
        {"equal_bytes", shape && !a.isEmpty() && a == b},
        {"left_sha256", left["sha256"]}, {"right_sha256", right["sha256"]}};
    if (!shape || a.isEmpty() || a.size() != b.size())
        return result;
    int differences = 0;
    for (qsizetype p = 0; p < a.size(); p += 4) {
        if (QByteArrayView(a).sliced(p, 4) == QByteArrayView(b).sliced(p, 4))
            continue;
        if (differences++ == 0) {
            result["first_pixel"] = int(p / 4);
            result["left_bgra"] = QString::fromLatin1(a.mid(p, 4).toHex());
            result["right_bgra"] = QString::fromLatin1(b.mid(p, 4).toHex());
        }
    }
    result["different_pixels"] = differences;
    return result;
}

// GetIconInfo returns owned, unselected bitmap copies. Keep the raw BGRA bytes:
// declaring the layout does not convert or assert the source's alpha semantics.
QImage colorBitmap(HICON icon, QJsonObject& metadata)
{
    ICONINFO info{};
    if (!GetIconInfo(icon, &info))
        return {};
    BITMAP bitmap{};
    QImage image;
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap)
            && bitmap.bmWidth > 0 && bitmap.bmWidth <= Extent
            && bitmap.bmHeight > 0 && bitmap.bmHeight <= Extent) {
        metadata["source_bpp"] = int(bitmap.bmBitsPixel);
        metadata["source_planes"] = int(bitmap.bmPlanes);
        BITMAPINFO dib{};
        dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        dib.bmiHeader.biWidth = bitmap.bmWidth;
        dib.bmiHeader.biHeight = -bitmap.bmHeight;
        dib.bmiHeader.biPlanes = 1;
        dib.bmiHeader.biBitCount = 32;
        dib.bmiHeader.biCompression = BI_RGB;
        image = QImage(bitmap.bmWidth, bitmap.bmHeight, QImage::Format_ARGB32_Premultiplied);
        image.fill(0);
        HDC dc = GetDC(nullptr);
        const int rows = dc ? GetDIBits(dc, info.hbmColor, 0, UINT(bitmap.bmHeight),
            image.bits(), &dib, DIB_RGB_COLORS) : 0;
        if (dc)
            ReleaseDC(nullptr, dc);
        if (rows != bitmap.bmHeight)
            image = {};
    }
    if (info.hbmColor)
        DeleteObject(info.hbmColor);
    if (info.hbmMask)
        DeleteObject(info.hbmMask);
    return image;
}

QJsonObject capture(const QString& path, bool gui)
{
    trace("capture_begin");
    QJsonObject result;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
        return result;
    SHFILEINFOW info{};
    const QString native = QDir::toNativeSeparators(path);
    const UINT flags = SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_ADDOVERLAYS
        | SHGFI_OVERLAYINDEX | SHGFI_LARGEICON;
    trace("shell_file_info_begin");
    if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(native.utf16()), 0, &info, sizeof(info), flags)) {
        trace("shell_file_info_end");
        IImageList* list = nullptr;
        if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&list))) && list) {
            HICON icon = nullptr;
            const unsigned index = unsigned(info.iIcon);
            result["image_index"] = int(index & 0x00ffffff);
            result["overlay"] = int(index >> 24);
            if (SUCCEEDED(list->GetIcon(int(index & 0x00ffffff),
                    ILD_TRANSPARENT | INDEXTOOVERLAYMASK(index >> 24), &icon)) && icon) {
                // Capture the first conversion before GetIconInfo/GetDIBits. The
                // original probe inspected the bitmap first, unlike production.
                unsigned control = 0;
                _controlfp_s(&control, 0, 0);
                result["fp_before_conversion"] = static_cast<qint64>(control);
                result["mxcsr_before_conversion"] = static_cast<qint64>(_mm_getcsr());
                result["qt_before_source_read"] = raster(QImage::fromHICON(icon));
                trace("qt_before_source_read_captured");
                result["source"] = raster(colorBitmap(icon, result));
                trace("source_captured");
                _controlfp_s(&control, 0, 0);
                result["fp_after_source_read"] = static_cast<qint64>(control);
                result["mxcsr_after_source_read"] = static_cast<qint64>(_mm_getcsr());
                result["qt_first"] = raster(QImage::fromHICON(icon));
                result["qt_second"] = raster(QImage::fromHICON(icon));
                trace("qt_captured");
                result["source_after"] = raster(colorBitmap(icon, result));
                DestroyIcon(icon);
            }
            list->Release();
        }
    }
    if (info.hIcon)
        DestroyIcon(info.hIcon);
    CoUninitialize();
    trace("production_begin");
    result["production"] = raster(filemanager::platform::readFileIconInProcess(path, Extent));
    trace("production_end");
    if (gui) {
        trace("provider_begin");
        QFileIconProvider provider;
        result["provider"] = raster(provider.icon(QFileInfo(path)).pixmap(QSize(Extent, Extent), 1.0)
            .toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied));
        trace("provider_end");
    }
    result["dpi_awareness"] = int(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()));
    result["system_dpi"] = int(GetDpiForSystem());
    return result;
}

QJsonObject nativeConversionChecks()
{
    QJsonArray cases;
    for (int side : {16, 32, 48, 256}) {
        for (const auto* kind : {"alpha", "opaque", "mask"}) {
            HICON icon = nullptr;
            if (QByteArrayView(kind) == "mask") {
                const int rowBytes = ((side + 15) / 16) * 2;
                QByteArray mask(rowBytes * side * 2, char(0));
                for (int y = 0; y < side; ++y) {
                    for (int x = 0; x < side; ++x) {
                        const int planeRow = x < side / 2 ? y + side : y;
                        mask[planeRow * rowBytes + x / 8] |= char(0x80 >> (x % 8));
                    }
                }
                HBITMAP bitmap = CreateBitmap(side, side * 2, 1, 1, mask.constData());
                if (bitmap) {
                    ICONINFO info{};
                    info.fIcon = TRUE;
                    info.hbmMask = bitmap;
                    icon = CreateIconIndirect(&info);
                    DeleteObject(bitmap);
                }
            } else {
                QImage pixels(side, side, QImage::Format_ARGB32);
                for (int y = 0; y < side; ++y) {
                    for (int x = 0; x < side; ++x) {
                        const int alpha = QByteArrayView(kind) == "opaque" ? 255 : (x * 17 + y * 31) % 256;
                        pixels.setPixel(x, y, qRgba((x * 13) % 256, (y * 11) % 256, (x + y) % 256, alpha));
                    }
                }
                icon = pixels.toHICON();
            }
            const QImage expected = icon ? QImage::fromHICON(icon) : QImage{};
            const QImage actual = filemanager::platform::readNativeIconImage(reinterpret_cast<quintptr>(icon));
            QJsonObject row = compare(raster(actual), raster(expected));
            row["kind"] = kind;
            row["side"] = side;
            row["requested_shape"] = actual.size() == QSize(side, side);
            ICONINFO stillOwned{};
            row["borrowed_handle_preserved"] = icon && GetIconInfo(icon, &stillOwned);
            if (stillOwned.hbmColor) DeleteObject(stillOwned.hbmColor);
            if (stillOwned.hbmMask) DeleteObject(stillOwned.hbmMask);
            if (icon) DestroyIcon(icon);
            cases.append(row);
        }
    }
    return {{"null_handle_rejected", filemanager::platform::readNativeIconImage(0).isNull()},
        {"cases", cases}};
}

bool complete(const QJsonObject& sample, bool gui)
{
    for (const auto* key : {"qt_before_source_read", "source", "qt_first", "qt_second", "source_after", "production"})
        if (sample[key].toObject()["pixels"].toString().isEmpty())
            return false;
    return !gui || !sample["provider"].toObject()["pixels"].toString().isEmpty();
}

bool writeJson(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    const QByteArray bytes = QJsonDocument(object).toJson();
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size();
}

int parent(const QString& root)
{
    // The runner owns/resolves this fresh directory, redirects app-data/temp into it,
    // and performs checked cleanup. Never remove the root or any path here.
    QDir dir(root);
    if (!dir.exists() || !QDir::isAbsolutePath(root) || !dir.mkdir("folder"))
        return 2;
    QImage fixture(37, 23, QImage::Format_ARGB32);
    fixture.fill(Qt::magenta);
    if (!fixture.save(dir.filePath("dimensions.png")))
        return 2;
    QJsonArray comparisons;
    QJsonObject previous;
    for (int repeat = 0; repeat < 3; ++repeat) {
        QJsonObject pair;
        for (const auto* mode : {"core", "gui"}) {
            QProcess child;
            child.setProgram(QCoreApplication::applicationFilePath());
            child.setArguments({"--child", QString::fromLatin1(mode), dir.absolutePath()});
            child.setWorkingDirectory(dir.absolutePath());
            QElapsedTimer timer;
            timer.start();
            child.start(QIODevice::ReadOnly);
            if (!child.waitForStarted(1000))
                return 3;
            QByteArray output;
            QByteArray errors;
            const auto saveTrace = [&] {
                QFile file(dir.filePath(QString("capture-%1-%2.trace.log").arg(repeat).arg(mode)));
                return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                    && file.write(errors) == errors.size();
            };
            do {
                child.waitForFinished(25);
                output += child.readAllStandardOutput();
                errors += child.readAllStandardError();
                if (timer.elapsed() > 15000 || output.size() + errors.size() > OutputLimit) {
                    child.kill();
                    child.waitForFinished(1000);
                    output += child.readAllStandardOutput();
                    errors += child.readAllStandardError();
                    saveTrace();
                    std::fprintf(stderr, "child deadline/output limit: repeat=%d mode=%s elapsed_ms=%lld "
                        "stdout_bytes=%lld stderr_bytes=%lld stopped=%d\n%s", repeat, mode,
                        static_cast<long long>(timer.elapsed()), static_cast<long long>(output.size()),
                        static_cast<long long>(errors.size()), child.state() == QProcess::NotRunning,
                        errors.constData());
                    return 3;
                }
            } while (child.state() != QProcess::NotRunning);
            if (!saveTrace())
                return 4;
            const auto object = QJsonDocument::fromJson(output).object();
            // Child stderr contains bounded stage reporting; validity comes from its
            // normal exit and complete raster payload, not absence of trace messages.
            if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0
                    || !complete(object["file"].toObject(), QByteArrayView(mode) == "gui")
                    || !complete(object["folder"].toObject(), QByteArrayView(mode) == "gui")) {
                std::fprintf(stderr, "incomplete child: repeat=%d mode=%s exit=%d stderr=%s\n",
                    repeat, mode, child.exitCode(), errors.constData());
                return 3;
            }
            if (!writeJson(dir.filePath(QString("capture-%1-%2.json").arg(repeat).arg(mode)), object))
                return 4;
            pair[mode] = object;
            std::fprintf(stdout, "capture repeat=%d mode=%s elapsed_ms=%lld stopped=1\n",
                repeat, mode, static_cast<long long>(timer.elapsed()));
            std::fflush(stdout);
        }
        for (const auto* kind : {"file", "folder"}) {
            const auto core = pair["core"].toObject()[kind].toObject();
            const auto gui = pair["gui"].toObject()[kind].toObject();
            // Compare the shipped helper binary, not just this diagnostic's copy
            // of fileicon.cpp. Preserve its normal protocol and startup path.
            const auto isolated = filemanager::platform::readFileIconIsolated(
                dir.filePath(QByteArrayView(kind) == "file" ? "dimensions.png" : "folder"), Extent);
            const QJsonObject helperRaster = raster(isolated.image);
            const QJsonObject helperReport{{"status", filemanager::platform::fileIconStatusName(isolated.status)},
                {"elapsed_ms", isolated.elapsedMilliseconds}, {"exit_code", isolated.helperExitCode},
                {"started", isolated.processStarted}, {"stopped", isolated.processStopped},
                {"raster", helperRaster}};
            if (!writeJson(dir.filePath(QString("helper-%1-%2.json").arg(repeat).arg(kind)), helperReport))
                return 4;
            if (isolated.status != filemanager::platform::FileIconStatus::Ready
                || !isolated.processStopped || helperRaster["pixels"].toString().isEmpty()) {
                std::fprintf(stderr, "actual helper capture failed: repeat=%d kind=%s status=%s stopped=%d\n",
                    repeat, kind, filemanager::platform::fileIconStatusName(isolated.status), isolated.processStopped);
                return 3;
            }
            QJsonObject row{{"repeat", repeat}, {"kind", kind},
                {"core_dpi", core["system_dpi"]}, {"gui_dpi", gui["system_dpi"]},
                {"core_awareness", core["dpi_awareness"]}, {"gui_awareness", gui["dpi_awareness"]}};
            row["actual_helper_vs_core_production"] = compare(helperRaster, core["production"].toObject());
            row["actual_helper_vs_gui_production"] = compare(helperRaster, gui["production"].toObject());
            row["actual_helper_vs_gui_provider"] = compare(helperRaster, gui["provider"].toObject());
            for (const auto* stage : {"qt_before_source_read", "source", "qt_first", "production"})
                row[QString("cross_process_%1").arg(stage)] = compare(core[stage].toObject(), gui[stage].toObject());
            for (const auto* mode : {"core", "gui"}) {
                const auto sample = pair[mode].toObject()[kind].toObject();
                row[QString("%1_before_vs_after_source_read").arg(mode)] = compare(
                    sample["qt_before_source_read"].toObject(), sample["qt_first"].toObject());
                row[QString("%1_same_hicon_repeat").arg(mode)] = compare(sample["qt_first"].toObject(), sample["qt_second"].toObject());
                row[QString("%1_source_unchanged").arg(mode)] = compare(sample["source"].toObject(), sample["source_after"].toObject());
                row[QString("%1_source_vs_qt").arg(mode)] = compare(sample["source"].toObject(), sample["qt_first"].toObject());
                row[QString("%1_qt_vs_production").arg(mode)] = compare(sample["qt_first"].toObject(), sample["production"].toObject());
                if (!previous.isEmpty()) {
                    const auto prior = previous[mode].toObject()[kind].toObject();
                    for (const auto* stage : {"qt_before_source_read", "source", "qt_first", "production"})
                        row[QString("%1_previous_%2").arg(mode).arg(stage)] = compare(prior[stage].toObject(), sample[stage].toObject());
                }
            }
            row["gui_qt_vs_provider"] = compare(gui["qt_first"].toObject(), gui["provider"].toObject());
            comparisons.append(row);
        }
        previous = pair;
    }
    const QJsonObject report{{"completed", true}, {"children", 12}, {"application_captures", 6},
        {"actual_helper_captures", 6}, {"extent", Extent}, {"comparisons", comparisons}};
    if (!writeJson(dir.filePath("comparison.json"), report))
        return 4;
    std::fprintf(stdout, "icon_extraction_probe completed=1 children=12 fixture_contained=1\n");
    return 0; // Data collection completed; pixel differences are evidence, not a passing app test.
}
}

int main(int argc, char** argv)
{
    const bool child = argc == 4 && QByteArrayView(argv[1]) == "--child";
    const bool gui = child && QByteArrayView(argv[2]) == "gui";
    if (child)
        trace(gui ? "gui_application_begin" : "core_application_begin");
    std::unique_ptr<QCoreApplication> app;
    if (gui)
        app = std::make_unique<QApplication>(argc, argv);
    else
        app = std::make_unique<QCoreApplication>(argc, argv);
    if (child)
        trace("application_ready");
    const auto args = app->arguments();
    if (child) {
        if (args[2] != "core" && args[2] != "gui")
            return 2;
        const QDir root(args[3]);
        trace("file_begin");
        const auto file = capture(root.filePath("dimensions.png"), gui);
        trace("folder_begin");
        const auto folder = capture(root.filePath("folder"), gui);
        const QJsonObject object{{"file", file}, {"folder", folder},
            {"native_conversion_checks", nativeConversionChecks()}};
        trace("serialize_begin");
        const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        return std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout) == size_t(bytes.size()) ? 0 : 4;
    }
    if (args.size() == 3 && args[1] == "--run-root")
        return parent(args[2]);
    std::fprintf(stderr, "An explicitly approved --run-root directory is required.\n");
    return 2;
}
