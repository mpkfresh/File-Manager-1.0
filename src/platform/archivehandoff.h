#pragma once

#include "platform/systemopen.h"

#include <QString>

namespace filemanager::platform {

enum class NativeArchiveFormat { None, Rar, SevenZip, Tar };

struct NativeArchiveHandoffValidation final
{
    bool valid = false;
    NativeArchiveFormat format = NativeArchiveFormat::None;
    QString absolutePath;
    QString error;
};

NativeArchiveFormat nativeArchiveFormat(const QString& path);
QString nativeArchiveFormatName(NativeArchiveFormat format);
NativeArchiveHandoffValidation validateNativeArchiveHandoff(const QString& path);

// Uses the Windows-selected file association. File Manager does not parse or extract the archive.
SystemOpenResult openNativeArchiveWithWindows(const QString& path);

} // namespace filemanager::platform
