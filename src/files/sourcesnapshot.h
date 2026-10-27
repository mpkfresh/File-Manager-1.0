#pragma once

#include <QString>

namespace filemanager::files {

struct SourceSnapshot
{
    bool valid = false;
    quint64 volumeSerial = 0;
    quint64 fileIndex = 0;
    quint64 size = 0;
    quint64 lastWriteTime = 0;
    quint32 attributes = 0;
    bool operator==(const SourceSnapshot&) const = default;
};

// Metadata/file identity only; not a content hash or an exclusive mutation lease.
SourceSnapshot captureSourceSnapshot(const QString& path);

} // namespace filemanager::files
