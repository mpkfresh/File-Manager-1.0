#pragma once

#include "previewservice.h"

#include <QObject>

#include <atomic>
#include <memory>

namespace filemanager::preview {

class PreviewController final : public QObject
{
    Q_OBJECT

public:
    struct IsolatedResult { bool success = false; PreviewData preview; QString error; };
    static constexpr int HelperTimeoutMilliseconds = 7000;
    static constexpr int MaximumReplyBytes = 32 * 1024 * 1024;
    explicit PreviewController(QObject* parent = nullptr);
    ~PreviewController() override;

    quint64 start(const QString& path);
    void cancel();
    static IsolatedResult loadIsolated(const QString& path,
        int timeoutMilliseconds = HelperTimeoutMilliseconds,
        const QString& helperExecutable = {}, const std::atomic_bool* cancel = nullptr);

signals:
    void ready(quint64 generation, const QString& path, const PreviewData& preview);

private:
    quint64 generation_ = 0;
    std::shared_ptr<std::atomic_bool> cancellation_;
};

} // namespace filemanager::preview
