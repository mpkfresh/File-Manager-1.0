#pragma once

#include <QString>

namespace filemanager::platform {

struct CloudFileStatus
{
    bool inspected = false;
    bool providerBacked = false;
    bool cloudFilesPlaceholder = false;
    bool syncRoot = false;
    bool inSync = false;
    bool contentReady = false;
    bool partiallyOnDisk = false;
    bool pinned = false;
    bool unpinned = false;
};

// Reads attributes and reparse metadata only. It neither opens file contents nor requests
// hydration from a provider.
CloudFileStatus inspectCloudFileStatus(const QString& path);
QString cloudFileStatusText(const CloudFileStatus& status, bool includeOrdinaryItem = false);

// Metadata-only preflight for built-in previews. A Cloud Files placeholder is
// eligible only when Windows reports its content ready and no recall is needed.
// The opened handle must be checked again before any bytes are read.
bool isLocallyPreviewableFile(const QString& path);

} // namespace filemanager::platform
