#include "filenamevalidator.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace filemanager::files {

ValidationResult validateWindowsName(const QString& candidate)
{
    const QString name = candidate;
    if (name.isEmpty()) return {false, {}, "A name is required."};
    if (name == "." || name == "..") return {false, {}, "That name is reserved."};
    if (name.size() > 255)
        return {false, {}, "Windows file names cannot exceed 255 characters."};
    if (name.endsWith('.') || name.endsWith(' '))
        return {false, {}, "Windows names cannot end with a period or space."};
    static const QRegularExpression invalid(R"([<>:"/\\|?*\x00-\x1F])");
    if (name.contains(invalid)) return {false, {}, "The name contains a character Windows does not allow."};

    QString stem = name.section('.', 0, 0);
    while (stem.endsWith('.') || stem.endsWith(' '))
        stem.chop(1);
    stem = stem.toUpper();
    static const QSet<QString> reserved = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
        "LPT6", "LPT7", "LPT8", "LPT9", "CONIN$", "CONOUT$",
        QString::fromUtf8("COM¹"), QString::fromUtf8("COM²"), QString::fromUtf8("COM³"),
        QString::fromUtf8("LPT¹"), QString::fromUtf8("LPT²"), QString::fromUtf8("LPT³")
    };
    if (reserved.contains(stem)) return {false, {}, "That device name is reserved by Windows."};
    return {true, name, {}};
}

ValidationResult validateWindowsPath(const QString& candidate)
{
    if (candidate.isEmpty())
        return {false, {}, "A path is required."};
    const QString absolute = QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
    if (absolute.size() > 32'767)
        return {false, {}, "The path exceeds the Windows extended-length limit."};
    const QFileInfo info(absolute);
    const ValidationResult name = validateWindowsName(info.fileName());
    if (!name.valid)
        return name;
    return {true, absolute, {}};
}

bool isPathInside(const QString& candidate, const QString& directory)
{
    const QString child = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(candidate).absoluteFilePath()));
    const QString parent = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(directory).absoluteFilePath()));
    if (child.compare(parent, Qt::CaseInsensitive) == 0)
        return true;
    return child.startsWith(parent.endsWith('/') ? parent : parent + '/', Qt::CaseInsensitive);
}

} // namespace filemanager::files
