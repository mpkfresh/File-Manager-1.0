#include "platform/fileicon.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QUuid>
#include <QtEndian>
#include <QScopeGuard>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>
#endif

namespace filemanager::platform {

#ifdef Q_OS_WIN
namespace {
QImage imageFromShellIcon(HICON icon)
{
    ICONINFO details{};
    if (!GetIconInfo(icon, &details))
        return {};
    const auto releaseDetails = qScopeGuard([&] {
        if (details.hbmColor) DeleteObject(details.hbmColor);
        if (details.hbmMask) DeleteObject(details.hbmMask);
    });
    // Shell icons use a centered hotspot; retain Qt's existing raster dimensions.
    if (details.xHotspot == 0 || details.yHotspot == 0
        || details.xHotspot > MaximumFileIconExtent / 2
        || details.yHotspot > MaximumFileIconExtent / 2)
        return {};
    const int width = static_cast<int>(details.xHotspot) * 2;
    const int height = static_cast<int>(details.yHotspot) * 2;
    HDC screen = GetDC(nullptr);
    if (!screen)
        return {};
    HDC memory = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!memory)
        return {};
    const auto releaseDc = qScopeGuard([&] { DeleteDC(memory); });
    BITMAPINFO format{};
    format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    format.bmiHeader.biWidth = width;
    format.bmiHeader.biHeight = -height;
    format.bmiHeader.biPlanes = 1;
    format.bmiHeader.biBitCount = 32;
    format.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &format, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap)
        return {};
    const auto releaseBitmap = qScopeGuard([&] { DeleteObject(bitmap); });
    if (!pixels)
        return {};
    HGDIOBJ previous = SelectObject(memory, bitmap);
    if (!previous || previous == HGDI_ERROR)
        return {};
    const auto restoreSelection = qScopeGuard([&] { SelectObject(memory, previous); });
    const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    std::memset(pixels, 0, byteCount);
    // Never call GetDIBits on a selected bitmap. Finish GDI drawing before
    // accessing DIB-section memory, as required by CreateDIBSection's contract.
    if (!DrawIconEx(memory, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL) || !GdiFlush())
        return {};
    QImage image = QImage(static_cast<const uchar*>(pixels), width, height, width * 4,
        QImage::Format_ARGB32_Premultiplied).copy();
    if (image.isNull())
        return {};
    bool alphaPresent = false;
    for (int y = 0; y < height && !alphaPresent; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < width; ++x)
            alphaPresent |= qAlpha(row[x]) != 0;
    }
    if (!alphaPresent) {
        std::memset(pixels, 0, byteCount);
        if (!DrawIconEx(memory, 0, 0, icon, width, height, 0, nullptr, DI_MASK) || !GdiFlush())
            return {};
        const auto* mask = static_cast<const QRgb*>(pixels);
        for (int y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < width; ++x)
                row[x] = qRed(mask[y * width + x]) != 0 ? 0 : row[x] | 0xff000000U;
        }
    }
    return image;
}
}
#endif

QImage readNativeIconImage(quintptr iconHandle)
{
#ifdef Q_OS_WIN
    return iconHandle ? imageFromShellIcon(reinterpret_cast<HICON>(iconHandle)) : QImage{};
#else
    Q_UNUSED(iconHandle);
    return {};
#endif
}

bool validFileIconRequest(const QString& path, int pixelExtent)
{
    return !path.isEmpty() && path.size() <= 8192 && !path.contains(QChar::Null)
        && QDir::isAbsolutePath(path) && pixelExtent > 0 && pixelExtent <= MaximumFileIconExtent;
}

