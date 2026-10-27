#include "previewcontroller.h"
#include "sandboxedpreviewprocess.h"
#include "files/scopeenumerator.h"

#include <QPointer>
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

namespace filemanager::preview {

PreviewController::PreviewController(QObject* parent)
    : QObject(parent)
{
}

PreviewController::~PreviewController()
{
    cancel();
}

quint64 PreviewController::start(const QString& path)
{
    cancel();
    const quint64 generation = ++generation_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    QPointer<PreviewController> guard(this);
    QThread* worker = QThread::create([guard, cancellation, generation, path] {
        const IsolatedResult result = loadIsolated(path, HelperTimeoutMilliseconds, {}, cancellation.get());
        PreviewData preview = result.preview;
        if (!result.success && !cancellation->load()) {
            preview.kind = PreviewKind::Error;
            preview.title = QFileInfo(path).fileName();
            preview.text = result.error;
        }
        if (!cancellation->load() && guard) {
            QMetaObject::invokeMethod(guard, [guard, generation, path, preview] {
                if (guard)
                    emit guard->ready(generation, path, preview);
            });
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return generation;
}

PreviewController::IsolatedResult PreviewController::loadIsolated(const QString& path,
    int timeoutMilliseconds, const QString& helperExecutable, const std::atomic_bool* cancel)
{
    if (cancel && cancel->load())
        return {false, {}, "Preview cancelled."};
    if (timeoutMilliseconds < 100 || timeoutMilliseconds > 30000)
        return {false, {}, "The preview helper timeout is outside policy."};
    const QFileInfo sourceInfo(path);
    if (helperExecutable.isEmpty() && sourceInfo.isDir()) {
        if (!filemanager::files::isRegularDirectoryChain(sourceInfo.absoluteFilePath()))
            return {false, {}, "Folder preview is unavailable for a cloud placeholder or "
                "redirected folder."};
        return {true, PreviewService{}.load(path), {}};
    }
    const auto process = runSandboxedPreviewHost(path, "preview", timeoutMilliseconds,
        MaximumReplyBytes, helperExecutable, cancel);
    if (!process.success)
        return {false, {}, process.error};
    const QByteArray payload = process.standardOutput;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return {false, {}, "The isolated preview reply was malformed."};
    const QJsonObject root = document.object();
    const int kind = root.value("kind").toInt(-1);
    if (kind < static_cast<int>(PreviewKind::None) || kind > static_cast<int>(PreviewKind::Error))
        return {false, {}, "The isolated preview reply contained an invalid kind."};
    PreviewData preview;
    preview.kind = static_cast<PreviewKind>(kind);
    preview.title = root.value("title").toString();
    preview.metadata = root.value("metadata").toString();
    preview.text = root.value("text").toString();
    if (preview.title.size() > 32768 || preview.metadata.size() > 1024 * 1024
            || preview.text.size() > 2 * 1024 * 1024)
        return {false, {}, "The isolated preview reply violated its text limits."};
    const QString encodedImage = root.value("image").toString();
    if (encodedImage.size() > 32 * 1024 * 1024)
        return {false, {}, "The isolated preview image exceeded its encoded limit."};
    const QByteArray imageBytes = QByteArray::fromBase64(encodedImage.toLatin1());
    if (!imageBytes.isEmpty() && !preview.image.loadFromData(imageBytes, "PNG"))
        return {false, {}, "The isolated preview image was malformed."};
    if (!preview.image.isNull() && (preview.image.width() > PreviewService::MaximumImageDimension
            || preview.image.height() > PreviewService::MaximumImageDimension))
        return {false, {}, "The isolated preview image exceeded its dimension limit."};
    return {true, preview, {}};
}

void PreviewController::cancel()
{
    if (cancellation_)
        cancellation_->store(true);
}

} // namespace filemanager::preview
