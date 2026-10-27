#include "platform/archivehandoff.h"

#include <QDir>
#include <QFileInfo>

namespace filemanager::platform {

NativeArchiveFormat nativeArchiveFormat(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toCaseFolded();
    if (suffix == QStringLiteral("rar"))
        return NativeArchiveFormat::Rar;
    if (suffix == QStringLiteral("7z"))
        return NativeArchiveFormat::SevenZip;
    if (suffix == QStringLiteral("tar"))
        return NativeArchiveFormat::Tar;
    return NativeArchiveFormat::None;
}

QString nativeArchiveFormatName(NativeArchiveFormat format)
{
    switch (format) {
    case NativeArchiveFormat::Rar:
        return QStringLiteral("RAR");
    case NativeArchiveFormat::SevenZip:
        return QStringLiteral("7z");
    case NativeArchiveFormat::Tar:
        return QStringLiteral("TAR");
    case NativeArchiveFormat::None:
        return {};
    }
    return {};
}

NativeArchiveHandoffValidation validateNativeArchiveHandoff(const QString& path)
{
    const QFileInfo info(QDir::cleanPath(QDir::fromNativeSeparators(path)));
    if (!info.isAbsolute() || !info.exists() || !info.isFile()) {
        return {false, NativeArchiveFormat::None, {},
            QStringLiteral("Select one available RAR, 7z, or TAR archive.")};
    }
    const NativeArchiveFormat format = nativeArchiveFormat(info.fileName());
    if (format == NativeArchiveFormat::None) {
        return {false, format, {},
            QStringLiteral("Windows archive handoff supports RAR, 7z, and TAR selections.")};
    }
    return {true, format, info.absoluteFilePath(), {}};
}

SystemOpenResult openNativeArchiveWithWindows(const QString& path)
{
    const NativeArchiveHandoffValidation validation = validateNativeArchiveHandoff(path);
    if (!validation.valid)
        return {false, 0, validation.error};
    return openWithSystemShell(validation.absolutePath);
}

} // namespace filemanager::platform