QImage readFileIconInProcess(const QString& path, int pixelExtent)
{
    if (!validFileIconRequest(path, pixelExtent))
        return {};
#ifdef Q_OS_WIN
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
        return {};
    QImage result;
    const QString nativePath = QDir::toNativeSeparators(path);
    SHFILEINFOW info{};
    const int smallSize = GetSystemMetrics(SM_CXSMICON);
    const int largeSize = GetSystemMetrics(SM_CXICON);
    const UINT flags = SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_ADDOVERLAYS | SHGFI_OVERLAYINDEX
        | (pixelExtent > smallSize ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    const auto found = SHGetFileInfoW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        0, &info, sizeof(info), flags);
    if (found && info.hIcon) {
        if (pixelExtent > largeSize) {
            const int preferredList = pixelExtent > largeSize + largeSize / 2
                ? SHIL_JUMBO : SHIL_EXTRALARGE;
            const auto readImageList = [&](int listSize) {
                QImage image;
                IImageList* list = nullptr;
                if (SUCCEEDED(SHGetImageList(listSize, IID_PPV_ARGS(&list))) && list) {
                    HICON icon = nullptr;
                    const unsigned index = static_cast<unsigned>(info.iIcon);
                    const unsigned overlay = (index >> 24) & 0xff;
                    if (SUCCEEDED(list->GetIcon(static_cast<int>(index & 0x00ffffff),
                            ILD_TRANSPARENT | INDEXTOOVERLAYMASK(overlay), &icon)) && icon) {
                        image = readNativeIconImage(reinterpret_cast<quintptr>(icon));
                        DestroyIcon(icon);
                    }
                    list->Release();
                }
                return image;
            };
            result = readImageList(preferredList);
            if (result.isNull() && preferredList == SHIL_JUMBO)
                result = readImageList(SHIL_EXTRALARGE);
        }
        if (result.isNull())
            result = readNativeIconImage(reinterpret_cast<quintptr>(info.hIcon));
    }
    if (info.hIcon)
        DestroyIcon(info.hIcon);
    CoUninitialize();
    if (result.width() > MaximumFileIconExtent || result.height() > MaximumFileIconExtent)
        return {};
    return result;
#else
    return {};
#endif
}

QByteArray encodeFileIconReply(const QImage& image, const QByteArray& requestToken)
{
    if (requestToken.size() != 32 || image.isNull() || image.width() > MaximumFileIconExtent
            || image.height() > MaximumFileIconExtent)
        return {};
    // The native HICON raster is ARGB32 premultiplied. Converting it through
    // RGBA8888 premultiplied can round a channel by one even though alpha is
    // unchanged, so carry its 32-bit pixel words without format conversion.
    const bool nativePremultiplied = image.format() == QImage::Format_ARGB32_Premultiplied;
    const bool rgbaPremultiplied = image.format() == QImage::Format_RGBA8888_Premultiplied;
    const QImage pixels = nativePremultiplied ? image
        : rgbaPremultiplied ? image : image.convertToFormat(QImage::Format_RGBA8888);
    QByteArray reply(nativePremultiplied ? "FMICON03"
        : rgbaPremultiplied ? "FMICON02" : "FMICON01", 8);
    reply.append(requestToken);
    char dimensions[8];
    qToLittleEndian<quint32>(static_cast<quint32>(pixels.width()), dimensions);
    qToLittleEndian<quint32>(static_cast<quint32>(pixels.height()), dimensions + 4);
    reply.append(dimensions, 8);
    for (int row = 0; row < pixels.height(); ++row)
        reply.append(reinterpret_cast<const char*>(pixels.constScanLine(row)), pixels.width() * 4);
    return reply;
}

QImage decodeFileIconReply(const QByteArray& reply, const QByteArray& requestToken)
{
    if (requestToken.size() != 32 || reply.size() < 48 || reply.size() > MaximumFileIconReplyBytes
            || (QByteArrayView(reply).first(8) != QByteArrayView("FMICON01", 8)
                && QByteArrayView(reply).first(8) != QByteArrayView("FMICON02", 8)
                && QByteArrayView(reply).first(8) != QByteArrayView("FMICON03", 8))
            || QByteArrayView(reply).sliced(8, 32) != QByteArrayView(requestToken))
        return {};
    const quint32 width = qFromLittleEndian<quint32>(reply.constData() + 40);
    const quint32 height = qFromLittleEndian<quint32>(reply.constData() + 44);
    if (!width || !height || width > MaximumFileIconExtent || height > MaximumFileIconExtent
            || reply.size() != 48 + static_cast<qsizetype>(width) * height * 4)
        return {};
    const bool nativePremultiplied = reply.startsWith("FMICON03");
    const bool rgbaPremultiplied = reply.startsWith("FMICON02");
    const QImage pixels(reinterpret_cast<const uchar*>(reply.constData() + 48),
        static_cast<int>(width), static_cast<int>(height), static_cast<int>(width * 4),
        nativePremultiplied ? QImage::Format_ARGB32_Premultiplied
            : rgbaPremultiplied ? QImage::Format_RGBA8888_Premultiplied
                                : QImage::Format_RGBA8888);
    // The copy takes ownership before the reply buffer is released. V3 retains
    // the native byte layout; V2 remains decodable for compatibility.
    return rgbaPremultiplied ? pixels.convertToFormat(QImage::Format_ARGB32_Premultiplied)
                             : pixels.copy();
}

