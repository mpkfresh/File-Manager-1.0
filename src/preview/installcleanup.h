#pragma once

#include <QString>

namespace filemanager::preview {

QString installedPreviewRuntimePath();

// Exposed for a bounded filesystem fixture. Production callers should use
// removeInstalledPreviewSandbox(), which computes the exact app-owned location.
bool removePreviewRuntimeTree(const QString& runtimePath, QString* error = nullptr);
bool removeInstalledPreviewSandbox(QString* error = nullptr);

} // namespace filemanager::preview
