#pragma once

#include <QString>

namespace filemanager::platform {

// Lexical parent of a normalized absolute filesystem path. Empty for roots/invalid input.
// Preserves extended Windows prefixes; accepts either separator and trailing separators.
// Does not check existence, resolve links, enumerate folders or bind a Shell provider.
QString fileSystemParentPath(const QString& path);

} // namespace filemanager::platform