const char* fileIconStatusName(FileIconStatus status)
{
    switch (status) {
    case FileIconStatus::Ready: return "ready";
    case FileIconStatus::InvalidRequest: return "invalid_request";
    case FileIconStatus::Cancelled: return "cancelled";
    case FileIconStatus::StartFailed: return "start_failed";
    case FileIconStatus::TimedOut: return "timed_out";
    case FileIconStatus::OutputLimit: return "output_limit";
    case FileIconStatus::StandardError: return "standard_error";
    case FileIconStatus::HelperFailed: return "helper_failed";
    case FileIconStatus::InvalidReply: return "invalid_reply";
    case FileIconStatus::CleanupFailed: return "cleanup_failed";
    }
    return "unknown";
}

FileIconResult readFileIconIsolated(const QString& path, int pixelExtent, int timeoutMilliseconds,
    const ShellCancellation& cancelled, const QString& helperExecutable)
{
    if (!validFileIconRequest(path, pixelExtent) || timeoutMilliseconds < 100
            || timeoutMilliseconds > 10000)
        return {};
    if (cancelled && cancelled->load())
        return {FileIconStatus::Cancelled};
    const QByteArray token = QUuid::createUuid().toString(QUuid::Id128).toLatin1();
    const QString executable = helperExecutable.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("FileManagerShellHost.exe"))
        : helperExecutable;
    QProcess helper;
    helper.setProgram(executable);
    helper.setArguments({QStringLiteral("--file-icon"), path, QString::number(pixelExtent),
        QString::fromLatin1(token)});
    QElapsedTimer deadline;
    deadline.start();
    FileIconResult result;
    const auto finish = [&](FileIconStatus status) {
        if (helper.state() != QProcess::NotRunning) {
            helper.kill();
            helper.waitForFinished(1000);
        }
        result.processStopped = helper.state() == QProcess::NotRunning;
        result.status = result.processStopped ? status : FileIconStatus::CleanupFailed;
        result.elapsedMilliseconds = deadline.elapsed();
        if (result.processStarted && result.processStopped)
            result.helperExitCode = helper.exitCode();
        if (result.status != FileIconStatus::Ready)
            result.image = {};
        return result;
    };
    helper.start(QIODevice::ReadOnly);
    if (!helper.waitForStarted(qMin(timeoutMilliseconds, 1000))) {
        return finish(cancelled && cancelled->load() ? FileIconStatus::Cancelled
            : deadline.elapsed() >= timeoutMilliseconds ? FileIconStatus::TimedOut
            : FileIconStatus::StartFailed);
    }
    result.processStarted = true;
    QByteArray payload;
    qsizetype receivedBytes = 0;
    do {
        helper.waitForFinished(25);
        const QByteArray output = helper.readAllStandardOutput();
        const QByteArray error = helper.readAllStandardError();
        receivedBytes += output.size() + error.size();
        if (cancelled && cancelled->load())
            return finish(FileIconStatus::Cancelled);
        if (receivedBytes > MaximumFileIconReplyBytes)
            return finish(FileIconStatus::OutputLimit);
        if (!error.isEmpty())
            return finish(FileIconStatus::StandardError);
        if (deadline.elapsed() >= timeoutMilliseconds)
            return finish(FileIconStatus::TimedOut);
        payload.append(output);
    } while (helper.state() != QProcess::NotRunning);
    if (helper.exitStatus() != QProcess::NormalExit || helper.exitCode() != 0)
        return finish(FileIconStatus::HelperFailed);
    result.image = decodeFileIconReply(payload, token);
    return finish(result.image.isNull() ? FileIconStatus::InvalidReply : FileIconStatus::Ready);
}

} // namespace filemanager::platform
