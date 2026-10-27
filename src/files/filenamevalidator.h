#pragma once

#include <QString>

namespace filemanager::files {

struct ValidationResult
{
    bool valid = false;
    QString normalizedName;
    QString error;
};

ValidationResult validateWindowsName(const QString& candidate);
ValidationResult validateWindowsPath(const QString& candidate);
bool isPathInside(const QString& candidate, const QString& directory);

} // namespace filemanager::files
