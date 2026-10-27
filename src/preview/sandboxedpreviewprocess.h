#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>

namespace filemanager::preview {

struct SandboxedPreviewProcessResult
{
    bool success = false;
    bool sandboxed = false;
    bool jobRestricted = false;
    int exitCode = -1;
    QByteArray standardOutput;
    QString error;
};

// Blocks until completion, timeout or cancellation; call from a worker thread.
// If supplied, cancel must outlive this call. An explicit helperExecutable selects
// the unsandboxed test helper; leave it empty to use the Windows AppContainer host.
SandboxedPreviewProcessResult runSandboxedPreviewHost(const QString& sourcePath,
    const QString& mode, int timeoutMilliseconds, qsizetype maximumOutputBytes,
    const QString& helperExecutable = {}, const std::atomic_bool* cancel = nullptr);

} // namespace filemanager::preview
