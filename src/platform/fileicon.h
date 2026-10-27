#pragma once

#include "platform/shellitem.h"
#include <QImage>

namespace filemanager::platform {

// Fixed raw-pixel transport: no image decoder runs on helper-controlled input.
inline constexpr int MaximumFileIconExtent = 512;
inline constexpr qsizetype MaximumFileIconReplyBytes = 48 + 4 * MaximumFileIconExtent * MaximumFileIconExtent;

bool validFileIconRequest(const QString& path, int pixelExtent);
// Borrowed native icon; the caller retains ownership. Returns owned pixels.
QImage readNativeIconImage(quintptr iconHandle);
QImage readFileIconInProcess(const QString& path, int pixelExtent);
QByteArray encodeFileIconReply(const QImage& image, const QByteArray& requestToken);
QImage decodeFileIconReply(const QByteArray& reply, const QByteArray& requestToken);

enum class FileIconStatus {
    Ready, InvalidRequest, Cancelled, StartFailed, TimedOut, OutputLimit,
    StandardError, HelperFailed, InvalidReply, CleanupFailed
};
const char* fileIconStatusName(FileIconStatus status);
struct FileIconResult {
    FileIconStatus status = FileIconStatus::InvalidRequest;
    QImage image;
    qint64 elapsedMilliseconds = 0;
    int helperExitCode = -1;
    bool processStarted = false;
    bool processStopped = true;
};

// Blocking worker-only API. Uses the existing Shell helper, not the preview
// AppContainer. A helper override is for containment tests, as with ShellItem.
FileIconResult readFileIconIsolated(const QString& path, int pixelExtent,
    int timeoutMilliseconds = 5000, const ShellCancellation& cancelled = {},
    const QString& helperExecutable = {});

} // namespace filemanager::platform
